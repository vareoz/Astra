#include "media/media_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

    if (std::memcmp(buf.data() + 4, "ftyp", 4) == 0) {
        parse_mp4_boxes(buf, 0, buf.size(), info);
        if (info.duration_sec > 0.0 && info.sample_count > 0) {
            info.fps = static_cast<double>(info.sample_count) / info.duration_sec;
        }
        return info;
    }

    info.error = "Unrecognized video container format";
    return info;
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
    return decode_y4m_range(y4m_stream, start_frame, max_frames, prefix, out_frames, error);
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

} // namespace astra
