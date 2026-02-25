# Android 12 vs Android 16: `services/audioflinger` 对比分析

## 1. 总体概述

| 指标 | Android 12 | Android 16 | 变化 |
|------|-----------|-----------|------|
| 源码文件数 (`.cpp`/`.h`) | 54 | 89 | +35 (+65%) |
| 总代码行数 | 33,305 | 44,217 | +10,912 (+33%) |
| 子目录数 | 0（扁平结构） | 5（`afutils/`、`datapath/`、`fastpath/`、`sounddose/`、`timing/`） | 模块化重构 |
| 构建产物 | 1 个共享库 `libaudioflinger` | 6 个库（1 主库 + 5 子模块库） | 构建解耦 |
| 测试文件 | 无 | 4 个测试文件（599 行） | 新增测试套件 |
| 接口抽象 | 无独立接口头文件 | 4 个 `IAf*.h` 接口头文件（2,207 行） | 接口与实现分离 |
| 线程安全注解 | 少量（~39 处） | 大量（~740 处） | 编译时线程安全检查 |
| 线程类型 | 9 种 | 12 种（新增 3 种） | 功能扩展 |

**核心变化**：Android 16 的 `audioflinger` 经历了一次重大的**架构模块化重构**，将原本单一扁平的 "上帝类"（God Class）结构拆分为多个独立的子模块库，并通过引入纯虚接口（`IAf*.h`）实现了接口与实现的分离。同时新增了 Sound Dose（声音剂量）、空间音频（Spatializer）、Bit-Perfect 回放等重要功能。

---

## 2. 架构变化

### 2.1 从扁平结构到模块化子目录

**Android 12** — 所有文件平铺在 `audioflinger/` 目录下，54 个源码文件编译成单一的 `libaudioflinger.so`。

**Android 16** — 按功能职责拆分为 6 个模块：

| 模块（子目录） | 库名 | 文件数 | 代码行数 | 职责 |
|---------------|------|--------|---------|------|
| `./`（根目录） | `libaudioflinger` | 28 | 35,415 | AudioFlinger 核心、线程、Track、Effect、PatchPanel |
| `afutils/` | `libaudioflinger_utils` | 16 | 2,133 | 工具类：Watchdog、BufLog、NBAIO_Tee、TypedLogger、权限、振动器、属性 |
| `datapath/` | `libaudioflinger_datapath` | 14 | 1,963 | 数据路径：HAL 设备封装、流 I/O、SPDIF、指标统计 |
| `fastpath/` | `libaudioflinger_fastpath` | 21 | 2,694 | 快速路径：FastMixer、FastCapture、StateQueue |
| `sounddose/` | `libsounddose` | 2+1 | 1,090+354 | 声音剂量管理（全新功能） |
| `timing/` | `libaudioflinger_timing` | 4+3 | 323+245 | 时序工具：单调帧计数器、同步事件、同步录音状态 |

### 2.2 接口与实现分离（IAf*.h 接口抽象层）

**Android 12** 中，所有内部类型（Thread、Track、Effect、PatchPanel）都是 `AudioFlinger` 的**内部类**或直接的具体类，彼此高度耦合：

```cpp
// Android 12: AudioFlinger.h 包含 ~40 个内部类声明
class AudioFlinger : public AudioFlingerServerAdapter::Delegate {
    class ThreadBase;     // 内部类
    class PlaybackThread; // 内部类
    class Track;          // 内部类
    class EffectModule;   // 内部类
    class PatchPanel;     // 内部类
    class Client;         // 内部类
    class SyncEvent;      // 内部类
    // ... 共 ~40 个内部类
};
```

**Android 16** 引入了 4 个纯虚接口头文件，将接口与实现彻底分离：

| 接口文件 | 行数 | 定义的接口 |
|---------|------|-----------|
| `IAfThread.h` | 728 | `IAfThreadBase`、`IAfPlaybackThread`、`IAfDirectOutputThread`、`IAfDuplicatingThread`、`IAfRecordThread`、`IAfMmapThread`、`IAfMmapPlaybackThread`、`IAfMmapCaptureThread`、`IAfThreadCallback`、`IAfClientCallback`、`IAfMelReporterCallback` |
| `IAfTrack.h` | 736 | `IAfTrackBase`、`IAfTrack`、`IAfOutputTrack`、`IAfMmapTrack`、`IAfRecordTrack`、`IAfPatchTrack`、`IAfPatchRecord`、`PatchProxyBufferProvider`、`VolumeProvider`、`AfPlaybackCommon` |
| `IAfEffect.h` | 428 | `EffectCallbackInterface`、`IAfEffectBase`、`IAfEffectModule`、`IAfEffectChain`、`IAfEffectHandle`、`IAfDeviceEffectProxy` |
| `IAfPatchPanel.h` | 315 | `IAfPatchPanel`、`IAfPatchPanelCallback`、`SoftwarePatch` |

```cpp
// Android 16: AudioFlinger.h — 仅 ~3 个内部类，通过接口通信
class AudioFlinger
    : public AudioFlingerServerAdapter::Delegate
    , public IAfClientCallback          // 新接口回调
    , public IAfDeviceEffectManagerCallback
    , public IAfMelReporterCallback
    , public IAfPatchPanelCallback
    , public IAfThreadCallback
{
    class NotificationClient;  // 仅保留少量内部类
};
```

### 2.3 "上帝类" 拆解

**Android 12** 中 `AudioFlinger.h` 有 **1,016 行**，包含约 40 个内部类声明，几乎所有核心类型都嵌套在 `AudioFlinger` 类中。

**Android 16** 中 `AudioFlinger.h` 缩减到 **808 行**，内部类从 ~40 个减少到仅 **1 个**（`NotificationClient`）。原内部类被提取为独立的顶层类：

| 原内部类（Android 12） | 新位置（Android 16） |
|----------------------|---------------------|
| `AudioFlinger::Client` | `Client.h` / `Client.cpp` （独立文件） |
| `AudioFlinger::SyncEvent` | `timing/SyncEvent.h` （timing 子模块） |
| `AudioFlinger::Source` | `datapath/AudioStreamIn.h`（`Source` 结构体） |
| `AudioFlinger::stream_type_t` | `IAfThread.h`（独立结构体） |
| `AudioFlinger::ThreadBase` 等 | `Threads.h` + `IAfThread.h` |
| `AudioFlinger::Track` 等 | `PlaybackTracks.h` / `RecordTracks.h` + `IAfTrack.h` |
| `AudioFlinger::EffectModule` 等 | `Effects.h` + `IAfEffect.h` |
| `AudioFlinger::PatchPanel` | `PatchPanel.h` + `IAfPatchPanel.h` |

### 2.4 回调接口模式

**Android 16** 引入了多个回调接口，替代了 Android 12 中各组件直接引用 `AudioFlinger` 的方式：

| 回调接口 | 职责 |
|---------|------|
| `IAfThreadCallback` | Thread 访问 AudioFlinger 功能的接口 |
| `IAfClientCallback` | Client 管理回调 |
| `IAfPatchPanelCallback` | PatchPanel 操作回调 |
| `IAfMelReporterCallback` | MEL 报告回调 |
| `IAfDeviceEffectManagerCallback` | 设备音效管理回调 |
| `IMelReporterCallback` | Sound Dose 模块与 MelReporter 的回调 |

### 2.5 线程安全注解体系

**Android 12**：使用少量的传统 `Mutex` 和 `mLock`，线程安全主要依赖开发者的注意力和代码审查，注解引用约 39 处。

**Android 16**：引入了全面的 **Clang 线程安全注解（Thread Safety Annotations）**，约 740 处注解引用：

```cpp
// Android 16: 典型的线程安全注解使用
virtual audio_utils::mutex& mutex() const
        RETURN_CAPABILITY(audio_utils::AudioFlinger_Mutex) = 0;
virtual bool isNonOffloadableGlobalEffectEnabled_l() const
        REQUIRES(mutex()) EXCLUDES_ThreadBase_Mutex = 0;

class SuspendedSessionDesc : public RefBase {
    // ...
    ssize_t mFramesToDrop GUARDED_BY(mLock) = 0;
    sp<SyncEvent> mSyncStartEvent GUARDED_BY(mLock);
};
```

新增的关键互斥量能力标注：
- `AudioFlinger_Mutex` — AudioFlinger 主锁
- `AudioFlinger_ClientMutex` — 客户端管理锁
- `ThreadBase_Mutex` — 线程基类锁
- `ThreadBase_ThreadLoop` — 线程循环的线程安全标注
- `EffectChain_Mutex` — 音效链锁
- `DeviceEffectHandle_Mutex` — 设备音效句柄锁

同时新增 `FallibleLockGuard` 工具类，用于 dump 等场景下的超时锁获取。

---

## 3. 文件级变化详情

### 3.1 仅在 Android 12 中存在的文件（已移除或迁移）

| 文件 | 行数 | 迁移目标 |
|------|------|---------|
| `AudioHwDevice.cpp` | 106 | → `datapath/AudioHwDevice.cpp` |
| `AudioHwDevice.h` | 97 | → `datapath/AudioHwDevice.h` |
| `AudioStreamOut.cpp` | 213 | → `datapath/AudioStreamOut.cpp` |
| `AudioStreamOut.h` | 107 | → `datapath/AudioStreamOut.h` |
| `AudioWatchdog.cpp` | 139 | → `afutils/AudioWatchdog.cpp` |
| `AudioWatchdog.h` | 88 | → `afutils/AudioWatchdog.h` |
| `AutoPark.h` | 61 | → `fastpath/AutoPark.h` |
| `BufLog.cpp` | 196 | → `afutils/BufLog.cpp` |
| `BufLog.h` | 199 | → `afutils/BufLog.h` |
| `FastCapture.cpp` | 240 | → `fastpath/FastCapture.cpp` |
| `FastCapture.h` | 69 | → `fastpath/FastCapture.h` |
| `FastCaptureDumpState.cpp` | 54 | → `fastpath/FastCaptureDumpState.cpp` |
| `FastCaptureDumpState.h` | 43 | → `fastpath/FastCaptureDumpState.h` |
| `FastCaptureState.cpp` | 45 | → `fastpath/FastCaptureState.cpp` |
| `FastCaptureState.h` | 60 | → `fastpath/FastCaptureState.h` |
| `FastMixer.cpp` | 566 | → `fastpath/FastMixer.cpp` |
| `FastMixer.h` | 118 | → `fastpath/FastMixer.h` |
| `FastMixerDumpState.cpp` | 206 | → `fastpath/FastMixerDumpState.cpp` |
| `FastMixerDumpState.h` | 86 | → `fastpath/FastMixerDumpState.h` |
| `FastMixerState.cpp` | 85 | → `fastpath/FastMixerState.cpp` |
| `FastMixerState.h` | 97 | → `fastpath/FastMixerState.h` |
| `FastThread.cpp` | 376 | → `fastpath/FastThread.cpp` |
| `FastThread.h` | 97 | → `fastpath/FastThread.h` |
| `FastThreadDumpState.cpp` | 59 | → `fastpath/FastThreadDumpState.cpp` |
| `FastThreadDumpState.h` | 72 | → `fastpath/FastThreadDumpState.h` |
| `FastThreadState.cpp` | 44 | → `fastpath/FastThreadState.cpp` |
| `FastThreadState.h` | 55 | → `fastpath/FastThreadState.h` |
| `NBAIO_Tee.cpp` | 517 | → `afutils/NBAIO_Tee.cpp` |
| `NBAIO_Tee.h` | 326 | → `afutils/NBAIO_Tee.h` |
| `SpdifStreamOut.cpp` | 128 | → `datapath/SpdifStreamOut.cpp` |
| `SpdifStreamOut.h` | 121 | → `datapath/SpdifStreamOut.h` |
| `StateQueue.cpp` | 193 | → `fastpath/StateQueue.cpp` |
| `StateQueue.h` | 215 | → `fastpath/StateQueue.h` |
| `StateQueueInstantiations.cpp` | 29 | 已移除（模板实例化不再需要独立文件） |
| `ThreadMetrics.h` | 205 | → `datapath/ThreadMetrics.h` |
| `TrackMetrics.h` | 230 | → `datapath/TrackMetrics.h` |
| `TypedLogger.cpp` | 27 | → `afutils/TypedLogger.cpp` |
| `TypedLogger.h` | 140 | → `afutils/TypedLogger.h` |

### 3.2 仅在 Android 16 中存在的文件（新增）

#### 接口抽象文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `IAfThread.h` | 728 | 线程接口抽象：定义所有线程类型的纯虚接口及回调 |
| `IAfTrack.h` | 736 | Track 接口抽象：定义所有 Track 类型的纯虚接口 |
| `IAfEffect.h` | 428 | 音效接口抽象：定义 Effect 模块、链、句柄的纯虚接口 |
| `IAfPatchPanel.h` | 315 | PatchPanel 接口抽象：定义 Patch 管理的纯虚接口 |

#### 功能模块新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `Client.cpp` / `Client.h` | 38 / 64 | 从 AudioFlinger 内部类提取的 Client 管理 |
| `MelReporter.cpp` / `MelReporter.h` | 352 / 152 | MEL（音乐暴露水平）报告器 — 监听音频 patch 并计算 MEL |
| `PatchCommandThread.cpp` / `PatchCommandThread.h` | 194 / 142 | Patch 命令异步执行线程 — 解决锁顺序问题 |
| `EffectConfiguration.h` | 46 | 音效配置抽象（判断 HIDL/AIDL） |
| `ResamplerBufferProvider.h` | 65 | 从 RecordThread 内部类提取的重采样缓冲区提供者 |

#### afutils/ 新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `AllocatorFactory.h` | 105 | 共享内存分配器工厂 — 实现分级内存池（专用/共享大/共享小） |
| `FallibleLockGuard.h` | 69 | 可超时的锁守卫 — 用于 dump 等场景 |
| `Permission.cpp` / `Permission.h` | 54 / 26 | 权限检查工具（Attribution Source 验证） |
| `PropertyUtils.cpp` / `PropertyUtils.h` | 104 / 32 | MMAP 策略和 AAudio 属性查询 |
| `Vibrator.cpp` / `Vibrator.h` | 78 / 29 | 外部振动控制封装 |

#### datapath/ 新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `AudioStreamIn.cpp` / `AudioStreamIn.h` | 137 / 87 | HAL 输入流封装（Android 12 中无独立的输入流封装） |
| `SpdifStreamIn.cpp` / `SpdifStreamIn.h` | 133 / 134 | SPDIF 输入流封装（Android 12 仅有输出） |
| `VolumeInterface.h` | 34 | 音量控制接口 |
| `VolumePortInterface.h` | 32 | 端口音量控制接口 |

#### sounddose/ 新增文件（全新功能）

| 文件 | 行数 | 说明 |
|------|------|------|
| `SoundDoseManager.cpp` / `SoundDoseManager.h` | 817 / 273 | 声音剂量管理器 — 计算 CSD（累积声音剂量）以保护听力 |
| `tests/sounddosemanager_tests.cpp` | 354 | Sound Dose 管理器单元测试 |

#### timing/ 新增文件（全新功能）

| 文件 | 行数 | 说明 |
|------|------|------|
| `MonotonicFrameCounter.cpp` / `MonotonicFrameCounter.h` | 57 / 83 | 单调帧计数器 — 用于 VolumeShaper 等自动化控制 |
| `SyncEvent.h` | 71 | 同步事件（从 AudioFlinger 内部类提取） |
| `SynchronizedRecordState.h` | 112 | 同步录音状态管理 |
| `tests/mediasyncevent_tests.cpp` | 70 | 同步事件测试 |
| `tests/monotonicframecounter_tests.cpp` | 97 | 单调帧计数器测试 |
| `tests/synchronizedrecordstate_tests.cpp` | 78 | 同步录音状态测试 |

### 3.3 两个版本中都存在但有修改的文件

| 文件 | Android 12 行数 | Android 16 行数 | 主要变化 |
|------|----------------|----------------|---------|
| `AudioFlinger.cpp` | 4,265 | 5,282 | +1,017 行；新增 MEL、Sound Dose、权限管理、空间音频等功能 |
| `AudioFlinger.h` | 1,016 | 808 | -208 行；内部类提取到独立文件和接口，多回调接口继承 |
| `Configuration.h` | 55 | 44 | 移除 `FLOAT_EFFECT_CHAIN`、`FLOAT_AUX`、`MULTICHANNEL_EFFECT_CHAIN` 宏定义（已成为默认行为） |
| `DeviceEffectManager.cpp` | 290 | 281 | 小幅重构，使用接口抽象 |
| `DeviceEffectManager.h` | 205 | 166 | 使用回调接口替代直接引用 AudioFlinger |
| `Effects.cpp` | 3,383 | 3,871 | +488 行；新增 `HwAccDeviceEffectModule`、`InternalEffectHandle`、线程安全注解 |
| `Effects.h` | 737 | 898 | +161 行；类继承接口（如 `IAfEffectModule`），新增类 |
| `MmapTracks.h` | 75 | 83 | 继承 `IAfMmapTrack` 接口 |
| `PatchPanel.cpp` | 911 | 1,009 | +98 行；配合 `PatchCommandThread` 异步处理 |
| `PatchPanel.h` | 268 | 155 | -113 行；内部类迁移到 `IAfPatchPanel.h` |
| `PlaybackTracks.h` | 450 | 530 | +80 行；继承 `IAfTrack` 等接口，新增端口音量控制 |
| `RecordTracks.h` | 217 | 235 | +18 行；继承 `IAfRecordTrack` 接口 |
| `Threads.cpp` | 10,150 | 11,837 | +1,687 行；新增空间音频、Bit-Perfect、DirectRecord、MEL 集成 |
| `Threads.h` | 2,074 | 2,530 | +456 行；新增线程类型，线程安全注解，接口继承 |
| `TrackBase.h` | 449 | 446 | 小幅调整，继承 `IAfTrackBase` |
| `Tracks.cpp` | 3,051 | 3,980 | +929 行；新增端口音量控制、Bit-Perfect Track 支持等 |

---

## 4. 新增线程类型

### 4.1 线程类继承关系对比

**Android 12**（9 种线程类型）：
```
ThreadBase
├── PlaybackThread
│   ├── MixerThread
│   │   └── DuplicatingThread
│   ├── DirectOutputThread
│   │   └── OffloadThread
├── RecordThread
└── MmapThread
    ├── MmapPlaybackThread
    └── MmapCaptureThread

（+ AsyncCallbackThread — 独立辅助线程）
```

**Android 16**（12 种线程类型，新增 3 种）：
```
ThreadBase (implements IAfThreadBase)
├── PlaybackThread (implements IAfPlaybackThread)
│   ├── MixerThread
│   │   ├── DuplicatingThread (implements IAfDuplicatingThread)
│   │   ├── SpatializerThread            ← 新增
│   │   └── BitPerfectThread             ← 新增
│   ├── DirectOutputThread (implements IAfDirectOutputThread)
│   │   └── OffloadThread
├── RecordThread (implements IAfRecordThread)
│   └── DirectRecordThread               ← 新增
└── MmapThread (implements IAfMmapThread)
    ├── MmapPlaybackThread (implements IAfMmapPlaybackThread)
    └── MmapCaptureThread (implements IAfMmapCaptureThread)

（+ AsyncCallbackThread — 独立辅助线程）
（+ PatchCommandThread — 新增的 Patch 异步命令线程）
```

### 4.2 SpatializerThread（空间音频线程）

```cpp
class SpatializerThread : public MixerThread {
    // 专用于空间音频（Spatial Audio）处理
    // - 支持延迟模式请求（audio_latency_mode_t）
    // - 管理 Final DownMixer 音效
    // - 检查输出阶段音效
    audio_latency_mode_t mRequestedLatencyMode = AUDIO_LATENCY_MODE_FREE;
    sp<IAfEffectHandle> mFinalDownMixer;
};
```

### 4.3 BitPerfectThread（比特精确回放线程）

```cpp
class BitPerfectThread : public MixerThread {
    // 支持比特精确（Bit-Perfect）音频回放
    // - 绕过混音器处理，直接输出原始比特流
    // - 用于高保真音频播放（如 Hi-Res Audio）
    bool mIsBitPerfect = false;
    float mVolumeLeft = 0.f;
    float mVolumeRight = 0.f;
};
```

### 4.4 DirectRecordThread（直接录音线程）

```cpp
class DirectRecordThread final : public RecordThread {
    // 直接录音线程 — 支持 HAL 直接录音路径
    // 无需经过常规的重采样/混音处理
};
```

---

## 5. 新增功能模块

### 5.1 Sound Dose（声音剂量）

Android 16 全新引入的听力保护功能（1,090 行源码 + 354 行测试），包含：

- **`SoundDoseManager`**：计算累积声音剂量（CSD），基于 7 天滑动窗口（IEC 62368-1 标准）
  - 默认 RS2 上限为 100 dBA
  - 使用 `MelAggregator` 聚合 MEL（音乐暴露水平）数据
  - 支持 AIDL HAL 的 `ISoundDose` 接口
- **`MelReporter`**：监听音频 patch 创建/释放，自动启停 MEL 计算
  - 作为 `PatchCommandThread::PatchCommandListener` 监听 patch 变更
  - 管理每个设备的 MEL 处理器

### 5.2 PatchCommandThread（异步 Patch 命令）

解决了 Android 12 中音频 patch 操作的锁顺序（lock ordering）问题：

**Android 12**：`createAudioPatch` / `releaseAudioPatch` 在 AudioPolicyService 持有锁时同步执行，可能导致死锁。

**Android 16**：新增 `PatchCommandThread` 异步执行 patch 操作：
```cpp
class PatchCommandThread : public Thread {
    enum { CREATE_AUDIO_PATCH, RELEASE_AUDIO_PATCH, UPDATE_AUDIO_PATCH };

    class PatchCommandListener : public virtual RefBase {
        virtual void onCreateAudioPatch(...) = 0;
        virtual void onReleaseAudioPatch(...) = 0;
        virtual void onUpdateAudioPatch(...) = 0;
    };
};
```

### 5.3 EffectConfiguration（音效配置抽象）

```cpp
class EffectConfiguration {
    static bool isHidl();                    // 判断当前 HAL 是 HIDL 还是 AIDL
    static const sp<EffectsFactoryHalInterface>& getEffectsFactoryHal();
    static const AudioHalVersionInfo& getAudioHalVersionInfo();
};
```

### 5.4 AllocatorFactory（内存分配器工厂）

Android 16 引入了分层共享内存池管理：

| 内存池 | 大小 | 用途 |
|-------|------|------|
| 专用池（Dedicated） | 40 MiB | 每个客户端的专属配额 |
| 共享大池（Shared Large） | 40 MiB | 大分配请求的后备池 |
| 共享小池（Shared Small） | 20 MiB（阈值 40 KiB） | 小分配请求的后备池 |

使用 `FallbackAllocator` 模式：专用池 → 大共享池 → 小共享池。

### 5.5 MonotonicFrameCounter（单调帧计数器）

解决帧计数在 flush 后重置的问题，确保 VolumeShaper 等自动化控制的连续性：

```cpp
class MonotonicFrameCounter {
    int64_t updateAndGetMonotonicFrameCount(int64_t newFrameCount, int64_t newTime);
    int64_t onFlush();  // flush 后保持单调递增
};
```

### 5.6 SynchronizedRecordState（同步录音状态）

从 RecordThread 提取的同步录音状态管理类，支持录音同步启动事件的超时和取消：

```cpp
class SynchronizedRecordState {
    void startRecording(const sp<SyncEvent>& event);   // 开始录音同步
    void onPlaybackFinished(const sp<SyncEvent>& event, size_t framesToDrop);  // 播放结束回调
    ssize_t updateRecordFrames(size_t frames);  // 更新帧丢弃计数
};
```

---

## 6. 音效系统变化

### 6.1 新增音效类

| 新增类 | 说明 |
|-------|------|
| `HwAccDeviceEffectModule` | 硬件加速设备音效模块 — 直接使用 HAL 的硬件加速 |
| `InternalEffectHandle` | 内部音效句柄 — 用于 AudioFlinger 内部使用的音效（非客户端创建） |

### 6.2 接口抽象

所有音效相关类均增加了对 `IAf*.h` 接口的继承：

| 类 | Android 12 继承 | Android 16 继承 |
|---|----------------|----------------|
| `EffectBase` | `RefBase` | `virtual IAfEffectBase` |
| `EffectModule` | `EffectBase` | `IAfEffectModule` + `EffectBase` |
| `EffectHandle` | `android::media::BnEffect` | `IAfEffectHandle` + `android::media::BnEffect` |
| `EffectChain` | `RefBase` | `IAfEffectChain` |
| `DeviceEffectProxy` | `EffectBase` | `IAfDeviceEffectProxy` + `EffectBase` |

---

## 7. 构建系统变化

### 7.1 整体对比

| 维度 | Android 12 | Android 16 |
|------|-----------|-----------|
| 构建文件数 | 1 (`Android.bp`) | 6 (`Android.bp` × 6) |
| 输出库数 | 1 (`libaudioflinger`) | 6（见下表） |
| 库类型 | `cc_library_shared` | `cc_library` (static + shared) |
| 代码检查 | 基础 `-Werror -Wall` | 全面的 `clang-tidy` 检查 + 严格告警 |
| tidy 检查项 | 无 | ~50 项 `clang-tidy` 规则 |

### 7.2 输出库详情

| 库名 | 类型 | 说明 |
|-----|------|------|
| `libaudioflinger` | `cc_library` | 核心库 |
| `libaudioflinger_utils` | `cc_library` | 工具库 |
| `libaudioflinger_datapath` | `cc_library` | 数据路径库 |
| `libaudioflinger_fastpath` | `cc_library_shared` | 快速路径库 |
| `libsounddose` | `cc_library`（`double_loadable`） | 声音剂量库 |
| `libaudioflinger_timing` | `cc_library`（`host_supported`） | 时序工具库 |

### 7.3 新增依赖

**Android 16 新增的关键依赖**：

| 依赖 | 说明 |
|------|------|
| `audio-permission-aidl-cpp` | 音频权限 AIDL 接口 |
| `com.android.media.audio-aconfig-cc` | 音频功能标志（Feature Flags） |
| `com.android.media.audioserver-aconfig-cc` | AudioServer 功能标志 |
| `libactivitymanager_aidl` | ActivityManager AIDL 接口 |
| `libaudiomanager` | AudioManager 接口 |
| `libaudiopermission` | 音频权限管理 |
| `libbinder_ndk` | NDK Binder（用于 AIDL HAL） |
| `libsounddose` | 声音剂量管理 |
| `packagemanager_aidl-cpp` | PackageManager AIDL 接口 |

**Android 12 中存在但 Android 16 中移除的依赖**：

| 依赖 | 说明 |
|------|------|
| `libmedialogservice` | 媒体日志服务（已整合到其他组件） |
| `libsndfile`（静态） | 音频文件处理（移入 `afutils`） |
| `libnblog` | NBLog（从主库移入 `fastpath`） |
| `libaudiospdif`（共享） | SPDIF（改为静态链接） |

### 7.4 代码质量工具

**Android 16** 引入了全面的 `clang-tidy` 检查（约 50 条规则），按子模块定制不同的检查级别：

```python
# 基础检查规则（所有子模块共享）
audioflinger_base_tidy_errors = [
    "android-*",
    "bugprone-*",
    "cert-*",
    "clang-analyzer-security*",
    "google-*",
    "misc-*",
    "modernize-*",  # 选择性开启
    "performance-*",
]

# 各子模块可额外添加或豁免特定规则
audioflinger_datapath_tidy_errors = base + [
    "modernize-avoid-c-arrays",
    "modernize-use-auto",
    "modernize-use-nodiscard",
    # ...
]
```

---

## 8. 代码风格与现代化变化

### 8.1 移除 Float Effect Chain 编译条件

**Android 12**：通过宏定义控制浮点音效链的启用：
```cpp
#define FLOAT_EFFECT_CHAIN
#ifdef FLOAT_EFFECT_CHAIN
#define FLOAT_AUX
#define MULTICHANNEL_EFFECT_CHAIN
#endif
```

**Android 16**：这些宏已被移除，浮点音效处理和多通道音效成为**默认行为**。

### 8.2 头文件保护

**Android 12**：使用传统的 `#ifndef` / `#define` 保护：
```cpp
#ifndef ANDROID_AUDIO_FLINGER_H
#define ANDROID_AUDIO_FLINGER_H
// ...
#endif
```

**Android 16**：新增文件使用 `#pragma once`：
```cpp
#pragma once
```

### 8.3 命名空间

**Android 16** 引入了更细粒度的命名空间：
- `android::audioflinger` — timing 模块的类
- `android::afutils` — 工具类（Permission、Vibrator）
- `android::AllocatorFactory` — 内存分配器

### 8.4 编译器标志

**Android 16** 新增的编译器标志：
```
-Wdeprecated
-Werror=conditional-uninitialized
-Werror=implicit-fallthrough
-Werror=sometimes-uninitialized
-Wextra
-Wredundant-decls
-Wshadow
-Wstrict-aliasing
-Wthread-safety
-Wunreachable-code
-Wunreachable-code-break
-Wunreachable-code-return
-Wunused
-Wused-but-marked-unused
-fstrict-aliasing
```

---

## 9. 测试变化

**Android 12**：无测试文件。

**Android 16**：新增 4 个测试文件（599 行）：

| 测试文件 | 行数 | 说明 |
|---------|------|------|
| `sounddose/tests/sounddosemanager_tests.cpp` | 354 | Sound Dose 管理器单元测试 |
| `timing/tests/mediasyncevent_tests.cpp` | 70 | SyncEvent 单元测试 |
| `timing/tests/monotonicframecounter_tests.cpp` | 97 | MonotonicFrameCounter 单元测试 |
| `timing/tests/synchronizedrecordstate_tests.cpp` | 78 | SynchronizedRecordState 单元测试 |

`TEST_MAPPING` 配置了预提交测试，包括 `CtsNativeMediaAAudioTestCases`（AAudio 基础测试）。

---

## 10. 总结

Android 12 到 Android 16 的 `services/audioflinger` 经历了以下重大变化：

1. **架构模块化**：从单一扁平目录（54 个文件、1 个库）重构为 6 个子模块（89 个文件、6 个库），职责更清晰
2. **接口与实现分离**：新增 4 个 `IAf*.h` 纯虚接口（2,207 行），AudioFlinger "上帝类" 的约 40 个内部类被提取为独立类，通过接口通信
3. **线程安全强化**：线程安全注解从约 39 处增长到约 740 处，引入编译时的线程安全检查
4. **新增功能**：
   - Sound Dose（声音剂量）— 听力保护
   - SpatializerThread — 空间音频处理
   - BitPerfectThread — 比特精确高保真播放
   - DirectRecordThread — 直接录音路径
   - PatchCommandThread — 异步 Patch 命令
   - MonotonicFrameCounter — 单调帧计数
   - AllocatorFactory — 分层内存池管理
5. **代码质量提升**：引入 ~50 条 `clang-tidy` 规则、更严格的编译器告警、现代 C++ 实践
6. **测试覆盖**：从零测试到 599 行测试代码，覆盖核心组件
7. **构建现代化**：从单一库变为模块化独立库，支持 `double_loadable`、`host_supported` 等高级构建特性
8. **浮点音效默认化**：`FLOAT_EFFECT_CHAIN` 等编译条件被移除，浮点与多通道音效成为默认行为
