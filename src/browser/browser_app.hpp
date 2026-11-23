#pragma once

#include "core/arena.hpp"
#include "core/memory_governor.hpp"
#include "docs/doc_viewer.hpp"
#include "ext/extension_runtime.hpp"
#include "gfx/astra3d.hpp"
#include "gfx/image_decoder.hpp"
#include "gfx/rasterizer.hpp"
#include "ipc/ipc_channel.hpp"
#include "js/astra_js.hpp"
#include "layout/layout_engine.hpp"
#include "media/media_engine.hpp"
#include "net/http_cache.hpp"
#include "net/http_client.hpp"
#include "privacy/shield.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace astra {

enum class TabState {
    Active,
    Warm,
    Dehydrated
};

struct Tab {
    uint32_t id = 0;
    std::string url;
    std::string title;
    std::string raw_html;
    std::string extra_css;
    TabState state = TabState::Active;
    uint64_t last_used_tick = 0;
    size_t dom_nodes = 0;
    size_t arena_bytes = 0;
    float scroll_y = 0.0f;
    bool tls_verified = false;
    bool from_cache = false;
    std::vector<std::string> history;
    int history_index = -1;
    std::unique_ptr<SlabArena> live_arena;
    std::unique_ptr<HtmlDocument> live_doc;
    std::unique_ptr<AstraJsEngine> js_vm;
    DisplayList cached_display_list;
    std::string focused_input_selector;
    std::vector<std::string> console_logs;
};

struct NavigationResult {
    bool ok = false;
    uint32_t tab_id = 0;
    std::string final_url;
    std::string title;
    std::string error;
    size_t display_items = 0;
    size_t hit_regions = 0;
    size_t ipc_blob_bytes = 0;
    size_t arena_bytes = 0;
    size_t external_css_loaded = 0;
    double elapsed_ms = 0.0;
    bool rendered_via_ipc = false;
    bool tls_verified = false;
    bool from_cache = false;
};

struct ClickResult {
    bool handled = false;
    std::string action; // "switch_tab", "new_tab", "close_tab", "focus_omnibox", "back", "forward", "reload", "navigate", "focus_input", "js_click", "orbit_3d", "seek_video", "step_video"
    std::string detail;
};

// Astra v4 Multi-Process Desktop Browser Coordinator:
// - Main + Content + RDD Media workers over AF_UNIX IPC
// - Hard <= 2 GB RAM governor + LRU Tab Dehydration + 32 MiB HttpCache
// - Interactive Chrome v4 (Omnibox, Back/Forward/Reload, [+]/[x] tabs, History stack)
// - Strict TLS + ALPN + parallel batch subresources + AstraShield + Multi-Extension MV3
// - AstraJS v4 Bytecode VM (Promises, GC, fetch GET/POST, JSON, disk-persisted localStorage)
// - CSS Grid + position:absolute/fixed + z-index + box-shadow + AstraFont + JPEG/PNG + Astra3D + AstraMedia v3
class BrowserApp {
public:
    explicit BrowserApp(uint64_t ram_ceiling_bytes = MemoryGovernor::kDefaultCeilingBytes);
    ~BrowserApp();

    bool start_workers();
    void stop_workers();

    bool load_extension(const std::string& dir_path, std::string* error = nullptr);

    uint32_t new_tab(const std::string& url = "astra://home");
    bool close_tab(uint32_t tab_id);
    NavigationResult navigate(uint32_t tab_id, const std::string& url, bool push_history = true);
    bool go_back();
    bool go_forward();
    bool reload_active_tab();
    bool switch_tab(uint32_t tab_id);
    bool dehydrate_tab(uint32_t tab_id);
    bool dehydrate_lru_background_tab();
    size_t enforce_memory_budget();

    // Interactive APZ scroll + hit-testing click + Omnibox & form keyboard input
    void scroll_active_tab(float delta_y);
    ClickResult click_at(int x, int y);
    void focus_address_bar(bool clear_text = false);
    bool address_bar_focused() const { return address_bar_focused_; }
    const std::string& address_bar_text() const { return address_bar_buffer_; }
    bool type_text(const std::string& text);
    bool backspace();
    bool press_enter();
    void set_3d_camera(const Camera3D& cam) { cam3d_ = cam; }
    void set_video_frame(uint32_t idx) { video_frame_idx_ = idx; }
    uint32_t video_frame() const { return video_frame_idx_; }

    bool paint_active_tab(Framebuffer& fb);
    bool render_active_tab(const std::string& out_png_path,
                           int width = 1280, int height = 720,
                           bool open_x11_window = false);

    bool decode_video_via_rdd(const std::string& path, uint32_t frame_idx, DecodedVideoFrame& out);
    bool load_image_subresource(const std::string& base_url, const std::string& src, DecodedImage& out);

    MemorySnapshot memory_snapshot() const;
    const AstraShield& shield() const { return shield_; }
    AstraShield& shield_mut() { return shield_; }
    const HttpCache& cache() const { return cache_; }
    HttpClient& http_client() { return http_; }
    const ExtensionRuntime& extensions() const { return extensions_; }
    const std::vector<Tab>& tabs() const { return tabs_; }
    uint32_t active_tab_id() const { return active_tab_id_; }
    const Tab* active_tab() const { return find_tab(active_tab_id_); }

    pid_t content_pid() const { return content_worker_.pid(); }
    pid_t media_pid() const { return media_worker_.pid(); }

private:
    Tab* find_tab(uint32_t id);
    const Tab* find_tab(uint32_t id) const;
    void relayout_live_tab(Tab& tab);
    std::string resolve_source_html(const std::string& url,
                                    std::string& final_url,
                                    bool& tls_verified_out,
                                    bool& from_cache_out,
                                    std::string& error_out);
    std::string fetch_external_stylesheets(const HtmlDocument& doc,
                                           const std::string& base_url,
                                           size_t& loaded_count);
    void preload_remote_images_parallel(const HtmlDocument& doc, const std::string& base_url);
    JsFetchResult perform_js_fetch(const std::string& base_url,
                                   const std::string& url,
                                   const std::string& method,
                                   const std::string& body);
    std::string origin_storage_get(const std::string& origin_key, const std::string& k);
    void origin_storage_set(const std::string& origin_key, const std::string& k, const std::string& v);
    bool render_html_via_content_ipc(const std::string& html,
                                     const std::string& extra_css,
                                     uint32_t vw, uint32_t vh,
                                     DisplayList& out_dl,
                                     std::string& out_title,
                                     size_t& out_blob_bytes,
                                     size_t& out_arena_bytes);
    void run_content_worker_loop(IpcChannel& ch);
    void run_media_worker_loop(IpcChannel& ch);

    AstraShield shield_;
    HttpCache cache_;
    ExtensionRuntime extensions_;
    HttpClient http_;
    MemoryGovernor governor_;

    WorkerProcess content_worker_;
    WorkerProcess media_worker_;

    std::vector<Tab> tabs_;
    std::unordered_map<std::string, DecodedImage> image_cache_;
    uint32_t next_tab_id_ = 1;
    uint32_t active_tab_id_ = 0;
    uint64_t tick_counter_ = 0;
    uint32_t next_req_id_ = 1;

    bool address_bar_focused_ = false;
    std::string address_bar_buffer_;

    Camera3D cam3d_;
    float time3d_sec_ = 0.72f;
    uint32_t video_frame_idx_ = 12;
    uint32_t last_video_total_frames_ = 48;
};

} // namespace astra
