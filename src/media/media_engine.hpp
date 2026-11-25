#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace astra {

struct VideoContainerInfo {
    std::string format;       // "MP4/ISOBMFF", "WebM/Matroska", "Y4M/YUV420", "WAV/PCM"
    std::string major_brand;  // e.g. "isom", "mp42"
    std::string video_codec;  // e.g. "avc1", "vp09", "av01", "yuv420p"
    std::string audio_codec;  // e.g. "mp4a", "Opus", "pcm_s16le"
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t timescale = 0;
    uint64_t duration_ticks = 0;
    double duration_sec = 0.0;
    double fps = 30.0;
    uint32_t sample_count = 0;
    bool has_audio = false;
    uint32_t audio_sample_rate = 0;
    uint16_t audio_channels = 0;
    bool valid = false;
    std::string error;
};

struct DecodedVideoFrame {
    int width = 0;
    int height = 0;
    uint32_t frame_index = 0;
    uint32_t total_frames = 0;
    double pts_sec = 0.0;
    double duration_sec = 0.0;
    bool has_audio = false;
    std::vector<uint32_t> argb;
    std::string codec_label;
};

struct DecodedAudioBuffer {
    uint32_t sample_rate = 44100;
    uint16_t channels = 2;
    double duration_sec = 0.0;
    double rms_level = 0.0;
    std::vector<int16_t> pcm_s16;
    bool valid = false;
    std::string error;
};

// Bounded-RAM continuous video playback session with A/V presentation clock
// (inspired by Firefox MediaDecoderStateMachine). Caps frame queue to 16 MiB.
class MediaPlaybackSession {
public:
    static constexpr size_t kMaxBufferBytes = 16ULL * 1024 * 1024;

    bool open(const std::string& path, std::string* error = nullptr);
    bool decode_batch(uint32_t start_frame, uint32_t count, std::string* error = nullptr);
    bool seek(double time_sec, DecodedVideoFrame& out_frame);
    bool next_frame(DecodedVideoFrame& out_frame);

    const VideoContainerInfo& info() const { return info_; }
    size_t buffered_frames() const { return ring_.size(); }
    size_t buffered_bytes() const { return buffered_bytes_; }
    double current_pts_sec() const { return current_pts_sec_; }

private:
    std::string path_;
    VideoContainerInfo info_;
    std::deque<DecodedVideoFrame> ring_;
    size_t buffered_bytes_ = 0;
    size_t cursor_ = 0;
    double current_pts_sec_ = 0.0;
};

struct AudioPlaybackReport {
    bool alsa_library_loaded = false;
    bool device_opened = false;
    bool played = false;
    size_t frames_written = 0;
    std::string status; // "played", "headless_no_soundcard", "alsa_not_installed"
};

struct HlsPlaylist {
    bool valid = false;
    bool is_master = false;
    double target_duration_sec = 0.0;
    std::vector<std::string> variant_urls;
    std::vector<std::string> segment_urls;
};

// AstraMedia v5: Firefox RDD-inspired isolated media container parser (MP4/WebM/Y4M/WAV/MPEG-TS/MP3/OGG/FLAC),
// HLS (.m3u8) Master & Media playlist parser, native BT.709 YUV420p -> ARGB32 converter,
// real multi-frame video decoder, bounded PCM audio decoder, waveform peak analyzer,
// and native Linux ALSA playback (dlopen libasound.so.2).
class MediaEngine {
public:
    static VideoContainerInfo probe_container(const std::string& path);

    static HlsPlaylist parse_hls_playlist(std::string_view m3u8_text, const std::string& base_url);

    static void yuv420p_to_argb(const uint8_t* y_plane,
                                const uint8_t* u_plane,
                                const uint8_t* v_plane,
                                int width, int height,
                                std::vector<uint32_t>& out_argb);

    static bool decode_frame(const std::string& path,
                             uint32_t target_frame_index,
                             DecodedVideoFrame& out_frame,
                             std::string* error = nullptr);

    static bool decode_frames(const std::string& path,
                              uint32_t start_frame,
                              uint32_t max_frames,
                              std::vector<DecodedVideoFrame>& out_frames,
                              std::string* error = nullptr);

    static bool decode_audio_pcm(const std::string& path,
                                 double max_seconds,
                                 DecodedAudioBuffer& out_audio);

    static std::vector<float> compute_waveform_peaks(const DecodedAudioBuffer& audio, size_t buckets = 48);
    static bool alsa_available();
    static AudioPlaybackReport play_audio_alsa(const DecodedAudioBuffer& audio);
};

} // namespace astra
