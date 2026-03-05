# AOSP 16 本地 HAL — 录音失败根本原因分析

## 背景

PR#3 针对 AOSP 16 重新引入了本地直通（LOCAL/Passthrough）音频 HAL，将 Android 12 的实现
适配到扩展后的 `DeviceHalInterface` / `StreamHalInterface` 接口（新增了约 20 个纯虚方法）。
在 `FactoryHal.cpp` 中定义了三条激活路径：

1. **强制启用** — 设置系统属性 `ro.audio.hal.force_local=true` 或 `persist.audio.hal.local.enabled=true`
2. **正常路径** — AIDL/HIDL 服务发现（保持不变）
3. **自动降级** — 当未发现任何 AIDL/HIDL 服务时，自动回退到本地模式

## 缺陷：降级条件过于宽泛

PR#3 引入的降级路径如下：

```cpp
// 只要 AIDL/HIDL 兼容性检查因任何原因失败，就触发：
if (isDevice) {          // ← 条件过宽
    createLocalHalService(true, &rawInterface);
    return rawInterface;
}
```

原意是"仅当完全找不到 AIDL/HIDL 服务时才降级"，实际行为却是"只要**版本兼容性检查**失败就降级"，
涵盖了 AIDL/HIDL 服务**确实存在**但满足以下任意一个条件的情况：

| 触发条件 | 示例 |
|---------|------|
| Effects HAL 不存在 | 设备有 AIDL 设备 HAL，但没有 effects HAL |
| 类型不匹配 | 设备 HAL 是 AIDL，effects HAL 是 HIDL 7.x |
| 主版本号不匹配 | 设备 HAL 是 AIDL v1，effects HAL 是 AIDL v2 |
| `createHalService()` 运行时失败 | `dlopen` 或工厂函数调用失败 |

`createPreferredImpl()` 中的兼容性检查逻辑：

```cpp
if (ifaceVersionIt != sAudioHALVersions.end() &&
    siblingVersionIt != sAudioHALVersions.end() &&
    ifaceVersionIt->getType() == siblingVersionIt->getType() &&
    ifaceVersionIt->getMajorVersion() == siblingVersionIt->getMajorVersion()) {
    // 正常路径 — 创建 AIDL/HIDL 服务
} else {
    ALOGW("Found no HAL version ...");
    // 流程进入本地 HAL 降级路径
}
```

只要上述四种触发条件之一发生，代码就会落入本地 HAL 降级分支，并在进程内加载 `libaudiohal@local.so`。

## 为何会导致录音失败

### 内存损坏路径

在 AIDL/HIDL 服务已在其他进程中运行的情况下，将厂商 HAL 加载到 audioserver 进程内部是**不安全的**。
具体过程如下：

1. `DevicesFactoryHalLocal::openDevice()` 调用 `hw_get_module_by_class()`。
2. 该函数在 **audioserver 进程**内 `dlopen()` 厂商 HAL 共享库。
3. 厂商 HAL 的静态初始化及 `init_check()` 内部会调用 `mmap()`。
4. `mmap()` 可能将新的匿名页映射到与 `audio_utils_fifo` 节流索引页**相同的地址**——
   该页由 RecordThread 和 FastCapture 线程通过 `MonoPipe` 共享。
5. `audio_utils_fifo` 写端内部存储的节流索引指针此时指向厂商 HAL 占用的错误内存页。

### 录音启动时崩溃

当第一条录音 Track 启动时：

1. `RecordThread` 构造函数调用 `AudioStreamInSource::negotiate()`，
   后者调用 `mStream->getBufferSize()` 和 `mStream->getAudioProperties()`，均成功。
2. 构造函数同时初始化 `FastCapture` 并将其与 `MonoPipe` 连接。
3. 录音循环第一次迭代时，`FastCapture::onWork()` 调用
   `mPipeSink->write(mReadBuffer, mReadBufferState)`。
4. `MonoPipe::write()` → `audio_utils_fifo_writer::release()` 在被损坏的节流地址上
   执行 `futex(FUTEX_WAKE_PRIVATE, …)`。
5. 内核返回 `EFAULT`（errno 14，错误地址）。
6. `LOG_ALWAYS_FATAL("release: unexpected err=%d errno=%d", …)` → **SIGABRT**。

audioserver 进程崩溃，录音永远无法完成。

### 静默失败路径（无 Legacy HAL）

对于只搭载 AIDL HAL、`/vendor/lib` 中没有 Legacy `.so` 的设备：

1. 错误的降级条件触发 `createLocalHalService()`。
2. `DevicesFactoryHalLocal::openDevice("primary")` 调用
   `hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, "primary", &mod)`。
3. 没有 Legacy HAL 模块 → `rc = -ENOENT`。
4. `loadHwModule_ll()` 返回 `nullptr`（未加载任何设备）。
5. AudioFlinger 没有主设备 → `openInputStream()` 永远不可达 → 录音**静默失败**。

## 修复方案

将本地 HAL 降级限制为设备 HAL 服务**确实不存在**的情况，
即 `ifaceVersionIt == sAudioHALVersions.end()`：

```cpp
// 修复前（PR#3 — 条件过宽）：
if (isDevice) {

// 修复后（本 PR — 仅当未找到任何设备服务时）：
if (isDevice && ifaceVersionIt == sAudioHALVersions.end()) {
```

### 为何此修复是安全的

* 当 `ifaceVersionIt == end()` 时，不存在任何 AIDL/HIDL 设备服务。
  此时在进程内加载本地厂商 HAL 是唯一选择；由于没有 AIDL 流处于活动状态，
  `audio_utils_fifo` 内存页不会受到污染。

* 当设备 HAL **确实存在**但 effects HAL 缺失或不兼容时，返回 `nullptr`（无设备工厂）
  是正确结果。这会向 AudioFlinger 表明 HAL 无法加载，比导致内存损坏要安全得多。

* **强制启用**路径（`ro.audio.hal.force_local=true`）不受影响。明确选择 LOCAL 模式的用户
  清楚自己在做什么。

## 相关 Pull Request

| PR | 标题 | 状态 |
|----|------|------|
| PR#3 | 为 AOSP 16 libaudiohal 新增 Local/Passthrough HAL 支持 | 已合并 |
| PR#4 | 修复 AOSP 16 libaudiohal Local/Passthrough 编译错误 | 草稿 |
| PR#5 | 修复 FactoryHal.cpp 中错误的 LOCAL HAL 降级导致 audioserver SIGABRT | 草稿 |
| PR#8 | 修复 AOSP16 上本地加载兼容性问题（本 PR） | 开放 |

PR#5（仍为草稿，尚未合并）提出了与本 PR 相同的单行修复，并对 SIGABRT 做了简要说明。
本 PR（PR#8）独立实现了同样的修复，并在此补充了完整的根本原因分析文档。
