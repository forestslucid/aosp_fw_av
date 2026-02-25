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

#ifndef ANDROID_HARDWARE_DEVICES_FACTORY_HAL_LOCAL_H
#define ANDROID_HARDWARE_DEVICES_FACTORY_HAL_LOCAL_H

#include <media/audiohal/DevicesFactoryHalInterface.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>

namespace android {

class DevicesFactoryHalLocal : public DevicesFactoryHalInterface
{
  public:
    DevicesFactoryHalLocal() = default;

    status_t getDeviceNames(std::vector<std::string> *names) override;

    // Opens a device with the specified name. To close the device, it is
    // necessary to release references to the returned object.
    status_t openDevice(const char *name, sp<DeviceHalInterface> *device) override;

    status_t setCallbackOnce(sp<DevicesFactoryHalCallback> callback) override;

    android::detail::AudioHalVersionInfo getHalVersion() const override;

    status_t getSurroundSoundConfig(media::SurroundSoundConfig *config) override;

    status_t getEngineConfig(
            media::audio::common::AudioHalEngineConfig *config) override;

  private:
    virtual ~DevicesFactoryHalLocal() = default;
};

} // namespace android

#endif // ANDROID_HARDWARE_DEVICES_FACTORY_HAL_LOCAL_H
