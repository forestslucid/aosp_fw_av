# AOSP 16 Local HAL — Recording Failure Root-Cause Analysis

## Background

PR#3 re-introduced the LOCAL (passthrough) audio HAL for AOSP 16, adapting the
Android 12 implementation to the expanded `DeviceHalInterface` / `StreamHalInterface`
APIs. Three activation paths were defined in `FactoryHal.cpp`:

1. **Forced** — `ro.audio.hal.force_local=true` or `persist.audio.hal.local.enabled=true`
2. **Normal** — AIDL/HIDL discovery (unchanged)
3. **Fallback** — Automatic fallback when no AIDL/HIDL service is found

## The Bug: Overly Broad Fallback Condition

The fallback path that PR#3 introduced was:

```cpp
// After the AIDL/HIDL compatibility check fails for any reason:
if (isDevice) {          // ← too broad
    createLocalHalService(true, &rawInterface);
    return rawInterface;
}
```

The intent was "fall back only when no AIDL/HIDL service is reachable at all."
The actual behaviour was "fall back whenever the **version-compatibility check** fails,"
which includes cases where an AIDL/HIDL service **does** exist but one of the following
conditions was true:

| Trigger | Example |
|---------|---------|
| Effects HAL not present | Device has AIDL device HAL but no effects HAL |
| Type mismatch | Device HAL is AIDL, effects HAL is HIDL 7.x |
| Major-version mismatch | Device HAL AIDL v1, effects HAL AIDL v2 |
| `createHalService()` runtime failure | `dlopen` / factory function failure |

The compatibility check in `createPreferredImpl()`:

```cpp
if (ifaceVersionIt != sAudioHALVersions.end() &&
    siblingVersionIt != sAudioHALVersions.end() &&
    ifaceVersionIt->getType() == siblingVersionIt->getType() &&
    ifaceVersionIt->getMajorVersion() == siblingVersionIt->getMajorVersion()) {
    // Normal path — create AIDL/HIDL service
} else {
    ALOGW("Found no HAL version ...");
    // falls through to the LOCAL fallback
}
```

When any of the four triggers above occurs, the code falls through to the LOCAL
fallback and loads `libaudiohal@local.so` **in-process**.

## Why Recording Fails

### Memory Corruption Path

Loading a vendor HAL in-process while AIDL/HIDL services already run elsewhere is
unsafe on Android 16.  The sequence is:

1. `DevicesFactoryHalLocal::openDevice()` calls `hw_get_module_by_class()`.
2. This `dlopen()`s the vendor HAL shared library inside the **audioserver** process.
3. The vendor HAL's static initialisation and `init_check()` call `mmap()`.
4. `mmap()` can map new anonymous pages at addresses that **alias** the existing
   `audio_utils_fifo` throttle-index page, which is shared between the
   RecordThread and the FastCapture thread through `MonoPipe`.
5. The throttle-index pointer stored in the `audio_utils_fifo` writer now points to
   the wrong (vendor HAL) page.

### Crash During Recording Start

When the first recording track starts:

1. `RecordThread` constructor calls `AudioStreamInSource::negotiate()`, which calls
   `mStream->getBufferSize()` and `mStream->getAudioProperties()`.  These succeed.
2. The constructor also initialises `FastCapture` and links it to `MonoPipe`.
3. On the first recording loop iteration, `FastCapture::onWork()` calls
   `mPipeSink->write(mReadBuffer, mReadBufferState)`.
4. `MonoPipe::write()` → `audio_utils_fifo_writer::release()` attempts a
   `futex(FUTEX_WAKE_PRIVATE, …)` on the corrupted throttle address.
5. The kernel returns `EFAULT` (errno 14, bad address).
6. `LOG_ALWAYS_FATAL("release: unexpected err=%d errno=%d", …)` → **SIGABRT**.

The audioserver crashes and recording never completes.

### Silent Failure Path (no legacy HAL)

On devices that ship **only** AIDL HAL (no legacy `.so` in `/vendor/lib`):

1. The incorrect fallback triggers `createLocalHalService()`.
2. `DevicesFactoryHalLocal::openDevice("primary")` calls
   `hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, "primary", &mod)`.
3. No legacy HAL module is present → `rc = -ENOENT`.
4. `loadHwModule_ll()` returns `nullptr` (no device loaded).
5. AudioFlinger has no primary device → `openInputStream()` is never reachable →
   recording silently fails.

## The Fix

Restrict the LOCAL fallback to the case where the device HAL service is **truly
absent** — i.e., `ifaceVersionIt == sAudioHALVersions.end()`:

```cpp
// Before (PR#3 — too broad):
if (isDevice) {

// After (this PR — only when no device service was found):
if (isDevice && ifaceVersionIt == sAudioHALVersions.end()) {
```

### Why This Is Safe

* When `ifaceVersionIt == end()`, there is no AIDL/HIDL device service at all.
  Loading the LOCAL vendor HAL in-process is the only option; no existing
  `audio_utils_fifo` pages will be corrupted because no AIDL streaming is active.

* When the device HAL **is** found but the effects HAL is missing or incompatible,
  returning `nullptr` (no device factory) is the correct outcome.  It signals to
  AudioFlinger that the HAL cannot be loaded and is preferable to memory corruption.

* The **forced** path (`ro.audio.hal.force_local=true`) is unaffected.  Users who
  explicitly opt into LOCAL mode on a device that has a legacy HAL do so knowingly.

## Related Pull Requests

| PR | Title | Status |
|----|-------|--------|
| PR#3 | Add Local/Passthrough HAL support for AOSP 16 libaudiohal | Merged |
| PR#4 | Fix AOSP 16 libaudiohal Local/Passthrough compilation errors | Draft |
| PR#5 | Fix audioserver SIGABRT caused by incorrect LOCAL HAL fallback in FactoryHal.cpp | Draft |
| PR#8 | Fix local loading compatibility issues in AOSP16 (this PR) | Open |

PR#5 (still in Draft, not yet merged) proposed the identical one-line fix to the
fallback condition and provided a brief description of the SIGABRT.  This PR
(PR#8) independently applies that same fix, promotes it out of Draft, and adds
the full root-cause analysis documented here.
