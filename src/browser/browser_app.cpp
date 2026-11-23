#include "browser/browser_app.hpp"
#include "dom/html_parser.hpp"
#include "style/css_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
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

std::string build_home_html(const AstraShield& shield, size_t ext_count,
                            const MemorySnapshot& mem, const CacheStats& cstats) {
    std::ostringstream html;
    html << "<!doctype html><html><head><title>Astra Browser v2 — Native Core</title>"
         << "<style>"
         << "body { background: #0b0f19; color: #e2e8f0; padding: 20px 26px; }"
         << ".hero { background: #111827; border: 1px solid #1e293b; border-radius: 12px; padding: 18px 22px; margin-bottom: 16px; }"
         << ".kicker { color: #38bdf8; font-size: 12px; font-weight: bold; margin-bottom: 6px; }"
         << ".grid { display: flex; flex-direction: row; gap: 14px; margin-bottom: 16px; }"
         << ".card { background: #131c2e; border: 1px solid #24324a; border-radius: 10px; padding: 14px; }"
         << ".card-title { color: #7dd3fc; font-size: 13px; font-weight: bold; margin-bottom: 6px; }"
         << ".metric { color: #f8fafc; font-size: 20px; font-weight: bold; margin-bottom: 4px; }"
         << ".muted { color: #94a3b8; font-size: 12px; }"
         << ".media-row { display: flex; flex-direction: row; gap: 16px; }"
         << ".panel { background: #111827; border: 1px solid #1f2937; border-radius: 10px; padding: 12px; }"
         << "</style></head><body>"
         << "<div class=\"hero\">"
         << "<div class=\"kicker\">ASTRA v2 NATIVE C++20 | BYTECODE JS VM + GC | STRICT TLS | SMOOTH 3D</div>"
         << "<h1 id=\"headline\">Astra Desktop Browser — Session 2</h1>"
         << "<p>Multi-process e10s architecture (Main + Content + RDD Media) with AstraJS v2 bytecode VM & Mark-and-Sweep GC, bounded 32 MB HTTP disk cache, native PNG/BMP/PPM image decoder, APZ scroll & click hit-testing, smooth Blinn-Phong Astra3D, and bounded-RAM video playback.</p>"
         << "</div>"
         << "<div class=\"grid\">"
         << "<div class=\"card\"><div class=\"card-title\">MEMORY GOVERNOR</div>"
         << "<div class=\"metric\">" << (mem.total_rss_bytes / (1024 * 1024)) << " / " << (mem.ceiling_bytes / (1024 * 1024)) << " MB</div>"
         << "<div class=\"muted\">Real Linux /proc RSS + Cache Hits: " << cstats.hits << "</div></div>"
         << "<div class=\"card\"><div class=\"card-title\">ASTRA SHIELD PRIVACY</div>"
         << "<div class=\"metric\">" << shield.blocklist_size() << " Rules | " << shield.stats().trackers_blocked << " Blocked</div>"
         << "<div class=\"muted\">Total Cookie Partitioning + Strict TLS + GPC</div></div>"
         << "<div class=\"card\"><div class=\"card-title\">WEBEXTENSIONS MV3</div>"
         << "<div class=\"metric\">" << ext_count << " Active</div>"
         << "<div class=\"muted\">DeclarativeNetRequest + AstraJS v2 Bytecode VM</div></div>"
         << "</div>"
         << "<div class=\"media-row\">"
         << "<div class=\"panel\"><div class=\"card-title\">ASTRA3D v2 SMOOTH SHADER ENGINE</div>"
         << "<astra-3d scene=\"planet\" style=\"height: 250px;\"></astra-3d></div>"
         << "<div class=\"panel\"><div class=\"card-title\">ASTRAMEDIA RDD REAL VIDEO PIPELINE</div>"
         << "<video src=\"examples/astra_demo.mp4\" style=\"height: 250px;\"></video></div>"
         << "</div>"
         << "</body></html>";
    return html.str();
}

} // namespace

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
    if (!url.empty()) {
        navigate(active_tab_id_, url);
    }
    return active_tab_id_;
}

std::string BrowserApp::resolve_source_html(const std::string& url,
                                            std::string& final_url,
                                            bool& tls_verified_out,
                                            bool& from_cache_out,
                                            std::string& error_out) {
    final_url = url;
    tls_verified_out = false;
    from_cache_out = false;

    if (url == "astra://home" || url == "astra://start") {
        return build_home_html(shield_, extensions_.extensions().size(), memory_snapshot(), cache_.stats());
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
        std::string text;
        if (!read_local_file(path, text)) { error_out = "Cannot open local file: " + path; return {}; }
        return text;
    }

    HttpResponse resp = http_.get(url, "", true);
    final_url = resp.final_url.empty() ? url : resp.final_url;
    tls_verified_out = resp.tls_verified;
    from_cache_out = resp.from_cache;

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
            HttpResponse r = http_.get(resolved, top_site, false);
            if (!r.blocked_by_privacy && !r.blocked_by_extension && r.status_code == 200) {
                combined += "\n" + r.body;
                ++loaded_count;
            }
        }
    }
    return combined;
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

void BrowserApp::relayout_live_tab(Tab& tab) {
    if (!tab.live_arena || !tab.live_doc) return;
    CssStylesheet sheet;
    sheet.parse(tab.live_doc->collect_stylesheets());
    if (!tab.extra_css.empty()) sheet.parse(tab.extra_css);
    LayoutFrame* root = LayoutEngine::build_layout(*tab.live_arena, *tab.live_doc, sheet, 1280.0f, 636.0f);
    tab.cached_display_list = LayoutEngine::build_display_list(root, 1280, 636);
    tab.arena_bytes = tab.live_arena->bytes_used();
}

NavigationResult BrowserApp::navigate(uint32_t tab_id, const std::string& url) {
    auto t0 = std::chrono::steady_clock::now();
    NavigationResult res;
    res.tab_id = tab_id;

    Tab* tab = find_tab(tab_id);
    if (!tab) {
        res.error = "Tab not found";
        return res;
    }

    std::string final_url, err;
    bool tls_ok = false, cached_hit = false;
    std::string html = resolve_source_html(url, final_url, tls_ok, cached_hit, err);
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

    // Load external <link rel="stylesheet"> subresources through AstraShield + HttpCache
    size_t ext_css_count = 0;
    std::string subresource_css = fetch_external_stylesheets(*tab->live_doc, final_url, ext_css_count);

    // Execute page <script> blocks via AstraJS v2 Bytecode VM
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
    if (!res.rendered_via_ipc) {
        CssStylesheet sheet;
        sheet.parse(tab->live_doc->collect_stylesheets());
        if (!all_extra_css.empty()) sheet.parse(all_extra_css);
        LayoutFrame* root = LayoutEngine::build_layout(*tab->live_arena, *tab->live_doc, sheet, 1280.0f, 636.0f);
        dl = LayoutEngine::build_display_list(root, 1280, 636);
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
    target->last_used_tick = ++tick_counter_;

    if (target->state == TabState::Dehydrated) {
        target->state = TabState::Active;
        navigate(tab_id, target->url);
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
    // 1. Tab strip clicks
    if (y >= 0 && y < 38 && x >= 88) {
        size_t idx = static_cast<size_t>((x - 88) / 203);
        if (idx < tabs_.size()) {
            switch_tab(tabs_[idx].id);
            out.handled = true;
            out.action = "switch_tab";
            out.detail = tabs_[idx].title;
            return out;
        }
    }

    Tab* active = find_tab(active_tab_id_);
    if (!active || y < Framebuffer::kChromeHeight) return out;

    const HitRegion* hit = active->cached_display_list.hit_test(
        static_cast<float>(x),
        static_cast<float>(y - Framebuffer::kChromeHeight),
        active->scroll_y);
    if (!hit) return out;

    if (hit->kind == HitRegionKind::Link) {
        std::string next_url = HttpClient::resolve_relative_url(active->url, hit->target);
        navigate(active->id, next_url);
        out.handled = true;
        out.action = "navigate";
        out.detail = next_url;
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
        video_frame_idx_ += 6;
        out.handled = true;
        out.action = "step_video";
        out.detail = "frame=" + std::to_string(video_frame_idx_);
    }
    return out;
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
                uint32_t ok_flag = 0, w = 0, h = 0, idx = 0, bytes_len = 0;
                std::string label;
                if (read_u32(resp.payload, off, ok_flag) && ok_flag == 1 &&
                    read_str(resp.payload, off, label) &&
                    read_u32(resp.payload, off, w) &&
                    read_u32(resp.payload, off, h) &&
                    read_u32(resp.payload, off, idx) &&
                    read_u32(resp.payload, off, bytes_len) &&
                    off + bytes_len <= resp.payload.size()) {
                    out.width = static_cast<int>(w);
                    out.height = static_cast<int>(h);
                    out.frame_index = idx;
                    out.codec_label = label;
                    out.argb.resize(bytes_len / sizeof(uint32_t));
                    std::memcpy(out.argb.data(), resp.payload.data() + off, bytes_len);
                    return true;
                }
            }
        }
    }
    return MediaEngine::decode_frame(path, frame_idx, out);
}

bool BrowserApp::paint_active_tab(Framebuffer& fb) {
    const Tab* active = find_tab(active_tab_id_);
    if (!active) return false;

    ChromeStatus cs;
    cs.active_url = active->url;
    cs.active_title = active->title;
    cs.tls_verified = active->tls_verified;
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
        [this](const std::string& src, VideoBlitFrame& out) -> bool {
            DecodedVideoFrame df;
            if (!decode_video_via_rdd(src, video_frame_idx_, df)) return false;
            out.width = df.width;
            out.height = df.height;
            out.argb = std::move(df.argb);
            out.label = df.codec_label;
            return true;
        },
        [this, base_url](const std::string& src, DecodedImage& out) -> bool {
            return load_image_subresource(base_url, src, out);
        },
        cam3d_, time3d_sec_);

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
        cbs.on_repaint = [this](Framebuffer& f) { paint_active_tab(f); };
        fb.present_x11_interactive("Astra Browser — " + (active ? active->title : "Desktop"), cbs);
    }
    return saved;
}

MemorySnapshot BrowserApp::memory_snapshot() const {
    return governor_.sample();
}

} // namespace astra
