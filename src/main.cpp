#include "browser/browser_app.hpp"
#include "core/arena.hpp"
#include "core/json.hpp"
#include "core/memory_governor.hpp"
#include "docs/doc_viewer.hpp"
#include "ext/extension_runtime.hpp"
#include "gfx/astra3d.hpp"
#include "gfx/image_decoder.hpp"
#include "gfx/rasterizer.hpp"
#include "ipc/ipc_channel.hpp"
#include "js/astra_js.hpp"
#include "media/media_engine.hpp"
#include "net/http_cache.hpp"
#include "net/http_client.hpp"
#include "privacy/shield.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace astra;

namespace {

int g_tests_passed = 0;
int g_tests_failed = 0;

void expect(bool cond, const std::string& label) {
    if (cond) {
        ++g_tests_passed;
        std::cout << "  [PASS] " << label << "\n";
    } else {
        ++g_tests_failed;
        std::cerr << "  [FAIL] " << label << "\n";
    }
}

int run_full_test_suite() {
    std::cout << "=== Astra Browser Core v2 — Full Subsystem Verification Suite ===\n\n";

    // 1. SlabArena + OS Reclamation
    {
        SlabArena arena(64 * 1024);
        for (int i = 0; i < 4000; ++i) (void)arena.create<int>(i);
        size_t before = arena.bytes_used();
        arena.reset(true);
        expect(before >= 16000 && arena.bytes_used() == 0 && arena.slab_count() == 0,
               "SlabArena allocates cleanly and releases OS slabs via munmap/madvise");
    }

    // 2. JSON Parser
    {
        JsonValue v;
        std::string err;
        bool ok = JsonValue::parse(R"({"name":"Astra","mv":3,"perms":["storage","declarativeNetRequest"],"active":true})", v, &err);
        expect(ok && v.get_string("name") == "Astra" && v.get_number("mv") == 3 &&
               v.get_bool("active") && v.get("perms")->as_array().size() == 2,
               "RFC 8259 JSON parser parses Manifest V3 structures");
    }

    // 3. AstraShield Privacy Engine
    {
        AstraShield shield;
        auto d1 = shield.inspect_request("http://example.com/article?id=42&utm_source=ads&fbclid=xyz999", "example.com", true);
        expect(!d1.blocked && d1.upgraded_to_https && d1.params_stripped == 2 &&
               d1.sanitized_url == "https://example.com/article?id=42",
               "AstraShield upgrades HTTP->HTTPS and strips utm_source/fbclid params");

        auto d2 = shield.inspect_request("https://stats.doubleclick.net/pixel.js", "example.com", false);
        expect(d2.blocked && d2.category == TrackerCategory::Advertising,
               "AstraShield suffix-trie blocks subdomain trackers (stats.doubleclick.net)");

        shield.store_cookie("site-a.com", "https://cdn.widget.net", "uid=ALPHA; Secure; HttpOnly");
        std::string on_a = shield.cookie_header_for("site-a.com", "https://cdn.widget.net", true);
        std::string on_b = shield.cookie_header_for("site-b.com", "https://cdn.widget.net", true);
        expect(on_a == "uid=ALPHA" && on_b.empty(),
               "Total Cookie Protection isolates third-party cookies across top-level sites");
    }

    // 4. Bounded RAM+Disk HttpCache
    {
        HttpCache cache("/tmp/astra_test_cache", 2048);
        cache.store({"https://astra.test/a", "text/css", "\"e1\"", std::string(1000, 'A'), 0, 600});
        cache.store({"https://astra.test/b", "text/css", "\"e2\"", std::string(1000, 'B'), 0, 600});
        cache.store({"https://astra.test/c", "text/css", "\"e3\"", std::string(1000, 'C'), 0, 600});
        CachedResource hit;
        bool found_c = cache.lookup("https://astra.test/c", hit);
        expect(found_c && hit.body.size() == 1000 && cache.stats().evictions >= 1 && cache.stats().current_bytes <= 2048,
               "HttpCache stores to RAM+Disk and enforces bounded LRU byte eviction");
    }

    // 5. Native PNG Encoder + Native PNG Decoder roundtrip (all zero external build deps)
    {
        Framebuffer fb(64, 48);
        fb.fill_rect(0, 0, 64, 48, 0xFF112233u);
        fb.fill_rect(10, 10, 20, 20, 0xFF38BDF8u);
        bool saved = fb.save_png("/tmp/astra_roundtrip_test.png");
        DecodedImage dec = ImageDecoder::decode_file("/tmp/astra_roundtrip_test.png");
        expect(saved && dec.valid && dec.width == 64 && dec.height == 48 &&
               dec.argb[15 * 64 + 15] == 0xFF38BDF8u && dec.argb[0] == 0xFF112233u,
               "AstraImage native PNG decoder roundtrips exact ARGB32 pixels cleanly");
    }

    // 6. HTML5 + CSS3 + Layout + HitRegions + Binary DisplayList v2
    {
        SlabArena arena;
        std::string html =
            "<!doctype html><html><head><title>Astra v2</title>"
            "<style>.box { background: #1e293b; padding: 12px; border-radius: 8px; }</style>"
            "</head><body><div class=\"box\"><a href=\"astra://3d\">Open 3D</a><button id=\"go\">Click</button></div></body></html>";
        HtmlDocument doc = HtmlDocument::parse(arena, html);
        CssStylesheet sheet;
        sheet.parse(doc.collect_stylesheets());
        LayoutFrame* root = LayoutEngine::build_layout(arena, doc, sheet, 1024, 600);
        DisplayList dl = LayoutEngine::build_display_list(root, 1024, 600);
        auto blob = dl.serialize();
        DisplayList rt;
        bool dec_ok = DisplayList::deserialize(blob, rt);
        expect(dec_ok && rt.items.size() == dl.items.size() && rt.hit_regions.size() == 2,
               "HTML5 -> CSS3 -> Layout -> ASD2 Binary DisplayList + HitRegions roundtrip");
    }

    // 7. AstraJS v2: Bytecode VM (functions, loops, arrays, objects, Mark-and-Sweep GC, Microtasks, Timers, DOM Events)
    {
        SlabArena arena;
        HtmlDocument doc = HtmlDocument::parse(
            arena, "<html><body><h1 id=\"t\">0</h1><button id=\"inc\">+1</button></body></html>");
        AstraJsEngine vm;
        std::map<std::string, std::string> store;
        vm.bind_storage([&](const std::string& k) { return store[k]; },
                        [&](const std::string& k, const std::string& v) { store[k] = v; });

        std::string script =
            "function fib(n) { if (n <= 1) return n; return fib(n - 1) + fib(n - 2); }\n"
            "let sum = 0;\n"
            "let arr = [10, 20, 30];\n"
            "arr.push(40);\n"
            "for (let i = 0; i < arr.length; i = i + 1) { sum = sum + arr[i]; }\n"
            "let f8 = fib(8);\n"
            "let order = \"\";\n"
            "setTimeout(function() { order = order + \"T\"; }, 10);\n"
            "queueMicrotask(function() { order = order + \"M\"; });\n"
            "let garbage = { a: [1, 2, 3], b: { c: 9 } };\n"
            "garbage = null;\n"
            "let clicks = 0;\n"
            "let btn = document.querySelector(\"#inc\");\n"
            "btn.addEventListener(\"click\", function() {\n"
            "  clicks = clicks + 1;\n"
            "  document.querySelector(\"#t\").textContent = \"Clicks:\" + clicks + \" Fib8:\" + f8 + \" Sum:\" + sum;\n"
            "});\n"
            "console.log(\"fib8=\", f8, \"sum=\", sum);\n";

        auto rep = vm.execute(script, doc);
        size_t freed = vm.gc_collect();
        auto click1 = vm.dispatch_event(doc.query_selector("#inc"), "click", doc);
        auto click2 = vm.dispatch_event(doc.query_selector("#inc"), "click", doc);
        (void)click1; (void)click2;

        std::string order = vm.globals().count("order") ? vm.globals().at("order").to_string() : "";
        std::string dom_txt = doc.query_selector("#t")->text_content();
        expect(rep.ok && rep.bytecode_size > 40 && rep.microtasks_run == 1 && rep.timers_run == 1 &&
               order == "MT" && freed >= 3 && dom_txt == "Clicks:2 Fib8:21 Sum:100",
               "AstraJS v2 Bytecode VM executes recursion, loops, arrays, GC sweep, Microtask->Timer order, and DOM click events");
    }

    // 8. Astra3D v2 Smooth Shader Engine
    {
        Framebuffer fb(320, 200);
        Camera3D cam;
        cam.smooth_shading = true;
        cam.wireframe = false;
        auto st = Astra3D::render_scene_into(fb.pixels(), 320, 0, 0, 320, 200, "planet", 0.5f, cam);
        expect(st.vertices > 600 && st.triangles_rasterized > 400,
               "Astra3D v2 smooth-normal planetary shader engine rasterizes high-detail geometry");
    }

    // 9. Native PDF Viewer
    {
        std::string pdf =
            "%PDF-1.4\n"
            "1 0 obj << /Title (Astra Spec) /Author (Astra Team) >> endobj\n"
            "2 0 obj << /Length 44 >> stream\n"
            "BT /F1 12 Tf 72 720 Td (Astra Native PDF Works!) Tj ET\n"
            "endstream endobj\n%%EOF\n";
        ParsedDocument d = DocViewer::parse_pdf_bytes(pdf);
        expect(d.ok && d.title == "Astra Spec" && d.page_count == 1 &&
               d.pages[0].find("Astra Native PDF Works!") != std::string::npos,
               "AstraDocs parses real PDF header, metadata, and BT/ET/Tj text streams natively");
    }

    // 10. AstraMedia v2 Bounded Playback Session (multi-frame MP4 decode)
    {
        MediaPlaybackSession sess;
        std::string mp4 = "astra/examples/astra_demo.mp4";
        if (!std::ifstream(mp4)) mp4 = "examples/astra_demo.mp4";
        bool opened = sess.open(mp4);
        bool batched = opened && sess.decode_batch(0, 6);
        DecodedVideoFrame f0, f1;
        bool got2 = batched && sess.next_frame(f0) && sess.next_frame(f1);
        expect(got2 && f0.frame_index == 0 && f1.frame_index == 1 &&
               sess.buffered_bytes() <= MediaPlaybackSession::kMaxBufferBytes,
               "AstraMedia v2 decodes multi-frame H.264 MP4 sequence within 16 MiB ring buffer cap");
    }

    // 11. Multi-Process IPC + Strict TLS + Subresource Loader + Interactive Click/Scroll
    {
        std::error_code ec;
        std::filesystem::remove_all("/tmp/astra_http_cache", ec);

        BrowserApp app(2ULL * 1024 * 1024 * 1024);
        bool started = app.start_workers();
        expect(started && app.content_pid() > 0 && app.media_pid() > 0,
               "Multi-process architecture spawns isolated Content and RDD Media workers over AF_UNIX IPC");

        uint32_t tab1 = app.new_tab("astra://home");
        auto nav = app.navigate(tab1, "astra://home");
        expect(nav.ok && nav.rendered_via_ipc && nav.display_items > 10 && nav.hit_regions >= 2,
               "Content Process renders page into ASD2 DisplayList + HitRegions over IPC");

        // Test live strict-verified HTTPS + HttpCache hit on second fetch
        auto https1 = app.navigate(tab1, "https://example.com");
        auto https2 = app.navigate(tab1, "https://example.com");
        expect(https1.ok && https1.tls_verified && !https1.from_cache && https2.ok && https2.from_cache,
               "AstraNet enforces strict TLS certificate+hostname verification and hits HttpCache on repeat load");

        auto mem = app.memory_snapshot();
        expect(mem.total_rss_bytes > 0 && mem.total_rss_bytes < 256ULL * 1024 * 1024 && mem.processes.size() >= 3,
               "MemoryGovernor tracks real Linux /proc RSS across Main + Content + RDD workers");

        app.stop_workers();
    }

    std::cout << "\nSummary: " << g_tests_passed << " passed, " << g_tests_failed << " failed.\n";
    return g_tests_failed == 0 ? 0 : 1;
}

int run_stress_test(int tab_count, uint64_t ceiling_mb) {
    std::cout << "=== Astra v2 Multi-Tab Stress Test (" << tab_count << " tabs, ceiling " << ceiling_mb << " MB) ===\n";
    BrowserApp app(ceiling_mb * 1024ULL * 1024ULL);
    app.start_workers();

    auto t0 = std::chrono::steady_clock::now();

    std::ostringstream payload;
    payload << "<!doctype html><html><head><title>Stress Tab</title>"
            << "<style>.item{background:#131c2e;padding:10px;margin:6px;border-radius:6px;}</style></head><body>"
            << "<h1>Astra Heavy Tab Stress Benchmark</h1>";
    for (int i = 0; i < 160; ++i) {
        payload << "<div class=\"item\"><h3>Section " << i << "</h3>"
                << "<p>Astra SlabArena + MemoryGovernor stress payload verifying sub-2GB RAM across concurrent tabs.</p></div>";
    }
    payload << "</body></html>";

    const std::string stress_path = "/tmp/astra_stress_page.html";
    {
        std::ofstream f(stress_path);
        f << payload.str();
    }

    for (int i = 0; i < tab_count; ++i) {
        app.new_tab("file://" + stress_path);
        if (i >= 8) app.dehydrate_lru_background_tab();
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    auto mem = app.memory_snapshot();

    size_t active_or_warm = 0, dehydrated = 0;
    for (const auto& t : app.tabs()) {
        if (t.state == TabState::Dehydrated) ++dehydrated;
        else ++active_or_warm;
    }

    std::cout << "  Tabs created:       " << app.tabs().size() << "\n";
    std::cout << "  Active/Warm tabs:   " << active_or_warm << "\n";
    std::cout << "  Dehydrated tabs:    " << dehydrated << "\n";
    std::cout << "  Total elapsed:      " << std::fixed << std::setprecision(1) << elapsed_ms << " ms ("
              << (elapsed_ms / std::max(1, tab_count)) << " ms/tab)\n";
    std::cout << "  Total OS RSS:       " << std::setprecision(2)
              << (static_cast<double>(mem.total_rss_bytes) / (1024.0 * 1024.0)) << " MB / "
              << ceiling_mb << " MB hard cap\n";
    for (const auto& p : mem.processes) {
        std::cout << "    - PID " << p.pid << " [" << p.role << "]: "
                  << (static_cast<double>(p.rss_bytes) / (1024.0 * 1024.0)) << " MB RSS\n";
    }

    bool switched = app.switch_tab(1);
    std::cout << "  Rehydrate Tab #1:   " << (switched ? "OK" : "FAILED") << "\n";

    app.stop_workers();
    return (mem.total_rss_bytes < ceiling_mb * 1024ULL * 1024ULL && switched) ? 0 : 1;
}

void print_help() {
    std::cout
        << "Astra Browser Core v2 (Native C++20 Multi-Process Engine)\n"
        << "Usage:\n"
        << "  astra --test                                Run full Session 2 subsystem suite\n"
        << "  astra --url <URL> [--out <file.png>]        Load URL & render PNG\n"
        << "        [--ext <extension_dir>]               Load Manifest V3 WebExtension\n"
        << "        [--scroll <px>] [--click <x,y>]       Simulate APZ scroll or mouse click before render\n"
        << "        [--video-frame <N>] [--wireframe]     Select video frame or 3D wireframe mode\n"
        << "        [--gui]                               Open interactive Linux X11 desktop window\n"
        << "  astra --stress-tabs <N> [--cap <MB>]        Stress-test N tabs with real /proc RSS check\n"
        << "  astra --repl                                Launch interactive Astra shell\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    std::string mode_url, out_png, ext_dir, click_spec;
    bool run_tests = false, open_gui = false, run_repl = false, wireframe = false;
    int stress_tabs = 0;
    float scroll_px = 0.0f;
    uint32_t video_frame = 12;
    uint64_t cap_mb = 2048;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--test") run_tests = true;
        else if (arg == "--gui") open_gui = true;
        else if (arg == "--repl") run_repl = true;
        else if (arg == "--wireframe") wireframe = true;
        else if (arg == "--url" && i + 1 < argc) mode_url = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out_png = argv[++i];
        else if (arg == "--ext" && i + 1 < argc) ext_dir = argv[++i];
        else if (arg == "--click" && i + 1 < argc) click_spec = argv[++i];
        else if (arg == "--scroll" && i + 1 < argc) scroll_px = std::strtof(argv[++i], nullptr);
        else if (arg == "--video-frame" && i + 1 < argc) video_frame = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (arg == "--stress-tabs" && i + 1 < argc) stress_tabs = std::atoi(argv[++i]);
        else if (arg == "--cap" && i + 1 < argc) cap_mb = static_cast<uint64_t>(std::atoll(argv[++i]));
        else if (arg == "--help" || arg == "-h") { print_help(); return 0; }
    }

    if (run_tests) return run_full_test_suite();
    if (stress_tabs > 0) return run_stress_test(stress_tabs, cap_mb);

    BrowserApp app(cap_mb * 1024ULL * 1024ULL);
    app.start_workers();

    if (wireframe) {
        Camera3D cam;
        cam.wireframe = true;
        app.set_3d_camera(cam);
    }
    app.set_video_frame(video_frame);

    if (!ext_dir.empty()) {
        std::string err;
        if (app.load_extension(ext_dir, &err)) {
            std::cout << "[AstraExt] Loaded extension from " << ext_dir << "\n";
        } else {
            std::cerr << "[AstraExt] Failed to load extension: " << err << "\n";
        }
    }

    if (!mode_url.empty()) {
        uint32_t tid = app.new_tab("");
        auto nav = app.navigate(tid, mode_url);
        if (!nav.ok) {
            std::cerr << "[Astra] Navigation failed: " << nav.error << "\n";
            app.stop_workers();
            return 1;
        }

        if (scroll_px != 0.0f) {
            app.scroll_active_tab(scroll_px);
        }
        if (!click_spec.empty()) {
            size_t comma = click_spec.find(',');
            if (comma != std::string::npos) {
                int cx = std::atoi(click_spec.substr(0, comma).c_str());
                int cy = std::atoi(click_spec.substr(comma + 1).c_str());
                auto cr = app.click_at(cx, cy);
                std::cout << "[Astra] Click(" << cx << "," << cy << "): "
                          << (cr.handled ? (cr.action + " -> " + cr.detail) : "no hit") << "\n";
            }
        }

        std::cout << "[Astra] Loaded:       " << nav.final_url << "\n"
                  << "        Title:        " << nav.title << "\n"
                  << "        TLS Verified: " << (nav.tls_verified ? "YES (OS CA Store)" : "Local/N-A") << "\n"
                  << "        Cache Hit:    " << (nav.from_cache ? "YES" : "NO") << "\n"
                  << "        Ext CSS:      " << nav.external_css_loaded << " stylesheet(s)\n"
                  << "        IPC Render:   " << (nav.rendered_via_ipc ? "YES (Content Process)" : "In-Process (JS/Ext mutated)") << "\n"
                  << "        DisplayItems: " << nav.display_items << " | HitRegions: " << nav.hit_regions << "\n"
                  << "        Arena Used:   " << (nav.arena_bytes / 1024) << " KiB | Time: "
                  << std::fixed << std::setprecision(2) << nav.elapsed_ms << " ms\n";

        auto mem = app.memory_snapshot();
        std::cout << "        Total OS RSS: " << std::setprecision(2)
                  << (static_cast<double>(mem.total_rss_bytes) / (1024.0 * 1024.0)) << " MB / "
                  << (mem.ceiling_bytes / (1024 * 1024)) << " MB\n";

        if (!out_png.empty() || open_gui) {
            bool ok = app.render_active_tab(out_png, 1280, 720, open_gui);
            if (!out_png.empty()) {
                std::cout << "        PNG Output:   " << (ok ? out_png : "FAILED") << "\n";
            }
        }
        app.stop_workers();
        return 0;
    }

    if (run_repl) {
        std::cout << "Astra v2 Interactive Shell (open <url>, tabs, switch <id>, scroll <px>, click <x> <y>, mem, shield, cache, render <file.png>, quit)\n";
        app.new_tab("astra://home");
        std::string line;
        while (std::cout << "astra> " && std::getline(std::cin, line)) {
            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;
            if (cmd == "quit" || cmd == "exit") break;
            else if (cmd == "open") {
                std::string u; iss >> u;
                uint32_t id = app.new_tab(u);
                std::cout << "Opened tab #" << id << " -> " << u << "\n";
            } else if (cmd == "tabs") {
                for (const auto& t : app.tabs()) {
                    std::cout << "  #" << t.id << (t.id == app.active_tab_id() ? " * " : "   ")
                              << "[" << (t.state == TabState::Dehydrated ? "DEHYDRATED" : "LIVE") << "] "
                              << t.title << " (" << t.url << ")\n";
                }
            } else if (cmd == "switch") {
                uint32_t id = 0; iss >> id;
                std::cout << (app.switch_tab(id) ? "Switched.\n" : "Not found.\n");
            } else if (cmd == "scroll") {
                float dy = 0; iss >> dy;
                app.scroll_active_tab(dy);
                std::cout << "Scrolled by " << dy << " px.\n";
            } else if (cmd == "click") {
                int x = 0, y = 0; iss >> x >> y;
                auto cr = app.click_at(x, y);
                std::cout << (cr.handled ? (cr.action + ": " + cr.detail + "\n") : "No interactive hit.\n");
            } else if (cmd == "mem") {
                auto m = app.memory_snapshot();
                std::cout << "Total RSS: " << (m.total_rss_bytes / (1024.0 * 1024.0)) << " MB / "
                          << (m.ceiling_bytes / (1024 * 1024)) << " MB\n";
            } else if (cmd == "cache") {
                const auto& c = app.cache().stats();
                std::cout << "Hits: " << c.hits << " | Misses: " << c.misses
                          << " | Bytes: " << (c.current_bytes / 1024) << " KiB\n";
            } else if (cmd == "shield") {
                const auto& st = app.shield().stats();
                std::cout << "Inspected: " << st.requests_inspected
                          << " | Blocked: " << st.trackers_blocked
                          << " | Params stripped: " << st.params_stripped
                          << " | HTTPS upgrades: " << st.https_upgrades << "\n";
            } else if (cmd == "render") {
                std::string out; iss >> out;
                if (out.empty()) out = "astra_frame.png";
                std::cout << (app.render_active_tab(out) ? ("Saved " + out + "\n") : "Render failed.\n");
            }
        }
    }

    app.stop_workers();
    return 0;
}
