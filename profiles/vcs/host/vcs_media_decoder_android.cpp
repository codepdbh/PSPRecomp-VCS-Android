#include "vcs_media_decoder.hpp"

namespace vcs {
struct AudioStreamDecoder::State {};
AudioStreamDecoder::AudioStreamDecoder() : state_(std::make_unique<State>()) {}
AudioStreamDecoder::~AudioStreamDecoder() = default;
AudioStreamDecoder::AudioStreamDecoder(AudioStreamDecoder &&) noexcept = default;
AudioStreamDecoder &AudioStreamDecoder::operator=(AudioStreamDecoder &&) noexcept = default;
bool AudioStreamDecoder::open(const std::filesystem::path &, std::uint32_t,
                              std::uint32_t, std::uint64_t) { return false; }
std::size_t AudioStreamDecoder::read(std::span<std::uint8_t>) { return 0u; }
bool AudioStreamDecoder::is_open() const noexcept { return false; }
void AudioStreamDecoder::close() noexcept {}

struct PmfAudioDecoder::State {};
PmfAudioDecoder::PmfAudioDecoder() : state_(std::make_unique<State>()) {}
PmfAudioDecoder::~PmfAudioDecoder() = default;
PmfAudioDecoder::PmfAudioDecoder(PmfAudioDecoder &&) noexcept = default;
PmfAudioDecoder &PmfAudioDecoder::operator=(PmfAudioDecoder &&) noexcept = default;
bool PmfAudioDecoder::open(const std::filesystem::path &) { return false; }
std::size_t PmfAudioDecoder::read(std::span<std::uint8_t>) { return 0u; }
bool PmfAudioDecoder::is_open() const noexcept { return false; }
void PmfAudioDecoder::close() noexcept {}

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
