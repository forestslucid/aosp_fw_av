# AOSP 16 本地直通模式 (Local/Passthrough) 兼容实现文档

## 目录

1. [项目背景](#1-项目背景)
2. [问题定义与目标](#2-问题定义与目标)
3. [技术分析](#3-技术分析)
4. [实现方案设计](#4-实现方案设计)
5. [详细实现](#5-详细实现)
6. [构建系统集成](#6-构建系统集成)
7. [工厂加载机制](#7-工厂加载机制)
8. [使用指南](#8-使用指南)
9. [局限性与后续工作](#9-局限性与后续工作)

---

## 1. 项目背景

### 1.1 Android 音频 HAL 架构演进

Android 的音频子系统通过 **Audio HAL (Hardware Abstraction Layer)** 与底层硬件驱动交互。在不同的 Android 版本中，HAL 的加载和通信方式经历了重大演变：

| 版本 | HAL 通信方式 | 本地直通模式 |
|------|------------|-------------|
| Android 8.0 之前 | 直接函数调用（Legacy HAL） | 默认方式 |
| Android 8.0 ~ 11 | HIDL IPC | 支持（Passthrough 模式） |
| Android 12 | HIDL IPC | 支持（Local + Hybrid 模式） |
| Android 16 | AIDL / HIDL IPC | **已移除** |

### 1.2 Android 12 的本地直通模式

在 Android 12 中，`media/libaudiohal` 提供了三种 HAL 加载模式：

1. **HIDL 远程模式**（`DeviceHalHidl` / `StreamHalHidl`）：通过 HIDL IPC 与运行在独立进程中的 HAL 通信
2. **本地直通模式**（`DeviceHalLocal` / `StreamHalLocal`）：在 AudioFlinger 进程内直接加载 HAL `.so` 库并调用函数
3. **混合模式**（`DevicesFactoryHalHybrid`）：同时持有 Local 和 HIDL 实现，根据场景选择

关键文件：
```
impl/DeviceHalLocal.cpp / .h          — 本地设备 HAL 实现
impl/StreamHalLocal.cpp / .h          — 本地音频流实现
impl/DevicesFactoryHalLocal.cpp / .h  — 本地设备工厂
impl/DevicesFactoryHalHybrid.cpp / .h — 混合模式调度器
```

### 1.3 Android 16 移除本地直通模式的原因

Android 16 移除了本地直通模式，理由是：

- **安全性**：HAL 运行在独立进程中，崩溃不影响 AudioFlinger
- **稳定性**：进程隔离避免内存泄漏等问题
- **架构简化**：减少了约 1,400 行旧代码，消除了 Hybrid 调度复杂度
- **AIDL 优先**：新的 AIDL HAL 成为首选，HIDL 保留向后兼容

### 1.4 为什么需要恢复本地直通模式

在某些特定场景下，本地直通模式仍有其价值：

- **嵌入式/定制设备**：不运行完整 Android HAL 服务的场景
- **调试和开发**：直接函数调用更易于调试
- **低延迟需求**：消除 IPC 开销
- **兼容老旧 HAL**：部分厂商 HAL 未迁移到 HIDL/AIDL 服务模式
- **精简系统**：无需运行 HAL 服务进程

---

## 2. 问题定义与目标

### 2.1 核心问题

在 AOSP 16 上，当系统满足特定条件时，需要能够回退到本地直通（Local/Passthrough）方式加载音频 HAL，绕过 AIDL/HIDL IPC 机制，直接在 AudioFlinger 进程内调用 `audio_hw_device_t` 等 C 接口。

### 2.2 实现目标

1. **编译通过**：新增文件与修改能在 AOSP 16 构建系统中正确编译
2. **基础功能**：LOCAL 加载模式至少具备基础的播放和录音功能
3. **条件触发**：通过系统属性控制是否启用 LOCAL 模式
4. **自动降级**：当没有 AIDL/HIDL 服务可用时，自动尝试 LOCAL 模式
5. **最小侵入**：对现有代码的修改尽可能小，不影响 AIDL/HIDL 路径

### 2.3 非目标

- 不实现 LOCAL 模式下的音效（Effects）支持——音效仍走 AIDL/HIDL
- 不实现 AIDL-only 的高级特性（Sound Dose、空间音频等）
- 不修改 AudioFlinger 本身

---

## 3. 技术分析

### 3.1 Android 12 与 Android 16 接口差异

在将 Android 12 的本地直通实现移植到 Android 16 之前，必须理清两个版本之间 HAL 接口的差异。

#### 3.1.1 DeviceHalInterface 新增的纯虚方法

Android 16 的 `DeviceHalInterface` 相比 Android 12 新增了 13 个纯虚方法：

| 新增方法 | 说明 | LOCAL 实现策略 |
|---------|------|--------------|
| `getAudioPorts()` | 获取 AIDL 类型的音频端口列表 | 返回 `INVALID_OPERATION` |
| `getAudioRoutes()` | 获取音频路由列表 | 返回 `INVALID_OPERATION` |
| `getSupportedModes()` | 获取支持的音频模式 | 返回 `INVALID_OPERATION` |
| `getMmapPolicyInfos()` | 获取 MMAP 策略信息 | 返回 `INVALID_OPERATION` |
| `getAAudioMixerBurstCount()` | AAudio 混合器突发计数 | 返回 `INVALID_OPERATION` |
| `getAAudioHardwareBurstMinUsec()` | AAudio 硬件突发最小微秒 | 返回 `INVALID_OPERATION` |
| `supportsBluetoothVariableLatency()` | 蓝牙可变延迟支持 | 返回 `false` |
| `setConnectedState()` | 外部设备连接状态 | 返回 `INVALID_OPERATION` |
| `setSimulateDeviceConnections()` | 模拟设备连接 | 返回 `INVALID_OPERATION` |
| `getHwAvSync()` | 获取硬件 AV 同步 | 委托给 HAL（如支持） |
| `getSoundDoseInterface()` | 声音剂量接口 | 返回 `INVALID_OPERATION` |
| `prepareToDisconnectExternalDevice()` | 准备断开外部设备 | 返回 `INVALID_OPERATION` |
| `getAudioMixPort()` | 获取混合端口信息 | 返回 `INVALID_OPERATION` |

#### 3.1.2 DeviceHalInterface 签名变化的方法

| 方法 | Android 12 签名 | Android 16 签名 | LOCAL 适配 |
|------|----------------|----------------|-----------|
| `getInputBufferSize` | `const struct audio_config*` | `struct audio_config*` | 参数改为非 const |
| `openOutputStream` | 6 个参数 | 7 个参数（新增 `sourceMetadata`） | 忽略新参数 |
| `getMicrophones` | `vector<MicrophoneInfo>` | `vector<audio_microphone_characteristic_t>` | 使用新类型 |
| `addDeviceEffect` | `audio_port_handle_t` | `const struct audio_port_config*` | 返回不支持 |
| `removeDeviceEffect` | `audio_port_handle_t` | `const struct audio_port_config*` | 返回不支持 |
| `dump` | `dump(int fd)` | `dump(int fd, const Vector<String16>& args)` | 忽略 args |
| `getHwAvSync` | 返回 `status_t` + 输出参数 | 返回 `error::Result<audio_hw_sync_t>` | 使用新返回类型 |

#### 3.1.3 StreamOutHalInterface 新增的纯虚方法

| 新增方法 | LOCAL 实现策略 |
|---------|--------------|
| `presentationComplete()` | 返回 `INVALID_OPERATION` |
| `setLatencyMode()` | 返回 `INVALID_OPERATION` |
| `getRecommendedLatencyModes()` | 返回 `INVALID_OPERATION` |
| `setLatencyModeCallback()` | 返回 `INVALID_OPERATION` |
| `exit()` | 返回 `OK` |

#### 3.1.4 StreamHalInterface 新增的纯虚方法

| 新增方法 | LOCAL 实现策略 |
|---------|--------------|
| `legacyCreateAudioPatch()` | 返回 `INVALID_OPERATION` |
| `legacyReleaseAudioPatch()` | 返回 `INVALID_OPERATION` |

#### 3.1.5 StreamOutHalInterface 签名变化

| 方法 | Android 12 | Android 16 | LOCAL 适配 |
|------|-----------|-----------|-----------|
| `getRenderPosition` | `uint32_t *dspFrames` | `uint64_t *dspFrames` | 从 HAL 32-bit 值扩展到 64-bit |
| `onError` 回调 | `onError()` | `onError(bool isHardError)` | 传递 `false` |
| `onCodecFormatChanged` | `basic_string<uint8_t>` | `vector<uint8_t>` | 使用 `vector` |

#### 3.1.6 DevicesFactoryHalInterface 新增方法

| 新增方法 | LOCAL 实现策略 |
|---------|--------------|
| `getDeviceNames()` | 返回 `{"primary"}` |
| `getHalVersion()` | 返回 HIDL 0.0（表示 LOCAL 模式） |
| `getSurroundSoundConfig()` | 返回 `INVALID_OPERATION` |
| `getEngineConfig()` | 返回 `INVALID_OPERATION` |

### 3.2 构建系统差异

Android 16 的构建系统相比 Android 12 有以下关键变化：

1. **filegroup 模块化**：源文件通过 `filegroup` 组织，便于复用
2. **cc_defaults 分层**：`libaudiohal_default`（通用）+ `libaudiohal_hidl_default`（HIDL）+ `libaudiohal_aidl_default`（AIDL）
3. **新增依赖**：`audioclient-types-aidl-cpp`、`libaudiofoundation`、`libbinder_ndk` 等
4. **dlopen 加载**：各版本库通过 `libaudiohal@{version}.so` 命名，由 `FactoryHal.cpp` 运行时 dlopen

### 3.3 工厂加载机制变化

Android 12 的 `FactoryHalHidl.cpp` 仅支持 HIDL 服务发现；Android 16 的 `FactoryHal.cpp` 同时支持 AIDL 和 HIDL，并引入了 sibling 版本协调机制。

---

## 4. 实现方案设计

### 4.1 整体架构

```
┌────────────────────────────────────────────────────────────┐
│                     AudioFlinger                            │
│                         │                                   │
│                    libaudiohal                               │
│                         │                                   │
│              FactoryHal.cpp (修改)                           │
│            ┌────────┼────────┬──────────┐                   │
│            ▼        ▼        ▼          ▼                   │
│     libaudiohal  libaudiohal  libaudiohal  libaudiohal      │
│       @aidl       @7.1        @7.0       @local (新增)      │
│            │        │        │          │                   │
│     DevicesFactory DevicesFactory DevicesFactory DevicesFactory│
│     HalAidl    HalHidl    HalHidl    HalLocal              │
│            │        │        │          │                   │
│     DeviceHal  DeviceHal  DeviceHal  DeviceHal              │
│     Aidl       Hidl       Hidl       Local                  │
│            │        │        │          │                   │
│     StreamHal  StreamHal  StreamHal  StreamHal              │
│     Aidl       Hidl       Hidl       Local                  │
│            │        │        │          │                   │
│            ▼        ▼        ▼          ▼                   │
│         AIDL HAL  HIDL HAL  HIDL HAL  audio_hw_device_t     │
│         (IPC)     (IPC)     (IPC)     (直接调用)             │
└────────────────────────────────────────────────────────────┘
```

### 4.2 文件规划

#### 新增文件（6 个）

| 文件 | 说明 |
|------|------|
| `impl/DevicesFactoryHalLocal.h` | 本地设备工厂头文件 |
| `impl/DevicesFactoryHalLocal.cpp` | 本地设备工厂实现 |
| `impl/DeviceHalLocal.h` | 本地设备 HAL 头文件 |
| `impl/DeviceHalLocal.cpp` | 本地设备 HAL 实现 |
| `impl/StreamHalLocal.h` | 本地音频流头文件 |
| `impl/StreamHalLocal.cpp` | 本地音频流实现 |

#### 修改文件（3 个）

| 文件 | 修改内容 |
|------|---------|
| `FactoryHal.cpp` | 新增 LOCAL 模式发现与加载逻辑 |
| `Android.bp`（顶层） | 新增 `libaudiohal@local` 到 `required`，新增 `libcutils` 依赖 |
| `impl/Android.bp` | 新增 `libaudiohal@local` 库定义 |

### 4.3 激活条件设计

LOCAL 模式通过以下三种方式激活（优先级从高到低）：

1. **强制模式**：系统属性 `ro.audio.hal.force_local=true` 或 `persist.audio.hal.local.enabled=true`
2. **正常模式**：AIDL/HIDL 服务发现（不变）
3. **降级模式**：当没有 AIDL/HIDL 服务可用时，自动尝试 LOCAL 模式

```
createPreferredImpl(isDevice)
    │
    ├─ [isDevice && 属性强制] → createLocalHalService() → 成功 → 返回
    │                                                    → 失败 → 继续
    │
    ├─ [AIDL/HIDL 服务发现] → createHalService() → 成功 → 返回
    │                                              → 失败 → 继续
    │
    └─ [isDevice && 自动降级] → createLocalHalService() → 成功 → 返回
                                                        → 失败 → 返回 nullptr
```

### 4.4 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| LOCAL 仅支持设备 HAL | 是 | 音效仍走 AIDL/HIDL，简化实现 |
| 不复用 Hybrid 模式 | 是 | Android 16 已删除 Hybrid，使用更简洁的 Entry 模式 |
| 不支持的 API 返回 `INVALID_OPERATION` | 是 | 保持接口完整，不影响调用方判断 |
| 使用 `DevicesFactoryHalEntry.cpp` | 是 | 复用现有入口点机制 |
| HAL 版本号报告 0.0 | 是 | 明确标识 LOCAL 模式，区别于 HIDL 版本 |

---

## 5. 详细实现

### 5.1 DevicesFactoryHalLocal

#### 头文件 (`DevicesFactoryHalLocal.h`)

```cpp
class DevicesFactoryHalLocal : public DevicesFactoryHalInterface {
  public:
    DevicesFactoryHalLocal() = default;

    status_t getDeviceNames(std::vector<std::string> *names) override;
    status_t openDevice(const char *name, sp<DeviceHalInterface> *device) override;
    status_t setCallbackOnce(sp<DevicesFactoryHalCallback> callback) override;
    android::detail::AudioHalVersionInfo getHalVersion() const override;
    status_t getSurroundSoundConfig(media::SurroundSoundConfig *config) override;
    status_t getEngineConfig(media::audio::common::AudioHalEngineConfig *config) override;

  private:
    virtual ~DevicesFactoryHalLocal() = default;
};
```

#### 实现要点

- **`getDeviceNames()`**：返回 `{"primary"}`，表示只有一个主音频设备
- **`openDevice()`**：通过 `hw_get_module_by_class()` 加载 HAL 模块，然后 `audio_hw_device_open()` 打开设备
- **`setCallbackOnce()`**：立即触发 `onNewDevicesAvailable()` 回调（因为 LOCAL 模式下设备立即可用）
- **`getHalVersion()`**：返回 `AudioHalVersionInfo(Type::HIDL, 0, 0)`，标识 LOCAL 模式
- **`getSurroundSoundConfig()` / `getEngineConfig()`**：返回 `INVALID_OPERATION`

#### 入口点

```cpp
extern "C" __attribute__((visibility("default"))) void* createIDevicesFactoryImpl() {
    return new DevicesFactoryHalLocal();
}
```

这是 `libaudiohal@local.so` 的工厂入口点，被 `FactoryHal.cpp` 通过 `dlsym()` 调用。

### 5.2 DeviceHalLocal

#### 核心功能实现

DeviceHalLocal 包装了 `audio_hw_device_t*`，实现了 Android 16 `DeviceHalInterface` 的全部 27 个纯虚方法：

**基础设备控制**（委托给 `audio_hw_device_t`）：
```cpp
status_t initCheck()     → mDev->init_check(mDev)
status_t setMode()       → mDev->set_mode(mDev, mode)
status_t setMicMute()    → mDev->set_mic_mute(mDev, state)
status_t getMicMute()    → mDev->get_mic_mute(mDev, state)
status_t setParameters() → mDev->set_parameters(mDev, kvPairs.c_str())
status_t getParameters() → mDev->get_parameters(mDev, keys.c_str())
```

**音量控制**（带空指针检查）：
```cpp
status_t setMasterVolume(float volume) {
    if (mDev->set_master_volume == NULL) return INVALID_OPERATION;
    return mDev->set_master_volume(mDev, volume);
}
```

**输出流/输入流打开**：
```cpp
status_t openOutputStream(...) {
    audio_stream_out_t *halStream;
    int openResult = mDev->open_output_stream(
            mDev, handle, deviceType, flags, config, &halStream, address);
    if (openResult == OK) {
        *outStream = new StreamOutHalLocal(halStream, this);
    }
    return openResult;
}
```

**音频端口操作**（带版本检查）：
```cpp
status_t getAudioPort(struct audio_port_v7 *port) {
    if (version() >= AUDIO_DEVICE_API_VERSION_3_2) {
        return mDev->get_audio_port_v7(mDev, port);
    }
    // 回退到旧接口
    struct audio_port audioPort = {};
    if (!audio_populate_audio_port(port, &audioPort)) {
        return BAD_VALUE;
    }
    status_t status = getAudioPort(&audioPort);
    if (status == NO_ERROR) {
        audio_populate_audio_port_v7(&audioPort, port);
    }
    return status;
}
```

**Audio Patch 操作**（带版本检查）：
```cpp
status_t createAudioPatch(...) {
    if (version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        return mDev->create_audio_patch(mDev, num_sources, sources, num_sinks, sinks, patch);
    }
    return INVALID_OPERATION;
}
```

**Android 16 新增 API 的处理**（返回不支持）：
```cpp
status_t getAudioPorts(...)                    → INVALID_OPERATION
status_t getAudioRoutes(...)                   → INVALID_OPERATION
status_t getSupportedModes(...)                → INVALID_OPERATION
status_t getMmapPolicyInfos(...)               → INVALID_OPERATION
status_t getSoundDoseInterface(...)            → INVALID_OPERATION
status_t prepareToDisconnectExternalDevice(...) → INVALID_OPERATION
status_t getAudioMixPort(...)                  → INVALID_OPERATION
```

**特殊处理**：
```cpp
// getHwAvSync 使用新的 error::Result 返回类型
error::Result<audio_hw_sync_t> getHwAvSync() {
    if (mDev->get_audio_hw_sync != NULL) {
        return mDev->get_audio_hw_sync(mDev);
    }
    return base::unexpected(INVALID_OPERATION);
}

// supportsBluetoothVariableLatency 明确返回 false
status_t supportsBluetoothVariableLatency(bool *supports) {
    if (supports != nullptr) *supports = false;
    return OK;
}
```

### 5.3 StreamHalLocal

#### 基类 StreamHalLocal

包装 `audio_stream_t*`，实现通用流操作：

```cpp
StreamHalLocal::StreamHalLocal(audio_stream_t *stream, sp<DeviceHalLocal> device)
        : mDevice(device), mStream(stream) {
    // 初始化信号功率日志
    if (mStream != nullptr) {
        mStreamPowerLog.init(mStream->get_sample_rate(mStream),
                mStream->get_channels(mStream),
                mStream->get_format(mStream));
    }
}
```

通用方法实现：
- `getBufferSize()` → `mStream->get_buffer_size(mStream)`
- `getAudioProperties()` → 组装 `audio_config_base_t`
- `setParameters()` / `getParameters()` → 直接委托
- `standby()` → `mStream->standby(mStream)`
- `setHalThreadPriority()` → 空操作（LOCAL 模式在 AudioFlinger 线程上执行）

#### StreamOutHalLocal

包装 `audio_stream_out_t*`，实现输出流操作：

**核心写入**：
```cpp
status_t write(const void *buffer, size_t bytes, size_t *written) {
    ssize_t writeResult = mStream->write(mStream, buffer, bytes);
    if (writeResult > 0) {
        *written = writeResult;
        mStreamPowerLog.log(buffer, *written);  // 功率日志
        return OK;
    } else {
        *written = 0;
        return writeResult;
    }
}
```

**异步回调**（适配 Android 16 的 `onError(bool)` 签名）：
```cpp
int StreamOutHalLocal::asyncCallback(stream_callback_event_t event, void*, void *cookie) {
    // ...
    switch (event) {
        case STREAM_CBK_EVENT_WRITE_READY:
            callback->onWriteReady();
            break;
        case STREAM_CBK_EVENT_DRAIN_READY:
            callback->onDrainReady();
            break;
        case STREAM_CBK_EVENT_ERROR:
            callback->onError(false /*isHardError*/);  // Android 16 新增参数
            break;
    }
    return 0;
}
```

**getRenderPosition 64-bit 适配**：
```cpp
status_t getRenderPosition(uint64_t *dspFrames) {
    uint32_t halPosition;  // HAL 返回 32-bit
    status_t status = mStream->get_render_position(mStream, &halPosition);
    if (status == OK) {
        *dspFrames = halPosition;  // 扩展到 64-bit
    }
    return status;
}
```

**Source Metadata 更新**（支持 v7 和旧版）：
```cpp
status_t updateSourceMetadata(const SourceMetadata& sourceMetadata) {
    // 优先使用 v7 接口
    if (mStream->update_source_metadata_v7 != nullptr) {
        const source_metadata_v7_t metadata { ... };
        mStream->update_source_metadata_v7(mStream, &metadata);
        return OK;
    }
    // 回退到旧接口
    if (mStream->update_source_metadata != nullptr) {
        // 从 v7 转换到旧格式
        std::vector<playback_track_metadata> halTracks;
        for (auto& metadata : sourceMetadata.tracks) {
            playback_track_metadata halTrackMetadata;
            playback_track_metadata_from_v7(&halTrackMetadata, &metadata);
            halTracks.push_back(halTrackMetadata);
        }
        // ...
    }
    return INVALID_OPERATION;
}
```

**事件回调**（适配 Android 16 的 `vector<uint8_t>` 类型）：
```cpp
status_t setEventCallback(const sp<StreamOutHalInterfaceEventCallback>& callback) {
    // ...
    asyncCb = [](stream_event_callback_type_t event, void *param, void *cookie) -> int {
        // ...
        case STREAM_EVENT_CBK_TYPE_CODEC_FORMAT_CHANGED: {
            // Android 16: 使用 std::vector<uint8_t> 代替 basic_string<uint8_t>
            std::vector<uint8_t> metadataBs(
                    (const uint8_t*)param,
                    (const uint8_t*)param +
                            audio_utils::metadata::dataByteStringLen((const uint8_t*)param));
            cb->onCodecFormatChanged(metadataBs);
            break;
        }
    };
}
```

#### StreamInHalLocal

包装 `audio_stream_in_t*`，实现输入流操作：

**核心读取**：
```cpp
status_t read(void *buffer, size_t bytes, size_t *read) {
    ssize_t readResult = mStream->read(mStream, buffer, bytes);
    if (readResult > 0) {
        *read = readResult;
        mStreamPowerLog.log(buffer, *read);
        return OK;
    } else {
        *read = 0;
        return readResult;
    }
}
```

**Sink Metadata 更新**（同样支持 v7 和旧版）：
```cpp
status_t updateSinkMetadata(const SinkMetadata& sinkMetadata) {
    if (mStream->update_sink_metadata_v7 != nullptr) { ... }
    if (mStream->update_sink_metadata != nullptr) { ... }
    return INVALID_OPERATION;
}
```

---

## 6. 构建系统集成

### 6.1 新增 `libaudiohal@local` 库

在 `impl/Android.bp` 中新增：

```blueprint
cc_library_shared {
    name: "libaudiohal@local",
    defaults: [
        "libaudiohal_default",
    ],
    srcs: [
        "DevicesFactoryHalEntry.cpp",
        "DevicesFactoryHalLocal.cpp",
        "DeviceHalLocal.cpp",
        "StreamHalLocal.cpp",
    ],
}
```

**设计考量**：
- 使用 `libaudiohal_default` 默认配置，继承通用编译选项和依赖
- **不**使用 `libaudiohal_hidl_default`（不需要 HIDL 依赖）
- **不**使用 `libaudiohal_aidl_default`（不需要 AIDL 依赖）
- 包含 `DevicesFactoryHalEntry.cpp`（提供标准的 `createIDevicesFactory` 入口）

### 6.2 顶层 `Android.bp` 修改

```diff
 required: [
     "libaudiohal@6.0",
     "libaudiohal@7.0",
     "libaudiohal@7.1",
     "libaudiohal@aidl",
+    "libaudiohal@local",
 ],

 shared_libs: [
     "audioclient-types-aidl-cpp",
     "libaudiofoundation",
     "libbinder_ndk",
+    "libcutils",        // 用于 property_get()
     "libdl",
     "libhidlbase",
     "liblog",
     "libutils",
 ],
```

### 6.3 库加载机制

各 `libaudiohal@*.so` 库的加载由 `FactoryHal.cpp` 在运行时通过 `dlopen()` 完成：

```
libaudiohal.so
    ├── dlopen("libaudiohal@aidl.so")   → AIDL HAL
    ├── dlopen("libaudiohal@7.1.so")    → HIDL 7.1
    ├── dlopen("libaudiohal@7.0.so")    → HIDL 7.0
    ├── dlopen("libaudiohal@6.0.so")    → HIDL 6.0
    └── dlopen("libaudiohal@local.so")  → LOCAL HAL (新增)
```

---

## 7. 工厂加载机制

### 7.1 FactoryHal.cpp 修改详解

#### 7.1.1 新增头文件

```cpp
#include <cutils/properties.h>  // 用于 property_get()
```

#### 7.1.2 系统属性检查函数

```cpp
/** Check if LOCAL (passthrough) mode is forced via system property. */
bool shouldUseLocalHal() {
    char value[PROPERTY_VALUE_MAX] = {0};
    // 只读属性，需要在构建时设定
    property_get("ro.audio.hal.force_local", value, "false");
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        return true;
    }
    // 可持久化属性，可在运行时通过 setprop 修改（重启后生效）
    property_get("persist.audio.hal.local.enabled", value, "false");
    return strcmp(value, "true") == 0 || strcmp(value, "1") == 0;
}
```

两个系统属性的设计：
- `ro.audio.hal.force_local`：只读属性，适合设备制造商在固件中设定
- `persist.audio.hal.local.enabled`：可持久化属性，适合开发和调试

#### 7.1.3 LOCAL HAL 加载函数

```cpp
bool createLocalHalService(bool isDevice, void** rawInterface) {
    const std::string libName = "libaudiohal@local.so";
    const std::string factoryFunctionName =
            isDevice ? "createIDevicesFactory" : "createIEffectsFactory";
    // dlopen + dlsym 模式，与 createHalService() 相同
    void* handle = dlopen(libName.c_str(), RTLD_LAZY);
    if (handle == nullptr) return false;
    void* (*factoryFunction)();
    *(void **)(&factoryFunction) = dlsym(handle, factoryFunctionName.c_str());
    if (!factoryFunction) { dlclose(handle); return false; }
    *rawInterface = (*factoryFunction)();
    return *rawInterface != nullptr;
}
```

#### 7.1.4 修改后的 createPreferredImpl()

```cpp
void *createPreferredImpl(bool isDevice) {
    // 阶段 1：检查强制 LOCAL 模式
    if (isDevice && shouldUseLocalHal()) {
        void* rawInterface = nullptr;
        if (createLocalHalService(true, &rawInterface)) {
            ALOGI("Using LOCAL (passthrough) audio HAL (forced by system property)");
            return rawInterface;
        }
        ALOGE("LOCAL audio HAL forced but failed to load, falling through to AIDL/HIDL");
    }

    // 阶段 2：正常的 AIDL/HIDL 服务发现（原有逻辑，不变）
    auto findMostRecentVersion = [](const auto& iMap) { ... };
    // ... 版本匹配和 sibling 协调 ...

    // 阶段 3：自动降级到 LOCAL 模式
    if (isDevice) {
        ALOGW("No AIDL/HIDL audio HAL service found, trying LOCAL (passthrough) fallback");
        void* rawInterface = nullptr;
        if (createLocalHalService(true, &rawInterface)) {
            ALOGI("Using LOCAL (passthrough) audio HAL as fallback");
            return rawInterface;
        }
    }

    return nullptr;
}
```

### 7.2 加载时序图

```
AudioFlinger 启动
    │
    ▼
DevicesFactoryHalInterface::create()
    │
    ▼
createPreferredImpl(isDevice=true)
    │
    ├── shouldUseLocalHal() ?
    │   ├── 是 → dlopen("libaudiohal@local.so")
    │   │       → dlsym("createIDevicesFactory")
    │   │       → DevicesFactoryHalLocal → DevicesFactoryHalEntry
    │   │       → 返回
    │   └── 否 → 继续
    │
    ├── hasAidlHalService("android.hardware.audio.core.IModule/default") ?
    │   ├── 是 → dlopen("libaudiohal@aidl.so") → DevicesFactoryHalAidl
    │   └── 否 → 继续
    │
    ├── hasHidlHalService("android.hardware.audio@7.1::IDevicesFactory") ?
    │   ├── 是 → dlopen("libaudiohal@7.1.so") → DevicesFactoryHalHidl
    │   └── 否 → 继续
    │
    ├── ... (检查 7.0, 6.0)
    │
    └── 自动降级 → dlopen("libaudiohal@local.so")
                  → DevicesFactoryHalLocal
```

---

## 8. 使用指南

### 8.1 启用 LOCAL 模式

#### 方法 1：构建时设定（推荐用于产品化）

在设备的 `device.mk` 或 `system.prop` 中添加：

```makefile
PRODUCT_PROPERTY_OVERRIDES += \
    ro.audio.hal.force_local=true
```

#### 方法 2：运行时设定（推荐用于开发调试）

```bash
# 设定属性（需要 root 权限）
adb root
adb shell setprop persist.audio.hal.local.enabled true

# 重启 audioserver 使其生效
adb shell kill $(adb shell pidof audioserver)
```

#### 方法 3：自动降级

如果系统中没有注册任何 AIDL 或 HIDL 音频 HAL 服务，框架会自动尝试加载 LOCAL 模式。无需设定任何属性。

### 8.2 验证 LOCAL 模式已启用

查看 logcat 中的标志性日志：

```bash
adb logcat -s FactoryHal
```

预期输出（强制模式）：
```
I FactoryHal: Using LOCAL (passthrough) audio HAL (forced by system property)
```

预期输出（自动降级）：
```
W FactoryHal: No AIDL/HIDL audio HAL service found, trying LOCAL (passthrough) fallback
I FactoryHal: Using LOCAL (passthrough) audio HAL as fallback
```

### 8.3 基础功能验证

**播放测试**：
```bash
# 使用 tinyplay 播放 WAV 文件
adb push test.wav /data/local/tmp/
adb shell tinyplay /data/local/tmp/test.wav

# 或使用 media 命令
adb shell am start -a android.intent.action.VIEW -d file:///data/local/tmp/test.wav -t audio/wav
```

**录音测试**：
```bash
# 使用 tinycap 录制
adb shell tinycap /data/local/tmp/record.wav -D 0 -d 0 -c 2 -r 48000 -b 16 -T 5

# 回放录制的内容
adb shell tinyplay /data/local/tmp/record.wav
```

### 8.4 支持的功能矩阵

| 功能 | 支持情况 | 说明 |
|------|---------|------|
| PCM 播放 | ✅ | 通过 `audio_stream_out_t->write()` |
| PCM 录音 | ✅ | 通过 `audio_stream_in_t->read()` |
| 音量控制 | ✅ | Master/Voice 音量 |
| 静音控制 | ✅ | 麦克风和主静音 |
| 参数设置/获取 | ✅ | 全局和流级别参数 |
| 待机 (Standby) | ✅ | |
| 暂停/恢复 | ✅ | 如 HAL 支持 |
| Drain/Flush | ✅ | 如 HAL 支持 |
| 异步回调 | ✅ | 写就绪、drain 完成、错误 |
| 呈现位置 | ✅ | `get_presentation_position()` |
| 渲染位置 | ✅ | `get_render_position()`（32→64 位扩展） |
| MMAP 模式 | ✅ | 如 HAL 支持 |
| Source/Sink Metadata | ✅ | v7 优先，回退到旧版 |
| Audio Patches | ✅ | HAL v3.0+ |
| Dual Mono | ✅ | 如 HAL 支持 |
| Audio Description Mix | ✅ | 如 HAL 支持 |
| 播放速率参数 | ✅ | 如 HAL 支持 |
| 编解码格式变更回调 | ✅ | |
| 麦克风列表 | ✅ | |
| 麦克风方向控制 | ✅ | 如 HAL 支持 |
| 音频端口 (v7) | ✅ | 带旧版本回退 |
| 音频端口配置 | ✅ | HAL v3.0+ |
| 音效 | ❌ | 需要独立的音效 HAL |
| AIDL 音频端口 | ❌ | AIDL-only 特性 |
| AIDL 音频路由 | ❌ | AIDL-only 特性 |
| Sound Dose | ❌ | AIDL-only 特性 |
| 蓝牙可变延迟 | ❌ | 报告为不支持 |
| AAudio 策略信息 | ❌ | 需要 AIDL HAL |
| 延迟模式控制 | ❌ | AIDL-only 特性 |
| 环绕声配置 | ❌ | 需要 AIDL/HIDL 服务 |
| 引擎配置 | ❌ | 需要 AIDL/HIDL 服务 |

---

## 9. 局限性与后续工作

### 9.1 已知局限性

1. **无音效支持**：LOCAL 模式下的流不支持 `addEffect()` / `removeEffect()`（会触发 `LOG_ALWAYS_FATAL`）。如果需要音效，仍需要 AIDL/HIDL 音效 HAL 服务。

2. **无设备音效**：`addDeviceEffect()` / `removeDeviceEffect()` 返回 `INVALID_OPERATION`。

3. **getActiveMicrophones() 不支持**：Android 16 使用 `MicrophoneInfoFw` 类型，与旧版 HAL 的 `audio_microphone_characteristic_t` 不直接兼容，因此返回 `INVALID_OPERATION`。

4. **Legacy Audio Patch 不支持**：`legacyCreateAudioPatch()` / `legacyReleaseAudioPatch()` 在 LOCAL 模式下返回 `INVALID_OPERATION`，因为这些是 HIDL HAL 的遗留兼容路径。

5. **HAL 版本报告**：`getHalVersion()` 返回 `HIDL 0.0`，某些依赖版本号的逻辑可能需要适配。

### 9.2 后续工作

1. **音效 HAL LOCAL 模式**：如需在 LOCAL 模式下使用音效，需要实现 `EffectsFactoryHalLocal` 和 `EffectHalLocal`。

2. **更丰富的设备发现**：当前 `getDeviceNames()` 仅返回 `"primary"`，未来可扫描 `/vendor/lib/hw/` 下的所有 `audio.*.so` 模块。

3. **MicrophoneInfoFw 适配**：实现从 `audio_microphone_characteristic_t` 到 `MicrophoneInfoFw` 的转换。

4. **集成测试**：编写端到端测试，验证 LOCAL 模式下的完整播放/录音流程。

5. **性能基准测试**：对比 LOCAL 模式与 AIDL/HIDL 模式的延迟差异。

---

## 附录：文件变更清单

### 新增文件

| 文件路径 | 行数 | 说明 |
|---------|------|------|
| `impl/DevicesFactoryHalLocal.h` | 52 | 本地设备工厂头文件 |
| `impl/DevicesFactoryHalLocal.cpp` | 107 | 本地设备工厂实现 |
| `impl/DeviceHalLocal.h` | 173 | 本地设备 HAL 头文件 |
| `impl/DeviceHalLocal.cpp` | 313 | 本地设备 HAL 实现 |
| `impl/StreamHalLocal.h` | 260 | 本地音频流头文件 |
| `impl/StreamHalLocal.cpp` | 479 | 本地音频流实现 |
| **合计** | **1,384** | |

### 修改文件

| 文件路径 | 增加行数 | 说明 |
|---------|---------|------|
| `FactoryHal.cpp` | +70 | LOCAL 模式发现与加载逻辑 |
| `Android.bp` | +2 | 新增 `libaudiohal@local` 和 `libcutils` |
| `impl/Android.bp` | +13 | `libaudiohal@local` 库定义 |
| **合计** | **+85** | |

### 总变更量

- **新增 6 个文件，修改 3 个文件**
- **新增约 1,469 行代码**
- **修改约 85 行代码**
