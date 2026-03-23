# AOSP 16 全链路落地清单：系统应用调用应用静音/解静音

> 目标：仅保证**系统应用**可以稳定调用“按包名静音/解静音”接口并生效。  
> 范围：`frameworks/base + frameworks/av + 权限 + SELinux + 持久化`。  
> 说明：当前仓库是 `frameworks/av` 子树，本清单用于在**完整 AOSP 源码树**执行。

---

## 1. 最小可用能力定义（验收标准）

- 系统应用可通过 Java API 调用：
  - `setAppMuted(String packageName, int userId, boolean muted)`
  - `isAppMuted(String packageName, int userId)`
- 非系统应用调用应被权限拒绝。
- 静音可作用到目标应用所有播放输出（媒体/通知/闹钟等，统一按 UID 维度生效）。
- 重启后静音状态可恢复。
- 应用卸载（含对应 user）后，静音记录自动清理。

---

## 2. 需要修改的完整文件清单（按层次）

## A. frameworks/base（系统 API + 系统服务）

1. `frameworks/base/core/java/android/media/AudioManager.java`
   - 新增系统 API（`@SystemApi`）。

2. `frameworks/base/media/java/android/media/IAudioService.aidl`
   - 新增 Binder 接口定义（packageName + userId 维度）。

3. `frameworks/base/services/core/java/com/android/server/audio/AudioService.java`
   - 新增 API 实现、权限检查、转发到 `AudioSystem/AudioPolicyService`。
   - 负责开机恢复（持久化读取并回灌）。

4. `frameworks/base/services/core/java/com/android/server/audio/AudioServiceEvents.java`（可选）
   - 增加调试事件日志（便于 dumpsys 追踪）。

5. `frameworks/base/services/tests/servicestests/src/com/android/server/audio/...`
   - 新增/扩展 AudioService 单元测试。

## B. frameworks/av（音频策略执行层）

1. `frameworks/av/media/libaudioclient/aidl/android/media/IAudioPolicyService.aidl`
   - 新增 app mute 接口（UID + package/user 查询接口）。

2. `frameworks/av/services/audiopolicy/service/AudioPolicyService.h`
3. `frameworks/av/services/audiopolicy/service/AudioPolicyInterfaceImpl.cpp`
4. `frameworks/av/services/audiopolicy/service/AudioPolicyService.cpp`
   - 实现 Binder 接口、权限栅栏、事务白名单。

5. `frameworks/av/services/audiopolicy/AudioPolicyInterface.h`
6. `frameworks/av/services/audiopolicy/managerdefault/AudioPolicyManager.h`
7. `frameworks/av/services/audiopolicy/managerdefault/AudioPolicyManager.cpp`
   - 维护 UID 维度 mute 状态，更新活跃 track internal mute。

8. `frameworks/av/services/audiopolicy/tests/audiopolicymanager_tests.cpp`
   - 新增策略层用例（静音/解静音前后 track 状态）。

## C. 权限定义

1. `frameworks/base/core/res/AndroidManifest.xml`
   - 新增权限：`android.permission.MANAGE_APP_AUDIO_MUTE`
   - protectionLevel：`signature|privileged`

2. `frameworks/base/data/etc/platform.xml`（按产品策略可选）
   - 若需给特定系统组件默认授予，可配置。

## D. SELinux（system_server ↔ audioserver 调用链）

1. `system/sepolicy/public/service.te` / `private/service_contexts`（按实际 service 命名）
2. `system/sepolicy/private/system_server.te`
3. `system/sepolicy/private/audioserver.te`
   - 放通 system_server 对 audioserver 音频策略 Binder 调用。
   - 仅增加最小必要规则，避免过授权。

## E. 持久化

1. `frameworks/base/services/core/java/com/android/server/audio/AudioService.java`
2. `frameworks/base/packages/SettingsProvider/...`（若采用 SettingsProvider）
   - 存储建议：`Settings.Global`（JSON/CSV）或独立原子文件（`AtomicFile`）。
   - 建议 key：`audio_app_mute_state_v1`。

---

## 3. 接口与实现要点（推荐最小方案）

## 3.1 AudioManager（系统 API）

```java
@SystemApi
@RequiresPermission(android.Manifest.permission.MANAGE_APP_AUDIO_MUTE)
public void setAppMuted(@NonNull String packageName, @UserIdInt int userId, boolean muted);

@SystemApi
@RequiresPermission(android.Manifest.permission.MANAGE_APP_AUDIO_MUTE)
public boolean isAppMuted(@NonNull String packageName, @UserIdInt int userId);
```

## 3.2 IAudioService.aidl

```aidl
void setAppMuted(in String packageName, int userId, boolean muted);
boolean isAppMuted(in String packageName, int userId);
```

## 3.3 AudioService 权限与路由

- 统一使用：`enforceCallingOrSelfPermission(MANAGE_APP_AUDIO_MUTE, ...)`。
- 解析 `packageName + userId -> uid`（PackageManager）。
- 调用到 `AudioSystem` / `IAudioPolicyService`：
  - `setAppMuteForPackage(packageName, userId, muted)`
  - `isAppMutedForPackage(packageName, userId)`
- 将状态写入持久化存储。

## 3.4 frameworks/av 策略生效

- 在 AudioPolicyManager 维护 `uid -> muted` 映射。
- 对活跃播放 client 通过 internal mute 立即生效。
- 新建输出 client 时检查 UID muted 状态，保证增量流也被静音。
- 在 `releaseResourcesForUid(uid)` 时清理内存态记录。

---

## 4. 持久化与恢复（可选但建议开启）

## 4.1 写入时机

- `setAppMuted(..., true/false)` 成功后即写入。
- 结构建议：`{"userId:packageName": true/false}`。

## 4.2 恢复时机

- `AudioService.onBootPhase(PHASE_SYSTEM_SERVICES_READY)` 后恢复。
- 遍历持久化记录，逐项调用策略接口回灌。

## 4.3 卸载自动清理

- 在 `AudioService` 注册 `PackageMonitor` 或 `ACTION_PACKAGE_FULLY_REMOVED`。
- 删除对应 `userId + packageName` 持久化项。
- 同步调用取消静音（防止残留）。

---

## 5. SELinux 最小策略原则

- 只允许 system_server 发起该控制链路。
- 不给普通 app domain 放行。
- 新增规则后用 `audit2allow` 仅做参考，最终人工收敛到最小集。

---

## 6. 测试清单（最小闭环）

## 6.1 单元测试

- AudioService：
  - 系统权限通过；无权限拒绝。
  - package/user -> uid 映射失败路径。
  - 持久化写入/恢复。
- AudioPolicyManager：
  - mute/unmute 对活跃与新启动流生效。

## 6.2 集成测试（设备侧）

- 系统应用调用 API 后目标应用无声；解静音恢复。
- 覆盖多种 usage（media/notification/alarm）。
- 重启后状态恢复。
- 卸载应用后记录被清理，重装默认非静音。
- 多用户隔离：user 0 与 user 10 同包互不影响。

---

## 7. 与当前仓库对应关系

- 本仓库（`frameworks/av`）已可承载策略执行与 audioserver Binder 部分。
- `frameworks/base / permission / sepolicy / SettingsProvider` 需在完整 AOSP 源码树补齐。
- 若只追求“系统应用可调用并生效”，优先完成顺序：
  1) `frameworks/base` API + AudioService 转发 + 权限；
  2) `frameworks/av` mute 生效；
  3) 持久化与卸载清理；
  4) SELinux 收敛。

