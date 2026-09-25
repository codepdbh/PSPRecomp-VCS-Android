#include "vcs_media_decoder.hpp"

#include "at3_decoders.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include <android/log.h>

// Android media decoding for the PSP media HLE, without FFmpeg.
//
// The desktop build decodes through a minimal FFmpeg (vcs_media_decoder.cpp).
// There is no FFmpeg on Android, and until this file these were stubs that
// never opened anything - so every streamed sound was silent: the radio, and
// the cutscene voices and music, which is also what a cutscene waiting on its
// own audio stream hung on. ATRAC3 and ATRAC3+ are decoded here with PPSSPP's
// standalone extraction of FFmpeg's decoders (third_party/at3_standalone,
// LGPL 2.1+), behind the same interface and the same output contract:
// interleaved signed 16-bit PCM at the caller's rate.
//
// Video (H.264 inside PMF) is not decoded yet; VideoStreamDecoder stays a stub
// and the movie HLE ends such movies cleanly (see sceMpegAvcDecode).

namespace vcs {
namespace {

constexpr int kMaxSamplesPerFrame = 2048;  // ATRAC3+; ATRAC3 is 1024

std::uint16_t le16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::vector<std::uint8_t> read_file(const std::filesystem::path &path) {
    std::vector<std::uint8_t> bytes;
    std::FILE *file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) return bytes;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0) {
        bytes.resize(static_cast<std::size_t>(size));
        if (std::fread(bytes.data(), 1u, bytes.size(), file) != bytes.size()) bytes.clear();
    }
    std::fclose(file);
    return bytes;
}

// One ATRAC3 or ATRAC3+ decoder, whichever the stream needs.
struct AtracCodec {
    ATRAC3Context *at3{};
    ATRAC3PContext *at3p{};
    int channels{2};
    int block_align{};
    std::array<std::vector<float>, 2> planes{std::vector<float>(kMaxSamplesPerFrame),
                                             std::vector<float>(kMaxSamplesPerFrame)};

    ~AtracCodec() { release(); }
    void release() noexcept {
        if (at3 != nullptr) atrac3_free(at3);
        if (at3p != nullptr) atrac3p_free(at3p);
        at3 = nullptr;
        at3p = nullptr;
    }
    [[nodiscard]] bool open() const noexcept { return at3 != nullptr || at3p != nullptr; }
    void flush() noexcept {
        if (at3 != nullptr) atrac3_flush_buffers(at3);
        if (at3p != nullptr) atrac3p_flush_buffers(at3p);
    }
    [[nodiscard]] int samples_per_frame() const noexcept { return at3p != nullptr ? 2048 : 1024; }

    // Decodes one block_align-sized frame; returns the sample count, 0 on error.
    int decode(const std::uint8_t *frame, int size) {
        float *out[2]{planes[0].data(), planes[1].data()};
        int samples = 0;
        const int result = at3p != nullptr
            ? atrac3p_decode_frame(at3p, out, &samples, frame, size)
            : atrac3_decode_frame(at3, out, &samples, frame, size);
        return result < 0 ? 0 : std::clamp(samples, 0, kMaxSamplesPerFrame);
    }

    // Appends `samples` decoded frames as interleaved s16 with `out_channels`.
    void append_pcm(std::vector<std::uint8_t> &pcm, int samples, std::size_t skip,
                    std::uint32_t out_channels) const {
        const float *left = planes[0].data();
        const float *right = channels > 1 ? planes[1].data() : left;
        const auto to_s16 = [](float value) {
            return static_cast<std::int16_t>(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
        };
        for (int i = static_cast<int>(skip); i < samples; ++i) {
            const std::int16_t l = to_s16(left[i]), r = to_s16(right[i]);
            if (out_channels == 1u) {
                const std::int16_t m = static_cast<std::int16_t>((static_cast<int>(l) + r) / 2);
                pcm.insert(pcm.end(), reinterpret_cast<const std::uint8_t *>(&m),
                           reinterpret_cast<const std::uint8_t *>(&m) + 2);
            } else {
                const std::int16_t pair[2]{l, r};
                const auto *bytes = reinterpret_cast<const std::uint8_t *>(pair);
                pcm.insert(pcm.end(), bytes, bytes + sizeof(pair));
            }
        }
    }
};

} // namespace

// --- .AT3 files (radio, cutscene voices and music) ---------------------------------------

struct AudioStreamDecoder::State {
    AtracCodec codec;
    std::vector<std::uint8_t> file;
    std::size_t data_begin{}, data_end{}, cursor{};
    std::uint32_t source_rate{44100}, sample_rate{44100}, channels{2};
    std::size_t skip_samples{};           // leading samples to drop after a seek
    std::vector<std::uint8_t> pending;    // decoded, not yet read
    std::size_t pending_read{};
    // Linear resampler state, used only when the file's rate differs from the request.
    double resample_position{};
};

AudioStreamDecoder::AudioStreamDecoder() : state_(std::make_unique<State>()) {}
AudioStreamDecoder::~AudioStreamDecoder() = default;
AudioStreamDecoder::AudioStreamDecoder(AudioStreamDecoder &&) noexcept = default;
AudioStreamDecoder &AudioStreamDecoder::operator=(AudioStreamDecoder &&) noexcept = default;

bool AudioStreamDecoder::is_open() const noexcept { return state_ && state_->codec.open(); }

void AudioStreamDecoder::close() noexcept {
    if (!state_) return;
    state_->codec.release();
    state_->file.clear();
    state_->file.shrink_to_fit();
    state_->pending.clear();
    state_->pending_read = 0u;
}

bool AudioStreamDecoder::open(const std::filesystem::path &path, std::uint32_t sample_rate,
                              std::uint32_t channels, std::uint64_t start_sample) {
    close();
    if (sample_rate == 0u || channels == 0u) return false;
    State &s = *state_;
    s.file = read_file(path);
    const std::vector<std::uint8_t> &f = s.file;
    if (f.size() < 12u || std::memcmp(f.data(), "RIFF", 4) != 0 || std::memcmp(f.data() + 8, "WAVE", 4) != 0)
        return false;

    // Walk the RIFF chunks for "fmt " and "data".
    std::uint16_t format_tag = 0u, file_channels = 0u, block_align = 0u;
    std::uint32_t file_rate = 0u;
    const std::uint8_t *extra = nullptr;
    int extra_size = 0;
    bool atrac3plus = false;
    for (std::size_t offset = 12u; offset + 8u <= f.size();) {
        const std::uint32_t size = le32(f.data() + offset + 4u);
        const std::size_t body = offset + 8u;
        if (body + size > f.size() && std::memcmp(f.data() + offset, "data", 4) != 0) break;
        if (std::memcmp(f.data() + offset, "fmt ", 4) == 0 && size >= 16u) {
            format_tag = le16(f.data() + body);
            file_channels = le16(f.data() + body + 2u);
            file_rate = le32(f.data() + body + 4u);
            block_align = le16(f.data() + body + 12u);
            const std::uint16_t cb = size >= 18u ? le16(f.data() + body + 16u) : 0u;
            if (cb > 0u && body + 18u + cb <= f.size()) {
                extra = f.data() + body + 18u;
                extra_size = cb;
            }
            // WAVE_FORMAT_EXTENSIBLE carrying the ATRAC3+ sub-format GUID
            // (E923AABF-CB58-4471-A119-FFFA01E4CE62); plain 0x0270 is ATRAC3.
            static constexpr std::uint8_t kAtrac3PlusGuid[16] = {
                0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
                0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};
            if (format_tag == 0xFFFEu && cb >= 22u &&
                std::memcmp(f.data() + body + 18u + 6u, kAtrac3PlusGuid, 16) == 0)
                atrac3plus = true;
        } else if (std::memcmp(f.data() + offset, "data", 4) == 0) {
            s.data_begin = body;
            s.data_end = std::min<std::size_t>(f.size(), body + size);
            break;
        }
        offset = body + size + (size & 1u);
    }
    static unsigned logged_opens = 0u;
    const bool log_open = logged_opens < 400u;
    if (log_open) ++logged_opens;
    if (s.data_begin == 0u || block_align == 0u || file_channels == 0u || file_rate == 0u ||
        (!atrac3plus && format_tag != 0x0270u)) {
        if (log_open)
            __android_log_print(ANDROID_LOG_WARN, "VCSAtrac", "unsupported stream %s tag=%04x ch=%u rate=%u align=%u",
                                path.filename().string().c_str(), format_tag, file_channels, file_rate, block_align);
        return false;
    }
    if (log_open)
        __android_log_print(ANDROID_LOG_INFO, "VCSAtrac", "open %s %s ch=%u rate=%u align=%u start=%llu",
                            path.filename().string().c_str(), atrac3plus ? "ATRAC3+" : "ATRAC3",
                            file_channels, file_rate, block_align,
                            static_cast<unsigned long long>(start_sample));

    s.codec.channels = std::min<int>(file_channels, 2);
    int align = block_align;
    if (atrac3plus) s.codec.at3p = atrac3p_alloc(s.codec.channels, &align);
    else s.codec.at3 = atrac3_alloc(s.codec.channels, &align, extra, extra_size);
    if (!s.codec.open()) return false;
    s.codec.block_align = align;
    s.source_rate = file_rate;
    s.sample_rate = sample_rate;
    s.channels = std::min<std::uint32_t>(channels, 2u);
    s.resample_position = 0.0;

    // Seek: whole frames by position, the remainder dropped from the first decode.
    const std::uint64_t per_frame = static_cast<std::uint64_t>(s.codec.samples_per_frame());
    const std::uint64_t frame_index = start_sample / per_frame;
    s.cursor = s.data_begin + static_cast<std::size_t>(frame_index) * static_cast<std::size_t>(align);
    if (s.cursor > s.data_end) s.cursor = s.data_end;
    s.skip_samples = static_cast<std::size_t>(start_sample % per_frame);
    s.codec.flush();
    return true;
}

std::size_t AudioStreamDecoder::read(std::span<std::uint8_t> output) {
    if (!is_open()) return 0u;
    State &s = *state_;
    std::size_t written = 0u;
    std::vector<std::uint8_t> decoded;
    while (written < output.size()) {
        if (s.pending_read >= s.pending.size()) {
            s.pending.clear();
            s.pending_read = 0u;
            const std::size_t align = static_cast<std::size_t>(s.codec.block_align);
            if (s.cursor + align > s.data_end) break;  // end of stream
            const int samples = s.codec.decode(s.file.data() + s.cursor, static_cast<int>(align));
            s.cursor += align;
            if (samples == 0) continue;  // a bad frame: skip it rather than stop the stream
            const std::size_t skip = std::min<std::size_t>(s.skip_samples, static_cast<std::size_t>(samples));
            s.skip_samples -= skip;
            if (s.source_rate == s.sample_rate) {
                s.codec.append_pcm(s.pending, samples, skip, s.channels);
            } else {
                // Rare (the game's streams are 44.1 kHz); linear interpolation keeps
                // pitch and duration right without pulling in a resampler library.
                decoded.clear();
                s.codec.append_pcm(decoded, samples, skip, s.channels);
                const std::size_t frame_bytes = s.channels * sizeof(std::int16_t);
                const std::size_t in_frames = decoded.size() / frame_bytes;
                const double step = static_cast<double>(s.source_rate) / s.sample_rate;
                const auto *in = reinterpret_cast<const std::int16_t *>(decoded.data());
                while (s.resample_position + 1.0 < static_cast<double>(in_frames)) {
                    const std::size_t i = static_cast<std::size_t>(s.resample_position);
                    const double t = s.resample_position - static_cast<double>(i);
                    for (std::uint32_t c = 0; c < s.channels; ++c) {
                        const double a = in[i * s.channels + c], b = in[(i + 1) * s.channels + c];
                        const auto v = static_cast<std::int16_t>(a + (b - a) * t);
                        s.pending.insert(s.pending.end(), reinterpret_cast<const std::uint8_t *>(&v),
                                         reinterpret_cast<const std::uint8_t *>(&v) + 2);
                    }
                    s.resample_position += step;
                }
                s.resample_position -= static_cast<double>(in_frames > 0 ? in_frames - 1 : 0);
            }
            if (s.pending.empty()) continue;
        }
        const std::size_t take = std::min(s.pending.size() - s.pending_read, output.size() - written);
        std::memcpy(output.data() + written, s.pending.data() + s.pending_read, take);
        s.pending_read += take;
        written += take;
    }
    return written;
}

// --- Movie soundtracks (ATRAC3+ in a PMF's private stream) --------------------------------

namespace {

// Every 0xBD PES payload, minus its four-byte PSP substream header. Same walk as
// the desktop decoder: no generic demuxer surfaces this stream.
std::vector<std::uint8_t> extract_pmf_private_stream(const std::vector<std::uint8_t> &file) {
    std::vector<std::uint8_t> elementary;
    for (std::size_t i = 0u; i + 9u < file.size();) {
        if (!(file[i] == 0x00u && file[i + 1u] == 0x00u && file[i + 2u] == 0x01u && file[i + 3u] == 0xBDu)) {
            ++i;
            continue;
        }
        const std::size_t packet_length = static_cast<std::size_t>(file[i + 4u]) * 256u + file[i + 5u];
        const std::size_t header_data_length = file[i + 8u];
        const std::size_t payload = i + 9u + header_data_length;
        if (packet_length < 3u + header_data_length) { ++i; continue; }
        const std::size_t payload_size = packet_length - 3u - header_data_length;
        constexpr std::size_t kSubstreamHeader = 4u;
        if (payload + payload_size > file.size() || payload_size <= kSubstreamHeader) { ++i; continue; }
        elementary.insert(elementary.end(),
                          file.begin() + static_cast<std::ptrdiff_t>(payload + kSubstreamHeader),
                          file.begin() + static_cast<std::ptrdiff_t>(payload + payload_size));
        i = payload + payload_size;
    }
    return elementary;
}

// Each PSP movie audio frame is an 8-byte header (starting with the 0x0FD0
// sync) plus the ATRAC3+ payload; the frame size is the distance between syncs.
constexpr std::size_t kPmfFrameHeader = 8u;

std::size_t measure_atrac3p_frame_size(const std::vector<std::uint8_t> &stream, std::size_t &first) {
    const auto sync_at = [&](std::size_t index) {
        return index + 1u < stream.size() && stream[index] == 0x0Fu && stream[index + 1u] == 0xD0u;
    };
    first = stream.size();
    for (std::size_t i = 0u; i + 1u < stream.size(); ++i)
        if (sync_at(i)) { first = i; break; }
    if (first == stream.size()) return 0u;
    for (std::size_t i = first + 2u; i + 1u < stream.size(); ++i)
        if (sync_at(i)) return i - first;
    return 0u;
}

} // namespace

struct PmfAudioDecoder::State {
    bool open{};
    std::vector<std::uint8_t> pcm;  // whole soundtrack, decoded at open
    std::size_t pcm_read{};
};

PmfAudioDecoder::PmfAudioDecoder() : state_(std::make_unique<State>()) {}
PmfAudioDecoder::~PmfAudioDecoder() = default;
PmfAudioDecoder::PmfAudioDecoder(PmfAudioDecoder &&) noexcept = default;
PmfAudioDecoder &PmfAudioDecoder::operator=(PmfAudioDecoder &&) noexcept = default;

bool PmfAudioDecoder::is_open() const noexcept { return state_ && state_->open; }

void PmfAudioDecoder::close() noexcept {
    if (!state_) return;
    state_->open = false;
    state_->pcm.clear();
    state_->pcm.shrink_to_fit();
    state_->pcm_read = 0u;
}

bool PmfAudioDecoder::open(const std::filesystem::path &path) {
    close();
    const std::vector<std::uint8_t> stream = extract_pmf_private_stream(read_file(path));
    std::size_t cursor = 0u;
    const std::size_t frame_size = measure_atrac3p_frame_size(stream, cursor);
    if (frame_size <= kPmfFrameHeader) return false;
    AtracCodec codec;
    int align = static_cast<int>(frame_size - kPmfFrameHeader);
    codec.channels = 2;
    codec.at3p = atrac3p_alloc(2, &align);
    if (!codec.open()) return false;
    codec.block_align = align;
    // Decoded whole at open, like the desktop version: a soundtrack is ~2 MB of
    // PCM and takes milliseconds, and it keeps decoding off the guest's timeline.
    State &s = *state_;
    s.pcm.reserve(stream.size() / frame_size * 2048u * 4u);
    while (cursor + frame_size <= stream.size()) {
        const int samples = codec.decode(stream.data() + cursor + kPmfFrameHeader, align);
        cursor += frame_size;
        if (samples > 0) codec.append_pcm(s.pcm, samples, 0u, 2u);
    }
    s.pcm_read = 0u;
    s.open = !s.pcm.empty();
    __android_log_print(ANDROID_LOG_INFO, "VCSAtrac", "movie audio %s frame=%zu pcm=%zu bytes",
                        path.filename().string().c_str(), frame_size, s.pcm.size());
    return s.open;
}

std::size_t PmfAudioDecoder::read(std::span<std::uint8_t> output) {
    if (!is_open()) return 0u;
    State &s = *state_;
    const std::size_t take = std::min(s.pcm.size() - s.pcm_read, output.size());
    if (take == 0u) return 0u;
    std::memcpy(output.data(), s.pcm.data() + s.pcm_read, take);
    s.pcm_read += take;
    return take;
}

// --- Video: not yet decoded on Android ------------------------------------------------------

struct VideoStreamDecoder::State {};
VideoStreamDecoder::VideoStreamDecoder() : state_(std::make_unique<State>()) {}
VideoStreamDecoder::~VideoStreamDecoder() = default;
VideoStreamDecoder::VideoStreamDecoder(VideoStreamDecoder &&) noexcept = default;
VideoStreamDecoder &VideoStreamDecoder::operator=(VideoStreamDecoder &&) noexcept = default;
bool VideoStreamDecoder::open(const std::filesystem::path &) { return false; }
std::size_t VideoStreamDecoder::read(std::span<std::uint8_t>) { return 0u; }
bool VideoStreamDecoder::is_open() const noexcept { return false; }
void VideoStreamDecoder::close() noexcept {}

} // namespace vcs
