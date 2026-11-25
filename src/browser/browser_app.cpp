#include "browser/browser_app.hpp"
#include "dom/html_parser.hpp"
#include "style/css_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unistd.h>

namespace astra {

namespace {

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool read_local_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream oss;
    oss << f.rdbuf();
    out = oss.str();
    return true;
}

uint64_t fnv1a_64(std::string_view s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string origin_storage_path(const std::string& origin_key) {
    std::error_code ec;
    std::filesystem::create_directories("/tmp/astra_origin_storage", ec);
    std::ostringstream oss;
    oss << "/tmp/astra_origin_storage/" << std::hex << fnv1a_64(origin_key) << ".kv";
    return oss.str();
}

std::string format_mmss(double sec) {
    if (sec < 0.0) sec = 0.0;
    int mins = static_cast<int>(sec) / 60;
    double rem = sec - mins * 60;
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << mins << ":"
        << std::fixed << std::setprecision(1) << std::setw(4) << std::setfill('0') << rem;
    return oss.str();
}

std::string normalize_omnibox_url(std::string raw) {
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.erase(raw.begin());
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();
    if (raw.empty()) return "astra://home";
    if (raw.rfind("http://", 0) == 0 || raw.rfind("https://", 0) == 0 ||
        raw.rfind("file://", 0) == 0 || raw.rfind("astra://", 0) == 0) {
        return raw;
    }
    return "https://" + raw;
}

std::string escape_html_text(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out.push_back(c);
    }
    return out;
}

std::string json_escape_str(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    return out;
}

std::string build_downloads_html(const std::vector<DownloadRecord>& downloads) {
    size_t total_bytes = 0;
    for (const auto& d : downloads) {
        if (d.ok) total_bytes += d.bytes;
    }
    std::ostringstream html;
    html << "<!doctype html><html><head><title>Astra Downloads — Native Manager</title>"
         << "<style>"
         << "body { background: #0b0f19; color: #e2e8f0; padding: 20px 28px; }"
         << ".hero { background: #111827; border: 1px solid #1e293b; border-radius: 12px; padding: 16px 22px; margin-bottom: 16px; box-shadow: 0 6px 16px rgba(0,0,0,0.4); }"
         << ".kicker { color: #38bdf8; font-size: 12px; font-weight: bold; font-family: monospace; margin-bottom: 6px; }"
         << ".summary { display: grid; grid-template-columns: repeat(3, 1fr); gap: 14px; margin-bottom: 16px; }"
         << ".stat { background: #131c2e; border: 1px solid #24324a; border-radius: 10px; padding: 12px 16px; }"
         << ".stat-lbl { color: #7dd3fc; font-size: 11px; font-weight: bold; font-family: monospace; margin-bottom: 4px; }"
         << ".stat-val { color: #f8fafc; font-size: 20px; font-weight: bold; }"
         << ".dl-card { background: #111827; border: 1px solid #1e293b; border-radius: 10px; padding: 14px 18px; margin-bottom: 12px; }"
         << ".dl-name { color: #f8fafc; font-size: 16px; font-weight: bold; margin-bottom: 4px; }"
         << ".dl-meta { color: #94a3b8; font-size: 12px; font-family: monospace; margin-bottom: 4px; }"
         << ".dl-hash { color: #34d399; font-size: 11px; font-family: monospace; }"
         << "</style></head><body>"
         << "<div class=\"hero\">"
         << "<div class=\"kicker\">ASTRA DOWNLOADS MANAGER | SHA-256 VERIFIED | HTTP/2 + TLS 1.3 + ASTRASHIELD</div>"
         << "<h1>Native Downloads Ledger</h1>"
         << "<p>Every file downloaded through Astra is filtered by AstraShield + MV3 DNR, written to /tmp/astra_downloads/, and checksummed with native FIPS 180-4 SHA-256.</p>"
         << "</div>"
         << "<div class=\"summary\">"
         << "<div class=\"stat\"><div class=\"stat-lbl\">TOTAL FILES</div><div class=\"stat-val\">" << downloads.size() << "</div></div>"
         << "<div class=\"stat\"><div class=\"stat-lbl\">TOTAL SAVED</div><div class=\"stat-val\">" << total_bytes << " B</div></div>"
         << "<div class=\"stat\"><div class=\"stat-lbl\">INTEGRITY</div><div class=\"stat-val\">SHA-256</div></div>"
         << "</div>";
    if (downloads.empty()) {
        html << "<div class=\"dl-card\"><div class=\"dl-name\">No downloads yet</div>"
             << "<div class=\"dl-meta\">Click any &lt;a download&gt; link or run: astra --download &lt;url&gt;</div></div>";
    } else {
        for (auto it = downloads.rbegin(); it != downloads.rend(); ++it) {
            const auto& d = *it;
            html << "<div class=\"dl-card\">"
                 << "<div class=\"dl-name\">" << escape_html_text(d.filename)
                 << " [" << escape_html_text(d.protocol.empty() ? "http/1.1" : d.protocol) << "]</div>"
                 << "<div class=\"dl-meta\">Path: " << escape_html_text(d.saved_path)
                 << " | Size: " << d.bytes << " B | Time: " << std::fixed << std::setprecision(1) << d.elapsed_ms << " ms</div>"
                 << "<div class=\"dl-hash\">SHA-256: " << escape_html_text(d.sha256_hex) << "</div>"
                 << "</div>";
        }
    }
    html << "</body></html>";
    return html.str();
}

std::string build_home_html(const AstraShield& shield, size_t ext_count,
                            const MemorySnapshot& mem, const CacheStats& cstats,
                            size_t h2_count, size_t h2_mux_count, size_t dl_count) {
    std::ostringstream html;
    html << "<!doctype html><html><head><title>Astra Browser v6 — Native Core</title>"
         << "<style>"
         << "body { background: #0b0f19; color: #e2e8f0; padding: 14px 24px; }"
         << ".hero { box-sizing: border-box; background: linear-gradient(135deg, #0f172a, #1e3a5f); border: 1px solid #2b4c7e; border-radius: 12px; padding: 14px 20px; margin-bottom: 12px; box-shadow: 0 6px 16px rgba(0,0,0,0.45); }"
         << ".kicker { color: #38bdf8; font-size: 12px; font-weight: bold; font-family: monospace; margin-bottom: 6px; }"
         << ".grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 14px; margin-bottom: 12px; }"
         << ".card { box-sizing: border-box; background: #131c2e; border: 1px solid #24324a; border-radius: 10px; padding: 12px 14px; box-shadow: 0 4px 12px rgba(0,0,0,0.35); }"
         << ".card-title { color: #7dd3fc; font-size: 12px; font-weight: bold; font-family: monospace; margin-bottom: 6px; }"
         << ".metric { color: #f8fafc; font-size: 20px; font-weight: bold; margin-bottom: 4px; }"
         << ".muted { color: #94a3b8; font-size: 12px; }"
         << ".media-row { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }"
         << ".panel { box-sizing: border-box; background: #111827; border: 1px solid #1f2937; border-radius: 10px; padding: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.35); }"
         << "</style></head><body>"
         << "<div class=\"hero\">"
         << "<div class=\"kicker\">ASTRA v6 NATIVE C++20 | H2 MULTI-STREAM MUX | CANVAS 2D + rAF | TRY/CATCH + FOR..OF | LINEAR-GRADIENT</div>"
         << "<h1 id=\"headline\">Astra Desktop Browser — Session 6</h1>"
         << "<p>Multi-process e10s engine (Main + Content + RDD Media) with true single-socket HTTP/2 multi-stream multiplexing over TLS 1.3 ALPN, AstraJS v6 (try/catch/throw, for..of, requestAnimationFrame, native HTML5 &lt;canvas&gt; 2D), CSS linear-gradient + box-sizing: border-box (ASD5), ALSA audio waveform, and SHA-256 Downloads.</p>"
         << "</div>"
         << "<div class=\"grid\">"
         << "<div class=\"card\"><div class=\"card-title\">MEMORY &amp; H2 MULTIPLEX</div>"
         << "<div class=\"metric\">" << (mem.total_rss_bytes / (1024 * 1024)) << " / " << (mem.ceiling_bytes / (1024 * 1024)) << " MB</div>"
         << "<div class=\"muted\">Real /proc RSS | Cache: " << cstats.hits << " | H2: " << h2_count << " (Mux: " << h2_mux_count << ")</div></div>"
         << "<div class=\"card\"><div class=\"card-title\">ASTRA SHIELD PRIVACY</div>"
         << "<div class=\"metric\">" << shield.blocklist_size() << " Rules | " << shield.stats().trackers_blocked << " Blocked</div>"
         << "<div class=\"muted\">Total Cookie Partitioning + TLS 1.3 ALPN h2 + GPC</div></div>"
         << "<div class=\"card\"><div class=\"card-title\">EXTENSIONS &amp; DOWNLOADS</div>"
         << "<div class=\"metric\">" << ext_count << " Ext | " << dl_count << " Downloads</div>"
         << "<div class=\"muted\"><a href=\"astra://downloads\">Open astra://downloads</a> | MV3 DNR</div></div>"
         << "</div>"
         << "<div class=\"media-row\">"
         << "<div class=\"panel\"><div class=\"card-title\">ASTRA3D v2 SMOOTH SHADER ENGINE</div>"
         << "<astra-3d scene=\"planet\" style=\"height: 216px;\"></astra-3d></div>"
         << "<div class=\"panel\"><div class=\"card-title\">ASTRAMEDIA v4 RDD INTERACTIVE PLAYER</div>"
         << "<video src=\"examples/astra_demo.mp4\" style=\"height: 216px;\"></video></div>"
         << "</div>"
         << "</body></html>";
    return html.str();
}

} // namespace

std::string BrowserApp::compute_sha256_hex(std::string_view data) {
    static constexpr uint32_t kK[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    auto rotr = [](uint32_t x, uint32_t n) -> uint32_t { return (x >> n) | (x << (32u - n)); };

    uint32_t h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    std::vector<uint8_t> msg(data.begin(), data.end());
    uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8ULL;
    msg.push_back(0x80u);
    while ((msg.size() % 64) != 56) msg.push_back(0x00u);
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFu));
    }

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[off + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(msg[off + i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + S1 + ch + kK[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (int i = 0; i < 8; ++i) hex << std::setw(8) << h[i];
    return hex.str();
}

void BrowserApp::persist_downloads_ledger() const {
    std::error_code ec;
    std::filesystem::create_directories("/tmp/astra_downloads", ec);
    std::ofstream f("/tmp/astra_downloads/ledger.json", std::ios::trunc);
    if (!f) return;
    f << "[\n";
    for (size_t i = 0; i < downloads_.size(); ++i) {
        const auto& d = downloads_[i];
        f << "  {\"filename\":\"" << json_escape_str(d.filename)
          << "\",\"url\":\"" << json_escape_str(d.url)
          << "\",\"saved_path\":\"" << json_escape_str(d.saved_path)
          << "\",\"bytes\":" << d.bytes
          << ",\"protocol\":\"" << json_escape_str(d.protocol)
          << "\",\"sha256\":\"" << d.sha256_hex << "\"}"
          << (i + 1 < downloads_.size() ? "," : "") << "\n";
    }
    f << "]\n";
}

DownloadRecord BrowserApp::download_url(const std::string& url, const std::string& dest_dir) {
    auto t0 = std::chrono::steady_clock::now();
    DownloadRecord rec;
    rec.url = url;

    std::string payload;
    if (url.rfind("file://", 0) == 0) {
        std::string src_path = url.substr(7);
        if (!read_local_file(src_path, payload)) {
            rec.error = "Cannot read local file: " + src_path;
            return rec;
        }
        rec.protocol = "file";
    } else if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
        HttpResponse resp = http_.get(url, "", false);
        if (resp.blocked_by_privacy || resp.blocked_by_extension) {
            rec.error = resp.error;
            return rec;
        }
        if (resp.status_code < 200 || resp.status_code >= 400) {
            rec.error = resp.error.empty() ? ("HTTP " + std::to_string(resp.status_code)) : resp.error;
            return rec;
        }
        payload = std::move(resp.body);
        rec.protocol = !resp.alpn_protocol.empty() ? resp.alpn_protocol : "http/1.1";
    } else {
        rec.error = "Unsupported download scheme";
        return rec;
    }

    // Derive clean filename
    std::string clean_url = url;
    size_t qpos = clean_url.find('?');
    if (qpos != std::string::npos) clean_url.resize(qpos);
    size_t slash = clean_url.rfind('/');
    std::string fname = (slash != std::string::npos && slash + 1 < clean_url.size())
                            ? clean_url.substr(slash + 1)
                            : "astra_download.bin";
    for (char& c : fname) {
        if (c == '/' || c == '\\' || c == ':' || c == '\0') c = '_';
    }
    if (fname.empty() || fname == "." || fname == "..") fname = "astra_download.bin";

    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    std::string out_path = dest_dir + "/" + fname;

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        rec.error = "Cannot write download file: " + out_path;
        return rec;
    }
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.close();

    auto t1 = std::chrono::steady_clock::now();
    rec.filename = fname;
    rec.saved_path = out_path;
    rec.bytes = payload.size();
    rec.sha256_hex = compute_sha256_hex(payload);
    rec.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    rec.ok = true;

    downloads_.push_back(rec);
    persist_downloads_ledger();
    return rec;
}

BrowserApp::BrowserApp(uint64_t ram_ceiling_bytes)
    : cache_("/tmp/astra_http_cache", 32ULL * 1024 * 1024),
      http_(shield_, &cache_),
      governor_(ram_ceiling_bytes) {
    http_.set_dnr_hook([this](const std::string& url, const std::string& top_site) {
        return extensions_.evaluate_dnr(url, top_site);
    });
}

BrowserApp::~BrowserApp() {
    stop_workers();
}

void BrowserApp::run_content_worker_loop(IpcChannel& ch) {
    SlabArena arena(256 * 1024);
    IpcMessage req;
    while (ch.recv(req)) {
        if (req.type == IpcMsgType::Shutdown) break;
        if (req.type == IpcMsgType::Ping) {
            IpcMessage pong{IpcMsgType::Pong, req.request_id, {}};
            ch.send(pong);
            continue;
        }
        if (req.type == IpcMsgType::MemoryReportReq) {
            IpcMessage resp{IpcMsgType::MemoryReportResp, req.request_id, {}};
            write_u32(resp.payload, static_cast<uint32_t>(getpid()));
            write_u64(resp.payload, MemoryGovernor::read_process_rss_bytes(getpid()));
            write_u64(resp.payload, arena.bytes_used());
            ch.send(resp);
            continue;
        }
        if (req.type == IpcMsgType::RenderHtml) {
            size_t off = 0;
            uint32_t vw = 1280, vh = 720;
            std::string html, extra_css;
            read_u32(req.payload, off, vw);
            read_u32(req.payload, off, vh);
            read_str(req.payload, off, html);
            read_str(req.payload, off, extra_css);

            arena.reset(false);
            HtmlDocument doc = HtmlDocument::parse(arena, html);
            CssStylesheet sheet;
            sheet.parse(doc.collect_stylesheets());
            if (!extra_css.empty()) sheet.parse(extra_css);

            LayoutFrame* root = LayoutEngine::build_layout(arena, doc, sheet,
                                                           static_cast<float>(vw),
                                                           static_cast<float>(vh));
            DisplayList dl = LayoutEngine::build_display_list(root, vw, vh);
            std::vector<uint8_t> dl_blob = dl.serialize();

            IpcMessage resp{IpcMsgType::DisplayListBlob, req.request_id, {}};
            write_str(resp.payload, doc.title());
            write_u64(resp.payload, arena.bytes_used());
            write_u32(resp.payload, static_cast<uint32_t>(dl_blob.size()));
            resp.payload.insert(resp.payload.end(), dl_blob.begin(), dl_blob.end());
            ch.send(resp);

            arena.reset(true);
        }
    }
}

void BrowserApp::run_media_worker_loop(IpcChannel& ch) {
    IpcMessage req;
    while (ch.recv(req)) {
        if (req.type == IpcMsgType::Shutdown) break;
        if (req.type == IpcMsgType::Ping) {
            IpcMessage pong{IpcMsgType::Pong, req.request_id, {}};
            ch.send(pong);
            continue;
        }
        if (req.type == IpcMsgType::DecodeVideoReq) {
            size_t off = 0;
            std::string path;
            uint32_t frame_idx = 0;
            read_str(req.payload, off, path);
            read_u32(req.payload, off, frame_idx);

            DecodedVideoFrame frame;
            std::string err;
            bool ok = MediaEngine::decode_frame(path, frame_idx, frame, &err);

            IpcMessage resp{IpcMsgType::DecodeVideoResp, req.request_id, {}};
            write_u32(resp.payload, ok ? 1u : 0u);
            write_str(resp.payload, ok ? frame.codec_label : err);
            write_u32(resp.payload, static_cast<uint32_t>(frame.width));
            write_u32(resp.payload, static_cast<uint32_t>(frame.height));
            write_u32(resp.payload, frame.frame_index);
            write_u32(resp.payload, frame.total_frames);
            write_f32(resp.payload, static_cast<float>(frame.pts_sec));
            write_f32(resp.payload, static_cast<float>(frame.duration_sec));
            write_u32(resp.payload, frame.has_audio ? 1u : 0u);
            if (ok && !frame.argb.empty()) {
                size_t byte_len = frame.argb.size() * sizeof(uint32_t);
                write_u32(resp.payload, static_cast<uint32_t>(byte_len));
                const uint8_t* raw = reinterpret_cast<const uint8_t*>(frame.argb.data());
                resp.payload.insert(resp.payload.end(), raw, raw + byte_len);
            } else {
                write_u32(resp.payload, 0u);
            }
            ch.send(resp);
        }
    }
}

bool BrowserApp::start_workers() {
    bool ok_content = content_worker_.spawn("content", [this](IpcChannel& ch) {
        run_content_worker_loop(ch);
    });
    bool ok_media = media_worker_.spawn("rdd-media", [this](IpcChannel& ch) {
        run_media_worker_loop(ch);
    });
    if (ok_content) governor_.register_process(content_worker_.pid(), "content");
    if (ok_media) governor_.register_process(media_worker_.pid(), "rdd-media");
    return ok_content && ok_media;
}

void BrowserApp::stop_workers() {
    if (content_worker_.alive()) governor_.unregister_process(content_worker_.pid());
    if (media_worker_.alive()) governor_.unregister_process(media_worker_.pid());
    content_worker_.stop();
    media_worker_.stop();
}

bool BrowserApp::load_extension(const std::string& dir_path, std::string* error) {
    return extensions_.load_extension_dir(dir_path, error);
}

Tab* BrowserApp::find_tab(uint32_t id) {
    for (auto& t : tabs_) if (t.id == id) return &t;
    return nullptr;
}

const Tab* BrowserApp::find_tab(uint32_t id) const {
    for (const auto& t : tabs_) if (t.id == id) return &t;
    return nullptr;
}

uint32_t BrowserApp::new_tab(const std::string& url) {
    Tab t;
    t.id = next_tab_id_++;
    t.url = url;
    t.title = "New Tab";
    t.state = TabState::Active;
    t.last_used_tick = ++tick_counter_;
    t.live_arena = std::make_unique<SlabArena>();

    for (auto& existing : tabs_) {
        if (existing.state == TabState::Active) existing.state = TabState::Warm;
    }

    tabs_.push_back(std::move(t));
    active_tab_id_ = tabs_.back().id;
    address_bar_focused_ = false;
    if (!url.empty()) {
        navigate(active_tab_id_, url, true);
    }
    return active_tab_id_;
}

bool BrowserApp::close_tab(uint32_t tab_id) {
    auto it = std::find_if(tabs_.begin(), tabs_.end(), [tab_id](const Tab& t) { return t.id == tab_id; });
    if (it == tabs_.end()) return false;
    size_t idx = static_cast<size_t>(std::distance(tabs_.begin(), it));
    bool was_active = (tab_id == active_tab_id_);
    tabs_.erase(it);
    address_bar_focused_ = false;

    if (tabs_.empty()) {
        new_tab("astra://home");
        return true;
    }
    if (was_active) {
        size_t next_idx = std::min(idx, tabs_.size() - 1);
        switch_tab(tabs_[next_idx].id);
    }
    return true;
}

bool BrowserApp::go_back() {
    Tab* active = find_tab(active_tab_id_);
    if (!active || active->history_index <= 0) return false;
    --active->history_index;
    std::string target = active->history[static_cast<size_t>(active->history_index)];
    address_bar_focused_ = false;
    return navigate(active->id, target, false).ok;
}

bool BrowserApp::go_forward() {
    Tab* active = find_tab(active_tab_id_);
    if (!active || active->history_index < 0 ||
        static_cast<size_t>(active->history_index + 1) >= active->history.size()) {
        return false;
    }
    ++active->history_index;
    std::string target = active->history[static_cast<size_t>(active->history_index)];
    address_bar_focused_ = false;
    return navigate(active->id, target, false).ok;
}

bool BrowserApp::reload_active_tab() {
    Tab* active = find_tab(active_tab_id_);
    if (!active || active->url.empty()) return false;
    address_bar_focused_ = false;
    return navigate(active->id, active->url, false).ok;
}

void BrowserApp::focus_address_bar(bool clear_text) {
    Tab* active = find_tab(active_tab_id_);
    address_bar_focused_ = true;
    if (clear_text) {
        address_bar_buffer_.clear();
    } else if (active) {
        address_bar_buffer_ = active->url;
    }
    if (active && active->live_doc && !active->focused_input_selector.empty()) {
        for (DomNode* inp : active->live_doc->query_selector_all("input")) inp->set_attr("data-astra-focused", "0");
        for (DomNode* ta : active->live_doc->query_selector_all("textarea")) ta->set_attr("data-astra-focused", "0");
        active->focused_input_selector.clear();
        relayout_live_tab(*active);
    }
}

std::string BrowserApp::origin_storage_get(const std::string& origin_key, const std::string& k) {
    std::ifstream f(origin_storage_path(origin_key));
    if (!f) return "";
    std::string line;
    std::string found;
    while (std::getline(f, line)) {
        size_t tab = line.find('\t');
        if (tab != std::string::npos && line.substr(0, tab) == k) {
            found = line.substr(tab + 1);
        }
    }
    return found;
}

void BrowserApp::origin_storage_set(const std::string& origin_key, const std::string& k, const std::string& v) {
    std::string path = origin_storage_path(origin_key);
    std::unordered_map<std::string, std::string> kv;
    {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            size_t tab = line.find('\t');
            if (tab != std::string::npos) kv[line.substr(0, tab)] = line.substr(tab + 1);
        }
    }
    std::string clean_v = v;
    std::replace(clean_v.begin(), clean_v.end(), '\n', ' ');
    std::replace(clean_v.begin(), clean_v.end(), '\r', ' ');
    kv[k] = clean_v;

    // Enforce bounded storage (max 1 MiB per origin)
    std::ostringstream oss;
    size_t total = 0;
    for (const auto& [ek, ev] : kv) {
        if (total + ek.size() + ev.size() + 2 > 1024 * 1024) break;
        oss << ek << '\t' << ev << '\n';
        total += ek.size() + ev.size() + 2;
    }
    std::ofstream out(path, std::ios::trunc);
    out << oss.str();
}

std::string BrowserApp::resolve_source_html(const std::string& url,
                                            std::string& final_url,
                                            bool& tls_verified_out,
                                            bool& from_cache_out,
                                            std::string& protocol_out,
                                            std::string& error_out) {
    final_url = url;
    tls_verified_out = false;
    from_cache_out = false;
    protocol_out = "astra";

    if (url == "astra://home" || url == "astra://start") {
        return build_home_html(shield_, extensions_.extensions().size(), memory_snapshot(),
                               cache_.stats(), http_.http2_requests(), http_.http2_multiplexed_streams(),
                               downloads_.size());
    }
    if (url == "astra://downloads") {
        return build_downloads_html(downloads_);
    }
    if (url == "astra://3d") {
        return "<!doctype html><html><head><title>Astra3D Studio</title>"
               "<style>body{background:#090d16;color:#e2e8f0;padding:20px;} .row{display:flex;gap:14px;}</style></head><body>"
               "<h1>Astra3D v2 Smooth Shader Studio</h1>"
               "<div class=\"row\">"
               "<astra-3d scene=\"planet\" style=\"height:420px;\"></astra-3d>"
               "<astra-3d scene=\"torus\" style=\"height:420px;\"></astra-3d>"
               "</div></body></html>";
    }
    if (url.rfind("file://", 0) == 0) {
        protocol_out = "file";
        std::string path = url.substr(7);
        if (ends_with(path, ".pdf")) {
            ParsedDocument doc = DocViewer::parse_pdf_file(path);
            if (!doc.ok) { error_out = doc.error; return {}; }
            return DocViewer::to_astra_html(doc);
        }
        if (ends_with(path, ".md")) {
            ParsedDocument doc = DocViewer::parse_markdown_file(path);
            if (!doc.ok) { error_out = doc.error; return {}; }
            return DocViewer::to_astra_html(doc);
        }
        if (ends_with(path, ".obj")) {
            return "<!doctype html><html><head><title>Astra3D Model: " + path + "</title></head><body>"
                   "<h1>Native 3D OBJ Viewer</h1><astra-3d scene=\"" + path + "\" style=\"height:480px;\"></astra-3d></body></html>";
        }
        if (ends_with(path, ".mp4") || ends_with(path, ".webm") || ends_with(path, ".y4m")) {
            return "<!doctype html><html><head><title>AstraMedia Player: " + path + "</title></head><body>"
                   "<h1>AstraMedia Native Video Player</h1><video src=\"" + path + "\" style=\"height:460px;\"></video></body></html>";
        }
        if (ends_with(path, ".wav")) {
            return "<!doctype html><html><head><title>AstraMedia Audio: " + path + "</title></head><body>"
                   "<h1>AstraMedia v4 Native Audio Player</h1><audio src=\"" + path + "\"></audio></body></html>";
        }
        std::string text;
        if (!read_local_file(path, text)) { error_out = "Cannot open local file: " + path; return {}; }
        return text;
    }

    HttpResponse resp = http_.get(url, "", true);
    final_url = resp.final_url.empty() ? url : resp.final_url;
    tls_verified_out = resp.tls_verified;
    from_cache_out = resp.from_cache;
    protocol_out = !resp.alpn_protocol.empty() ? resp.alpn_protocol : "http/1.1";

    if (resp.blocked_by_privacy || resp.blocked_by_extension) {
        error_out = resp.error;
        return "<!doctype html><html><head><title>Request Blocked</title></head><body>"
               "<h1>Protected by Astra</h1><p>" + resp.error + "</p><p>URL: " + final_url + "</p></body></html>";
    }
    if (!resp.error.empty() || resp.status_code == 0) {
        error_out = resp.error.empty() ? "Network error" : resp.error;
        return {};
    }
    if (resp.content_type.find("application/pdf") != std::string::npos) {
        ParsedDocument pdf = DocViewer::parse_pdf_bytes(resp.body, final_url);
        return pdf.ok ? DocViewer::to_astra_html(pdf) : ("<html><body><h1>PDF Error</h1><p>" + pdf.error + "</p></body></html>");
    }
    return resp.body;
}

std::string BrowserApp::fetch_external_stylesheets(const HtmlDocument& doc,
                                                   const std::string& base_url,
                                                   size_t& loaded_count) {
    std::string combined;
    loaded_count = 0;
    ParsedUrl base = parse_url(base_url);
    std::string top_site = base.valid ? base.registrable_domain() : "";

    std::vector<std::string> remote_urls;
    for (DomNode* link : doc.query_selector_all("link")) {
        if (link->get_attr("rel") != "stylesheet") continue;
        std::string href = link->get_attr("href");
        if (href.empty()) continue;
        std::string resolved = HttpClient::resolve_relative_url(base_url, href);

        if (resolved.rfind("file://", 0) == 0) {
            std::string css;
            if (read_local_file(resolved.substr(7), css)) {
                combined += "\n" + css;
                ++loaded_count;
            }
        } else if (resolved.rfind("http://", 0) == 0 || resolved.rfind("https://", 0) == 0) {
            remote_urls.push_back(resolved);
        }
    }

    if (!remote_urls.empty()) {
        auto responses = http_.fetch_batch(remote_urls, top_site, 4);
        for (const auto& r : responses) {
            if (!r.blocked_by_privacy && !r.blocked_by_extension && r.status_code == 200) {
                combined += "\n" + r.body;
                ++loaded_count;
            }
        }
    }
    return combined;
}

void BrowserApp::preload_remote_images_parallel(const HtmlDocument& doc, const std::string& base_url) {
    std::vector<std::string> remote_imgs;
    for (DomNode* img : doc.query_selector_all("img")) {
        std::string src = img->get_attr("src");
        if (src.empty()) continue;
        std::string resolved = HttpClient::resolve_relative_url(base_url, src);
        if ((resolved.rfind("http://", 0) == 0 || resolved.rfind("https://", 0) == 0) &&
            image_cache_.find(resolved) == image_cache_.end()) {
            remote_imgs.push_back(resolved);
            if (remote_imgs.size() >= 8) break; // Bound concurrent preload per page
        }
    }
    if (remote_imgs.size() <= 1) return;
    ParsedUrl base = parse_url(base_url);
    auto responses = http_.fetch_batch(remote_imgs, base.valid ? base.registrable_domain() : "", 4);
    for (size_t i = 0; i < responses.size(); ++i) {
        const auto& r = responses[i];
        if (!r.blocked_by_privacy && !r.blocked_by_extension && r.status_code == 200 && !r.body.empty()) {
            DecodedImage dec = ImageDecoder::decode_bytes(
                reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size());
            if (dec.valid) {
                if (image_cache_.size() > 32) image_cache_.clear();
                image_cache_[remote_imgs[i]] = std::move(dec);
            }
        }
    }
}

bool BrowserApp::load_image_subresource(const std::string& base_url, const std::string& src, DecodedImage& out) {
    if (src.empty()) return false;
    std::string resolved = HttpClient::resolve_relative_url(base_url, src);

    auto it = image_cache_.find(resolved);
    if (it != image_cache_.end()) {
        out = it->second;
        return out.valid;
    }

    DecodedImage decoded;
    if (resolved.rfind("file://", 0) == 0) {
        decoded = ImageDecoder::decode_file(resolved.substr(7));
    } else if (resolved.rfind("http://", 0) == 0 || resolved.rfind("https://", 0) == 0) {
        ParsedUrl b = parse_url(base_url);
        HttpResponse r = http_.get(resolved, b.valid ? b.registrable_domain() : "", false);
        if (!r.blocked_by_privacy && !r.blocked_by_extension && r.status_code == 200 && !r.body.empty()) {
            decoded = ImageDecoder::decode_bytes(reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size());
        }
    } else {
        decoded = ImageDecoder::decode_file(src);
    }

    if (decoded.valid) {
        if (image_cache_.size() > 32) image_cache_.clear(); // Keep decoded image RAM bounded
        image_cache_[resolved] = decoded;
        out = std::move(decoded);
        return true;
    }
    return false;
}

bool BrowserApp::render_html_via_content_ipc(const std::string& html,
                                             const std::string& extra_css,
                                             uint32_t vw, uint32_t vh,
                                             DisplayList& out_dl,
                                             std::string& out_title,
                                             size_t& out_blob_bytes,
                                             size_t& out_arena_bytes) {
    if (!content_worker_.alive()) return false;
    IpcMessage req{IpcMsgType::RenderHtml, next_req_id_++, {}};
    write_u32(req.payload, vw);
    write_u32(req.payload, vh);
    write_str(req.payload, html);
    write_str(req.payload, extra_css);

    if (!content_worker_.channel().send(req)) return false;
    IpcMessage resp;
    if (!content_worker_.channel().recv(resp) || resp.type != IpcMsgType::DisplayListBlob) {
        return false;
    }

    size_t off = 0;
    uint64_t arena_used = 0;
    uint32_t blob_len = 0;
    if (!read_str(resp.payload, off, out_title)) return false;
    if (!read_u64(resp.payload, off, arena_used)) return false;
    if (!read_u32(resp.payload, off, blob_len)) return false;
    if (off + blob_len > resp.payload.size()) return false;

    std::vector<uint8_t> blob(resp.payload.begin() + static_cast<ptrdiff_t>(off),
                              resp.payload.begin() + static_cast<ptrdiff_t>(off + blob_len));
    out_blob_bytes = blob.size();
    out_arena_bytes = static_cast<size_t>(arena_used);
    return DisplayList::deserialize(blob, out_dl);
}

void BrowserApp::relayout_live_tab(Tab& tab, float vw, float vh) {
    if (!tab.live_arena || !tab.live_doc) return;
    // La hoja se reconstruye en el mismo objeto persistente para que el
    // puntero que guarda la línea de tiempo siga siendo válido.
    tab.live_sheet = std::make_unique<CssStylesheet>();
    CssStylesheet& sheet = *tab.live_sheet;
    sheet.parse(tab.live_doc->collect_stylesheets());
    if (!tab.extra_css.empty()) sheet.parse(tab.extra_css);
    if (!tab.timeline) tab.timeline = std::make_unique<AnimationTimeline>();
    tab.timeline->retain_only(*tab.live_doc);
    LayoutFrame* root = LayoutEngine::build_layout(*tab.live_arena, *tab.live_doc, sheet, vw, vh,
                                                  tab.timeline.get());
    tab.cached_display_list = LayoutEngine::build_display_list(root, static_cast<uint32_t>(vw), static_cast<uint32_t>(vh));
    tab.arena_bytes = tab.live_arena->bytes_used();
}

NavigationResult BrowserApp::navigate(uint32_t tab_id, const std::string& url, bool push_history) {
    auto t0 = std::chrono::steady_clock::now();
    NavigationResult res;
    res.tab_id = tab_id;

    Tab* tab = find_tab(tab_id);
    if (!tab) {
        res.error = "Tab not found";
        return res;
    }

    std::string final_url, err, proto;
    bool tls_ok = false, cached_hit = false;
    std::string html = resolve_source_html(url, final_url, tls_ok, cached_hit, proto, err);
    if (html.empty()) {
        res.error = err;
        return res;
    }

    // Reset arena cleanly (destroy live_doc first since its nodes live in live_arena)
    tab->live_doc.reset();
    tab->js_vm.reset();
    if (!tab->live_arena) tab->live_arena = std::make_unique<SlabArena>();
    tab->live_arena->reset(false);

    tab->live_doc = std::make_unique<HtmlDocument>(HtmlDocument::parse(*tab->live_arena, html));
    tab->js_vm = std::make_unique<AstraJsEngine>();
    // Los nodos del documento anterior vivían en la arena que acabamos de
    // reciclar: la línea de tiempo se reinicia para no guardar punteros muertos.
    tab->live_sheet.reset();
    tab->timeline = std::make_unique<AnimationTimeline>();
    tab->focused_input_selector.clear();

    // Bind disk-persisted per-origin storage & network fetch (GET/POST)
    ParsedUrl pu = parse_url(final_url);
    std::string origin_key = pu.valid ? pu.origin() : final_url;
    tab->js_vm->bind_storage(
        [this, origin_key](const std::string& k) { return origin_storage_get(origin_key, k); },
        [this, origin_key](const std::string& k, const std::string& v) { origin_storage_set(origin_key, k, v); });
    // Session 8: `document.cookie` queda enganchado al tarro particionado de
    // AstraShield, con la misma clave (top_level_site, origin) que usan las
    // peticiones de red. Así lo que escribe un script sigue sin poder filtrarse
    // a otro sitio de primer nivel bajo Total Cookie Protection.
    const std::string cookie_top_site = pu.valid ? pu.registrable_domain() : origin_key;
    const std::string cookie_origin = origin_key;
    const bool cookie_https = pu.valid && pu.scheme == "https";
    tab->js_vm->bind_cookies(
        [this, cookie_top_site, cookie_origin, cookie_https]() {
            return shield_.cookie_header_for(cookie_top_site, cookie_origin, cookie_https);
        },
        [this, cookie_top_site, cookie_origin](const std::string& set_cookie) {
            shield_.store_cookie(cookie_top_site, cookie_origin, set_cookie);
        });

    std::string fetch_base = final_url;
    tab->js_vm->bind_fetch([this, fetch_base](const std::string& req_url,
                                              const std::string& method,
                                              const std::string& body) {
        return perform_js_fetch(fetch_base, req_url, method, body);
    });

    // Load external <link rel="stylesheet"> subresources through AstraShield + HttpCache (parallel batch)
    size_t ext_css_count = 0;
    std::string subresource_css = fetch_external_stylesheets(*tab->live_doc, final_url, ext_css_count);
    preload_remote_images_parallel(*tab->live_doc, final_url);

    // Execute page <script> blocks via AstraJS v4 Bytecode VM (Promises + Event Loop)
    tab->console_logs.clear();
    auto scripts = tab->live_doc->collect_scripts();
    for (const auto& sc : scripts) {
        auto rep = tab->js_vm->execute(sc, *tab->live_doc);
        for (const auto& l : rep.logs) tab->console_logs.push_back(l);
    }

    // Execute matching Manifest V3 WebExtension content scripts & CSS
    std::string ext_css;
    auto ext_sum = extensions_.run_content_scripts(final_url, *tab->live_doc, ext_css);
    for (const auto& l : ext_sum.logs) tab->console_logs.push_back(l);

    std::string all_extra_css = subresource_css + "\n" + ext_css;

    if (push_history) {
        if (tab->history_index >= 0 &&
            static_cast<size_t>(tab->history_index + 1) < tab->history.size()) {
            tab->history.resize(static_cast<size_t>(tab->history_index + 1));
        }
        if (tab->history.empty() || tab->history.back() != final_url) {
            tab->history.push_back(final_url);
            tab->history_index = static_cast<int>(tab->history.size()) - 1;
        }
    }

    tab->url = final_url;
    tab->raw_html = html;
    tab->extra_css = all_extra_css;
    tab->dom_nodes = tab->live_doc->node_count();
    tab->scroll_y = 0.0f;
    tab->tls_verified = tls_ok;
    tab->from_cache = cached_hit;
    tab->last_used_tick = ++tick_counter_;
    tab->state = (tab->id == active_tab_id_) ? TabState::Active : TabState::Warm;

    DisplayList dl;
    std::string rendered_title;
    size_t blob_bytes = 0, arena_used = 0;

    if (content_worker_.alive() && ext_sum.scripts_injected == 0 && scripts.empty()) {
        if (render_html_via_content_ipc(html, all_extra_css, 1280, 636, dl, rendered_title, blob_bytes, arena_used)) {
            res.rendered_via_ipc = true;
        }
    }
    // Session 8: el proceso principal construye siempre el estado de estilo
    // vivo de la pestaña (hoja persistente + línea de tiempo de animaciones),
    // porque es el que atiende clics, teclado y requestAnimationFrame. Sembrarlo
    // ya en la navegación es lo que permite que una transición CSS arranque
    // desde el valor base en vez de saltar al destino en el primer fotograma, y
    // que una animación @keyframes declarada en la hoja empiece a correr aunque
    // la página no traiga ningún script.
    relayout_live_tab(*tab, 1280.0f, 636.0f);

    if (!res.rendered_via_ipc) {
        dl = tab->cached_display_list;
        rendered_title = tab->live_doc->title();
        blob_bytes = dl.serialize().size();
        arena_used = tab->live_arena->bytes_used();
    }

    tab->title = rendered_title.empty() ? final_url : rendered_title;
    tab->cached_display_list = std::move(dl);
    tab->arena_bytes = arena_used;

    enforce_memory_budget();

    auto t1 = std::chrono::steady_clock::now();
    res.ok = true;
    res.final_url = tab->url;
    res.title = tab->title;
    res.display_items = tab->cached_display_list.items.size();
    res.hit_regions = tab->cached_display_list.hit_regions.size();
    res.ipc_blob_bytes = blob_bytes;
    res.arena_bytes = arena_used;
    res.external_css_loaded = ext_css_count;
    res.tls_verified = tls_ok;
    res.from_cache = cached_hit;
    res.protocol = proto;
    res.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return res;
}

bool BrowserApp::switch_tab(uint32_t tab_id) {
    Tab* target = find_tab(tab_id);
    if (!target) return false;
    for (auto& t : tabs_) {
        if (t.state == TabState::Active) t.state = TabState::Warm;
    }
    active_tab_id_ = tab_id;
    address_bar_focused_ = false;
    target->last_used_tick = ++tick_counter_;

    if (target->state == TabState::Dehydrated) {
        target->state = TabState::Active;
        navigate(tab_id, target->url, false);
    } else {
        target->state = TabState::Active;
    }
    return true;
}

bool BrowserApp::dehydrate_tab(uint32_t tab_id) {
    Tab* tab = find_tab(tab_id);
    if (!tab || tab->id == active_tab_id_ || tab->state == TabState::Dehydrated) return false;

    tab->live_doc.reset();
    tab->js_vm.reset();
    if (tab->live_arena) {
        tab->live_arena->reset(true);
        tab->live_arena.reset();
    }
    tab->cached_display_list.items.clear();
    tab->cached_display_list.items.shrink_to_fit();
    tab->cached_display_list.hit_regions.clear();
    tab->cached_display_list.hit_regions.shrink_to_fit();
    tab->arena_bytes = 0;
    tab->state = TabState::Dehydrated;
    return true;
}

bool BrowserApp::dehydrate_lru_background_tab() {
    Tab* oldest = nullptr;
    for (auto& t : tabs_) {
        if (t.id == active_tab_id_ || t.state == TabState::Dehydrated) continue;
        if (!oldest || t.last_used_tick < oldest->last_used_tick) oldest = &t;
    }
    if (!oldest) return false;
    return dehydrate_tab(oldest->id);
}

size_t BrowserApp::enforce_memory_budget() {
    auto res = governor_.enforce(
        [this]() {
            for (auto& t : tabs_) {
                if (t.id != active_tab_id_ && t.state == TabState::Warm) {
                    t.live_doc.reset();
                    t.js_vm.reset();
                    if (t.live_arena) {
                        t.live_arena->reset(true);
                        t.arena_bytes = 0;
                    }
                }
            }
        },
        [this]() { return dehydrate_lru_background_tab(); });
    return res.tabs_dehydrated;
}

void BrowserApp::scroll_active_tab(float delta_y) {
    Tab* active = find_tab(active_tab_id_);
    if (!active) return;
    float view_h = static_cast<float>(active->cached_display_list.viewport_height);
    float max_scroll = std::max(0.0f, active->cached_display_list.content_height - view_h);
    active->scroll_y = std::clamp(active->scroll_y + delta_y, 0.0f, max_scroll);
}

ClickResult BrowserApp::click_at(int x, int y) {
    ClickResult out;

    // 1. Top Tab Strip (y: 0..38)
    if (y >= 0 && y < 38) {
        address_bar_focused_ = false;
        if (x >= 10 && x <= 82) {
            if (Tab* act = find_tab(active_tab_id_)) {
                navigate(act->id, "astra://home", true);
                out.handled = true;
                out.action = "navigate";
                out.detail = "astra://home";
                return out;
            }
        }
        size_t max_visible = std::min<size_t>(tabs_.size(), 5);
        int tx = 92;
        for (size_t i = 0; i < max_visible; ++i) {
            const int tw = 205;
            if (x >= tx && x < tx + tw) {
                // Per-tab [x] close chip
                if (x >= tx + tw - 26 && x <= tx + tw - 4 && y >= 9 && y <= 31) {
                    uint32_t cid = tabs_[i].id;
                    std::string ctitle = tabs_[i].title;
                    close_tab(cid);
                    out.handled = true;
                    out.action = "close_tab";
                    out.detail = ctitle;
                    return out;
                }
                switch_tab(tabs_[i].id);
                out.handled = true;
                out.action = "switch_tab";
                out.detail = tabs_[i].title;
                return out;
            }
            tx += tw + 8;
        }
        // New Tab [+] button
        if (x >= tx && x <= tx + 32) {
            uint32_t nid = new_tab("astra://home");
            out.handled = true;
            out.action = "new_tab";
            out.detail = "tab#" + std::to_string(nid);
            return out;
        }
        return out;
    }

    // 2. Navigation Bar (y: 38..84)
    if (y >= 38 && y < Framebuffer::kChromeHeight) {
        if (x >= 14 && x <= 42) {
            if (go_back()) {
                out.handled = true;
                out.action = "back";
                out.detail = active_tab() ? active_tab()->url : "";
            }
            return out;
        }
        if (x >= 48 && x <= 76) {
            if (go_forward()) {
                out.handled = true;
                out.action = "forward";
                out.detail = active_tab() ? active_tab()->url : "";
            }
            return out;
        }
        if (x >= 82 && x <= 110) {
            if (reload_active_tab()) {
                out.handled = true;
                out.action = "reload";
                out.detail = active_tab() ? active_tab()->url : "";
            }
            return out;
        }
        // Clickable Editable Omnibox (x: 120..880)
        if (x >= 120 && x <= 880) {
            focus_address_bar(true);
            out.handled = true;
            out.action = "focus_omnibox";
            out.detail = "omnibox";
            return out;
        }
        return out;
    }

    // Clicking inside viewport unfocuses the Omnibox
    address_bar_focused_ = false;

    Tab* active = find_tab(active_tab_id_);
    if (!active) return out;

    const HitRegion* hit = active->cached_display_list.hit_test(
        static_cast<float>(x),
        static_cast<float>(y - Framebuffer::kChromeHeight),
        active->scroll_y);
    if (!hit) return out;

    if (hit->kind == HitRegionKind::Link) {
        std::string next_url = HttpClient::resolve_relative_url(active->url, hit->target);
        navigate(active->id, next_url, true);
        out.handled = true;
        out.action = "navigate";
        out.detail = next_url;
    } else if (hit->kind == HitRegionKind::Download) {
        std::string dl_url = HttpClient::resolve_relative_url(active->url, hit->target);
        DownloadRecord rec = download_url(dl_url);
        out.handled = rec.ok;
        out.action = "download";
        out.detail = rec.ok ? (rec.saved_path + " sha256:" + rec.sha256_hex.substr(0, 12)) : rec.error;
    } else if (hit->kind == HitRegionKind::Audio) {
        std::string path = hit->target;
        std::string resolved = HttpClient::resolve_relative_url(active->url, hit->target);
        if (resolved.rfind("file://", 0) == 0 && std::ifstream(resolved.substr(7))) {
            path = resolved.substr(7);
        } else if (!std::ifstream(path) && std::ifstream("astra/" + path)) {
            path = "astra/" + path;
        }
        DecodedAudioBuffer abuf;
        if (MediaEngine::decode_audio_pcm(path, 2.0, abuf)) {
            auto rep = MediaEngine::play_audio_alsa(abuf);
            out.handled = true;
            out.action = "play_audio";
            out.detail = rep.status + " (" + std::to_string(abuf.sample_rate) + " Hz)";
        }
    } else if (hit->kind == HitRegionKind::Input && active->live_doc) {
        for (DomNode* inp : active->live_doc->query_selector_all("input")) {
            inp->set_attr("data-astra-focused", "0");
        }
        for (DomNode* ta : active->live_doc->query_selector_all("textarea")) {
            ta->set_attr("data-astra-focused", "0");
        }
        DomNode* el = active->live_doc->query_selector(hit->target);
        if (el) {
            el->set_attr("data-astra-focused", "1");
            active->focused_input_selector = hit->target;
            relayout_live_tab(*active);
            out.handled = true;
            out.action = "focus_input";
            out.detail = hit->target;
        }
    } else if (hit->kind == HitRegionKind::Button && active->live_doc && active->js_vm) {
        DomNode* btn = active->live_doc->query_selector(hit->target);
        if (btn) {
            auto rep = active->js_vm->dispatch_event(btn, "click", *active->live_doc);
            for (const auto& l : rep.logs) active->console_logs.push_back(l);
            relayout_live_tab(*active);
            out.handled = true;
            out.action = "js_click";
            out.detail = hit->target;
        }
    } else if (hit->kind == HitRegionKind::Scene3D) {
        cam3d_.yaw += 0.45f;
        time3d_sec_ += 0.35f;
        out.handled = true;
        out.action = "orbit_3d";
        out.detail = hit->target;
    } else if (hit->kind == HitRegionKind::Video) {
        float local_y = static_cast<float>(y - Framebuffer::kChromeHeight) +
                        (hit->fixed_viewport ? 0.0f : active->scroll_y) - hit->rect.y;
        float local_x = static_cast<float>(x) - hit->rect.x;
        if (local_y >= hit->rect.h - 42.0f && local_x >= 44.0f && hit->rect.w > 64.0f) {
            float frac = std::clamp((local_x - 44.0f) / (hit->rect.w - 60.0f), 0.0f, 1.0f);
            video_frame_idx_ = static_cast<uint32_t>(std::round(frac * std::max<uint32_t>(1, last_video_total_frames_ - 1)));
            out.handled = true;
            out.action = "seek_video";
            out.detail = "frame=" + std::to_string(video_frame_idx_);
        } else {
            video_frame_idx_ = (video_frame_idx_ + 6) % std::max<uint32_t>(1, last_video_total_frames_);
            out.handled = true;
            out.action = "step_video";
            out.detail = "frame=" + std::to_string(video_frame_idx_);
        }
    }
    return out;
}

JsFetchResult BrowserApp::perform_js_fetch(const std::string& base_url,
                                           const std::string& url,
                                           const std::string& method,
                                           const std::string& body) {
    JsFetchResult out;
    std::string resolved = HttpClient::resolve_relative_url(base_url, url);
    if (resolved.rfind("file://", 0) == 0) {
        std::string path = resolved.substr(7);
        std::string fbody;
        if (read_local_file(path, fbody)) {
            out.ok = true;
            out.status = 200;
            out.body = std::move(fbody);
        } else {
            out.status = 404;
            out.error = "File not found: " + path;
        }
        return out;
    }
    ParsedUrl base_p = parse_url(base_url);
    std::string top_site = base_p.valid ? base_p.registrable_domain() : "";
    HttpResponse resp = (method == "POST" || method == "post")
                            ? http_.post(resolved, body, "application/json", top_site)
                            : http_.get(resolved, top_site, false);
    out.status = resp.status_code;
    out.body = std::move(resp.body);
    out.error = resp.error;
    out.from_cache = resp.from_cache;
    out.blocked_by_shield = resp.blocked_by_privacy || resp.blocked_by_extension;
    out.ok = (!out.blocked_by_shield && resp.status_code >= 200 && resp.status_code < 400);
    return out;
}

bool BrowserApp::type_text(const std::string& text) {
    if (address_bar_focused_) {
        address_bar_buffer_ += text;
        return true;
    }
    Tab* active = find_tab(active_tab_id_);
    if (!active || !active->live_doc || active->focused_input_selector.empty()) return false;
    DomNode* el = active->live_doc->query_selector(active->focused_input_selector);
    if (!el) return false;
    std::string cur = el->get_attr("value");
    cur += text;
    el->set_attr("value", cur);
    if (active->js_vm) {
        auto rep = active->js_vm->dispatch_event(el, "input", *active->live_doc);
        for (const auto& l : rep.logs) active->console_logs.push_back(l);
    }
    relayout_live_tab(*active);
    return true;
}

bool BrowserApp::backspace() {
    if (address_bar_focused_) {
        if (!address_bar_buffer_.empty()) address_bar_buffer_.pop_back();
        return true;
    }
    Tab* active = find_tab(active_tab_id_);
    if (!active || !active->live_doc || active->focused_input_selector.empty()) return false;
    DomNode* el = active->live_doc->query_selector(active->focused_input_selector);
    if (!el) return false;
    std::string cur = el->get_attr("value");
    if (!cur.empty()) cur.pop_back();
    el->set_attr("value", cur);
    if (active->js_vm) {
        auto rep = active->js_vm->dispatch_event(el, "input", *active->live_doc);
        for (const auto& l : rep.logs) active->console_logs.push_back(l);
    }
    relayout_live_tab(*active);
    return true;
}

bool BrowserApp::press_enter() {
    if (address_bar_focused_) {
        std::string target = normalize_omnibox_url(address_bar_buffer_);
        address_bar_focused_ = false;
        return navigate(active_tab_id_, target, true).ok;
    }
    Tab* active = find_tab(active_tab_id_);
    if (!active || !active->live_doc || active->focused_input_selector.empty()) return false;
    DomNode* el = active->live_doc->query_selector(active->focused_input_selector);
    if (!el) return false;
    if (active->js_vm) {
        auto rep = active->js_vm->dispatch_event(el, "submit", *active->live_doc);
        for (const auto& l : rep.logs) active->console_logs.push_back(l);
    }
    relayout_live_tab(*active);
    return true;
}

bool BrowserApp::decode_video_via_rdd(const std::string& path, uint32_t frame_idx, DecodedVideoFrame& out) {
    if (media_worker_.alive()) {
        IpcMessage req{IpcMsgType::DecodeVideoReq, next_req_id_++, {}};
        write_str(req.payload, path);
        write_u32(req.payload, frame_idx);
        if (media_worker_.channel().send(req)) {
            IpcMessage resp;
            if (media_worker_.channel().recv(resp) && resp.type == IpcMsgType::DecodeVideoResp) {
                size_t off = 0;
                uint32_t ok_flag = 0, w = 0, h = 0, idx = 0, total = 0, audio_flag = 0, bytes_len = 0;
                float pts = 0.0f, dur = 0.0f;
                std::string label;
                if (read_u32(resp.payload, off, ok_flag) && ok_flag == 1 &&
                    read_str(resp.payload, off, label) &&
                    read_u32(resp.payload, off, w) &&
                    read_u32(resp.payload, off, h) &&
                    read_u32(resp.payload, off, idx) &&
                    read_u32(resp.payload, off, total) &&
                    read_f32(resp.payload, off, pts) &&
                    read_f32(resp.payload, off, dur) &&
                    read_u32(resp.payload, off, audio_flag) &&
                    read_u32(resp.payload, off, bytes_len) &&
                    off + bytes_len <= resp.payload.size()) {
                    out.width = static_cast<int>(w);
                    out.height = static_cast<int>(h);
                    out.frame_index = idx;
                    out.total_frames = total;
                    out.pts_sec = pts;
                    out.duration_sec = dur;
                    out.has_audio = (audio_flag != 0);
                    out.codec_label = label;
                    out.argb.resize(bytes_len / sizeof(uint32_t));
                    std::memcpy(out.argb.data(), resp.payload.data() + off, bytes_len);
                    if (total > 0) last_video_total_frames_ = total;
                    return true;
                }
            }
        }
    }
    bool ok = MediaEngine::decode_frame(path, frame_idx, out);
    if (ok && out.total_frames > 0) last_video_total_frames_ = out.total_frames;
    return ok;
}

bool BrowserApp::step_animation_frames(int count, double dt_ms) {
    Tab* active = find_tab(active_tab_id_);
    if (!active || !active->live_doc) return false;

    bool ran_any = false;
    for (int i = 0; i < count; ++i) {
        const bool has_js_raf = active->js_vm && active->js_vm->pending_raf_count() > 0;
        const bool has_css_anim =
            active->timeline && (active->timeline->active_animation_count() > 0 ||
                                 active->timeline->active_transition_count() > 0);
        // Session 8: el mismo reloj alimenta los callbacks de
        // requestAnimationFrame y las animaciones/transiciones declarativas.
        if (!has_js_raf && !has_css_anim) break;

        raf_clock_ms_ += dt_ms;
        bool frame_changed = false;

        if (has_js_raf) {
            auto rep = active->js_vm->run_animation_frame(raf_clock_ms_, *active->live_doc);
            for (const auto& l : rep.logs) active->console_logs.push_back(l);
            if (rep.raf_run > 0) frame_changed = true;
        }
        if (active->timeline && active->timeline->advance(raf_clock_ms_)) {
            frame_changed = true;
        }
        if (frame_changed) {
            // Recalcular estilo + layout en cada fotograma: así las
            // transiciones que arrancan a mitad de la animación se registran
            // con el reloj correcto.
            relayout_live_tab(*active);
            ran_any = true;
        }
    }
    return ran_any;
}

bool BrowserApp::paint_active_tab(Framebuffer& fb) {
    const Tab* active = find_tab(active_tab_id_);
    if (!active) return false;

    ChromeStatus cs;
    cs.active_url = active->url;
    cs.active_title = active->title;
    cs.tls_verified = active->tls_verified;
    cs.can_go_back = (active->history_index > 0);
    cs.can_go_forward = (active->history_index >= 0 &&
                         static_cast<size_t>(active->history_index + 1) < active->history.size());
    cs.address_bar_focused = address_bar_focused_;
    cs.address_bar_text = address_bar_buffer_;

    for (size_t i = 0; i < tabs_.size(); ++i) {
        std::string ttl = tabs_[i].title;
        if (tabs_[i].state == TabState::Dehydrated) ttl = "[Z] " + ttl;
        cs.tab_titles.push_back(std::move(ttl));
        if (tabs_[i].id == active_tab_id_) cs.active_tab_index = i;
    }
    auto mem = memory_snapshot();
    cs.rss_mb = mem.total_rss_bytes / (1024 * 1024);
    cs.ceiling_mb = mem.ceiling_bytes / (1024 * 1024);
    cs.trackers_blocked = shield_.stats().trackers_blocked + extensions_.dnr_blocks_count();
    cs.extensions_loaded = extensions_.extensions().size();

    int content_y = fb.draw_browser_chrome(cs);

    std::string base_url = active->url;
    fb.composite_display_list(
        active->cached_display_list, content_y, active->scroll_y,
        [this, base_url](const std::string& src, VideoBlitFrame& out) -> bool {
            std::string path = src;
            std::string resolved = HttpClient::resolve_relative_url(base_url, src);
            if (resolved.rfind("file://", 0) == 0 && std::ifstream(resolved.substr(7))) {
                path = resolved.substr(7);
            } else if (!std::ifstream(path) && std::ifstream("astra/" + path)) {
                path = "astra/" + path;
            }
            DecodedVideoFrame df;
            if (!decode_video_via_rdd(path, video_frame_idx_, df)) return false;
            out.width = df.width;
            out.height = df.height;
            out.argb = std::move(df.argb);
            out.label = df.codec_label;
            uint32_t total = std::max<uint32_t>(1, df.total_frames > 0 ? df.total_frames : 48);
            out.progress = static_cast<float>(df.frame_index) / static_cast<float>(std::max<uint32_t>(1, total - 1));
            out.has_audio = df.has_audio;
            out.time_label = format_mmss(df.pts_sec) + " / " + format_mmss(df.duration_sec > 0.0 ? df.duration_sec : 2.0) +
                             (df.has_audio ? " [AUD]" : "");
            return true;
        },
        [this, base_url](const std::string& src, DecodedImage& out) -> bool {
            return load_image_subresource(base_url, src, out);
        },
        cam3d_, time3d_sec_,
        [this, base_url](const std::string& src, AudioBlitCard& out) -> bool {
            std::string path = src;
            std::string resolved = HttpClient::resolve_relative_url(base_url, src);
            if (resolved.rfind("file://", 0) == 0 && std::ifstream(resolved.substr(7))) {
                path = resolved.substr(7);
            } else if (!std::ifstream(path) && std::ifstream("astra/" + path)) {
                path = "astra/" + path;
            }
            auto it = audio_cache_.find(path);
            if (it != audio_cache_.end()) {
                out = it->second;
                return out.valid;
            }
            DecodedAudioBuffer abuf;
            if (!MediaEngine::decode_audio_pcm(path, 2.0, abuf) || !abuf.valid) return false;
            AudioBlitCard card;
            card.valid = true;
            size_t sl = path.rfind('/');
            card.label = (sl != std::string::npos) ? path.substr(sl + 1) : path;
            double rms_db = (abuf.rms_level > 1e-6) ? (20.0 * std::log10(abuf.rms_level)) : -60.0;
            std::ostringstream meta;
            meta << abuf.sample_rate << " Hz | " << abuf.channels << "ch | "
                 << std::fixed << std::setprecision(1) << abuf.duration_sec << "s | RMS "
                 << std::setprecision(1) << rms_db << " dB | "
                 << (MediaEngine::alsa_available() ? "ALSA Ready" : "PCM Decoded");
            card.meta = meta.str();
            card.waveform_peaks = MediaEngine::compute_waveform_peaks(abuf, 40);
            if (audio_cache_.size() > 16) audio_cache_.clear();
            audio_cache_[path] = card;
            out = std::move(card);
            return true;
        },
        [active](const std::string& id, CanvasBlitFrame& out) -> bool {
            if (!active || !active->js_vm) return false;
            const CanvasSurface* surf = active->js_vm->find_canvas(id);
            if (!surf || surf->argb.empty()) return false;
            out.width = surf->width;
            out.height = surf->height;
            out.argb = surf->argb.data();
            return true;
        });

    return true;
}

bool BrowserApp::render_active_tab(const std::string& out_png_path,
                                   int width, int height,
                                   bool open_x11_window) {
    Framebuffer fb(width, height);
    if (!paint_active_tab(fb)) return false;

    bool saved = true;
    if (!out_png_path.empty()) {
        saved = fb.save_png(out_png_path);
    }
    if (open_x11_window) {
        const Tab* active = find_tab(active_tab_id_);
        InteractiveCallbacks cbs;
        cbs.on_click = [this](int x, int y) { click_at(x, y); };
        cbs.on_scroll = [this](int dy) { scroll_active_tab(static_cast<float>(dy)); };
        cbs.on_text = [this](const std::string& t) { type_text(t); };
        cbs.on_backspace = [this]() { backspace(); };
        cbs.on_enter = [this]() { press_enter(); };
        cbs.on_tick = [this]() {
            if (step_animation_frames(1)) {
                time3d_sec_ += 0.03f;
            }
        };
        cbs.on_repaint = [this](Framebuffer& f) { paint_active_tab(f); };
        fb.present_x11_interactive("Astra Browser — " + (active ? active->title : "Desktop"), cbs);
    }
    return saved;
}

MemorySnapshot BrowserApp::memory_snapshot() const {
    return governor_.sample();
}

} // namespace astra
