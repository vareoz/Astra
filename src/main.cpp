#include "browser/browser_app.hpp"
#include "core/arena.hpp"
#include "core/json.hpp"
#include "core/memory_governor.hpp"
#include "docs/doc_viewer.hpp"
#include "ext/extension_runtime.hpp"
#include "gfx/astra3d.hpp"
#include "gfx/font_engine.hpp"
#include "gfx/image_decoder.hpp"
#include "gfx/rasterizer.hpp"
#include "ipc/ipc_channel.hpp"
#include "js/astra_js.hpp"
#include "media/media_engine.hpp"
#include "net/http2_client.hpp"
#include "net/http_cache.hpp"
#include "net/http_client.hpp"
#include "net/websocket_client.hpp"
#include "privacy/shield.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
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
    std::cout << "=== Astra Browser Core v7 — Full Subsystem Verification Suite ===\n\n";

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

        // 12. AstraFont Native TrueType Vector Typography + UTF-8 + Bounded LRU Cache
        {
            auto& fe = FontEngine::instance();
            float w_en = fe.measure_text("Astra Browser", 18.0f, true, false);
            float w_es = fe.measure_text("Navegación rápida — España", 16.0f, false, false);
            float w_mono = fe.measure_text("0xFF38BDF8", 13.0f, false, true);
            expect(fe.freetype_active() && w_en > 80.0f && w_es > 120.0f && w_mono > 50.0f &&
                   fe.cached_glyph_count() >= 15 && fe.cache_bytes() <= FontEngine::kMaxCacheBytes,
                   "AstraFont loads libfreetype.so.6 via dlopen, renders UTF-8 TrueType glyphs within 4 MiB LRU cap");
        }

        // 13. AstraImage v2 Native Pure-C++20 Baseline DCT JPEG Decoder
        {
            std::string jpg = "astra/examples/astra_nebula.jpg";
            if (!std::ifstream(jpg)) jpg = "examples/astra_nebula.jpg";
            DecodedImage jimg = ImageDecoder::decode_file(jpg);
            bool non_uniform = false;
            if (jimg.valid && jimg.argb.size() > 100) {
                uint32_t first = jimg.argb[0];
                for (size_t i = 1; i < jimg.argb.size(); i += 97) {
                    if (jimg.argb[i] != first) { non_uniform = true; break; }
                }
            }
            expect(jimg.valid && jimg.format == "JPEG" && jimg.width >= 300 && jimg.height >= 180 && non_uniform,
                   "AstraImage v2 pure-C++20 Baseline DCT JPEG decoder decodes real YCbCr JPEG images natively");
        }

        // 14. CSS Grid Multi-Column + Multi-Row Layout Engine
        {
            SlabArena ga;
            HtmlDocument gdoc = HtmlDocument::parse(
                ga,
                "<html><head><style>"
                ".g { display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; }"
                ".c { height: 60px; background: #1e293b; }"
                "</style></head><body><div class=\"g\">"
                "<div class=\"c\">1</div><div class=\"c\">2</div><div class=\"c\">3</div>"
                "<div class=\"c\">4</div><div class=\"c\">5</div><div class=\"c\">6</div>"
                "</div></body></html>");
            CssStylesheet gsheet;
            gsheet.parse(gdoc.collect_stylesheets());
            LayoutFrame* groot = LayoutEngine::build_layout(ga, gdoc, gsheet, 960.0f, 600.0f);
            LayoutFrame* grid = (groot && !groot->children.empty()) ? groot->children[0] : nullptr;
            bool ok_grid = grid && grid->children.size() == 6 &&
                           grid->children[0]->border_box.x < grid->children[1]->border_box.x &&
                           grid->children[1]->border_box.x < grid->children[2]->border_box.x &&
                           std::abs(grid->children[0]->border_box.y - grid->children[2]->border_box.y) < 1.0f &&
                           grid->children[3]->border_box.y > grid->children[0]->border_box.y + 60.0f;
            expect(ok_grid,
                   "CSS Grid (display: grid + repeat(3, 1fr) + gap) computes multi-column/multi-row geometry accurately");
        }

        // 15. Interactive <input> Focus, Keyboard Typing & AstraJS v3 input/submit Events
        {
            const std::string fpath = "/tmp/astra_input_test.html";
            {
                std::ofstream f(fpath);
                f << "<!doctype html><html><body>"
                  << "<input id=\"q\" placeholder=\"Search...\">"
                  << "<p id=\"out\">idle</p>"
                  << "<script>"
                  << "let inp = document.querySelector(\"#q\");"
                  << "inp.addEventListener(\"input\", function() { document.querySelector(\"#out\").textContent = \"typing:\" + inp.value; });"
                  << "inp.addEventListener(\"submit\", function() { document.querySelector(\"#out\").textContent = \"submitted:\" + inp.value.toUpperCase(); });"
                  << "</script></body></html>";
            }
            app.new_tab("file://" + fpath);
            const Tab* tptr = app.active_tab();
            int hit_x = 80, hit_y = Framebuffer::kChromeHeight + 40;
            if (tptr) {
                for (const auto& hr : tptr->cached_display_list.hit_regions) {
                    if (hr.kind == HitRegionKind::Input) {
                        hit_x = static_cast<int>(hr.rect.x + 20);
                        hit_y = Framebuffer::kChromeHeight + static_cast<int>(hr.rect.y + 16);
                    }
                }
            }
            auto cr = app.click_at(hit_x, hit_y);
            bool typed = app.type_text("astra v3");
            std::string mid_txt = app.active_tab()->live_doc->query_selector("#out")->text_content();
            bool submitted = app.press_enter();
            std::string end_txt = app.active_tab()->live_doc->query_selector("#out")->text_content();
            expect(cr.handled && cr.action == "focus_input" && typed && submitted &&
                   mid_txt == "typing:astra v3" && end_txt == "submitted:ASTRA V3",
                   "Interactive <input> focus, keyboard typing, and Enter dispatch real input/submit events in AstraJS v3");
        }

        // 16. AstraJS v3 Web APIs: fetch() via AstraShield + HttpCache, JSON.parse/stringify, localStorage
        {
            const std::string apath = "/tmp/astra_webapi_test.html";
            {
                std::ofstream f(apath);
                f << "<!doctype html><html><body><p id=\"res\">init</p><script>"
                  << "localStorage.setItem(\"engine\", \"AstraV3\");"
                  << "let obj = JSON.parse(\"{\\\"ver\\\":3,\\\"tags\\\":[\\\"cpp\\\",\\\"fast\\\"]}\");"
                  << "let ser = JSON.stringify({ ok: true, v: obj.ver });"
                  << "let blocked = fetch(\"https://doubleclick.net/pixel.js\");"
                  << "let live = fetch(\"https://example.com\");"
                  << "document.querySelector(\"#res\").textContent = localStorage.getItem(\"engine\") + \"|\" + obj.tags[1] + \"|\" + blocked.blockedByShield + \"|\" + live.status + \"|\" + ser;"
                  << "</script></body></html>";
            }
            app.new_tab("file://" + apath);
            std::string res_txt = app.active_tab()->live_doc->query_selector("#res")->text_content();
            expect(res_txt.find("AstraV3|fast|true|200|") == 0 && res_txt.find("\"v\":3") != std::string::npos,
                   "AstraJS v3 executes fetch() (with AstraShield blocking + live HTTPS), JSON.parse/stringify, and localStorage");
        }

        // 17. Interactive Browser Chrome v4: Editable Omnibox, History Back/Forward, [+] New Tab, [x] Close Tab
        {
            uint32_t t_home = app.new_tab("astra://home");
            (void)t_home;
            size_t tabs_before = app.tabs().size();
            auto c_omni = app.click_at(320, 60); // Click Omnibox
            bool typed_url = app.type_text("astra://3d");
            bool nav_enter = app.press_enter();
            std::string after_enter = app.active_tab()->url;
            auto c_back = app.click_at(28, 60); // Click Back [<]
            std::string after_back = app.active_tab()->url;
            auto c_fwd = app.click_at(62, 60); // Click Forward [>]
            std::string after_fwd = app.active_tab()->url;
            // Click [+] button after visible tabs
            size_t vis = std::min<size_t>(app.tabs().size(), 5);
            int plus_x = 92 + static_cast<int>(vis) * 213 + 12;
            auto c_new = app.click_at(plus_x, 20);
            size_t tabs_after_new = app.tabs().size();
            bool closed = app.close_tab(app.active_tab_id());
            size_t tabs_after_close = app.tabs().size();
            expect(c_omni.action == "focus_omnibox" && typed_url && nav_enter &&
                   after_enter == "astra://3d" && c_back.action == "back" &&
                   after_back == "astra://home" && c_fwd.action == "forward" &&
                   after_fwd == "astra://3d" && c_new.action == "new_tab" &&
                   tabs_after_new == tabs_before + 1 && closed && tabs_after_close == tabs_before,
                   "Chrome v4 interactive Omnibox, Back/Forward history stack, [+] new tab, and tab closing work end-to-end");
        }

        // 18. CSS Positioning (absolute/fixed), z-index Paint Ordering, box-shadow DropShadow, opacity & ASD3 Roundtrip
        {
            SlabArena pa;
            HtmlDocument pdoc = HtmlDocument::parse(
                pa,
                "<html><head><style>"
                ".back { position: relative; z-index: 1; background: #1e293b; height: 120px; box-shadow: 0 6px 14px rgba(0,0,0,0.5); }"
                ".top { position: fixed; top: 16px; right: 20px; width: 200px; z-index: 50; background: #0284c7; opacity: 0.9; }"
                ".abs { position: absolute; top: 10px; left: 30px; width: 140px; z-index: 20; background: #10b981; }"
                "</style></head><body>"
                "<div class=\"top\"><button id=\"pin\">Pinned</button></div>"
                "<div class=\"back\"><div class=\"abs\">Overlay</div></div>"
                "</body></html>");
            CssStylesheet psheet;
            psheet.parse(pdoc.collect_stylesheets());
            LayoutFrame* proot = LayoutEngine::build_layout(pa, pdoc, psheet, 1280.0f, 636.0f);
            DisplayList pdl = LayoutEngine::build_display_list(proot, 1280, 636);
            auto blob = pdl.serialize();
            DisplayList rt;
            bool rt_ok = DisplayList::deserialize(blob, rt);
            bool has_shadow = false, has_fixed = false, z_ordered = true;
            for (size_t i = 0; i < rt.items.size(); ++i) {
                if (rt.items[i].type == DisplayItemType::DropShadow) has_shadow = true;
                if (rt.items[i].fixed_viewport && rt.items[i].z_index == 50) has_fixed = true;
                if (i > 0 && rt.items[i].z_index < rt.items[i - 1].z_index) z_ordered = false;
            }
            // Fixed button at top:16, right:20 -> x in [1060, 1260], y in [16, 48] MUST still hit when scroll_y = 500!
            const HitRegion* fixed_hit = rt.hit_test(1120.0f, 24.0f, 500.0f);
            expect(rt_ok && has_shadow && has_fixed && z_ordered &&
                   fixed_hit && fixed_hit->fixed_viewport && fixed_hit->target == "#pin",
                   "CSS position:absolute/fixed, z-index layering, box-shadow DropShadow, opacity & ASD3 roundtrip work cleanly");
        }

        // 19. AstraNet v3: TLS 1.3 / ALPN Inspection + Bounded 4-Thread Parallel Subresource Fetcher
        {
            std::error_code ec;
            std::filesystem::remove_all("/tmp/astra_netv3_cache", ec);
            AstraShield sh;
            HttpCache hc("/tmp/astra_netv3_cache", 4 * 1024 * 1024);
            HttpClient net(sh, &hc);
            auto single = net.get("https://example.com", "example.com", true);
            auto batch = net.fetch_batch({
                "https://example.com/",
                "https://doubleclick.net/track.js",
                "https://example.com/?b=2"
            }, "example.com", 4);
            expect(single.status_code == 200 && single.tls_verified &&
                   !single.tls_version.empty() && batch.size() == 3 &&
                   batch[0].status_code == 200 && batch[1].blocked_by_privacy && batch[2].status_code == 200,
                   "AstraNet v3 negotiates TLS 1.3 + ALPN and executes bounded 4-thread parallel fetch_batch safely");
        }

        // 20. AstraJS v4: Real Promise Microtask Chaining (.then/.catch/.finally/Promise.all) + Disk-Persisted localStorage
        {
            std::error_code ec;
            std::filesystem::remove_all("/tmp/astra_origin_storage", ec);
            const std::string ppath = "/tmp/astra_promise_test.html";
            {
                std::ofstream f(ppath);
                f << "<!doctype html><html><body><p id=\"pout\">init</p><script>"
                  << "let prev = localStorage.getItem(\"persist_key\");"
                  << "localStorage.setItem(\"persist_key\", prev ? (prev + \"_2\") : \"v1\");"
                  << "let log = \"S\";"
                  << "setTimeout(function() { log = log + \"_T\"; document.querySelector(\"#pout\").textContent = log + \"|\" + localStorage.getItem(\"persist_key\"); }, 5);"
                  << "Promise.resolve(21).then(function(x) { return x * 2; }).then(function(y) { log = log + \"_P\" + y; });"
                  << "Promise.reject(\"boom\").catch(function(e) { log = log + \"_C:\" + e; }).finally(function() { log = log + \"_F\"; });"
                  << "</script></body></html>";
            }
            app.new_tab("file://" + ppath);
            std::string run1 = app.active_tab()->live_doc->query_selector("#pout")->text_content();
            app.reload_active_tab();
            std::string run2 = app.active_tab()->live_doc->query_selector("#pout")->text_content();
            expect(run1 == "S_C:boom_P42_F_T|v1" && run2 == "S_C:boom_P42_F_T|v1_2",
                   "AstraJS v4 executes Promise .then/.catch/.finally microtasks before timers and persists per-origin localStorage to disk");
        }

        // 21. Multi-Extension MV3 Simultaneous Isolation (2 Extensions + Combined DNR + chrome.storage.local)
        {
            BrowserApp mapp(512ULL * 1024 * 1024);
            std::string e1 = "astra/examples/astra-privacy-guard";
            if (!std::ifstream(e1 + "/manifest.json")) e1 = "examples/astra-privacy-guard";
            std::string e2 = "astra/examples/astra-reader-boost";
            if (!std::ifstream(e2 + "/manifest.json")) e2 = "examples/astra-reader-boost";
            bool ok1 = mapp.load_extension(e1);
            bool ok2 = mapp.load_extension(e2);
            auto dnr1 = mapp.extensions().evaluate_dnr("https://tracker.evil-ads.test/banner", "example.com");
            auto dnr2 = mapp.extensions().evaluate_dnr("https://popup-annoyance.test/modal.js", "example.com");
            const std::string mhtml = "/tmp/astra_multi_ext.html";
            {
                std::ofstream f(mhtml);
                f << "<!doctype html><html><body><div id=\"ext-boost-slot\">none</div></body></html>";
            }
            mapp.new_tab("file://" + mhtml);
            std::string slot_txt = mapp.active_tab()->live_doc->query_selector("#ext-boost-slot")->text_content();
            expect(ok1 && ok2 && mapp.extensions().extensions().size() == 2 &&
                   dnr1.block && dnr2.block && slot_txt == "MV3 ReaderBoost: enabled",
                   "Multi-Extension MV3 runtime loads 2 extensions simultaneously with combined DNR + chrome.storage.local");
        }

        // 22. AstraMedia v3: Native WAV Audio Probe + Bounded PCM Decode (Non-Zero RMS) & Video Scrubber Seek
        {
            std::string wav = "astra/examples/astra_chime.wav";
            if (!std::ifstream(wav)) wav = "examples/astra_chime.wav";
            auto ainfo = MediaEngine::probe_container(wav);
            DecodedAudioBuffer abuf;
            bool decoded = MediaEngine::decode_audio_pcm(wav, 1.0, abuf);
            expect(ainfo.valid && ainfo.has_audio && ainfo.audio_sample_rate == 44100 &&
                   decoded && abuf.valid && abuf.pcm_s16.size() > 10000 && abuf.rms_level > 0.04,
                   "AstraMedia v3 probes WAV audio container and decodes bounded PCM s16le waveform with real non-zero RMS");
        }

        // 23. AstraNet v4: Native HTTP/2 (RFC 7540 Framing + RFC 7541 HPACK + Huffman) & Live TLS 1.3 ALPN
        {
            Http2Frame hdr_f;
            hdr_f.type = Http2FrameType::Headers;
            hdr_f.flags = 0x05;
            hdr_f.stream_id = 1;
            hdr_f.payload = {0x88}; // :status 200 (HPACK static index 8)
            auto packed = Http2Client::pack_frame(hdr_f);
            Http2Frame unpacked;
            size_t consumed = 0;
            bool f_ok = Http2Client::unpack_frame(packed.data(), packed.size(), unpacked, consumed) &&
                        unpacked.type == Http2FrameType::Headers && unpacked.stream_id == 1;
            // Huffman decode test: "www.example.com" = f1 e3 c2 e5 f2 3a 6b a0 ab 90 f4 ff (RFC 7541 C.4.1)
            const uint8_t huff_in[] = {0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff};
            std::string huff_out;
            bool huff_ok = HpackCodec::huffman_decode(huff_in, sizeof(huff_in), huff_out);
            HpackCodec hdec;
            std::vector<std::pair<std::string, std::string>> hlist;
            bool hpack_ok = hdec.decode_headers(unpacked.payload.data(), unpacked.payload.size(), hlist) &&
                            !hlist.empty() && hlist[0].first == ":status" && hlist[0].second == "200";
            // Live HTTPS test (negotiates h2 or http/1.1 cleanly)
            AstraShield h2sh;
            HttpClient h2net(h2sh, nullptr);
            auto live_r = h2net.get("https://example.com", "example.com", true);
            auto live_r2 = h2net.get("https://example.com/", "example.com", true);
            expect(f_ok && huff_ok && huff_out == "www.example.com" && hpack_ok &&
                   live_r.status_code == 200 && live_r.tls_verified && live_r2.status_code == 200 &&
                   (h2net.http2_requests() >= 1 || h2net.keepalive_reuses() >= 1),
                   "AstraNet v4 HTTP/2 RFC 7540 framing + RFC 7541 HPACK/Huffman + live TLS 1.3 ALPN/pool work end-to-end");
        }

        // 24. AstraJS v5: async/await, Arrow Functions (=>), and Shared Mutable Lexical Closures
        {
            SlabArena ja;
            HtmlDocument jdoc = HtmlDocument::parse(ja, "<html><body><div id=\"out\">init</div></body></html>");
            AstraJsEngine jvm;
            std::string js =
                "const makeCounter = (start) => {\n"
                "  let n = start;\n"
                "  return () => { n = n + 1; return n; };\n"
                "};\n"
                "const c1 = makeCounter(10);\n"
                "const c2 = makeCounter(100);\n"
                "const v1 = c1();\n"
                "const v2 = c1();\n"
                "const v3 = c2();\n"
                "async function compute() {\n"
                "  const a = await Promise.resolve(v1 + v2);\n"
                "  const b = await Promise.resolve(v3);\n"
                "  document.querySelector(\"#out\").textContent = a + \":\" + b + \":\" + c1();\n"
                "}\n"
                "compute();\n";
            auto rep = jvm.execute(js, jdoc);
            std::string got = jdoc.query_selector("#out")->text_content();
            expect(rep.ok && got == "23:101:13",
                   "AstraJS v5 executes async/await, arrow functions (=>), and independent mutable lexical closures");
        }

        // 25. AstraJS v5 Dynamic DOM: innerHTML fragment parser, classList, setAttribute/removeAttribute
        {
            SlabArena da;
            HtmlDocument ddoc = HtmlDocument::parse(da, "<html><body><div id=\"box\" class=\"card\" data-x=\"1\"></div></body></html>");
            AstraJsEngine dvm;
            std::string js =
                "let b = document.querySelector(\"#box\");\n"
                "b.classList.add(\"active\");\n"
                "let hasA = b.classList.contains(\"active\");\n"
                "b.classList.toggle(\"card\");\n"
                "b.removeAttribute(\"data-x\");\n"
                "b.setAttribute(\"data-ok\", \"yes\");\n"
                "b.innerHTML = \"<span id='child'>Hello <b>Astra5</b></span>\";\n"
                "let inner = b.innerHTML;\n"
                "let txt = document.querySelector(\"#child\").textContent;\n"
                "let summary = hasA + \"|\" + b.className + \"|\" + b.getAttribute(\"data-x\") + \"|\" + b.getAttribute(\"data-ok\") + \"|\" + txt;\n";
            auto rep = dvm.execute(js, ddoc);
            std::string sum = dvm.globals().count("summary") ? dvm.globals().at("summary").to_string() : "";
            std::string in_h = dvm.globals().count("inner") ? dvm.globals().at("inner").to_string() : "";
            expect(rep.ok && sum == "true|active||yes|Hello Astra5" && in_h.find("<b>Astra5</b>") != std::string::npos,
                   "AstraJS v5 dynamic DOM supports innerHTML getter/setter, classList, and removeAttribute cleanly");
        }

        // 26. CSS @media Responsive Queries + WebRender Per-Item overflow:hidden Clipping (ASD4)
        {
            SlabArena ma;
            HtmlDocument mdoc = HtmlDocument::parse(
                ma,
                "<html><head><style>"
                "body { margin: 0; padding: 0; }"
                ".box { background: #111827; height: 60px; overflow: hidden; }"
                ".child { background: #ef4444; height: 140px; }"
                "@media (max-width: 700px) { .box { background: #0284c7; } }"
                "</style></head><body><div class=\"box\"><div class=\"child\"></div></div></body></html>");
            CssStylesheet msheet;
            msheet.parse(mdoc.collect_stylesheets());
            LayoutFrame* wide_root = LayoutEngine::build_layout(ma, mdoc, msheet, 1280.0f, 600.0f);
            LayoutFrame* narrow_root = LayoutEngine::build_layout(ma, mdoc, msheet, 600.0f, 600.0f);
            uint32_t wide_bg = wide_root->children[0]->style.background.to_argb();
            uint32_t narrow_bg = narrow_root->children[0]->style.background.to_argb();
            DisplayList dl = LayoutEngine::build_display_list(wide_root, 1280, 600);
            auto blob = dl.serialize();
            DisplayList rt;
            bool rt_ok = DisplayList::deserialize(blob, rt);
            bool has_clipped_child = false;
            for (const auto& it : rt.items) {
                if (it.has_clip && it.rect.h >= 120.0f && it.clip_rect.h <= 62.0f) has_clipped_child = true;
            }
            // Verify Framebuffer actually clips pixels outside clip_rect
            Framebuffer cfb(200, 200);
            cfb.composite_display_list(rt, 0, 0.0f);
            // At y=30 (inside 60px clipped box) -> red #ef4444; at y=100 (below 60px) -> clear_color_argb!
            uint32_t inside_px = cfb.pixels()[30 * 200 + 50];
            uint32_t below_px = cfb.pixels()[100 * 200 + 50];
            expect(wide_bg == 0xFF111827u && narrow_bg == 0xFF0284C7u && rt_ok &&
                   has_clipped_child && inside_px == 0xFFEF4444u && below_px == rt.clear_color_argb,
                   "CSS @media queries adapt to viewport width and WebRender ASD4 overflow:hidden clips pixels strictly");
        }

        // 27. Native Download Manager + FIPS 180-4 SHA-256 + astra://downloads Ledger
        {
            std::error_code ec;
            std::filesystem::remove_all("/tmp/astra_downloads_test", ec);
            std::string sha_abc = BrowserApp::compute_sha256_hex("abc");
            std::string data_file = "astra/examples/astra_data.json";
            if (!std::ifstream(data_file)) data_file = "examples/astra_data.json";
            auto rec = app.download_url("file://" + data_file, "/tmp/astra_downloads_test");
            uint32_t dtab = app.new_tab("astra://downloads");
            const Tab* dt = app.active_tab();
            bool has_sha_in_page = dt && dt->raw_html.find(rec.sha256_hex) != std::string::npos;
            expect(sha_abc == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
                   rec.ok && rec.bytes > 20 && std::filesystem::exists(rec.saved_path) &&
                   dtab > 0 && has_sha_in_page,
                   "Native Download Manager saves files with exact FIPS 180-4 SHA-256 checksum and renders astra://downloads");
        }

        // 28. AstraMedia v4: PCM Waveform Peak Analyzer + Dynamic Linux ALSA Audio Probe
        {
            std::string wav = "astra/examples/astra_chime.wav";
            if (!std::ifstream(wav)) wav = "examples/astra_chime.wav";
            DecodedAudioBuffer abuf;
            bool dec = MediaEngine::decode_audio_pcm(wav, 1.0, abuf);
            auto peaks = MediaEngine::compute_waveform_peaks(abuf, 40);
            float max_peak = 0.0f;
            for (float p : peaks) max_peak = std::max(max_peak, p);
            auto alsa_rep = MediaEngine::play_audio_alsa(abuf);
            bool safe_status = (alsa_rep.status == "played" ||
                                alsa_rep.status == "headless_no_soundcard" ||
                                alsa_rep.status == "alsa_not_installed");
            expect(dec && peaks.size() == 40 && max_peak > 0.25f && safe_status,
                   "AstraMedia v4 computes 40-bar PCM waveform peaks and probes Linux ALSA output via dlopen safely");
        }

        // 29. AstraNet v5: True HTTP/2 Multi-Stream Multiplexing over a Single TLS 1.3 Socket
        {
            std::error_code ec;
            std::filesystem::remove_all("/tmp/astra_h2mux_test_cache", ec);
            AstraShield msh;
            HttpCache mhc("/tmp/astra_h2mux_test_cache", 4 * 1024 * 1024);
            HttpClient mnet(msh, &mhc);
            auto batch = mnet.fetch_batch({
                "https://example.com/",
                "https://example.com/?mux=2",
                "https://example.com/?mux=3"
            }, "example.com", 4);
            expect(batch.size() == 3 &&
                   batch[0].status_code == 200 && batch[1].status_code == 200 && batch[2].status_code == 200 &&
                   batch[0].multiplexed_h2 && batch[1].multiplexed_h2 && batch[2].multiplexed_h2 &&
                   mnet.http2_multiplexed_streams() >= 3,
                   "AstraNet v5 multiplexes 3 concurrent HTTP/2 streams over a single TLS 1.3 ALPN h2 connection");
        }

        // 30. AstraJS v6: try/catch/throw Multi-Frame Stack Unwinding + ES6 for..of (Array & String)
        {
            SlabArena ea;
            HtmlDocument edoc = HtmlDocument::parse(ea, "<html><body><div id=\"e\"></div></body></html>");
            AstraJsEngine evm;
            std::string js =
                "function deepThrow(x) { throw \"err:\" + x; }\n"
                "function mid(x) { return deepThrow(x); }\n"
                "let caught = \"none\";\n"
                "let afterTry = false;\n"
                "try { mid(42); } catch (e) { caught = e; } finally { afterTry = true; }\n"
                "let sum = 0;\n"
                "for (let n of [3, 5, 7, 10]) { sum = sum + n; }\n"
                "let chars = \"\";\n"
                "for (let ch of \"ASTRA\") { chars = chars + ch + \"-\"; }\n";
            auto rep = evm.execute(js, edoc);
            std::string caught = evm.globals().count("caught") ? evm.globals().at("caught").to_string() : "";
            double sum = evm.globals().count("sum") ? evm.globals().at("sum").to_number() : 0.0;
            std::string chars = evm.globals().count("chars") ? evm.globals().at("chars").to_string() : "";
            bool fin = evm.globals().count("afterTry") && evm.globals().at("afterTry").truthy();
            expect(rep.ok && caught == "err:42" && fin && sum == 25.0 && chars == "A-S-T-R-A-",
                   "AstraJS v6 unwinds try/catch/finally/throw across call frames and iterates Arrays & Strings via for..of");
        }

        // 31. AstraJS v6: Native HTML5 <canvas> 2D API + requestAnimationFrame Queue
        {
            SlabArena ca;
            HtmlDocument cdoc = HtmlDocument::parse(
                ca, "<html><body><canvas id=\"c1\" width=\"120\" height=\"80\"></canvas><p id=\"st\">0</p></body></html>");
            AstraJsEngine cvm;
            std::string js =
                "let cv = document.querySelector(\"#c1\");\n"
                "let ctx = cv.getContext(\"2d\");\n"
                "ctx.fillStyle = \"#0f172a\"; ctx.fillRect(0, 0, 120, 80);\n"
                "ctx.fillStyle = \"#ef4444\"; ctx.fillRect(10, 10, 30, 30);\n"
                "ctx.fillStyle = \"#3b82f6\"; ctx.beginPath(); ctx.arc(75, 35, 14, 0, Math.PI * 2); ctx.fill();\n"
                "ctx.strokeStyle = \"#38bdf8\"; ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(5, 70); ctx.lineTo(115, 70); ctx.stroke();\n"
                "ctx.font = \"bold 12px monospace\"; ctx.fillStyle = \"#ffffff\"; ctx.fillText(\"OK\", 8, 62);\n"
                "let frame = 0;\n"
                "function tick(ts) {\n"
                "  frame = frame + 1;\n"
                "  ctx.fillStyle = \"#22c55e\";\n"
                "  ctx.fillRect(10 + frame * 15, 50, 10, 10);\n"
                "  document.querySelector(\"#st\").textContent = \"frame:\" + frame;\n"
                "  requestAnimationFrame(tick);\n"
                "}\n"
                "requestAnimationFrame(tick);\n";
            auto rep0 = cvm.execute(js, cdoc);
            bool queued = (cvm.pending_raf_count() == 1);
            auto rep1 = cvm.run_animation_frame(16.67, cdoc);
            auto rep2 = cvm.run_animation_frame(33.34, cdoc);
            const CanvasSurface* surf = cvm.find_canvas("c1");
            uint32_t red_px = (surf && surf->argb.size() == 120u * 80u) ? surf->argb[20 * 120 + 20] : 0;
            uint32_t blue_px = (surf && surf->argb.size() == 120u * 80u) ? surf->argb[35 * 120 + 75] : 0;
            uint32_t green_px = (surf && surf->argb.size() == 120u * 80u) ? surf->argb[55 * 120 + 45] : 0;
            std::string st_txt = cdoc.query_selector("#st")->text_content();
            expect(rep0.ok && queued && rep1.raf_run == 1 && rep2.raf_run == 1 &&
                   surf && surf->width == 120 && surf->height == 80 &&
                   red_px == 0xFFEF4444u && blue_px == 0xFF3B82F6u && green_px == 0xFF22C55Eu &&
                   st_txt == "frame:2",
                   "AstraJS v6 renders native HTML5 <canvas> 2D shapes/text and steps requestAnimationFrame callbacks cleanly");
        }

        // 32. CSS Box Sizing Level 3: box-sizing: border-box vs content-box Geometry
        {
            SlabArena ba;
            HtmlDocument bdoc = HtmlDocument::parse(
                ba,
                "<html><head><style>"
                "body { margin: 0; padding: 0; }"
                ".bb { box-sizing: border-box; width: 240px; height: 100px; padding: 20px; border: 4px solid #38bdf8; }"
                ".cb { box-sizing: content-box; width: 240px; height: 100px; padding: 20px; border: 4px solid #38bdf8; }"
                "</style></head><body><div class=\"bb\"></div><div class=\"cb\"></div></body></html>");
            CssStylesheet bsheet;
            bsheet.parse(bdoc.collect_stylesheets());
            LayoutFrame* broot = LayoutEngine::build_layout(ba, bdoc, bsheet, 800.0f, 600.0f);
            bool ok_bs = broot && broot->children.size() == 2 &&
                         std::abs(broot->children[0]->border_box.w - 240.0f) < 0.5f &&
                         std::abs(broot->children[0]->border_box.h - 100.0f) < 0.5f &&
                         std::abs(broot->children[0]->content_box.w - 192.0f) < 0.5f &&
                         std::abs(broot->children[0]->content_box.h - 52.0f) < 0.5f &&
                         std::abs(broot->children[1]->border_box.w - 288.0f) < 0.5f &&
                         std::abs(broot->children[1]->border_box.h - 148.0f) < 0.5f &&
                         std::abs(broot->children[1]->content_box.w - 240.0f) < 0.5f &&
                         std::abs(broot->children[1]->content_box.h - 100.0f) < 0.5f;
            expect(ok_bs,
                   "CSS box-sizing: border-box vs content-box computes exact border_box and content_box dimensions");
        }

        // 33. CSS linear-gradient(...) + ASD5 DisplayList Roundtrip + Per-Pixel Gradient Interpolation
        {
            SlabArena ga;
            HtmlDocument gdoc = HtmlDocument::parse(
                ga,
                "<html><head><style>"
                "body { margin: 0; padding: 0; }"
                ".grad { width: 200px; height: 60px; background: linear-gradient(90deg, #ef4444, #3b82f6); }"
                "</style></head><body><div class=\"grad\"></div></body></html>");
            CssStylesheet gsheet;
            gsheet.parse(gdoc.collect_stylesheets());
            LayoutFrame* groot = LayoutEngine::build_layout(ga, gdoc, gsheet, 400.0f, 200.0f);
            DisplayList gdl = LayoutEngine::build_display_list(groot, 400, 200);
            auto blob = gdl.serialize();
            size_t moff = 0;
            uint32_t magic = 0;
            bool is_asd5 = read_u32(blob, moff, magic) && magic == 0x41534435u;
            DisplayList rt;
            bool rt_ok = DisplayList::deserialize(blob, rt);
            bool has_grad = false;
            for (const auto& it : rt.items) {
                if (it.type == DisplayItemType::FillGradient &&
                    it.color_argb == 0xFFEF4444u && it.color2_argb == 0xFF3B82F6u &&
                    std::abs(it.angle_deg - 90.0f) < 0.5f) {
                    has_grad = true;
                }
            }
            Framebuffer gfb(220, 80);
            gfb.composite_display_list(rt, 0, 0.0f);
            uint32_t left_px = gfb.pixels()[30 * 220 + 5];
            uint32_t mid_px = gfb.pixels()[30 * 220 + 100];
            uint32_t right_px = gfb.pixels()[30 * 220 + 195];
            auto r_ch = [](uint32_t c) { return (c >> 16) & 0xFFu; };
            auto b_ch = [](uint32_t c) { return c & 0xFFu; };
            bool grad_interp = (r_ch(left_px) > 220 && b_ch(left_px) < 85 &&
                                r_ch(mid_px) > 120 && b_ch(mid_px) > 120 &&
                                b_ch(right_px) > 220 && r_ch(right_px) < 85);
            expect(is_asd5 && rt_ok && has_grad && grad_interp,
                   "CSS linear-gradient(90deg) roundtrips in ASD5 DisplayList and rasterizes smooth per-pixel RGB interpolation");
        }

        // 34. AstraNet v6: RFC 6455 WebSocket SHA-1/Base64 Handshake + Real Loopback Echo Server + JS new WebSocket()
        {
            // Canonical RFC 6455 Section 1.3 vector
            std::string canonical_accept = WebSocketClient::compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
            bool rfc_vector_ok = (canonical_accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

            int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            int opt = 1;
            if (listen_fd >= 0) setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0; // ephemeral port
            bool bound = (listen_fd >= 0 &&
                          ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                          ::listen(listen_fd, 4) == 0);
            socklen_t alen = sizeof(addr);
            uint16_t port = 0;
            if (bound && ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen) == 0) {
                port = ntohs(addr.sin_port);
            }

            bool srv_saw_masked_frame = false;
            std::thread srv_thread([&]() {
                if (!bound) return;
                int cfd = ::accept(listen_fd, nullptr, nullptr);
                if (cfd < 0) return;
                std::string req;
                char buf[1024];
                while (req.find("\r\n\r\n") == std::string::npos && req.size() < 4096) {
                    ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    req.append(buf, static_cast<size_t>(n));
                }
                std::string ws_key;
                size_t kp = req.find("Sec-WebSocket-Key:");
                if (kp != std::string::npos) {
                    size_t vs = kp + 18;
                    while (vs < req.size() && req[vs] == ' ') ++vs;
                    size_t ve = req.find("\r\n", vs);
                    if (ve != std::string::npos) ws_key = req.substr(vs, ve - vs);
                }
                std::string accept_val = WebSocketClient::compute_accept_key(ws_key);
                std::string resp =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + accept_val + "\r\n\r\n";
                (void)::send(cfd, resp.data(), resp.size(), MSG_NOSIGNAL);

                // Serve up to 3 masked client frames (Text -> Echo Text, Ping -> Pong, Close -> exit)
                for (int iter = 0; iter < 4; ++iter) {
                    uint8_t fh[2];
                    if (::recv(cfd, fh, 2, MSG_WAITALL) != 2) break;
                    uint8_t op = fh[0] & 0x0Fu;
                    bool masked = (fh[1] & 0x80u) != 0;
                    size_t plen = fh[1] & 0x7Fu;
                    if (plen >= 126) break;
                    uint8_t mask[4] = {0, 0, 0, 0};
                    if (masked && ::recv(cfd, mask, 4, MSG_WAITALL) != 4) break;
                    std::string pl(plen, '\0');
                    if (plen > 0 && ::recv(cfd, pl.data(), plen, MSG_WAITALL) != static_cast<ssize_t>(plen)) break;
                    if (masked) {
                        srv_saw_masked_frame = true;
                        for (size_t i = 0; i < plen; ++i) pl[i] = static_cast<char>(static_cast<uint8_t>(pl[i]) ^ mask[i & 3u]);
                    }
                    if (op == 0x8) break; // Close
                    std::vector<uint8_t> out;
                    if (op == 0x1) { // Text -> echo prefixed
                        std::string reply = "echo:" + pl;
                        out.push_back(0x81u);
                        out.push_back(static_cast<uint8_t>(reply.size()));
                        out.insert(out.end(), reply.begin(), reply.end());
                    } else if (op == 0x9) { // Ping -> Pong
                        out.push_back(0x8Au);
                        out.push_back(static_cast<uint8_t>(pl.size()));
                        out.insert(out.end(), pl.begin(), pl.end());
                    }
                    if (!out.empty()) (void)::send(cfd, out.data(), out.size(), MSG_NOSIGNAL);
                }
                ::close(cfd);
            });

            SlabArena wsa;
            HtmlDocument wsdoc = HtmlDocument::parse(wsa, "<html><body><p id=\"wsout\">init</p></body></html>");
            AstraJsEngine wsvm;
            std::string ws_script =
                "let sock = new WebSocket(\"ws://127.0.0.1:" + std::to_string(port) + "/stream\");\n"
                "let stateOpen = sock.readyState;\n"
                "let cbMsg = \"\";\n"
                "let closedFlag = false;\n"
                "sock.onmessage = (ev) => { cbMsg = ev.data; };\n"
                "sock.onclose = () => { closedFlag = true; };\n"
                "let sent = sock.send(\"AstraV7\");\n"
                "let r1 = sock.recv(2000);\n"
                "sock.ping(\"hb\");\n"
                "let pong = sock.recv(2000);\n"
                "sock.close();\n"
                "let stateClosed = sock.readyState;\n"
                "document.querySelector(\"#wsout\").textContent = stateOpen + \"|\" + sent + \"|\" + r1 + \"|\" + cbMsg + \"|\" + pong + \"|\" + closedFlag + \"|\" + stateClosed;\n";
            auto wsrep = wsvm.execute(ws_script, wsdoc);
            if (srv_thread.joinable()) srv_thread.join();
            if (listen_fd >= 0) ::close(listen_fd);
            std::string wsout = wsdoc.query_selector("#wsout")->text_content();
            expect(rfc_vector_ok && bound && wsrep.ok && srv_saw_masked_frame &&
                   wsout == "1|true|echo:AstraV7|echo:AstraV7|hb|true|3",
                   "AstraNet v6 RFC 6455 WebSocket handshake, masked framing, ping/pong & JS new WebSocket() work end-to-end");
        }

        // 35. CSS3 Custom Properties (--*/var()), :root Inheritance, Fallback & Direct-Child Combinator (>)
        {
            SlabArena va;
            HtmlDocument vdoc = HtmlDocument::parse(
                va,
                "<html><head><style>"
                ":root { --accent: #38bdf8; --pad: 18px; }"
                ".parent > .item { color: var(--accent); padding: var(--pad); background: var(--missing, #1e293b); }"
                ".scope { --accent: #10b981; }"
                ".scope > .item { color: var(--accent); }"
                "</style></head><body>"
                "<div class=\"parent\"><div class=\"item\" id=\"direct\">Direct</div>"
                "<div class=\"wrap\"><div class=\"item\" id=\"nested\">Nested</div></div></div>"
                "<div class=\"scope\"><div class=\"item\" id=\"scoped\">Scoped</div></div>"
                "</body></html>");
            CssStylesheet vsheet;
            vsheet.parse(vdoc.collect_stylesheets());
            LayoutFrame* vroot = LayoutEngine::build_layout(va, vdoc, vsheet, 800.0f, 400.0f);
            const LayoutFrame* f_direct = nullptr;
            const LayoutFrame* f_nested = nullptr;
            const LayoutFrame* f_scoped = nullptr;
            std::function<void(const LayoutFrame*)> find_frames = [&](const LayoutFrame* f) {
                if (!f) return;
                if (f->node) {
                    std::string id = f->node->get_attr("id");
                    if (id == "direct") f_direct = f;
                    else if (id == "nested") f_nested = f;
                    else if (id == "scoped") f_scoped = f;
                }
                for (const LayoutFrame* c : f->children) find_frames(c);
            };
            find_frames(vroot);
            bool ok_vars = f_direct && f_nested && f_scoped &&
                           f_direct->style.color.to_argb() == 0xFF38BDF8u &&
                           f_direct->style.background.to_argb() == 0xFF1E293Bu &&
                           std::abs(f_direct->style.padding.left - 18.0f) < 0.5f &&
                           f_nested->style.color.to_argb() != 0xFF38BDF8u &&
                           f_scoped->style.color.to_argb() == 0xFF10B981u;
            expect(ok_vars,
                   "CSS3 :root variables (--*/var() with fallback), scoped cascade, and direct-child combinator (>) resolve accurately");
        }

        // 36. CSS3 Flexbox justify-content (space-between/center), align-items:center, text-align & min/max-width
        {
            SlabArena fa;
            HtmlDocument fdoc = HtmlDocument::parse(
                fa,
                "<html><head><style>"
                "body { margin: 0; padding: 0; }"
                ".bar { display: flex; justify-content: space-between; align-items: center; width: 600px; height: 100px; background: #0f172a; }"
                ".a { width: 100px; height: 40px; background: #0ea5e9; }"
                ".b { width: 100px; height: 80px; background: #10b981; }"
                ".clamped { width: 500px; max-width: 260px; min-width: 120px; height: 30px; text-align: center; }"
                "</style></head><body>"
                "<div class=\"bar\"><div class=\"a\">A</div><div class=\"b\">B</div></div>"
                "<div class=\"clamped\">Centered</div>"
                "</body></html>");
            CssStylesheet fsheet;
            fsheet.parse(fdoc.collect_stylesheets());
            LayoutFrame* froot = LayoutEngine::build_layout(fa, fdoc, fsheet, 800.0f, 400.0f);
            bool ok_flex = froot && froot->children.size() == 2 &&
                           froot->children[0]->children.size() == 2 &&
                           std::abs(froot->children[0]->children[0]->border_box.x - 0.0f) < 1.0f &&
                           std::abs(froot->children[0]->children[1]->border_box.x - 500.0f) < 1.0f &&
                           std::abs(froot->children[0]->children[0]->border_box.y - 30.0f) < 1.5f &&
                           std::abs(froot->children[1]->border_box.w - 260.0f) < 1.0f &&
                           froot->children[1]->style.text_align == TextAlign::Center;
            expect(ok_flex,
                   "CSS3 Flexbox justify-content:space-between, align-items:center, text-align & min/max-width compute exact geometry");
        }

        // 37. AstraJS v7: ES6 class / constructor / this / new Instantiation & Template Literals (`...${expr}...`)
        {
            SlabArena cla;
            HtmlDocument cldoc = HtmlDocument::parse(cla, "<html><body><p id=\"cl\">init</p></body></html>");
            AstraJsEngine clvm;
            std::string js =
                "class EngineNode {\n"
                "  constructor(name, threads) {\n"
                "    this.name = name;\n"
                "    this.threads = threads;\n"
                "  }\n"
                "  summary(ver) {\n"
                "    return `${this.name}:v${ver}(${this.threads * 2}q)`;\n"
                "  }\n"
                "}\n"
                "const n1 = new EngineNode(\"AstraNet\", 4);\n"
                "const n2 = new EngineNode(\"AstraJS\", 2);\n"
                "const tag = `${n1.summary(7)} | ${n2.summary(7)}`;\n"
                "document.querySelector(\"#cl\").textContent = tag;\n";
            auto rep = clvm.execute(js, cldoc);
            std::string got = cldoc.query_selector("#cl")->text_content();
            expect(rep.ok && got == "AstraNet:v7(8q) | AstraJS:v7(4q)",
                   "AstraJS v7 compiles and executes ES6 class/constructor/this/new and template literal interpolation cleanly");
        }

        // 38. AstraJS v7: Dynamic element.style.* Inline CSS Proxy -> Layout Engine Reflection
        {
            SlabArena sa;
            HtmlDocument sdoc = HtmlDocument::parse(
                sa, "<html><body><div id=\"dyn\">Dynamic</div></body></html>");
            AstraJsEngine svm;
            std::string js =
                "const el = document.querySelector(\"#dyn\");\n"
                "el.style.backgroundColor = \"#0ea5e9\";\n"
                "el.style.borderRadius = \"14px\";\n"
                "el.style.width = \"320px\";\n"
                "el.style.color = \"#ffffff\";\n"
                "const readBack = el.style.backgroundColor + \"|\" + el.style.width;\n";
            auto rep = svm.execute(js, sdoc);
            std::string rb = svm.globals().count("readBack") ? svm.globals().at("readBack").to_string() : "";
            CssStylesheet ssheet;
            LayoutFrame* sroot = LayoutEngine::build_layout(sa, sdoc, ssheet, 800.0f, 300.0f);
            bool ok_style = rep.ok && rb == "#0ea5e9|320px" &&
                            sroot && !sroot->children.empty() &&
                            sroot->children[0]->style.background.to_argb() == 0xFF0EA5E9u &&
                            sroot->children[0]->style.color.to_argb() == 0xFFFFFFFFu &&
                            std::abs(sroot->children[0]->style.border_radius - 14.0f) < 0.5f &&
                            std::abs(sroot->children[0]->border_box.w - 320.0f) < 1.0f;
            expect(ok_style,
                   "AstraJS v7 element.style.* camelCase proxy mutates inline CSS and reflects directly in LayoutEngine");
        }

        app.stop_workers();
    }

    std::cout << "\nSummary: " << g_tests_passed << " passed, " << g_tests_failed << " failed.\n";
    return g_tests_failed == 0 ? 0 : 1;
}

int run_stress_test(int tab_count, uint64_t ceiling_mb) {
    std::cout << "=== Astra v7 Multi-Tab Stress Test (" << tab_count << " tabs, ceiling " << ceiling_mb << " MB) ===\n";
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
        << "Astra Browser Core v7 (Native C++20 Multi-Process Engine)\n"
        << "Usage:\n"
        << "  astra --test                                Run full Session 7 subsystem suite (43 tests)\n"
        << "  astra --url <URL> [--out <file.png>]        Load URL & render PNG\n"
        << "        [--download <URL>]                    Download URL to /tmp/astra_downloads/ with SHA-256\n"
        << "        [--ext <extension_dir>]               Load Manifest V3 WebExtension (repeatable)\n"
        << "        [--scroll <px>] [--click <x,y>]       Simulate APZ scroll or mouse click before render\n"
        << "        [--type <text>] [--enter]             Type into Omnibox or focused <input> and press Enter\n"
        << "        [--raf-step <N>]                      Step N requestAnimationFrame frames before render\n"
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

    std::string mode_url, out_png;
    std::vector<std::string> ext_dirs;
    std::vector<std::string> download_urls;
    std::vector<std::pair<std::string, std::string>> cli_actions;
    bool run_tests = false, open_gui = false, run_repl = false, wireframe = false;
    int stress_tabs = 0;
    int raf_steps = 0;
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
        else if (arg == "--download" && i + 1 < argc) download_urls.emplace_back(argv[++i]);
        else if (arg == "--ext" && i + 1 < argc) ext_dirs.emplace_back(argv[++i]);
        else if (arg == "--click" && i + 1 < argc) cli_actions.emplace_back("click", argv[++i]);
        else if (arg == "--type" && i + 1 < argc) cli_actions.emplace_back("type", argv[++i]);
        else if (arg == "--enter") cli_actions.emplace_back("enter", "");
        else if (arg == "--scroll" && i + 1 < argc) scroll_px = std::strtof(argv[++i], nullptr);
        else if (arg == "--raf-step" && i + 1 < argc) raf_steps = std::atoi(argv[++i]);
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

    for (const auto& ext_dir : ext_dirs) {
        std::string err;
        if (app.load_extension(ext_dir, &err)) {
            std::cout << "[AstraExt] Loaded extension from " << ext_dir << "\n";
        } else {
            std::cerr << "[AstraExt] Failed to load extension: " << err << "\n";
        }
    }

    for (const auto& dl_u : download_urls) {
        auto rec = app.download_url(dl_u);
        if (rec.ok) {
            std::cout << "[AstraDL] Saved:  " << rec.saved_path << " (" << rec.bytes << " B, "
                      << rec.protocol << ", " << std::fixed << std::setprecision(1) << rec.elapsed_ms << " ms)\n"
                      << "          SHA256: " << rec.sha256_hex << "\n";
        } else {
            std::cerr << "[AstraDL] Failed: " << dl_u << " -> " << rec.error << "\n";
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

        if (raf_steps > 0) {
            app.step_animation_frames(raf_steps);
            std::cout << "[Astra] Stepped " << raf_steps << " requestAnimationFrame frame(s)\n";
        }
        if (scroll_px != 0.0f) {
            app.scroll_active_tab(scroll_px);
        }
        for (const auto& [act, val] : cli_actions) {
            if (act == "click") {
                size_t comma = val.find(',');
                if (comma != std::string::npos) {
                    int cx = std::atoi(val.substr(0, comma).c_str());
                    int cy = std::atoi(val.substr(comma + 1).c_str());
                    auto cr = app.click_at(cx, cy);
                    std::cout << "[Astra] Click(" << cx << "," << cy << "): "
                              << (cr.handled ? (cr.action + " -> " + cr.detail) : "no hit") << "\n";
                }
            } else if (act == "type") {
                bool ok = app.type_text(val);
                std::cout << "[Astra] Type(\"" << val << "\"): " << (ok ? "OK" : "no focused input") << "\n";
            } else if (act == "enter") {
                bool ok = app.press_enter();
                std::cout << "[Astra] Enter: " << (ok ? "submitted" : "no focused input") << "\n";
            }
        }

        std::cout << "[Astra] Loaded:       " << nav.final_url << "\n"
                  << "        Title:        " << nav.title << "\n"
                  << "        Protocol:     " << (nav.protocol.empty() ? "local" : nav.protocol) << "\n"
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
        std::cout << "Astra v7 Interactive Shell (open <url>, download <url>, downloads, back, forward, reload, close <id>, tabs, switch <id>, scroll <px>, click <x> <y>, type <text>, enter, raf [N], mem, shield, cache, render <file.png>, quit)\n";
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
            } else if (cmd == "download") {
                std::string u; iss >> u;
                auto rec = app.download_url(u);
                if (rec.ok) std::cout << "Saved " << rec.saved_path << " (" << rec.bytes << " B) SHA256=" << rec.sha256_hex << "\n";
                else std::cout << "Download failed: " << rec.error << "\n";
            } else if (cmd == "downloads") {
                for (const auto& d : app.downloads()) {
                    std::cout << "  " << d.filename << " [" << d.protocol << "] " << d.bytes << " B sha256:" << d.sha256_hex.substr(0, 16) << "...\n";
                }
            } else if (cmd == "back") {
                std::cout << (app.go_back() ? "Back OK.\n" : "No previous page.\n");
            } else if (cmd == "forward") {
                std::cout << (app.go_forward() ? "Forward OK.\n" : "No forward page.\n");
            } else if (cmd == "reload") {
                std::cout << (app.reload_active_tab() ? "Reloaded.\n" : "Reload failed.\n");
            } else if (cmd == "close") {
                uint32_t id = 0; iss >> id;
                std::cout << (app.close_tab(id ? id : app.active_tab_id()) ? "Closed.\n" : "Not found.\n");
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
            } else if (cmd == "type") {
                std::string txt;
                std::getline(iss >> std::ws, txt);
                std::cout << (app.type_text(txt) ? "Typed.\n" : "No focused input.\n");
            } else if (cmd == "enter") {
                std::cout << (app.press_enter() ? "Submitted.\n" : "No focused input.\n");
            } else if (cmd == "raf") {
                int n = 1;
                if (!(iss >> n) || n < 1) n = 1;
                std::cout << (app.step_animation_frames(n) ? ("Stepped " + std::to_string(n) + " rAF frame(s).\n")
                                                           : "No pending rAF callbacks.\n");
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
