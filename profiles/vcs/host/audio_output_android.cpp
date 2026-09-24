#include "audio_output.hpp"
#include "audio_resampler.hpp"
#include "vcs_config.hpp"

#include <aaudio/AAudio.h>
#include <android/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

// Android sink for the sceAudio HLE. The guest-side half - nine PSP channels
// resampled and mixed onto the guest's virtual-time line, sealed once no later
// submission can still land in them - is the same design as audio_output.cpp's
// waveOut sink, and deliberately so: that timeline logic is what keeps
// simultaneous channels aligned. Only the device half differs. waveOut is
// pushed blocks from the emulator thread; AAudio pulls from its own realtime
// callback, so sealed frames go through a single-producer/single-consumer FIFO
// that the callback reads without ever taking the mixing mutex.

namespace vcs {
namespace {

constexpr char kLogTag[] = "VCSAudio";
constexpr std::uint32_t kSampleRate = StreamingLinearResampler::kOutputRate;
constexpr std::size_t kOutputChannels = 2u;
// Same meaning as in audio_output.cpp: the newest ~23 ms of guest time stay
// open for other PSP channels to mix into before they are handed to the device.
constexpr std::uint64_t kMixSafetyFrames = 1024u;
constexpr std::size_t kRingFrames = kSampleRate * 2u;
constexpr std::size_t kGuestChannels = 9u;
constexpr std::uint64_t kChannelDiscontinuityFrames = 64u;
// Frames moved from the mix ring to the FIFO at a time.
constexpr std::size_t kSealChunkFrames = 256u;
// ~370 ms of device-side buffering. Power of two so the indices can wrap freely.
constexpr std::size_t kFifoFrames = 16384u;
// Start (and restart after an underrun) only with ~46 ms queued. Playing each
// sealed chunk the moment it arrives turns a slow frame into a train of clicks;
// one clean gap and a fresh reserve sounds far better.
constexpr std::uint64_t kPrebufferFrames = 2048u;

struct ChannelStream {
    StreamingLinearResampler resampler;
    std::uint64_t cursor{};
    std::uint32_t source_rate{kSampleRate};
    bool stereo{true};
    bool active{};
};

struct AudioState {
    // Guest side: everything below is owned by whoever holds `mutex`.
    std::mutex mutex;
    AAudioStream *stream{};
    std::vector<std::int32_t> ring;
    std::uint64_t output_frame{};   // first mix-ring frame not yet sealed into the FIFO
    std::uint64_t guest_anchor_us{};
    bool timeline_anchored{};
    std::array<ChannelStream, kGuestChannels> channels{};
    bool opened{};
    bool failed{};
    std::uint64_t late_frames_dropped{};
    std::uint64_t overrun_frames_dropped{};

    // Device side. `fifo` is written only by the producer (under `mutex`) and
    // read only by the AAudio callback; the two indices are the whole protocol.
    std::vector<std::int16_t> fifo;
    std::atomic<std::uint64_t> fifo_write{};
    std::atomic<std::uint64_t> fifo_read{};
    std::atomic<bool> primed{};
    std::atomic<bool> disconnected{};
    std::atomic<std::uint64_t> underruns{};
    std::chrono::steady_clock::time_point last_diagnostic_time{};
    std::uint64_t last_diagnostic_guest_us{};
    std::uint64_t last_diagnostic_underruns{};
};

AudioState &audio_state() {
    static AudioState state;
    return state;
}

aaudio_data_callback_result_t data_callback(AAudioStream *, void *user, void *audio,
                                            std::int32_t num_frames) {
    AudioState &state = *static_cast<AudioState *>(user);
    auto *out = static_cast<std::int16_t *>(audio);
    const std::size_t wanted = num_frames > 0 ? static_cast<std::size_t>(num_frames) : 0u;
    const std::uint64_t read = state.fifo_read.load(std::memory_order_relaxed);
    const std::uint64_t available = state.fifo_write.load(std::memory_order_acquire) - read;

    if (!state.primed.load(std::memory_order_relaxed)) {
        if (available < kPrebufferFrames) {
            std::memset(out, 0, wanted * kOutputChannels * sizeof(std::int16_t));
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        state.primed.store(true, std::memory_order_relaxed);
    }

    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(available, wanted));
    for (std::size_t frame = 0u; frame < count; ++frame) {
        const std::size_t slot = static_cast<std::size_t>((read + frame) % kFifoFrames) * kOutputChannels;
        out[frame * kOutputChannels] = state.fifo[slot];
        out[frame * kOutputChannels + 1u] = state.fifo[slot + 1u];
    }
    if (count < wanted) {
        std::memset(out + count * kOutputChannels, 0,
                    (wanted - count) * kOutputChannels * sizeof(std::int16_t));
        // Ran dry: go quiet and rebuild the reserve rather than play scraps.
        state.primed.store(false, std::memory_order_relaxed);
        state.underruns.fetch_add(1u, std::memory_order_relaxed);
    }
    state.fifo_read.store(read + count, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void error_callback(AAudioStream *, void *user, aaudio_result_t error) {
    // Headphones pulled, Bluetooth switched, route changed. The stream is dead
    // and must not be closed from inside its own callback, so the next
    // submission reopens it on the emulator thread.
    auto &state = *static_cast<AudioState *>(user);
    state.disconnected.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_WARN, kLogTag, "stream error %s; will reopen",
                        AAudio_convertResultToText(error));
}

void close_stream_locked(AudioState &state) {
    if (state.stream != nullptr) {
        (void)AAudioStream_requestStop(state.stream);
        (void)AAudioStream_close(state.stream);
        state.stream = nullptr;
    }
    state.opened = false;
}

bool open_stream_locked(AudioState &state) {
    AAudioStreamBuilder *builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "createStreamBuilder: %s",
                            AAudio_convertResultToText(result));
        return false;
    }
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, static_cast<std::int32_t>(kOutputChannels));
    AAudioStreamBuilder_setSampleRate(builder, static_cast<std::int32_t>(kSampleRate));
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, data_callback, &state);
    AAudioStreamBuilder_setErrorCallback(builder, error_callback, &state);
    result = AAudioStreamBuilder_openStream(builder, &state.stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) {
        state.stream = nullptr;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "openStream: %s",
                            AAudio_convertResultToText(result));
        return false;
    }
    state.primed.store(false, std::memory_order_relaxed);
    state.disconnected.store(false, std::memory_order_release);
    result = AAudioStream_requestStart(state.stream);
    if (result != AAUDIO_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "requestStart: %s",
                            AAudio_convertResultToText(result));
        close_stream_locked(state);
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "AAudio open: %d Hz stereo, burst=%d frames, capacity=%d frames",
        AAudioStream_getSampleRate(state.stream),
        AAudioStream_getFramesPerBurst(state.stream),
        AAudioStream_getBufferCapacityInFrames(state.stream));
    state.opened = true;
    return true;
}

bool ensure_device(AudioState &state) {
    if (state.opened && state.disconnected.load(std::memory_order_acquire)) {
        close_stream_locked(state);
        // Whatever was queued belonged to the old route; start clean.
        state.fifo_read.store(state.fifo_write.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
    }
    if (state.opened) return true;
    if (state.failed) return false;
    if (state.ring.empty()) {
        state.ring.assign(kRingFrames * kOutputChannels, 0);
        state.fifo.assign(kFifoFrames * kOutputChannels, 0);
        state.output_frame = 0u;
    }
    if (!open_stream_locked(state)) {
        state.failed = true;
        return false;
    }
    return true;
}

std::uint64_t guest_frame_for(const AudioState &state, std::uint64_t guest_time_us) {
    if (!state.timeline_anchored || guest_time_us <= state.guest_anchor_us) return 0u;
    const std::uint64_t delta = guest_time_us - state.guest_anchor_us;
    return (delta * kSampleRate + 500000u) / 1000000u;
}

// Move one chunk of finished mix from the ring into the FIFO. Returns false
// when the device side has no room, which is the backpressure that stops the
// sealer from running ahead of the speaker.
bool seal_one_chunk(AudioState &state) {
    const std::uint64_t write = state.fifo_write.load(std::memory_order_relaxed);
    const std::uint64_t read = state.fifo_read.load(std::memory_order_acquire);
    if (kFifoFrames - (write - read) < kSealChunkFrames) return false;
    for (std::size_t frame = 0u; frame < kSealChunkFrames; ++frame) {
        const std::size_t ring_slot =
            static_cast<std::size_t>((state.output_frame + frame) % kRingFrames) * kOutputChannels;
        const std::size_t fifo_slot =
            static_cast<std::size_t>((write + frame) % kFifoFrames) * kOutputChannels;
        for (std::size_t channel = 0u; channel < kOutputChannels; ++channel) {
            state.fifo[fifo_slot + channel] = static_cast<std::int16_t>(
                std::clamp(state.ring[ring_slot + channel], -32768, 32767));
            state.ring[ring_slot + channel] = 0;
        }
    }
    state.output_frame += kSealChunkFrames;
    state.fifo_write.store(write + kSealChunkFrames, std::memory_order_release);
    return true;
}

void advance_locked(AudioState &state, std::uint64_t guest_time_us) {
    if (!state.timeline_anchored || !state.opened) return;
    const std::uint64_t guest_frame = guest_frame_for(state, guest_time_us);
    // Same emergency rule as the waveOut sink: when the device is nearly dry,
    // sealing what is already mixed beats waiting for late channels.
    const std::uint64_t queued = state.fifo_write.load(std::memory_order_relaxed) -
        state.fifo_read.load(std::memory_order_acquire);
    const std::uint64_t safety = queued < kSealChunkFrames * 2u ? 0u : kMixSafetyFrames;
    const std::uint64_t sealed_frame = guest_frame > safety ? guest_frame - safety : 0u;
    while (sealed_frame >= state.output_frame + kSealChunkFrames) {
        if (!seal_one_chunk(state)) break;
    }
}

} // namespace

bool audio_output_enabled() {
    static const bool enabled = [] {
        if (const char *text = std::getenv("PSPRECOMP_AUDIO"))
            return *text != '\0' && std::string(text) != "0";
        const VcsConfiguration &configuration = vcs_configuration();
        return !configuration.initialized || configuration.audio.enabled;
    }();
    return enabled;
}

void audio_output_submit(std::span<const std::int16_t> pcm, std::uint32_t frames,
                         bool stereo, std::uint32_t left, std::uint32_t right,
                         std::uint32_t source_rate, std::uint32_t channel,
                         std::uint64_t guest_time_us) {
    if (!audio_output_enabled() || frames == 0u || channel >= kGuestChannels) return;
    if (source_rate == 0u) source_rate = kSampleRate;
    const std::size_t needed = static_cast<std::size_t>(frames) * (stereo ? 2u : 1u);
    if (pcm.size() < needed) return;

    AudioState &state = audio_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    if (!ensure_device(state)) return;

    if (!state.timeline_anchored) {
        state.guest_anchor_us = guest_time_us;
        state.timeline_anchored = true;
        state.output_frame = 0u;
    }
    advance_locked(state, guest_time_us);

    ChannelStream &stream = state.channels[channel];
    const std::uint64_t scheduled = guest_frame_for(state, guest_time_us);
    const auto distance = [](std::uint64_t a, std::uint64_t b) { return a > b ? a - b : b - a; };
    const bool format_changed = stream.active &&
        (stream.source_rate != source_rate || stream.stereo != stereo);
    const bool discontinuity = stream.active &&
        distance(stream.cursor, scheduled) > kChannelDiscontinuityFrames;
    if (!stream.active || format_changed || discontinuity) {
        stream = ChannelStream{};
        stream.active = true;
        stream.source_rate = source_rate;
        stream.stereo = stereo;
        stream.resampler.reset(source_rate, stereo);
        stream.cursor = std::max(scheduled, state.output_frame);
    }
    if (stream.cursor < state.output_frame) {
        state.late_frames_dropped += state.output_frame - stream.cursor;
        stream.cursor = state.output_frame;
        stream.resampler.reset(source_rate, stereo);
    }

    const std::uint32_t master = vcs_configuration().audio.volume;
    const std::int64_t left_gain = (static_cast<std::int64_t>(left) * master) / 100;
    const std::int64_t right_gain = (static_cast<std::int64_t>(right) * master) / 100;
    const std::uint64_t ring_limit = state.output_frame + kRingFrames - kSealChunkFrames;

    stream.resampler.process(pcm, frames, stereo, source_rate,
        [&](std::int16_t source_left, std::int16_t source_right) {
            if (stream.cursor >= ring_limit) {
                ++state.overrun_frames_dropped;
                ++stream.cursor;
                return;
            }
            const std::size_t slot =
                static_cast<std::size_t>(stream.cursor % kRingFrames) * kOutputChannels;
            const std::int64_t mixed_left = (static_cast<std::int64_t>(source_left) * left_gain) >> 15;
            const std::int64_t mixed_right = (static_cast<std::int64_t>(source_right) * right_gain) >> 15;
            state.ring[slot] += static_cast<std::int32_t>(std::clamp<std::int64_t>(mixed_left,
                std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
            state.ring[slot + 1u] += static_cast<std::int32_t>(std::clamp<std::int64_t>(mixed_right,
                std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
            ++stream.cursor;
        });

    advance_locked(state, guest_time_us);
}

void audio_output_advance(std::uint64_t guest_time_us) {
    if (!audio_output_enabled()) return;
    AudioState &state = audio_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    if (!state.opened) return;
    advance_locked(state, guest_time_us);
    if (!vcs_configuration().audio.diagnostics) return;
    const auto now = std::chrono::steady_clock::now();
    if (state.last_diagnostic_time == std::chrono::steady_clock::time_point{}) {
        state.last_diagnostic_time = now;
        state.last_diagnostic_guest_us = guest_time_us;
        state.last_diagnostic_underruns = state.underruns.load(std::memory_order_relaxed);
        return;
    }
    const auto host_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.last_diagnostic_time).count();
    if (host_ms < 2000) return;
    const std::uint64_t write = state.fifo_write.load(std::memory_order_acquire);
    const std::uint64_t read = state.fifo_read.load(std::memory_order_acquire);
    const std::uint64_t underruns = state.underruns.load(std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "health host_ms=%lld guest_ms=%llu fifo_frames=%llu produced_frames=%llu consumed_frames=%llu underruns=%llu new_underruns=%llu late_frames=%llu overrun_frames=%llu primed=%d",
        static_cast<long long>(host_ms),
        static_cast<unsigned long long>((guest_time_us - state.last_diagnostic_guest_us) / 1000u),
        static_cast<unsigned long long>(write - read),
        static_cast<unsigned long long>(write),
        static_cast<unsigned long long>(read),
        static_cast<unsigned long long>(underruns),
        static_cast<unsigned long long>(underruns - state.last_diagnostic_underruns),
        static_cast<unsigned long long>(state.late_frames_dropped),
        static_cast<unsigned long long>(state.overrun_frames_dropped),
        state.primed.load(std::memory_order_relaxed) ? 1 : 0);
    state.last_diagnostic_time = now;
    state.last_diagnostic_guest_us = guest_time_us;
    state.last_diagnostic_underruns = underruns;
}

void audio_output_reset_channel(std::uint32_t channel) {
    AudioState &state = audio_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    if (channel < state.channels.size()) state.channels[channel] = ChannelStream{};
}

void audio_output_shutdown() {
    AudioState &state = audio_state();
    std::lock_guard<std::mutex> guard(state.mutex);
    if (state.opened) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
            "shutdown underruns=%llu late_frames=%llu overrun_frames=%llu",
            static_cast<unsigned long long>(state.underruns.load()),
            static_cast<unsigned long long>(state.late_frames_dropped),
            static_cast<unsigned long long>(state.overrun_frames_dropped));
    }
    close_stream_locked(state);
    state.ring.clear();
    state.fifo.clear();
    state.fifo_write.store(0u);
    state.fifo_read.store(0u);
    state.timeline_anchored = false;
    state.output_frame = 0u;
    state.failed = false;
    state.late_frames_dropped = 0u;
    state.overrun_frames_dropped = 0u;
    state.underruns.store(0u);
    state.last_diagnostic_time = {};
    state.last_diagnostic_guest_us = 0u;
    state.last_diagnostic_underruns = 0u;
    for (ChannelStream &stream : state.channels) stream = ChannelStream{};
}

} // namespace vcs
