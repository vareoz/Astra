#include "media/media_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace astra {

namespace {

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}

uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void parse_mp4_boxes(const std::vector<uint8_t>& buf, size_t start, size_t end, VideoContainerInfo& info) {
    size_t pos = start;
    while (pos + 8 <= end) {
        uint32_t sz = read_be32(buf.data() + pos);
        std::string type(reinterpret_cast<const char*>(buf.data() + pos + 4), 4);
        size_t hdr = 8;
        if (sz == 1 && pos + 16 <= end) {
            uint64_t sz64 = (static_cast<uint64_t>(read_be32(buf.data() + pos + 8)) << 32) |
                            read_be32(buf.data() + pos + 12);
            sz = static_cast<uint32_t>(std::min<uint64_t>(sz64, end - pos));
            hdr = 16;
        } else if (sz == 0) {
            sz = static_cast<uint32_t>(end - pos);
        }
        if (sz < hdr || pos + sz > end) break;

        size_t body = pos + hdr;
        size_t body_end = pos + sz;

        if (type == "ftyp" && body + 4 <= body_end) {
            info.format = "MP4/ISOBMFF";
            info.major_brand.assign(reinterpret_cast<const char*>(buf.data() + body), 4);
            info.valid = true;
        } else if (type == "mvhd" && body + 20 <= body_end) {
            uint8_t version = buf[body];
            if (version == 0 && body + 20 <= body_end) {
                info.timescale = read_be32(buf.data() + body + 12);
                info.duration_ticks = read_be32(buf.data() + body + 16);
                if (info.timescale > 0) {
                    info.duration_sec = static_cast<double>(info.duration_ticks) / info.timescale;
                }
            }
        } else if (type == "tkhd" && body + 84 <= body_end) {
            uint8_t version = buf[body];
            size_t dim_off = (version == 1) ? (body + 88) : (body + 76);
            if (dim_off + 8 <= body_end) {
                uint32_t w = read_be32(buf.data() + dim_off) >> 16;
                uint32_t h = read_be32(buf.data() + dim_off + 4) >> 16;
                if (w > 0 && h > 0) {
                    info.width = w;
                    info.height = h;
                }
            }
        } else if (type == "stsd" && body + 16 <= body_end) {
            std::string fourcc(reinterpret_cast<const char*>(buf.data() + body + 12), 4);
            if (fourcc == "avc1" || fourcc == "hvc1" || fourcc == "vp09" || fourcc == "av01" || fourcc == "mp4v") {
                info.video_codec = fourcc;
                if (body + 44 <= body_end && info.width == 0) {
                    info.width = read_be16(buf.data() + body + 40);
                    info.height = read_be16(buf.data() + body + 42);
                }
            } else if (fourcc == "mp4a" || fourcc == "Opus" || fourcc == "ac-3" || fourcc == "ec-3" || fourcc == "fLaC") {
                info.has_audio = true;
                info.audio_codec = fourcc;
                if (body + 44 <= body_end) {
                    info.audio_channels = read_be16(buf.data() + body + 32);
                    info.audio_sample_rate = read_be32(buf.data() + body + 40) >> 16;
                }
            }
        } else if (type == "stsz" && body + 12 <= body_end) {
            uint32_t count = read_be32(buf.data() + body + 8);
            if (count > info.sample_count) info.sample_count = count;
        } else if (type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "stbl") {
            parse_mp4_boxes(buf, body, body_end, info);
        }

        pos += sz;
    }
}

bool decode_y4m_range(std::istream& in,
                      uint32_t start_frame,
                      uint32_t max_frames,
                      const std::string& codec_prefix,
                      std::vector<DecodedVideoFrame>& out_frames,
                      std::string* error) {
    std::string header;
    if (!std::getline(in, header) || header.rfind("YUV4MPEG2", 0) != 0) {
        if (error) *error = "Not a valid YUV4MPEG2 stream";
        return false;
    }

    int w = 0, h = 0;
    double fps = 30.0;
    std::istringstream hss(header);
    std::string tok;
    while (hss >> tok) {
        if (tok.size() > 1 && tok[0] == 'W') w = std::atoi(tok.c_str() + 1);
        else if (tok.size() > 1 && tok[0] == 'H') h = std::atoi(tok.c_str() + 1);
        else if (tok.size() > 1 && tok[0] == 'F') {
            size_t colon = tok.find(':');
            if (colon != std::string::npos) {
                double num = std::atof(tok.substr(1, colon - 1).c_str());
                double den = std::atof(tok.substr(colon + 1).c_str());
                if (den > 0.0) fps = num / den;
            }
        }
    }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        if (error) *error = "Invalid Y4M frame dimensions";
        return false;
    }

    size_t y_size = static_cast<size_t>(w * h);
    size_t uv_size = static_cast<size_t>((w / 2) * (h / 2));
    size_t frame_bytes = y_size + uv_size * 2;
    std::vector<uint8_t> yuv(frame_bytes);

    out_frames.clear();
    std::string frame_hdr;
    uint32_t current_idx = 0;
    while (std::getline(in, frame_hdr)) {
        if (frame_hdr.rfind("FRAME", 0) != 0) break;
        if (!in.read(reinterpret_cast<char*>(yuv.data()), static_cast<std::streamsize>(frame_bytes))) {
            break;
        }
        if (current_idx >= start_frame && out_frames.size() < max_frames) {
            DecodedVideoFrame df;
            df.width = w;
            df.height = h;
            df.frame_index = current_idx;
            df.pts_sec = static_cast<double>(current_idx) / std::max(1.0, fps);
            df.codec_label = codec_prefix + " " + std::to_string(w) + "x" + std::to_string(h) +
                             " | Frame #" + std::to_string(current_idx);
            MediaEngine::yuv420p_to_argb(yuv.data(), yuv.data() + y_size, yuv.data() + y_size + uv_size,
                                         w, h, df.argb);
            out_frames.push_back(std::move(df));
        }
        ++current_idx;
        if (out_frames.size() >= max_frames) break;
    }

    if (out_frames.empty()) {
        if (error) *error = "No decoded frames in requested range";
        return false;
    }
    return true;
}

} // namespace

bool MediaPlaybackSession::open(const std::string& path, std::string* error) {
    path_ = path;
    info_ = MediaEngine::probe_container(path);
    ring_.clear();
    buffered_bytes_ = 0;
    cursor_ = 0;
    current_pts_sec_ = 0.0;
    if (!info_.valid) {
        if (error) *error = info_.error;
        return false;
    }
    return true;
}

bool MediaPlaybackSession::decode_batch(uint32_t start_frame, uint32_t count, std::string* error) {
    std::vector<DecodedVideoFrame> frames;
    if (!MediaEngine::decode_frames(path_, start_frame, count, frames, error)) {
        return false;
    }
    for (auto& f : frames) {
        size_t sz = f.argb.size() * sizeof(uint32_t);
        while (buffered_bytes_ + sz > kMaxBufferBytes && !ring_.empty()) {
            buffered_bytes_ -= ring_.front().argb.size() * sizeof(uint32_t);
            ring_.pop_front();
            if (cursor_ > 0) --cursor_;
        }
        buffered_bytes_ += sz;
        ring_.push_back(std::move(f));
    }
    return true;
}

bool MediaPlaybackSession::seek(double time_sec, DecodedVideoFrame& out_frame) {
    double fps = info_.fps > 0.0 ? info_.fps : 24.0;
    uint32_t target = static_cast<uint32_t>(std::max(0.0, std::round(time_sec * fps)));
    for (size_t i = 0; i < ring_.size(); ++i) {
        if (ring_[i].frame_index == target) {
            cursor_ = i;
            current_pts_sec_ = ring_[i].pts_sec;
            out_frame = ring_[i];
            return true;
        }
    }
    if (!MediaEngine::decode_frame(path_, target, out_frame)) return false;
    current_pts_sec_ = out_frame.pts_sec;
    return true;
}

bool MediaPlaybackSession::next_frame(DecodedVideoFrame& out_frame) {
    if (cursor_ >= ring_.size()) return false;
    out_frame = ring_[cursor_++];
    current_pts_sec_ = out_frame.pts_sec;
    return true;
}

VideoContainerInfo MediaEngine::probe_container(const std::string& path) {
    VideoContainerInfo info;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        info.error = "Cannot open media file: " + path;
        return info;
    }

    std::vector<uint8_t> buf(4 * 1024 * 1024);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    size_t n = static_cast<size_t>(f.gcount());
    buf.resize(n);
    if (n < 12) {
        info.error = "Media file too small";
        return info;
    }

    if (std::memcmp(buf.data(), "YUV4MPEG2", 9) == 0) {
        info.format = "Y4M/YUV420";
        info.video_codec = "yuv420p";
        std::string hdr(reinterpret_cast<const char*>(buf.data()), std::min<size_t>(n, 256));
        size_t nl = hdr.find('\n');
        if (nl != std::string::npos) hdr.resize(nl);
        std::istringstream iss(hdr);
        std::string tok;
        while (iss >> tok) {
            if (tok.size() > 1 && tok[0] == 'W') info.width = static_cast<uint32_t>(std::atoi(tok.c_str() + 1));
            else if (tok.size() > 1 && tok[0] == 'H') info.height = static_cast<uint32_t>(std::atoi(tok.c_str() + 1));
        }
        info.valid = (info.width > 0 && info.height > 0);
        return info;
    }

    if (buf[0] == 0x1A && buf[1] == 0x45 && buf[2] == 0xDF && buf[3] == 0xA3) {
        info.format = "WebM/Matroska";
        info.video_codec = "vp9/av1";
        info.valid = true;
        return info;
    }

    if (std::memcmp(buf.data(), "RIFF", 4) == 0 && std::memcmp(buf.data() + 8, "WAVE", 4) == 0) {
        info.format = "WAV/PCM";
        info.audio_codec = "pcm_s16le";
        info.has_audio = true;
        if (n >= 44) {
            info.audio_channels = static_cast<uint16_t>(buf[22] | (buf[23] << 8));
            info.audio_sample_rate = static_cast<uint32_t>(buf[24] | (buf[25] << 8) | (buf[26] << 16) | (buf[27] << 24));
            uint32_t byte_rate = static_cast<uint32_t>(buf[28] | (buf[29] << 8) | (buf[30] << 16) | (buf[31] << 24));
            if (byte_rate > 0 && n > 44) info.duration_sec = static_cast<double>(n - 44) / byte_rate;
        }
        info.valid = true;
        return info;
    }

    if (std::memcmp(buf.data() + 4, "ftyp", 4) == 0) {
        parse_mp4_boxes(buf, 0, buf.size(), info);
        if (info.duration_sec > 0.0 && info.sample_count > 0) {
            info.fps = static_cast<double>(info.sample_count) / info.duration_sec;
        }
        return info;
    }

    // MPEG-TS (HLS streaming video segments: sync byte 0x47 every 188 bytes)
    if (n >= 376 && buf[0] == 0x47 && buf[188] == 0x47) {
        info.format = "MPEG-TS/HLS";
        info.video_codec = "h264/aac";
        info.has_audio = true;
        info.valid = true;
        return info;
    }

    // Ogg container (Opus/Vorbis/Theora)
    if (std::memcmp(buf.data(), "OggS", 4) == 0) {
        info.format = "Ogg";
        info.audio_codec = "opus/vorbis";
        info.has_audio = true;
        info.valid = true;
        return info;
    }

    // FLAC audio
    if (std::memcmp(buf.data(), "fLaC", 4) == 0) {
        info.format = "FLAC";
        info.audio_codec = "flac";
        info.has_audio = true;
        info.valid = true;
        return info;
    }

    // MP3 (ID3 tag or raw MPEG audio frame sync)
    if (std::memcmp(buf.data(), "ID3", 3) == 0 ||
        (buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0)) {
        info.format = "MP3/MPEG";
        info.audio_codec = "mp3";
        info.has_audio = true;
        info.valid = true;
        return info;
    }

    info.error = "Unrecognized video container format";
    return info;
}

HlsPlaylist MediaEngine::parse_hls_playlist(std::string_view m3u8_text, const std::string& base_url) {
    HlsPlaylist pl;
    auto trim_sv = [](std::string_view sv) {
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
            sv.remove_prefix(1);
        }
        while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
            sv.remove_suffix(1);
        }
        return sv;
    };
    auto resolve_hls_url = [&](const std::string& rel) -> std::string {
        if (rel.rfind("http://", 0) == 0 || rel.rfind("https://", 0) == 0 || rel.rfind("file://", 0) == 0) {
            return rel;
        }
        size_t scheme_end = base_url.find("://");
        if (scheme_end == std::string::npos) return rel;
        if (rel.rfind("//", 0) == 0) {
            return base_url.substr(0, scheme_end + 1) + rel;
        }
        size_t path_start = base_url.find('/', scheme_end + 3);
        std::string origin = (path_start == std::string::npos) ? base_url : base_url.substr(0, path_start);
        if (!rel.empty() && rel[0] == '/') return origin + rel;
        std::string path_part = (path_start == std::string::npos) ? "/" : base_url.substr(path_start);
        size_t q = path_part.find('?');
        if (q != std::string::npos) path_part.resize(q);
        size_t last_slash = path_part.rfind('/');
        std::string dir = (last_slash == std::string::npos) ? "/" : path_part.substr(0, last_slash + 1);
        return origin + dir + rel;
    };

    // Skip optional UTF-8 BOM
    if (m3u8_text.size() >= 3 &&
        static_cast<unsigned char>(m3u8_text[0]) == 0xEF &&
        static_cast<unsigned char>(m3u8_text[1]) == 0xBB &&
        static_cast<unsigned char>(m3u8_text[2]) == 0xBF) {
        m3u8_text.remove_prefix(3);
    }

    std::istringstream iss{std::string(m3u8_text)};
    std::string raw_line;
    bool saw_header = false;
    bool expect_variant = false;
    bool expect_segment = false;

    while (std::getline(iss, raw_line)) {
        std::string_view line = trim_sv(raw_line);
        if (line.empty()) continue;
        if (!saw_header) {
            if (line.rfind("#EXTM3U", 0) == 0) {
                saw_header = true;
                pl.valid = true;
                continue;
            }
            return pl;
        }
        if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
            pl.is_master = true;
            expect_variant = true;
            continue;
        }
        if (line.rfind("#EXT-X-TARGETDURATION:", 0) == 0) {
            pl.target_duration_sec = std::atof(std::string(line.substr(22)).c_str());
            continue;
        }
        if (line.rfind("#EXTINF:", 0) == 0) {
            expect_segment = true;
            continue;
        }
        if (line[0] == '#') continue;

        std::string resolved = resolve_hls_url(std::string(line));
        if (expect_variant) {
            pl.variant_urls.push_back(std::move(resolved));
            expect_variant = false;
        } else if (expect_segment || !pl.is_master) {
            pl.segment_urls.push_back(std::move(resolved));
            expect_segment = false;
        }
    }
    return pl;
}

void MediaEngine::yuv420p_to_argb(const uint8_t* y_plane,
                                  const uint8_t* u_plane,
                                  const uint8_t* v_plane,
                                  int width, int height,
                                  std::vector<uint32_t>& out_argb) {
    out_argb.resize(static_cast<size_t>(width * height));
    int uv_stride = width / 2;

    for (int y = 0; y < height; ++y) {
        const uint8_t* y_row = y_plane + y * width;
        const uint8_t* u_row = u_plane + (y >> 1) * uv_stride;
        const uint8_t* v_row = v_plane + (y >> 1) * uv_stride;
        uint32_t* dst = out_argb.data() + y * width;

        for (int x = 0; x < width; ++x) {
            int c = static_cast<int>(y_row[x]) - 16;
            int d = static_cast<int>(u_row[x >> 1]) - 128;
            int e = static_cast<int>(v_row[x >> 1]) - 128;

            int r = (298 * c + 459 * e + 128) >> 8;
            int g = (298 * c - 55 * d - 136 * e + 128) >> 8;
            int b = (298 * c + 541 * d + 128) >> 8;

            uint8_t ir = static_cast<uint8_t>(std::clamp(r, 0, 255));
            uint8_t ig = static_cast<uint8_t>(std::clamp(g, 0, 255));
            uint8_t ib = static_cast<uint8_t>(std::clamp(b, 0, 255));
            dst[x] = 0xFF000000u | (static_cast<uint32_t>(ir) << 16) |
                     (static_cast<uint32_t>(ig) << 8) | static_cast<uint32_t>(ib);
        }
    }
}

bool MediaEngine::decode_frames(const std::string& path,
                                uint32_t start_frame,
                                uint32_t max_frames,
                                std::vector<DecodedVideoFrame>& out_frames,
                                std::string* error) {
    VideoContainerInfo info = probe_container(path);
    if (!info.valid) {
        if (error) *error = info.error;
        return false;
    }

    if (info.format == "Y4M/YUV420") {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        return decode_y4m_range(f, start_frame, max_frames, "AstraMedia Native YUV420p", out_frames, error);
    }

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        if (error) *error = "Failed to open RDD decoder pipe";
        return false;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        FILE* devnull = std::fopen("/dev/null", "w");
        if (devnull) {
            ::dup2(fileno(devnull), STDERR_FILENO);
            std::fclose(devnull);
        }
        std::string frames_limit = std::to_string(start_frame + max_frames);
        const char* argv[] = {
            "ffmpeg", "-v", "error", "-i", path.c_str(),
            "-pix_fmt", "yuv420p", "-frames:v", frames_limit.c_str(),
            "-f", "yuv4mpegpipe", "-", nullptr
        };
        ::execvp("ffmpeg", const_cast<char* const*>(argv));
        _exit(127);
    }

    ::close(pipefd[1]);
    std::string y4m_bytes;
    char chunk[16384];
    ssize_t n = 0;
    while ((n = ::read(pipefd[0], chunk, sizeof(chunk))) > 0) {
        y4m_bytes.append(chunk, static_cast<size_t>(n));
        if (y4m_bytes.size() > 64 * 1024 * 1024) break;
    }
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);

    std::istringstream y4m_stream(y4m_bytes);
    std::string prefix = "AstraMedia RDD " + info.format + " (" +
                         (info.video_codec.empty() ? "video" : info.video_codec) + ")";
    bool ok = decode_y4m_range(y4m_stream, start_frame, max_frames, prefix, out_frames, error);
    if (ok) {
        uint32_t total = info.sample_count > 0 ? info.sample_count : static_cast<uint32_t>(std::max<double>(1.0, info.duration_sec * info.fps));
        for (auto& df : out_frames) {
            df.total_frames = total;
            df.duration_sec = info.duration_sec;
            df.has_audio = info.has_audio;
        }
    }
    return ok;
}

bool MediaEngine::decode_frame(const std::string& path,
                               uint32_t target_frame_index,
                               DecodedVideoFrame& out_frame,
                               std::string* error) {
    std::vector<DecodedVideoFrame> frames;
    if (!decode_frames(path, target_frame_index, 1, frames, error) || frames.empty()) {
        return false;
    }
    out_frame = std::move(frames[0]);
    return true;
}

bool MediaEngine::decode_audio_pcm(const std::string& path,
                                   double max_seconds,
                                   DecodedAudioBuffer& out_audio) {
    out_audio = DecodedAudioBuffer{};
    VideoContainerInfo info = probe_container(path);
    if (!info.valid) {
        out_audio.error = info.error;
        return false;
    }

    constexpr size_t kMaxAudioBytes = 4 * 1024 * 1024; // Hard 4 MiB audio buffer cap
    std::vector<uint8_t> raw_pcm;

    if (info.format == "WAV/PCM") {
        std::ifstream f(path, std::ios::binary);
        if (!f) { out_audio.error = "Cannot open WAV"; return false; }
        std::vector<uint8_t> wav((std::istreambuf_iterator<char>(f)), {});
        if (wav.size() < 44) { out_audio.error = "Truncated WAV"; return false; }
        out_audio.channels = static_cast<uint16_t>(wav[22] | (wav[23] << 8));
        out_audio.sample_rate = static_cast<uint32_t>(wav[24] | (wav[25] << 8) | (wav[26] << 16) | (wav[27] << 24));
        size_t pos = 12;
        while (pos + 8 <= wav.size()) {
            uint32_t csz = static_cast<uint32_t>(wav[pos + 4] | (wav[pos + 5] << 8) | (wav[pos + 6] << 16) | (wav[pos + 7] << 24));
            if (std::memcmp(wav.data() + pos, "data", 4) == 0) {
                size_t avail = std::min<size_t>({csz, wav.size() - (pos + 8), kMaxAudioBytes});
                raw_pcm.assign(wav.begin() + static_cast<ptrdiff_t>(pos + 8),
                               wav.begin() + static_cast<ptrdiff_t>(pos + 8 + avail));
                break;
            }
            pos += 8 + csz + (csz & 1u);
        }
    } else {
        int pipefd[2];
        if (::pipe(pipefd) != 0) { out_audio.error = "Pipe error"; return false; }
        pid_t pid = ::fork();
        if (pid < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return false; }
        if (pid == 0) {
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);
            FILE* devnull = std::fopen("/dev/null", "w");
            if (devnull) { ::dup2(fileno(devnull), STDERR_FILENO); std::fclose(devnull); }
            std::string dur_s = std::to_string(std::clamp(max_seconds, 0.25, 10.0));
            const char* argv[] = {
                "ffmpeg", "-v", "error", "-i", path.c_str(),
                "-vn", "-t", dur_s.c_str(), "-ar", "44100", "-ac", "2",
                "-f", "s16le", "-", nullptr
            };
            ::execvp("ffmpeg", const_cast<char* const*>(argv));
            _exit(127);
        }
        ::close(pipefd[1]);
        uint8_t chunk[8192];
        ssize_t n = 0;
        while ((n = ::read(pipefd[0], chunk, sizeof(chunk))) > 0) {
            size_t take = std::min<size_t>(static_cast<size_t>(n), kMaxAudioBytes - raw_pcm.size());
            raw_pcm.insert(raw_pcm.end(), chunk, chunk + take);
            if (raw_pcm.size() >= kMaxAudioBytes) break;
        }
        ::close(pipefd[0]);
        int st = 0;
        ::waitpid(pid, &st, 0);
        out_audio.sample_rate = 44100;
        out_audio.channels = 2;
    }

    if (raw_pcm.size() < 4) {
        out_audio.error = "No audio stream decoded";
        return false;
    }
    size_t sample_count = raw_pcm.size() / sizeof(int16_t);
    out_audio.pcm_s16.resize(sample_count);
    std::memcpy(out_audio.pcm_s16.data(), raw_pcm.data(), sample_count * sizeof(int16_t));

    double sum_sq = 0.0;
    for (int16_t s : out_audio.pcm_s16) {
        double fn = static_cast<double>(s) / 32768.0;
        sum_sq += fn * fn;
    }
    out_audio.rms_level = std::sqrt(sum_sq / static_cast<double>(std::max<size_t>(1, sample_count)));
    uint32_t ch = std::max<uint16_t>(1, out_audio.channels);
    uint32_t sr = std::max<uint32_t>(1, out_audio.sample_rate);
    out_audio.duration_sec = static_cast<double>(sample_count) / static_cast<double>(ch * sr);
    out_audio.valid = true;
    return true;
}

namespace {

struct AlsaApi {
    void* lib = nullptr;
    bool loaded = false;

    using snd_pcm_open_fn = int (*)(void**, const char*, int, int);
    using snd_pcm_set_params_fn = int (*)(void*, int, int, unsigned int, unsigned int, int, unsigned int);
    using snd_pcm_writei_fn = long (*)(void*, const void*, unsigned long);
    using snd_pcm_prepare_fn = int (*)(void*);
    using snd_pcm_drain_fn = int (*)(void*);
    using snd_pcm_close_fn = int (*)(void*);

    snd_pcm_open_fn snd_pcm_open = nullptr;
    snd_pcm_set_params_fn snd_pcm_set_params = nullptr;
    snd_pcm_writei_fn snd_pcm_writei = nullptr;
    snd_pcm_prepare_fn snd_pcm_prepare = nullptr;
    snd_pcm_drain_fn snd_pcm_drain = nullptr;
    snd_pcm_close_fn snd_pcm_close = nullptr;

    static void silent_err(const char*, int, const char*, int, const char*, ...) {}

    AlsaApi() {
        lib = dlopen("libasound.so.2", RTLD_LAZY);
        if (!lib) lib = dlopen("libasound.so", RTLD_LAZY);
        if (!lib) return;
        using err_cb_t = void (*)(const char*, int, const char*, int, const char*, ...);
        using set_err_fn = int (*)(err_cb_t);
        auto set_err = reinterpret_cast<set_err_fn>(dlsym(lib, "snd_lib_error_set_handler"));
        if (set_err) set_err(&AlsaApi::silent_err);
        snd_pcm_open = reinterpret_cast<snd_pcm_open_fn>(dlsym(lib, "snd_pcm_open"));
        snd_pcm_set_params = reinterpret_cast<snd_pcm_set_params_fn>(dlsym(lib, "snd_pcm_set_params"));
        snd_pcm_writei = reinterpret_cast<snd_pcm_writei_fn>(dlsym(lib, "snd_pcm_writei"));
        snd_pcm_prepare = reinterpret_cast<snd_pcm_prepare_fn>(dlsym(lib, "snd_pcm_prepare"));
        snd_pcm_drain = reinterpret_cast<snd_pcm_drain_fn>(dlsym(lib, "snd_pcm_drain"));
        snd_pcm_close = reinterpret_cast<snd_pcm_close_fn>(dlsym(lib, "snd_pcm_close"));
        loaded = snd_pcm_open && snd_pcm_set_params && snd_pcm_writei &&
                 snd_pcm_prepare && snd_pcm_drain && snd_pcm_close;
    }
};

AlsaApi& alsa_api() {
    static AlsaApi api;
    return api;
}

} // namespace

std::vector<float> MediaEngine::compute_waveform_peaks(const DecodedAudioBuffer& audio, size_t buckets) {
    std::vector<float> peaks(buckets, 0.08f);
    if (!audio.valid || audio.pcm_s16.empty() || buckets == 0) return peaks;
    size_t total = audio.pcm_s16.size();
    size_t step = std::max<size_t>(1, total / buckets);
    float max_p = 0.01f;
    for (size_t b = 0; b < buckets; ++b) {
        size_t s0 = b * step;
        size_t s1 = std::min(total, s0 + step);
        float pk = 0.0f;
        for (size_t i = s0; i < s1; ++i) {
            float v = std::fabs(static_cast<float>(audio.pcm_s16[i]) / 32768.0f);
            if (v > pk) pk = v;
        }
        peaks[b] = pk;
        if (pk > max_p) max_p = pk;
    }
    for (float& p : peaks) {
        p = std::clamp(0.12f + 0.88f * (p / max_p), 0.10f, 1.0f);
    }
    return peaks;
}

bool MediaEngine::alsa_available() {
    return alsa_api().loaded;
}

AudioPlaybackReport MediaEngine::play_audio_alsa(const DecodedAudioBuffer& audio) {
    AudioPlaybackReport rep;
    auto& api = alsa_api();
    rep.alsa_library_loaded = api.loaded;
    if (!api.loaded) {
        rep.status = "alsa_not_installed";
        return rep;
    }
    if (!audio.valid || audio.pcm_s16.empty()) {
        rep.status = "invalid_audio_buffer";
        return rep;
    }

    void* pcm = nullptr;
    // SND_PCM_STREAM_PLAYBACK = 0, SND_PCM_NONBLOCK = 1
    if (api.snd_pcm_open(&pcm, "default", 0, 0) < 0 || !pcm) {
        rep.status = "headless_no_soundcard";
        return rep;
    }
    rep.device_opened = true;

    // SND_PCM_FORMAT_S16_LE = 2, SND_PCM_ACCESS_RW_INTERLEAVED = 3
    unsigned int ch = std::max<uint16_t>(1, audio.channels);
    unsigned int sr = std::max<uint32_t>(8000, audio.sample_rate);
    if (api.snd_pcm_set_params(pcm, 2, 3, ch, sr, 1, 100000) < 0) {
        api.snd_pcm_close(pcm);
        rep.status = "alsa_params_rejected";
        return rep;
    }

    size_t total_frames = audio.pcm_s16.size() / ch;
    size_t offset = 0;
    while (offset < total_frames) {
        unsigned long chunk = static_cast<unsigned long>(std::min<size_t>(2048, total_frames - offset));
        long written = api.snd_pcm_writei(pcm, audio.pcm_s16.data() + offset * ch, chunk);
        if (written < 0) {
            api.snd_pcm_prepare(pcm);
            break;
        }
        offset += static_cast<size_t>(written);
    }
    api.snd_pcm_drain(pcm);
    api.snd_pcm_close(pcm);

    rep.frames_written = offset;
    rep.played = (offset > 0);
    rep.status = rep.played ? "played" : "write_failed";
    return rep;
}

} // namespace astra
