/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "DevicesFactoryHalLocal"
//#define LOG_NDEBUG 0

#include <string.h>

#include <hardware/audio.h>
#include <utils/Log.h>

#include "DeviceHalLocal.h"
#include "DevicesFactoryHalLocal.h"

namespace android {

static status_t load_audio_interface(const char *if_name, audio_hw_device_t **dev)
{
    const hw_module_t *mod;
    int rc;

    rc = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, if_name, &mod);
    if (rc) {
        ALOGE("%s couldn't load audio hw module %s.%s (%s)", __func__,
                AUDIO_HARDWARE_MODULE_ID, if_name, strerror(-rc));
        goto out;
    }
    rc = audio_hw_device_open(mod, dev);
    if (rc) {
        ALOGE("%s couldn't open audio hw device in %s.%s (%s)", __func__,
                AUDIO_HARDWARE_MODULE_ID, if_name, strerror(-rc));
        goto out;
    }
    if ((*dev)->common.version < AUDIO_DEVICE_API_VERSION_MIN) {
        ALOGE("%s wrong audio hw device version %04x", __func__, (*dev)->common.version);
        rc = BAD_VALUE;
        audio_hw_device_close(*dev);
        goto out;
    }
    return OK;

out:
    *dev = NULL;
    return rc;
}

status_t DevicesFactoryHalLocal::getDeviceNames(std::vector<std::string> *names) {
    if (names == nullptr) {
        return BAD_VALUE;
    }
    names->push_back("primary");
    return OK;
}

status_t DevicesFactoryHalLocal::openDevice(const char *name, sp<DeviceHalInterface> *device) {
    audio_hw_device_t *dev;
    status_t rc = load_audio_interface(name, &dev);
    if (rc == OK) {
        *device = new DeviceHalLocal(dev);
    }
    return rc;
}

status_t DevicesFactoryHalLocal::setCallbackOnce(sp<DevicesFactoryHalCallback> callback) {
    // In local/passthrough mode, all devices are immediately available.
    ALOG_ASSERT(callback != nullptr);
    if (callback != nullptr) {
        callback->onNewDevicesAvailable();
    }
    return NO_ERROR;
}

android::detail::AudioHalVersionInfo DevicesFactoryHalLocal::getHalVersion() const {
    // Return HIDL type with version 0.0 to indicate local/passthrough mode.
    return android::detail::AudioHalVersionInfo(
            android::detail::AudioHalVersionInfo::Type::HIDL, 0, 0);
}

status_t DevicesFactoryHalLocal::getSurroundSoundConfig(
        media::SurroundSoundConfig *config __unused) {
    return INVALID_OPERATION;
}

status_t DevicesFactoryHalLocal::getEngineConfig(
        media::audio::common::AudioHalEngineConfig *config __unused) {
    return INVALID_OPERATION;
}

// Main entry-point to the shared library.
extern "C" __attribute__((visibility("default"))) void* createIDevicesFactoryImpl() {
    return new DevicesFactoryHalLocal();
}

} // namespace android
