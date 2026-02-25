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

#define LOG_TAG "DeviceHalLocal"
//#define LOG_NDEBUG 0

#include <utils/Log.h>

#include "DeviceHalLocal.h"
#include "StreamHalLocal.h"

namespace android {

DeviceHalLocal::DeviceHalLocal(audio_hw_device_t *dev)
        : mDev(dev) {
}

DeviceHalLocal::~DeviceHalLocal() {
    int status = audio_hw_device_close(mDev);
    ALOGW_IF(status, "Error closing audio hw device %p: %s", mDev, strerror(-status));
    mDev = 0;
}

status_t DeviceHalLocal::getAudioPorts(
        std::vector<media::audio::common::AudioPort> *ports __unused) {
    // Not supported in local/passthrough mode.
    return INVALID_OPERATION;
}

status_t DeviceHalLocal::getAudioRoutes(
        std::vector<media::AudioRoute> *routes __unused) {
    return INVALID_OPERATION;
}

status_t DeviceHalLocal::getSupportedModes(
        std::vector<media::audio::common::AudioMode> *modes __unused) {
    return INVALID_OPERATION;
}

status_t DeviceHalLocal::getSupportedDevices(uint32_t *devices) {
    if (mDev->get_supported_devices == NULL) return INVALID_OPERATION;
    *devices = mDev->get_supported_devices(mDev);
    return OK;
}

status_t DeviceHalLocal::initCheck() {
    return mDev->init_check(mDev);
}

status_t DeviceHalLocal::setVoiceVolume(float volume) {
    return mDev->set_voice_volume(mDev, volume);
}

status_t DeviceHalLocal::setMasterVolume(float volume) {
    if (mDev->set_master_volume == NULL) return INVALID_OPERATION;
    return mDev->set_master_volume(mDev, volume);
}

status_t DeviceHalLocal::getMasterVolume(float *volume) {
    if (mDev->get_master_volume == NULL) return INVALID_OPERATION;
    return mDev->get_master_volume(mDev, volume);
}

status_t DeviceHalLocal::setMode(audio_mode_t mode) {
    return mDev->set_mode(mDev, mode);
}

status_t DeviceHalLocal::setMicMute(bool state) {
    return mDev->set_mic_mute(mDev, state);
}

status_t DeviceHalLocal::getMicMute(bool *state) {
    return mDev->get_mic_mute(mDev, state);
}

status_t DeviceHalLocal::setMasterMute(bool state) {
    if (mDev->set_master_mute == NULL) return INVALID_OPERATION;
    return mDev->set_master_mute(mDev, state);
}

status_t DeviceHalLocal::getMasterMute(bool *state) {
    if (mDev->get_master_mute == NULL) return INVALID_OPERATION;
    return mDev->get_master_mute(mDev, state);
}

status_t DeviceHalLocal::setParameters(const String8& kvPairs) {
    return mDev->set_parameters(mDev, kvPairs.c_str());
}

status_t DeviceHalLocal::getParameters(const String8& keys, String8 *values) {
    char *halValues = mDev->get_parameters(mDev, keys.c_str());
    if (halValues != NULL) {
        values->setTo(halValues);
        free(halValues);
    } else {
        values->clear();
    }
    return OK;
}

status_t DeviceHalLocal::getInputBufferSize(
        struct audio_config *config, size_t *size) {
    *size = mDev->get_input_buffer_size(mDev, config);
    return OK;
}

status_t DeviceHalLocal::openOutputStream(
        audio_io_handle_t handle,
        audio_devices_t deviceType,
        audio_output_flags_t flags,
        struct audio_config *config,
        const char *address,
        sp<StreamOutHalInterface> *outStream,
        const std::vector<playback_track_metadata_v7_t>& /*sourceMetadata*/) {
    audio_stream_out_t *halStream;
    ALOGV("open_output_stream handle: %d devices: %x flags: %#x"
            "srate: %d format %#x channels %x address %s",
            handle, deviceType, flags,
            config->sample_rate, config->format, config->channel_mask,
            address);
    int openResult = mDev->open_output_stream(
            mDev, handle, deviceType, flags, config, &halStream, address);
    if (openResult == OK) {
        *outStream = new StreamOutHalLocal(halStream, this);
    }
    ALOGV("open_output_stream status %d stream %p", openResult, halStream);
    return openResult;
}

status_t DeviceHalLocal::openInputStream(
        audio_io_handle_t handle,
        audio_devices_t devices,
        struct audio_config *config,
        audio_input_flags_t flags,
        const char *address,
        audio_source_t source,
        audio_devices_t /*outputDevice*/,
        const char * /*outputDeviceAddress*/,
        sp<StreamInHalInterface> *inStream) {
    audio_stream_in_t *halStream;
    ALOGV("open_input_stream handle: %d devices: %x flags: %#x "
            "srate: %d format %#x channels %x address %s source %d",
            handle, devices, flags,
            config->sample_rate, config->format, config->channel_mask,
            address, source);
    int openResult = mDev->open_input_stream(
            mDev, handle, devices, config, &halStream, flags, address, source);
    if (openResult == OK) {
        *inStream = new StreamInHalLocal(halStream, this);
    }
    ALOGV("open_input_stream status %d stream %p", openResult, inStream);
    return openResult;
}

status_t DeviceHalLocal::supportsAudioPatches(bool *supportsPatches) {
    *supportsPatches = version() >= AUDIO_DEVICE_API_VERSION_3_0;
    return OK;
}

status_t DeviceHalLocal::createAudioPatch(
        unsigned int num_sources,
        const struct audio_port_config *sources,
        unsigned int num_sinks,
        const struct audio_port_config *sinks,
        audio_patch_handle_t *patch) {
    if (version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        return mDev->create_audio_patch(
                mDev, num_sources, sources, num_sinks, sinks, patch);
    } else {
        return INVALID_OPERATION;
    }
}

status_t DeviceHalLocal::releaseAudioPatch(audio_patch_handle_t patch) {
    if (version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        return mDev->release_audio_patch(mDev, patch);
    } else {
        return INVALID_OPERATION;
    }
}

status_t DeviceHalLocal::getAudioPort(struct audio_port *port) {
    return mDev->get_audio_port(mDev, port);
}

status_t DeviceHalLocal::getAudioPort(struct audio_port_v7 *port) {
    if (version() >= AUDIO_DEVICE_API_VERSION_3_2) {
        return mDev->get_audio_port_v7(mDev, port);
    }
    struct audio_port audioPort = {};
    if (!audio_populate_audio_port(port, &audioPort)) {
        ALOGE("Failed to populate legacy audio port from audio_port_v7");
        return BAD_VALUE;
    }
    status_t status = getAudioPort(&audioPort);
    if (status == NO_ERROR) {
        audio_populate_audio_port_v7(&audioPort, port);
    }
    return status;
}

status_t DeviceHalLocal::setAudioPortConfig(const struct audio_port_config *config) {
    if (version() >= AUDIO_DEVICE_API_VERSION_3_0)
        return mDev->set_audio_port_config(mDev, config);
    else
        return INVALID_OPERATION;
}

status_t DeviceHalLocal::getMicrophones(
        std::vector<audio_microphone_characteristic_t> *microphones) {
    if (mDev->get_microphones == NULL) return INVALID_OPERATION;
    size_t actual_mics = AUDIO_MICROPHONE_MAX_COUNT;
    audio_microphone_characteristic_t mic_array[AUDIO_MICROPHONE_MAX_COUNT];
    status_t status = mDev->get_microphones(mDev, &mic_array[0], &actual_mics);
    for (size_t i = 0; i < actual_mics; i++) {
        microphones->push_back(mic_array[i]);
    }
    return status;
}

status_t DeviceHalLocal::addDeviceEffect(
        const struct audio_port_config *device __unused,
        sp<EffectHalInterface> effect __unused) {
    // Local HAL does not support device effects.
    return INVALID_OPERATION;
}

status_t DeviceHalLocal::removeDeviceEffect(
        const struct audio_port_config *device __unused,
        sp<EffectHalInterface> effect __unused) {
    return INVALID_OPERATION;
}

status_t DeviceHalLocal::getMmapPolicyInfos(
        media::audio::common::AudioMMapPolicyType policyType __unused,
        std::vector<media::audio::common::AudioMMapPolicyInfo> *policyInfos __unused) {
    return INVALID_OPERATION;
}

int32_t DeviceHalLocal::getAAudioMixerBurstCount() {
    return INVALID_OPERATION;
}

int32_t DeviceHalLocal::getAAudioHardwareBurstMinUsec() {
    return INVALID_OPERATION;
}

status_t DeviceHalLocal::supportsBluetoothVariableLatency(bool *supports) {
    if (supports != nullptr) {
        *supports = false;
    }
    return OK;
}

status_t DeviceHalLocal::setConnectedState(
        const struct audio_port_v7 *port __unused, bool connected __unused) {
    return INVALID_OPERATION;
}

status_t DeviceHalLocal::setSimulateDeviceConnections(bool enabled __unused) {
    return INVALID_OPERATION;
}

error::Result<audio_hw_sync_t> DeviceHalLocal::getHwAvSync() {
    if (mDev->get_audio_hw_sync != NULL) {
        return mDev->get_audio_hw_sync(mDev);
    }
    return base::unexpected(INVALID_OPERATION);
}

status_t DeviceHalLocal::dump(int fd, const Vector<String16>& args __unused) {
    return mDev->dump(mDev, fd);
}

status_t DeviceHalLocal::getSoundDoseInterface(
        const std::string& module __unused,
        ::ndk::SpAIBinder* soundDoseBinder __unused) {
    return INVALID_OPERATION;
}

status_t DeviceHalLocal::prepareToDisconnectExternalDevice(
        const struct audio_port_v7 *port __unused) {
    return INVALID_OPERATION;
}

status_t DeviceHalLocal::getAudioMixPort(
        const struct audio_port_v7 *devicePort __unused,
        struct audio_port_v7 *mixPort __unused) {
    return INVALID_OPERATION;
}

void DeviceHalLocal::closeOutputStream(struct audio_stream_out *stream_out) {
    mDev->close_output_stream(mDev, stream_out);
}

void DeviceHalLocal::closeInputStream(struct audio_stream_in *stream_in) {
    mDev->close_input_stream(mDev, stream_in);
}

} // namespace android
