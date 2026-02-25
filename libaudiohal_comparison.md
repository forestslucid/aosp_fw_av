# Android 12 vs Android 16: `media/libaudiohal` 对比分析

## 1. 总体概述

| 指标 | Android 12 | Android 16 | 变化 |
|------|-----------|-----------|------|
| 源码文件数 (`.cpp`/`.h`) | 36 | 94 | +58 (+161%) |
| 总代码行数 | 5,840 | 20,615 | +14,775 (+253%) |
| HAL 接口类型 | 仅 HIDL | HIDL + AIDL | 新增 AIDL 支持 |
| 支持 HAL 版本 | 4.0, 5.0, 6.0, 7.0 | 6.0, 7.0, 7.1, AIDL v1 | 移除旧版本，新增 7.1 和 AIDL |
| 测试文件 | 无 | 5 个测试文件（3,071 行） | 新增测试套件 |
| 本地直通模式 (Local/Passthrough) | 支持 | 移除 | 架构简化 |

**核心变化**：Android 16 的 `libaudiohal` 最大变化是引入了 **AIDL (Android Interface Definition Language)** 作为与 Audio HAL 通信的新机制，同时保留了 HIDL 的向后兼容。此外，移除了旧版本（4.0、5.0）的支持和本地直通模式（Local/Passthrough），并在整体架构和接口上进行了大量增强。

---

## 2. 架构变化

### 2.1 HAL 接口发现与加载机制

**Android 12** — `FactoryHalHidl.cpp` / `FactoryHalHidl.h`
- 仅支持 HIDL 接口发现
- 通过 HIDL `ServiceManager` 的 `getTransport()` 检查服务是否存在
- 使用字符串数组 `{"7.0", "6.0", "5.0", "4.0"}` 定义优先级
- 工厂函数签名：`createPreferredImpl(package, interface)`

**Android 16** — `FactoryHal.cpp` / `FactoryHal.h`
- 同时支持 AIDL 和 HIDL 接口发现
- AIDL 使用 `AServiceManager_isDeclared()` 检查服务
- HIDL 仍使用 `getTransport()` 检查
- 使用结构化的 `AudioHalVersionInfo` 数组定义版本优先级：
  ```cpp
  static const std::array<AudioHalVersionInfo, 4> sAudioHALVersions = {
      AudioHalVersionInfo(AudioHalVersionInfo::Type::AIDL, 1, 0),  // 最高优先级
      AudioHalVersionInfo(AudioHalVersionInfo::Type::HIDL, 7, 1),
      AudioHalVersionInfo(AudioHalVersionInfo::Type::HIDL, 7, 0),
      AudioHalVersionInfo(AudioHalVersionInfo::Type::HIDL, 6, 0),
  };
  ```
- 工厂函数简化为：`createPreferredImpl(bool isCore)`
- 引入 **sibling 版本协调机制**：确保 core HAL 和 effect HAL 使用相同类型（AIDL/HIDL）和主版本号的实现，从而避免加载多个共享库

### 2.2 本地直通模式的移除

**Android 12** 中存在以下 "Local" 实现类，提供 HAL 的直通（passthrough）模式：
- `DeviceHalLocal.cpp` / `.h`
- `DevicesFactoryHalLocal.cpp` / `.h`
- `DevicesFactoryHalHybrid.cpp` / `.h` （混合模式调度器）
- `StreamHalLocal.cpp` / `.h`

**Android 16** 中这些文件全部被移除。所有 HAL 通信都通过 HIDL 或 AIDL 的 IPC 机制进行。这一变化：
- 简化了代码架构
- 消除了直接调用 HAL 实现的路径
- 使得安全性和稳定性更好（HAL 运行在独立进程中）

### 2.3 新增 `AudioHalVersionInfo` 类型系统

**Android 16** 新增 `AudioHalVersionInfo.h`，继承自 `android::media::AudioHalVersion`，提供：
- HAL 类型区分（`Type::AIDL` vs `Type::HIDL`）
- 结构化的版本号（major/minor）
- `toVersionString()` 方法：HIDL 返回 `"7.0"` 格式，AIDL 返回 `"aidl"`
- 用于版本比较和动态库加载

---

## 3. 文件级变化详情

### 3.1 仅在 Android 12 中存在的文件（已移除）

| 文件 | 行数 | 说明 |
|------|------|------|
| `FactoryHalHidl.cpp` | 107 | HIDL-only 的工厂加载器 |
| `FactoryHalHidl.h` | 40 | 对应头文件 |
| `impl/ConversionHelperHidl.cpp` | 94 | HIDL 参数转换辅助（Android 16 中重构为模板化的头文件） |
| `impl/DeviceHalLocal.cpp` | 461 | 本地模式设备 HAL |
| `impl/DeviceHalLocal.h` | 111 | 对应头文件 |
| `impl/DevicesFactoryHalHybrid.cpp` | 95 | 混合模式工厂 |
| `impl/DevicesFactoryHalHybrid.h` | 62 | 对应头文件 |
| `impl/DevicesFactoryHalLocal.cpp` | 64 | 本地模式工厂 |
| `impl/DevicesFactoryHalLocal.h` | 44 | 对应头文件 |
| `impl/StreamHalLocal.cpp` | 427 | 本地模式流 |
| `impl/StreamHalLocal.h` | 126 | 对应头文件 |

### 3.2 仅在 Android 16 中存在的文件（新增）

#### AIDL 相关新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `impl/DeviceHalAidl.cpp` | 1,531 | AIDL 设备 HAL 客户端实现 |
| `impl/DeviceHalAidl.h` | 271 | 对应头文件 |
| `impl/StreamHalAidl.cpp` | 1,380 | AIDL 音频流 HAL 客户端实现 |
| `impl/StreamHalAidl.h` | 525 | 对应头文件 |
| `impl/DevicesFactoryHalAidl.cpp` | 228 | AIDL 设备工厂 |
| `impl/DevicesFactoryHalAidl.h` | 53 | 对应头文件 |
| `impl/EffectHalAidl.cpp` | 414 | AIDL 音效 HAL 客户端 |
| `impl/EffectHalAidl.h` | 117 | 对应头文件 |
| `impl/EffectsFactoryHalAidl.cpp` | 406 | AIDL 音效工厂 |
| `impl/EffectsFactoryHalAidl.h` | 105 | 对应头文件 |
| `impl/EffectBufferHalAidl.cpp` | 119 | AIDL 音效缓冲区 |
| `impl/EffectBufferHalAidl.h` | 64 | 对应头文件 |
| `impl/ConversionHelperAidl.cpp` | 117 | AIDL 参数转换辅助 |
| `impl/ConversionHelperAidl.h` | 119 | 对应头文件 |
| `impl/EffectConversionHelperAidl.cpp` | 544 | AIDL 音效参数转换 |
| `impl/EffectConversionHelperAidl.h` | 161 | 对应头文件 |
| `impl/Hal2AidlMapper.cpp` | 1,190 | HIDL→AIDL 参数映射层 |
| `impl/Hal2AidlMapper.h` | 238 | 对应头文件 |
| `impl/AidlUtils.cpp` | 47 | AIDL 通用工具函数 |
| `impl/AidlUtils.h` | 61 | 对应头文件 |

#### 音效 AIDL 转换层 (`effectsAidlConversion/`)

16 个文件对（`.cpp` + `.h`），共计约 2,843 行代码，覆盖以下音效类型的 AIDL 参数转换：

| 音效类型 | 代码行数 |
|----------|---------|
| AEC (回声消除) | 121 |
| AGC1 (自动增益控制 v1) | 163 |
| AGC2 (自动增益控制 v2) | 126 |
| BassBoost (低音增强) | 111 |
| Downmix (下混音) | 97 |
| DynamicsProcessing (动态处理) | 477 |
| EnvReverb (环境混响) | 256 |
| Equalizer (均衡器) | 321 |
| HapticGenerator (触觉生成) | 102 |
| LoudnessEnhancer (响度增强) | 95 |
| NoiseSuppression (噪声抑制) | 112 |
| PresetReverb (预设混响) | 97 |
| Spatializer (空间化) | 353 |
| VendorExtension (厂商扩展) | 71 |
| Virtualizer (虚拟化器) | 163 |
| Visualizer (可视化器) | 178 |

#### 其他新增文件

| 文件 | 说明 |
|------|------|
| `FactoryHal.cpp` / `FactoryHal.h` | 替代 `FactoryHalHidl`，支持 AIDL+HIDL |
| `AudioHalVersionInfo.h` | 版本信息类型 |
| `impl/CoreConversionHelperHidl.cpp` / `.h` | 从 ConversionHelperHidl 拆分出的 Core HAL 转换辅助 |
| `impl/EffectConversionHelperHidl.cpp` / `.h` | 从 ConversionHelperHidl 拆分出的 Effect HAL 转换辅助 |
| `impl/DevicesFactoryHalEntry.cpp` | 设备工厂入口（替代旧的 Hybrid 调度） |
| `impl/EffectsFactoryHalEntry.cpp` | 音效工厂入口 |
| `impl/EffectProxy.cpp` / `.h` | 音效代理（用于 AIDL 音效的代理模式） |
| `impl/Cleanups.h` | 资源清理辅助 |
| `TEST_MAPPING` | 测试映射配置 |
| `tests/` | 完整的测试套件（5 个测试文件） |

### 3.3 两个版本中都存在但有修改的文件

| 文件 | 主要变化 |
|------|---------|
| `Android.bp` | 新增 AIDL 依赖，移除 4.0/5.0 版本，新增 7.1/AIDL 库；新增 `libaudiohalimpl_headers` |
| `DevicesFactoryHalInterface.cpp` | 引用从 `FactoryHalHidl.h` 改为 `FactoryHal.h`；参数简化为 `bool isCore` |
| `EffectsFactoryHalInterface.cpp` | 同上 |
| `impl/Android.bp` | 大幅重构：引入 `filegroup` 模块化，新增 AIDL 库定义，新增 `libaudiohal@7.1` 和 `libaudiohal@aidl` |
| `impl/ConversionHelperHidl.h` | 模板化重构：使用模板参数 `HalResult` 替代硬编码的 `CoreResult`；移除了命名空间 `CPP_VERSION` |
| `impl/DeviceHalHidl.cpp/.h` | 继承改为 `CoreConversionHelperHidl`；移除 `CPP_VERSION` 命名空间；新增多个方法 |
| `impl/DevicesFactoryHalHidl.cpp/.h` | 移除 `CPP_VERSION` 命名空间；新增 `getDeviceNames`、`getHalVersion`、`getSurroundSoundConfig`、`getEngineConfig` |
| `impl/StreamHalHidl.cpp/.h` | 继承改为 `CoreConversionHelperHidl`；移除 `CPP_VERSION` 命名空间；新增 legacy patch 方法 |
| `impl/EffectHalHidl.cpp/.h` | 继承新增 `EffectConversionHelperHidl`；移除 `isLocal()`；新增线程优先级管理 |
| `impl/EffectsFactoryHalHidl.cpp/.h` | 继承改为 `EffectConversionHelperHidl`；新增 `getProcessings()`、`getSkippedElements()` |
| `impl/EffectBufferHalHidl.cpp/.h` | 命名空间变化 |
| `impl/ParameterUtils.h` | 小幅调整 |
| `impl/StreamPowerLog.h` | 小幅调整 |
| `include/media/audiohal/DeviceHalInterface.h` | 大量新增 API（见下文） |
| `include/media/audiohal/DevicesFactoryHalInterface.h` | 新增 `getDeviceNames()`、`getHalVersion()`、`getSurroundSoundConfig()`、`getEngineConfig()`；移除 `getHalPids()` |
| `include/media/audiohal/EffectHalInterface.h` | 移除 `isLocal()`；新增 `setDevices()` |
| `include/media/audiohal/EffectsFactoryHalInterface.h` | 新增 `getDescriptors()`、`getProcessings()`、`getSkippedElements()`；`getHalVersion()` 返回类型从 `float` 改为 `AudioHalVersionInfo` |
| `include/media/audiohal/StreamHalInterface.h` | 大量新增 API（见下文） |

---

## 4. 接口 API 变化

### 4.1 `DeviceHalInterface` 新增 API

```cpp
// Android 16 新增
virtual status_t getAudioPorts(std::vector<media::audio::common::AudioPort> *ports) = 0;
virtual status_t getAudioRoutes(std::vector<media::AudioRoute> *routes) = 0;
virtual status_t getSupportedModes(std::vector<media::audio::common::AudioMode> *modes) = 0;
virtual status_t getMmapPolicyInfos(...) = 0;
virtual int32_t getAAudioMixerBurstCount() = 0;
virtual int32_t getAAudioHardwareBurstMinUsec() = 0;
virtual status_t supportsBluetoothVariableLatency(bool* supports) = 0;
virtual status_t setConnectedState(const struct audio_port_v7* port, bool connected) = 0;
virtual status_t setSimulateDeviceConnections(bool enabled) = 0;
virtual error::Result<audio_hw_sync_t> getHwAvSync() = 0;
virtual status_t getSoundDoseInterface(const std::string& module, ::ndk::SpAIBinder* soundDoseBinder) = 0;
virtual status_t prepareToDisconnectExternalDevice(const struct audio_port_v7* port) = 0;
virtual status_t getAudioMixPort(const struct audio_port_v7* devicePort, struct audio_port_v7* mixPort) = 0;
```

### 4.2 `DeviceHalInterface` API 签名变化

| 方法 | Android 12 | Android 16 |
|------|-----------|-----------|
| `getInputBufferSize` | `const struct audio_config*` | `struct audio_config*`（可修改配置） |
| `openOutputStream` | 6 个参数 | 7 个参数（新增 `sourceMetadata`） |
| `getMicrophones` | `vector<MicrophoneInfo>` | `vector<audio_microphone_characteristic_t>` |
| `addDeviceEffect` | `audio_port_handle_t` | `const struct audio_port_config*` |
| `removeDeviceEffect` | `audio_port_handle_t` | `const struct audio_port_config*` |
| `dump` | `dump(int fd)` | `dump(int fd, const Vector<String16>& args)` |

### 4.3 `StreamOutHalInterface` 新增 API

```cpp
// Android 16 新增
virtual status_t presentationComplete() = 0;
virtual status_t setLatencyMode(audio_latency_mode_t mode) = 0;
virtual status_t getRecommendedLatencyModes(std::vector<audio_latency_mode_t> *modes) = 0;
virtual status_t setLatencyModeCallback(const sp<StreamOutHalInterfaceLatencyModeCallback>& callback) = 0;
virtual status_t exit() = 0;
```

### 4.4 `StreamOutHalInterface` API 签名变化

| 方法 | Android 12 | Android 16 |
|------|-----------|-----------|
| `getRenderPosition` | `uint32_t *dspFrames` | `uint64_t *dspFrames`（扩大范围） |
| `getNextWriteTimestamp` | 存在 | **已移除** |
| `onError` 回调 | `onError()` | `onError(bool isHardError)` |
| `onCodecFormatChanged` 回调 | `std::basic_string<uint8_t>` | `std::vector<uint8_t>` |

### 4.5 `StreamHalInterface` 新增 API

```cpp
// Android 16 新增
virtual status_t legacyCreateAudioPatch(...) = 0;
virtual status_t legacyReleaseAudioPatch() = 0;
```

### 4.6 新增回调接口

```cpp
// Android 16 新增
class StreamOutHalInterfaceLatencyModeCallback : public virtual RefBase {
    virtual void onRecommendedLatencyModeChanged(std::vector<audio_latency_mode_t> modes) = 0;
};
```

### 4.7 `DevicesFactoryHalInterface` 变化

| 变化类型 | 详情 |
|---------|------|
| 新增 | `getDeviceNames()` — 获取可用设备名称列表 |
| 新增 | `getHalVersion()` — 返回 `AudioHalVersionInfo`（结构化版本信息） |
| 新增 | `getSurroundSoundConfig()` — 获取环绕声配置 |
| 新增 | `getEngineConfig()` — 获取音频引擎配置 |
| 移除 | `getHalPids()` — 不再通过此接口获取 HAL 进程 ID |

### 4.8 `EffectsFactoryHalInterface` 变化

| 变化类型 | 详情 |
|---------|------|
| 新增 | `getDescriptors(type, descriptors)` — 按类型批量获取描述符 |
| 新增 | `getProcessings()` — 获取音效处理配置 |
| 新增 | `getSkippedElements()` — 获取配置解析跳过的元素数 |
| 修改 | `getHalVersion()` 从 `float` 改为 `AudioHalVersionInfo` |

### 4.9 `EffectHalInterface` 变化

| 变化类型 | 详情 |
|---------|------|
| 移除 | `isLocal()` — 本地模式已移除 |
| 新增 | `setDevices(AudioDeviceTypeAddrVector)` — AIDL 音效的设备关联 |

---

## 5. 构建系统变化 (`Android.bp`)

### 5.1 顶层 `Android.bp`

| 变化 | Android 12 | Android 16 |
|------|-----------|-----------|
| 源文件 | `FactoryHalHidl.cpp` | `FactoryHal.cpp` |
| 依赖的实现库 | `libaudiohal@4.0`, `@5.0`, `@6.0`, `@7.0` | `libaudiohal@6.0`, `@7.0`, `@7.1`, `@aidl` |
| 新增共享库 | — | `audioclient-types-aidl-cpp`, `libaudiofoundation`, `libbinder_ndk` |
| 新增头文件库 | — | `liberror_headers` |
| 新增 | — | `libaudiohalimpl_headers` （导出 impl 目录头文件） |

### 5.2 `impl/Android.bp`

**Android 12**：
- 单一 `cc_defaults` (`libaudiohal_default`) 包含所有源文件
- 4 个共享库：`libaudiohal@4.0`, `@5.0`, `@6.0`, `@7.0`

**Android 16**：
- 引入 `filegroup` 实现源文件模块化复用：
  - `audio_core_hal_client_sources` — Core HAL HIDL 客户端源文件
  - `audio_effect_hidl_hal_client_sources` — Effect HAL HIDL 客户端源文件
  - `core_audio_hal_aidl_src_files` — Core HAL AIDL 客户端源文件
  - `audio_effect_hal_aidl_src_files` — Effect HAL AIDL 客户端源文件
  - `audio_effectproxy_src_files` — 音效代理
- 3 个 `cc_defaults`：
  - `libaudiohal_default` — 通用默认配置
  - `libaudiohal_hidl_default` — HIDL 特有依赖
  - `libaudiohal_aidl_default` — AIDL 特有依赖
- 5 个输出库：
  - `libaudiohal@6.0` — HIDL 6.0
  - `libaudiohal.effect@7.0` — 静态库，Effect HAL 7.0（被 7.0 和 7.1 复用）
  - `libaudiohal@7.0` — HIDL 7.0
  - `libaudiohal@7.1` — HIDL 7.1（新增）
  - `libaudiohal@aidl` — AIDL 实现（新增）

---

## 6. 命名空间与代码组织变化

### 6.1 移除版本化命名空间

**Android 12** 中大量使用 `namespace CPP_VERSION`：
```cpp
namespace android {
namespace CPP_VERSION {
class DeviceHalHidl : public DeviceHalInterface, public ConversionHelperHidl { ... };
} // namespace CPP_VERSION
} // namespace android
```

**Android 16** 中移除了 `CPP_VERSION` 命名空间包裹，直接放在 `android` 命名空间下：
```cpp
namespace android {
class DeviceHalHidl : public DeviceHalInterface, public CoreConversionHelperHidl { ... };
} // namespace android
```

这一变化影响几乎所有 HIDL 实现文件，简化了命名空间结构。

### 6.2 转换辅助类重构

**Android 12**：
- 单一类 `ConversionHelperHidl`（包含 `.cpp` 实现）
- 硬编码使用 `CoreResult` 类型

**Android 16**：
- `ConversionHelperHidl<HalResult>` — 模板化的基类，仅头文件
- `CoreConversionHelperHidl` — Core HAL 专用转换辅助（新增 `.cpp`）
- `EffectConversionHelperHidl` — Effect HAL 专用转换辅助（新增 `.cpp`）
- `ConversionHelperAidl` — AIDL 专用转换辅助（新增）
- `EffectConversionHelperAidl` — AIDL Effect 专用转换辅助（新增）

---

## 7. 关键设计模式变化

### 7.1 从 Hybrid 到 Entry 模式

**Android 12** 使用 "Hybrid" 模式 (`DevicesFactoryHalHybrid`)：
- 同时持有 Local 和 HIDL 实现
- 根据场景选择使用哪个实现
- 复杂的调度逻辑

**Android 16** 使用 "Entry" 模式 (`DevicesFactoryHalEntry`)：
- 单一入口点，由 `FactoryHal.cpp` 在加载时决定使用 AIDL 还是 HIDL
- 运行时不再有调度逻辑
- 更清晰的关注点分离

### 7.2 AIDL 的 `Hal2AidlMapper`

Android 16 中新增的 `Hal2AidlMapper`（1,190 + 238 行）是 AIDL 实现的核心组件：
- 负责将传统的 Audio Framework 概念（如 `audio_port_config`、`audio_patch`）映射到 AIDL HAL 的概念（如 `AudioPort`、`AudioRoute`）
- 管理端口和路由的状态
- 处理 AIDL HAL 中设备连接/断开的逻辑

### 7.3 EffectProxy 模式

Android 16 新增了 `EffectProxy`（356 + 161 行）：
- 用于音效的代理模式，主要用于 AIDL 音效 HAL
- 允许在 SW（软件）和 HW（硬件）音效实现之间进行代理

---

## 8. 功能增强总结

### 8.1 蓝牙与延迟控制
- 新增 `supportsBluetoothVariableLatency()` — 蓝牙可变延迟支持
- 新增 `setLatencyMode()` / `getRecommendedLatencyModes()` — 延迟模式控制
- 新增 `StreamOutHalInterfaceLatencyModeCallback` — 延迟模式变更回调

### 8.2 AAudio / MMAP 增强
- 新增 `getMmapPolicyInfos()` — MMAP 策略信息
- 新增 `getAAudioMixerBurstCount()` — AAudio 混合器突发计数
- 新增 `getAAudioHardwareBurstMinUsec()` — AAudio 硬件突发最小微秒数

### 8.3 音频路由与端口
- 新增 `getAudioPorts()` — 获取音频端口列表（AIDL 类型）
- 新增 `getAudioRoutes()` — 获取音频路由列表
- 新增 `getAudioMixPort()` — 获取混合端口信息
- 新增 `setConnectedState()` — 外部设备连接状态管理
- 新增 `prepareToDisconnectExternalDevice()` — 准备断开外部设备
- 新增 `setSimulateDeviceConnections()` — 模拟设备连接（调试用）

### 8.4 音效系统增强
- 新增 `getProcessings()` — 获取音效处理配置
- 新增 `getDescriptors()` — 按类型批量获取音效描述符
- 新增 `setDevices()` — 为 AIDL 音效设置关联设备
- 新增完整的 AIDL 音效参数转换层（16 种音效类型）
- 新增 `EffectProxy` — 音效代理

### 8.5 Sound Dose (声音剂量)
- 新增 `getSoundDoseInterface()` — 获取声音剂量接口（用于听力保护）

### 8.6 音频配置查询
- 新增 `getSurroundSoundConfig()` — 环绕声配置
- 新增 `getEngineConfig()` — 音频引擎配置
- 新增 `getSupportedModes()` — 支持的音频模式
- 新增 `getDeviceNames()` — 设备名称列表

### 8.7 播放控制增强
- 新增 `presentationComplete()` — 播放完成通知
- 新增 `exit()` — 退出信号
- `getRenderPosition` 从 `uint32_t` 升级为 `uint64_t`

### 8.8 流的 Legacy Patch 支持
- 新增 `legacyCreateAudioPatch()` / `legacyReleaseAudioPatch()` — 为向后兼容提供传统 patch 操作

---

## 9. 测试增强

**Android 12**：无测试文件

**Android 16**：新增完整测试套件
| 测试文件 | 行数 | 说明 |
|---------|------|------|
| `CoreAudioHalAidl_test.cpp` | 1,464 | Core AIDL HAL 客户端测试 |
| `EffectsFactoryHalInterface_test.cpp` | 527 | 音效工厂接口测试 |
| `EffectHalAidl_test.cpp` | 349 | AIDL 音效 HAL 测试 |
| `EffectHalVersionCompatibility_test.cpp` | 321 | 音效 HAL 版本兼容性测试 |
| `EffectProxy_test.cpp` | 292 | 音效代理测试 |

`TEST_MAPPING` 配置了预提交测试，包括 `CtsNativeMediaAAudioTestCases` 和 `CoreAudioHalAidlTest`。

---

## 10. 总结

Android 12 到 Android 16 的 `libaudiohal` 经历了一次重大架构升级：

1. **AIDL 优先**：AIDL 成为首选 HAL 接口（优先级最高），HIDL 保留向后兼容
2. **架构简化**：移除本地直通模式和旧版本（4.0/5.0）支持，减少了约 1,400 行旧代码
3. **功能大幅扩展**：代码量从 5,840 行增长到 20,615 行，新增约 14,775 行代码
4. **接口丰富化**：`DeviceHalInterface` 和 `StreamOutHalInterface` 新增大量 API，支持蓝牙延迟控制、AAudio/MMAP、声音剂量、空间音频等新功能
5. **构建系统现代化**：使用 `filegroup` 模块化管理源文件，提高代码复用
6. **测试覆盖**：从零测试到约 3,000 行测试代码
7. **代码质量**：统一代码风格，使用 `override`、`= default`、`#pragma once` 等现代 C++ 实践
