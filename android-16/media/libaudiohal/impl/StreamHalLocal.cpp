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

#define LOG_TAG "StreamHalLocal"
//#define LOG_NDEBUG 0

#include <audio_utils/Metadata.h>
#include <hardware/audio.h>
#include <media/AudioParameter.h>
#include <utils/Log.h>

#include "DeviceHalLocal.h"
#include "StreamHalLocal.h"

namespace android {

StreamHalLocal::StreamHalLocal(audio_stream_t *stream, sp<DeviceHalLocal> device)
        : mDevice(device),
          mStream(stream) {
    // Instrument audio signal power logging.
    // Note: This assumes channel mask, format, and sample rate do not change after creation.
    if (mStream != nullptr) {
        mStreamPowerLog.init(mStream->get_sample_rate(mStream),
                mStream->get_channels(mStream),
                mStream->get_format(mStream));
    }
}

StreamHalLocal::~StreamHalLocal() {
    mStream = nullptr;
    mDevice.clear();
}

status_t StreamHalLocal::getBufferSize(size_t *size) {
    *size = mStream->get_buffer_size(mStream);
    return OK;
}

status_t StreamHalLocal::getAudioProperties(audio_config_base_t *configBase) {
    configBase->sample_rate = mStream->get_sample_rate(mStream);
    configBase->channel_mask = mStream->get_channels(mStream);
    configBase->format = mStream->get_format(mStream);
    return OK;
}

status_t StreamHalLocal::setParameters(const String8& kvPairs) {
    return mStream->set_parameters(mStream, kvPairs.c_str());
}

status_t StreamHalLocal::getParameters(const String8& keys, String8 *values) {
    char *halValues = mStream->get_parameters(mStream, keys.c_str());
    if (halValues != NULL) {
        values->setTo(halValues);
        free(halValues);
    } else {
        values->clear();
    }
    return OK;
}

status_t StreamHalLocal::addEffect(sp<EffectHalInterface>) {
    LOG_ALWAYS_FATAL("Local streams can not have effects");
    return INVALID_OPERATION;
}

status_t StreamHalLocal::removeEffect(sp<EffectHalInterface>) {
    LOG_ALWAYS_FATAL("Local streams can not have effects");
    return INVALID_OPERATION;
}

status_t StreamHalLocal::standby() {
    return mStream->standby(mStream);
}

status_t StreamHalLocal::close() {
    // Stream cleanup is handled by the destructor.
    return OK;
}

status_t StreamHalLocal::dump(int fd, const Vector<String16>& /*args*/) {
    status_t status = mStream->dump(mStream, fd);
    mStreamPowerLog.dump(fd);
    return status;
}

status_t StreamHalLocal::setHalThreadPriority(int) {
    // Don't need to do anything as local hal is executed by audioflinger directly
    // on the same thread.
    return OK;
}

status_t StreamHalLocal::legacyCreateAudioPatch(
        const struct audio_port_config& /*port*/,
        std::optional<audio_source_t> /*source*/,
        audio_devices_t /*type*/) {
    return INVALID_OPERATION;
}

status_t StreamHalLocal::legacyReleaseAudioPatch() {
    return INVALID_OPERATION;
}

StreamOutHalLocal::StreamOutHalLocal(audio_stream_out_t *stream, sp<DeviceHalLocal> device)
        : StreamHalLocal(&stream->common, device), mStream(stream) {
}

StreamOutHalLocal::~StreamOutHalLocal() {
    mCallback.clear();
    mDevice->closeOutputStream(mStream);
    mStream = nullptr;
}

status_t StreamOutHalLocal::getFrameSize(size_t *size) {
    *size = audio_stream_out_frame_size(mStream);
    return OK;
}

status_t StreamOutHalLocal::getLatency(uint32_t *latency) {
    *latency = mStream->get_latency(mStream);
    return OK;
}

status_t StreamOutHalLocal::setVolume(float left, float right) {
    if (mStream->set_volume == NULL) return INVALID_OPERATION;
    return mStream->set_volume(mStream, left, right);
}

status_t StreamOutHalLocal::selectPresentation(int presentationId, int programId) {
    AudioParameter param;
    param.addInt(String8(AudioParameter::keyPresentationId), presentationId);
    param.addInt(String8(AudioParameter::keyProgramId), programId);
    return setParameters(param.toString());
}

status_t StreamOutHalLocal::write(const void *buffer, size_t bytes, size_t *written) {
    ssize_t writeResult = mStream->write(mStream, buffer, bytes);
    if (writeResult > 0) {
        *written = writeResult;
        mStreamPowerLog.log(buffer, *written);
        return OK;
    } else {
        *written = 0;
        return writeResult;
    }
}

status_t StreamOutHalLocal::getRenderPosition(uint64_t *dspFrames) {
    uint32_t halPosition;
    status_t status = mStream->get_render_position(mStream, &halPosition);
    if (status == OK) {
        *dspFrames = halPosition;
    }
    return status;
}

status_t StreamOutHalLocal::setCallback(wp<StreamOutHalInterfaceCallback> callback) {
    if (mStream->set_callback == NULL) return INVALID_OPERATION;
    status_t result = mStream->set_callback(mStream, StreamOutHalLocal::asyncCallback, this);
    if (result == OK) {
        mCallback = callback;
    }
    return result;
}

// static
int StreamOutHalLocal::asyncCallback(stream_callback_event_t event, void*, void *cookie) {
    wp<StreamOutHalLocal> weakSelf(static_cast<StreamOutHalLocal*>(cookie));
    sp<StreamOutHalLocal> self = weakSelf.promote();
    if (self == nullptr) return 0;
    sp<StreamOutHalInterfaceCallback> callback = self->mCallback.promote();
    if (callback == nullptr) return 0;
    ALOGV("asyncCallback() event %d", event);
    switch (event) {
        case STREAM_CBK_EVENT_WRITE_READY:
            callback->onWriteReady();
            break;
        case STREAM_CBK_EVENT_DRAIN_READY:
            callback->onDrainReady();
            break;
        case STREAM_CBK_EVENT_ERROR:
            callback->onError(false /*isHardError*/);
            break;
        default:
            ALOGW("asyncCallback() unknown event %d", event);
            break;
    }
    return 0;
}

status_t StreamOutHalLocal::supportsPauseAndResume(bool *supportsPause, bool *supportsResume) {
    *supportsPause = mStream->pause != NULL;
    *supportsResume = mStream->resume != NULL;
    return OK;
}

status_t StreamOutHalLocal::pause() {
    if (mStream->pause == NULL) return INVALID_OPERATION;
    return mStream->pause(mStream);
}

status_t StreamOutHalLocal::resume() {
    if (mStream->resume == NULL) return INVALID_OPERATION;
    return mStream->resume(mStream);
}

status_t StreamOutHalLocal::supportsDrain(bool *supportsDrain) {
    *supportsDrain = mStream->drain != NULL;
    return OK;
}

status_t StreamOutHalLocal::drain(bool earlyNotify) {
    if (mStream->drain == NULL) return INVALID_OPERATION;
    return mStream->drain(mStream, earlyNotify ? AUDIO_DRAIN_EARLY_NOTIFY : AUDIO_DRAIN_ALL);
}

status_t StreamOutHalLocal::flush() {
    if (mStream->flush == NULL) return INVALID_OPERATION;
    return mStream->flush(mStream);
}

status_t StreamOutHalLocal::getPresentationPosition(uint64_t *frames, struct timespec *timestamp) {
    if (mStream->get_presentation_position == NULL) return INVALID_OPERATION;
    return mStream->get_presentation_position(mStream, frames, timestamp);
}

status_t StreamOutHalLocal::presentationComplete() {
    // Not supported by legacy HAL.
    return INVALID_OPERATION;
}

status_t StreamOutHalLocal::updateSourceMetadata(
        const SourceMetadata& sourceMetadata) {
    if (mStream->update_source_metadata_v7 != nullptr) {
        const source_metadata_v7_t metadata {
            .track_count = sourceMetadata.tracks.size(),
            .tracks = const_cast<playback_track_metadata_v7*>(sourceMetadata.tracks.data()),
        };
        mStream->update_source_metadata_v7(mStream, &metadata);
        return OK;
    }
    if (mStream->update_source_metadata != nullptr) {
        std::vector<playback_track_metadata> halTracks;
        halTracks.reserve(sourceMetadata.tracks.size());
        for (auto& metadata : sourceMetadata.tracks) {
            playback_track_metadata halTrackMetadata;
            playback_track_metadata_from_v7(&halTrackMetadata, &metadata);
            halTracks.push_back(halTrackMetadata);
        }
        const source_metadata_t halMetadata = {
            .track_count = halTracks.size(),
            .tracks = halTracks.data(),
        };
        mStream->update_source_metadata(mStream, &halMetadata);
        return OK;
    }
    return INVALID_OPERATION;
}

status_t StreamOutHalLocal::start() {
    if (mStream->start == NULL) return INVALID_OPERATION;
    return mStream->start(mStream);
}

status_t StreamOutHalLocal::stop() {
    if (mStream->stop == NULL) return INVALID_OPERATION;
    return mStream->stop(mStream);
}

status_t StreamOutHalLocal::createMmapBuffer(int32_t minSizeFrames,
                                  struct audio_mmap_buffer_info *info) {
    if (mStream->create_mmap_buffer == NULL) return INVALID_OPERATION;
    return mStream->create_mmap_buffer(mStream, minSizeFrames, info);
}

status_t StreamOutHalLocal::getMmapPosition(struct audio_mmap_position *position) {
    if (mStream->get_mmap_position == NULL) return INVALID_OPERATION;
    return mStream->get_mmap_position(mStream, position);
}

status_t StreamOutHalLocal::getDualMonoMode(audio_dual_mono_mode_t* mode) {
    if (mStream->get_dual_mono_mode == nullptr) return INVALID_OPERATION;
    return mStream->get_dual_mono_mode(mStream, mode);
}

status_t StreamOutHalLocal::setDualMonoMode(audio_dual_mono_mode_t mode) {
    if (mStream->set_dual_mono_mode == nullptr) return INVALID_OPERATION;
    return mStream->set_dual_mono_mode(mStream, mode);
}

status_t StreamOutHalLocal::getAudioDescriptionMixLevel(float* leveldB) {
    if (mStream->get_audio_description_mix_level == nullptr) return INVALID_OPERATION;
    return mStream->get_audio_description_mix_level(mStream, leveldB);
}

status_t StreamOutHalLocal::setAudioDescriptionMixLevel(float leveldB) {
    if (mStream->set_audio_description_mix_level == nullptr) return INVALID_OPERATION;
    return mStream->set_audio_description_mix_level(mStream, leveldB);
}

status_t StreamOutHalLocal::getPlaybackRateParameters(audio_playback_rate_t* playbackRate) {
    if (mStream->get_playback_rate_parameters == nullptr) return INVALID_OPERATION;
    return mStream->get_playback_rate_parameters(mStream, playbackRate);
}

status_t StreamOutHalLocal::setPlaybackRateParameters(
        const audio_playback_rate_t& playbackRate) {
    if (mStream->set_playback_rate_parameters == nullptr) return INVALID_OPERATION;
    return mStream->set_playback_rate_parameters(mStream, &playbackRate);
}

status_t StreamOutHalLocal::setEventCallback(
        const sp<StreamOutHalInterfaceEventCallback>& callback) {
    if (mStream->set_event_callback == nullptr) {
        return INVALID_OPERATION;
    }
    stream_event_callback_t asyncCb = nullptr;
    if (callback != nullptr) {
        asyncCb = [](stream_event_callback_type_t event, void *param, void *cookie) -> int {
            wp<StreamOutHalLocal> weakSelf(static_cast<StreamOutHalLocal*>(cookie));
            sp<StreamOutHalLocal> self = weakSelf.promote();
            if (self == nullptr) return 0;
            sp<StreamOutHalInterfaceEventCallback> cb = self->mEventCallback.promote();
            if (cb == nullptr) return 0;
            switch (event) {
                case STREAM_EVENT_CBK_TYPE_CODEC_FORMAT_CHANGED: {
                    std::vector<uint8_t> metadataBs(
                            (const uint8_t*)param,
                            (const uint8_t*)param +
                                    audio_utils::metadata::dataByteStringLen(
                                            (const uint8_t*)param));
                    cb->onCodecFormatChanged(metadataBs);
                    break;
                }
                default:
                    ALOGW("%s unknown event %d", __func__, event);
                    break;
            }
            return 0;
        };
    }
    status_t result = mStream->set_event_callback(mStream, asyncCb, this);
    if (result == OK) {
        mEventCallback = callback;
    }
    return result;
}

status_t StreamOutHalLocal::setLatencyMode(audio_latency_mode_t mode __unused) {
    return INVALID_OPERATION;
}

status_t StreamOutHalLocal::getRecommendedLatencyModes(
        std::vector<audio_latency_mode_t> *modes __unused) {
    return INVALID_OPERATION;
}

status_t StreamOutHalLocal::setLatencyModeCallback(
        const sp<StreamOutHalInterfaceLatencyModeCallback>& callback __unused) {
    return INVALID_OPERATION;
}

status_t StreamOutHalLocal::exit() {
    return OK;
}

StreamInHalLocal::StreamInHalLocal(audio_stream_in_t *stream, sp<DeviceHalLocal> device)
        : StreamHalLocal(&stream->common, device), mStream(stream) {
}

StreamInHalLocal::~StreamInHalLocal() {
    mDevice->closeInputStream(mStream);
    mStream = nullptr;
}

status_t StreamInHalLocal::getFrameSize(size_t *size) {
    *size = audio_stream_in_frame_size(mStream);
    return OK;
}

status_t StreamInHalLocal::setGain(float gain) {
    return mStream->set_gain(mStream, gain);
}

status_t StreamInHalLocal::read(void *buffer, size_t bytes, size_t *read) {
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

status_t StreamInHalLocal::getInputFramesLost(uint32_t *framesLost) {
    *framesLost = mStream->get_input_frames_lost(mStream);
    return OK;
}

status_t StreamInHalLocal::getCapturePosition(int64_t *frames, int64_t *time) {
    if (mStream->get_capture_position == NULL) return INVALID_OPERATION;
    return mStream->get_capture_position(mStream, frames, time);
}

status_t StreamInHalLocal::updateSinkMetadata(const SinkMetadata& sinkMetadata) {
    if (mStream->update_sink_metadata_v7 != nullptr) {
        const sink_metadata_v7_t halMetadata {
            .track_count = sinkMetadata.tracks.size(),
            .tracks = const_cast<record_track_metadata_v7*>(sinkMetadata.tracks.data()),
        };
        mStream->update_sink_metadata_v7(mStream, &halMetadata);
        return OK;
    }
    if (mStream->update_sink_metadata != nullptr) {
        std::vector<record_track_metadata> halTracks;
        halTracks.reserve(sinkMetadata.tracks.size());
        for (auto& metadata : sinkMetadata.tracks) {
            record_track_metadata halTrackMetadata;
            record_track_metadata_from_v7(&halTrackMetadata, &metadata);
            halTracks.push_back(halTrackMetadata);
        }
        const sink_metadata_t halMetadata = {
            .track_count = halTracks.size(),
            .tracks = halTracks.data(),
        };
        mStream->update_sink_metadata(mStream, &halMetadata);
        return OK;
    }
    return INVALID_OPERATION;
}

status_t StreamInHalLocal::start() {
    if (mStream->start == NULL) return INVALID_OPERATION;
    return mStream->start(mStream);
}

status_t StreamInHalLocal::stop() {
    if (mStream->stop == NULL) return INVALID_OPERATION;
    return mStream->stop(mStream);
}

status_t StreamInHalLocal::createMmapBuffer(int32_t minSizeFrames,
                                  struct audio_mmap_buffer_info *info) {
    if (mStream->create_mmap_buffer == NULL) return INVALID_OPERATION;
    return mStream->create_mmap_buffer(mStream, minSizeFrames, info);
}

status_t StreamInHalLocal::getMmapPosition(struct audio_mmap_position *position) {
    if (mStream->get_mmap_position == NULL) return INVALID_OPERATION;
    return mStream->get_mmap_position(mStream, position);
}

status_t StreamInHalLocal::getActiveMicrophones(
        std::vector<media::MicrophoneInfoFw> *microphones __unused) {
    // Legacy local HAL does not directly support MicrophoneInfoFw.
    return INVALID_OPERATION;
}

status_t StreamInHalLocal::setPreferredMicrophoneDirection(
        audio_microphone_direction_t direction) {
    if (mStream->set_microphone_direction == NULL) return INVALID_OPERATION;
    return mStream->set_microphone_direction(mStream, direction);
}

status_t StreamInHalLocal::setPreferredMicrophoneFieldDimension(float zoom) {
    if (mStream->set_microphone_field_dimension == NULL) return INVALID_OPERATION;
    return mStream->set_microphone_field_dimension(mStream, zoom);
}

} // namespace android
