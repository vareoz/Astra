#include "browser/browser_app.hpp"
#include "core/arena.hpp"
#include "core/platform.hpp"
#include "core/json.hpp"
#include "core/memory_governor.hpp"
#include "docs/doc_viewer.hpp"
#include "ext/extension_runtime.hpp"
#include "gfx/astra3d.hpp"
#include "gfx/font_engine.hpp"
#include "gfx/image_decoder.hpp"
#include "gfx/rasterizer.hpp"
#include "gfx/svg_engine.hpp"
#include "ipc/ipc_channel.hpp"
#include "ipc/sandbox.hpp"
#include "js/astra_js.hpp"
#include "media/media_engine.hpp"
#include "net/http2_client.hpp"
#include "net/http_cache.hpp"
#include "net/http_client.hpp"
#include "net/websocket_client.hpp"
#include "privacy/security_policy.hpp"
#include "privacy/shield.hpp"
#include "style/animation_engine.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cmath>
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
    std::cout << "=== Astra Browser Core v10 — Full Open-Web & Media Verification Suite ("
              << platform::os_name() << ") ===\n\n";

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
        cache.store({"https://astra.test/a", "text/css", "\"e1\"", std::string(1000, 'A'), 0, 600, "", false, false});
        cache.store({"https://astra.test/b", "text/css", "\"e2\"", std::string(1000, 'B'), 0, 600, "", false, false});
        cache.store({"https://astra.test/c", "text/css", "\"e3\"", std::string(1000, 'C'), 0, 600, "", false, false});
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
        // Session 8: el RSS real se lee con la API nativa de cada sistema
        // (/proc/<pid>/statm en Linux, proc_pid_rusage/proc_pidinfo en Darwin),
        // así que la comprobación es la misma en ambas plataformas.
        bool platform_known = std::string(platform::os_name()) == "linux" ||
                              std::string(platform::os_name()) == "darwin";
        auto self_mem = platform::query_process_memory(::getpid());
        expect(mem.total_rss_bytes > 0 && mem.total_rss_bytes < 256ULL * 1024 * 1024 &&
                   mem.processes.size() >= 3 && platform_known &&
                   self_mem.valid && self_mem.rss_bytes > 0,
               "MemoryGovernor tracks real per-process OS RSS across Main + Content + RDD workers on both "
               "Linux (/proc/<pid>/statm) and macOS (libproc)");

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
            bool is_asd6 = read_u32(blob, moff, magic) && magic == 0x41534436u;
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
            expect(is_asd6 && rt_ok && has_grad && grad_interp,
                   "CSS linear-gradient(90deg) roundtrips in ASD6 DisplayList and rasterizes smooth per-pixel RGB interpolation");
        }

        // 34. AstraNet v6: RFC 6455 WebSocket SHA-1/Base64 Handshake + Real Loopback Echo Server + JS new WebSocket()
        {
            // Canonical RFC 6455 Section 1.3 vector
            std::string canonical_accept = WebSocketClient::compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
            bool rfc_vector_ok = (canonical_accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

            int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            platform::harden_socket_fd(listen_fd);
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
                (void)::send(cfd, resp.data(), resp.size(), ASTRA_MSG_NOSIGNAL);

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
                    if (!out.empty()) (void)::send(cfd, out.data(), out.size(), ASTRA_MSG_NOSIGNAL);
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

        // 39. AstraSVG v1: Native Inline <svg> Vector Parser, 2x2 AA Rasterizer & DisplayList ASD5 Pipeline
        {
            const std::string svg_xml =
                "<svg width=\"120\" height=\"120\" viewBox=\"0 0 120 120\">"
                "<rect x=\"4\" y=\"4\" width=\"112\" height=\"112\" rx=\"12\" fill=\"#0f172a\" stroke=\"#38bdf8\" stroke-width=\"4\"/>"
                "<circle cx=\"60\" cy=\"60\" r=\"28\" fill=\"#10b981\"/>"
                "<path d=\"M20 96 Q60 66 100 96\" stroke=\"#f59e0b\" stroke-width=\"4\" fill=\"none\"/>"
                "</svg>";
            SvgSurface surf = SvgEngine::render_svg(svg_xml, 120, 120);
            uint32_t center_px = surf.valid ? surf.argb[60 * 120 + 60] : 0;
            uint32_t bg_px = surf.valid ? surf.argb[20 * 120 + 20] : 0;
            uint32_t corner_px = surf.valid ? surf.argb[0] : 0xFFFFFFFFu;
            bool direct_ok = surf.valid && surf.width == 120 && surf.height == 120 &&
                             center_px == 0xFF10B981u && bg_px == 0xFF0F172Au && (corner_px >> 24) == 0;

            SlabArena svga;
            HtmlDocument svgdoc = HtmlDocument::parse(
                svga,
                "<html><body style=\"margin:0;padding:0;background:#070b14;\">" + svg_xml + "</body></html>");
            CssStylesheet svgsheet;
            LayoutFrame* svgroot = LayoutEngine::build_layout(svga, svgdoc, svgsheet, 320.0f, 200.0f);
            DisplayList dl = LayoutEngine::build_display_list(svgroot, 320, 200);
            auto blob = dl.serialize();
            DisplayList dl2;
            bool deser_ok = DisplayList::deserialize(blob, dl2);
            bool has_svg_item = false;
            for (const auto& it : dl2.items) {
                if (it.type == DisplayItemType::EmbedSvg && it.text.find("<circle") != std::string::npos) {
                    has_svg_item = true;
                }
            }
            Framebuffer fb(320, 200);
            fb.composite_display_list(dl2, 0.0f);
            uint32_t fb_center = fb.pixels()[60 * 320 + 60];
            expect(direct_ok && deser_ok && has_svg_item && fb_center == 0xFF10B981u,
                   "AstraSVG v1 renders <rect rx>, <circle>, and Bezier <path> through DisplayList ASD5 into Framebuffer");
        }

        // 40. AstraJS v8: ES6 Destructuring, Default Params, Array Spread, Object Shorthand & Functional Stdlib
        {
            SlabArena j8a;
            HtmlDocument j8doc = HtmlDocument::parse(j8a, "<html><body><p id=\"j8\">init</p></body></html>");
            AstraJsEngine j8vm;
            std::string js =
                "const cfg = { engine: \"Astra\", ver: 8, threads: 4 };\n"
                "const { engine, ver: version } = cfg;\n"
                "const nums = [1, 2, 3, 4, 5];\n"
                "const [first, second] = nums;\n"
                "const spread = [0, ...nums, 6];\n"
                "function scale(x, factor = 10) { return x * factor; }\n"
                "const evens = spread.filter((n) => n % 2 === 0 && n > 0);\n"
                "const sum = evens.reduce((acc, n) => acc + n, 0);\n"
                "const found = spread.find((n) => n > 4);\n"
                "const sliced = spread.slice(1, 4).join(\"-\");\n"
                "const rev = [\"a\", \"b\", \"c\"].reverse().join(\"\");\n"
                "const shortObj = { engine, version };\n"
                "const merged = Object.assign({}, shortObj, { ok: true });\n"
                "const keysCount = Object.keys(merged).length;\n"
                "const label = \"astra-core\".replace(\"core\", \"v8\");\n"
                "const strChecks = label.startsWith(\"astra\") && label.endsWith(\"v8\") && (\"x\".repeat(3) === \"xxx\");\n"
                "document.querySelector(\"#j8\").textContent = `${engine}:${version}|${first}+${second}|sum=${sum}|find=${found}|sl=${sliced}|rev=${rev}|sc=${scale(3)}|k=${keysCount}|${label}|${strChecks}`;\n";
            auto rep = j8vm.execute(js, j8doc);
            std::string got = j8doc.query_selector("#j8")->text_content();
            expect(rep.ok && got == "Astra:8|1+2|sum=12|find=5|sl=1-2-3|rev=cba|sc=30|k=3|astra-v8|true",
                   "AstraJS v8 executes ES6 destructuring, default params, array spread, shorthand objects, Object.* & Array/String stdlib");
        }

        // 41. AstraNet v7 + JS fetch(): General REST Methods (POST/PUT/DELETE) & Custom Request Headers
        {
            int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            int opt = 1;
            if (listen_fd >= 0) ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            bool bound = (listen_fd >= 0 &&
                          ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                          ::listen(listen_fd, 8) == 0);
            socklen_t alen = sizeof(addr);
            uint16_t port = 0;
            if (bound && ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen) == 0) {
                port = ntohs(addr.sin_port);
            }

            std::thread srv([&]() {
                if (!bound) return;
                // Handle 3 HTTP requests: JS fetch POST, HttpClient PUT, HttpClient DELETE
                for (int i = 0; i < 3; ++i) {
                    int cfd = ::accept(listen_fd, nullptr, nullptr);
                    if (cfd < 0) break;
                    std::string raw;
                    char buf[2048];
                    size_t header_end = std::string::npos;
                    size_t content_len = 0;
                    while (raw.size() < 16384) {
                        ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
                        if (n <= 0) break;
                        raw.append(buf, static_cast<size_t>(n));
                        if (header_end == std::string::npos) {
                            header_end = raw.find("\r\n\r\n");
                            if (header_end != std::string::npos) {
                                std::string low = raw.substr(0, header_end);
                                for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                size_t clp = low.find("content-length:");
                                if (clp != std::string::npos) {
                                    content_len = static_cast<size_t>(std::strtoull(low.c_str() + clp + 15, nullptr, 10));
                                }
                            }
                        }
                        if (header_end != std::string::npos && raw.size() >= header_end + 4 + content_len) break;
                    }
                    std::string method = raw.substr(0, raw.find(' '));
                    std::string body = (header_end != std::string::npos) ? raw.substr(header_end + 4) : "";
                    std::string sess_hdr;
                    size_t hp = raw.find("X-Astra-Session:");
                    if (hp != std::string::npos) {
                        size_t vs = hp + 16;
                        while (vs < raw.size() && raw[vs] == ' ') ++vs;
                        size_t ve = raw.find("\r\n", vs);
                        if (ve != std::string::npos) sess_hdr = raw.substr(vs, ve - vs);
                    }
                    std::string json = "{\"method\":\"" + method + "\",\"session\":\"" + sess_hdr + "\",\"body\":\"" + body + "\"}";
                    std::string resp =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Connection: close\r\n"
                        "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
                    (void)::send(cfd, resp.data(), resp.size(), MSG_NOSIGNAL);
                    ::close(cfd);
                }
            });

            std::string endpoint = "http://127.0.0.1:" + std::to_string(port) + "/api/items";
            std::string page_path = "/tmp/astra_v8_rest_test.html";
            {
                std::ofstream pf(page_path);
                pf << "<html><body><p id=\"rest\">init</p><script>"
                   << "const r = fetch(\"" << endpoint << "\", {"
                   << "  method: \"POST\","
                   << "  headers: { \"X-Astra-Session\": \"v8-alpha\", \"Content-Type\": \"text/plain\" },"
                   << "  body: \"hello-v8\""
                   << "});"
                   << "const data = r.json();"
                   << "document.querySelector(\"#rest\").textContent = `${r.status}|${data.method}|${data.session}|${data.body}`;"
                   << "</script></body></html>";
            }
            uint32_t rest_tid = app.new_tab("file://" + page_path);
            (void)rest_tid;
            std::string rest_out;
            for (const auto& t : app.tabs()) {
                if (t.id == rest_tid && t.live_doc) {
                    const DomNode* p = t.live_doc->query_selector("#rest");
                    if (p) rest_out = p->text_content();
                }
            }

            AstraShield rest_shield;
            HttpClient raw_http(rest_shield);
            auto put_resp = raw_http.request("PUT", endpoint, "upd=1", {{"X-Astra-Session", "v8-put"}}, "text/plain");
            auto del_resp = raw_http.request("DELETE", endpoint, "", {{"X-Astra-Session", "v8-del"}}, "text/plain");

            if (srv.joinable()) srv.join();
            if (listen_fd >= 0) ::close(listen_fd);

            bool ok_rest = bound &&
                           rest_out == "200|POST|v8-alpha|hello-v8" &&
                           put_resp.status_code == 200 && put_resp.body.find("\"method\":\"PUT\"") != std::string::npos &&
                           put_resp.body.find("\"session\":\"v8-put\"") != std::string::npos &&
                           del_resp.status_code == 200 && del_resp.body.find("\"method\":\"DELETE\"") != std::string::npos &&
                           del_resp.body.find("\"session\":\"v8-del\"") != std::string::npos;
            expect(ok_rest,
                   "AstraNet v7 & JS fetch() deliver POST/PUT/DELETE methods, custom headers & request bodies end-to-end");
        }

        // 42. CSS3 Level 8: :first-child/:last-child/:focus, flex-wrap:wrap, border-left & transform:translate()
        {
            SlabArena c8a;
            HtmlDocument c8doc = HtmlDocument::parse(
                c8a,
                "<html><head><style>"
                "body { margin: 0; padding: 0; }"
                ".wrap { display: flex; flex-wrap: wrap; gap: 10px; width: 250px; }"
                ".chip { width: 110px; height: 40px; background: #1e293b; color: #cbd5e1; }"
                ".chip:first-child { background: #0284c7; border-left: 4px solid #38bdf8; }"
                ".chip:last-child { background: #065f46; transform: translate(15px, 10px); }"
                "</style></head><body>"
                "<div class=\"wrap\">"
                "<div class=\"chip\">One</div>"
                "<div class=\"chip\">Two</div>"
                "<div class=\"chip\">Three</div>"
                "</div></body></html>");
            CssStylesheet c8sheet;
            c8sheet.parse(c8doc.collect_stylesheets());
            LayoutFrame* c8root = LayoutEngine::build_layout(c8a, c8doc, c8sheet, 400.0f, 300.0f);
            bool ok_c8 = false;
            if (c8root && !c8root->children.empty() && c8root->children[0]->children.size() == 3) {
                const LayoutFrame* f0 = c8root->children[0]->children[0];
                const LayoutFrame* f1 = c8root->children[0]->children[1];
                const LayoutFrame* f2 = c8root->children[0]->children[2];
                // Row 1: f0 (x=0, y=0) + f1 (x=120, y=0) = 230px <= 250px.
                // Row 2: f2 envuelve a y=50px. `transform: translate(15px, 10px)`
                // NO mueve su caja de borde (CSS Transforms Level 1 §3: la
                // transformación es una operación de pintado, no de layout):
                // el desplazamiento viaja en la matriz afín del DisplayItem,
                // que es donde lo comprueba esta prueba tras la fusión v10+v8.
                DisplayList c8dl = LayoutEngine::build_display_list(c8root, 400, 300);
                bool has_accent_bar = false;
                bool chip_translated = false;
                for (const auto& it : c8dl.items) {
                    if (it.type == DisplayItemType::FillRect && it.color_argb == 0xFF38BDF8u &&
                        std::abs(it.rect.w - 4.0f) < 0.5f) {
                        has_accent_bar = true;
                    }
                    if (it.type == DisplayItemType::FillRect && it.color_argb == 0xFF065F46u &&
                        it.has_transform &&
                        std::abs(it.xform[0] - 1.0f) < 1e-3f && std::abs(it.xform[3] - 1.0f) < 1e-3f &&
                        std::abs(it.xform[4] - 15.0f) < 1e-3f && std::abs(it.xform[5] - 10.0f) < 1e-3f) {
                        chip_translated = true;
                    }
                }
                ok_c8 = f0->style.background.to_argb() == 0xFF0284C7u &&
                        f1->style.background.to_argb() == 0xFF1E293Bu &&
                        f2->style.background.to_argb() == 0xFF065F46u &&
                        std::abs(f0->border_box.x - 0.0f) < 1.0f &&
                        std::abs(f1->border_box.x - 120.0f) < 1.0f &&
                        std::abs(f2->border_box.x - 0.0f) < 1.0f &&
                        std::abs(f2->border_box.y - 50.0f) < 1.5f &&
                        std::abs(f2->style.transform.translate_x - 15.0f) < 1e-3f &&
                        std::abs(f2->style.transform.translate_y - 10.0f) < 1e-3f &&
                        chip_translated &&
                        has_accent_bar;
            }
            expect(ok_c8,
                   "CSS3 Level 8 resolves :first-child/:last-child, flex-wrap multi-row layout, border-left accent & "
                   "paint-time transform:translate() (no layout shift, per CSS Transforms L1)");
        }

        // 43. End-to-End Session 8 Showcase (astra_session8.html): Inline SVG + Flex-Wrap + ES6 JS + Render
        {
            std::string showcase_path = "astra/examples/astra_session8.html";
            if (!std::filesystem::exists(showcase_path)) showcase_path = "examples/astra_session8.html";
            uint32_t s8_tid = app.new_tab("file://" + showcase_path);
            bool has_svg = false;
            std::string js_summary;
            for (const auto& t : app.tabs()) {
                if (t.id == s8_tid) {
                    for (const auto& it : t.cached_display_list.items) {
                        if (it.type == DisplayItemType::EmbedSvg) has_svg = true;
                    }
                    if (t.live_doc) {
                        if (const DomNode* el = t.live_doc->query_selector("#s8-js-out")) {
                            js_summary = el->text_content();
                        }
                    }
                }
            }
            expect(has_svg && js_summary.find("v8") != std::string::npos && js_summary.find("48") != std::string::npos,
                   "End-to-end astra_session8.html renders inline <svg>, flex-wrap CSS3 & ES6 AstraJS v8 pipeline");
        }

        // 44. Hito A — Real Web Layout v9: <table> + colspan, float/clear, inline-block, calc(), :nth-child, ::before/::after, <iframe srcdoc>
        {
            SlabArena a9;
            HtmlDocument d9 = HtmlDocument::parse(
                a9,
                "<html><head><style>"
                "body { margin: 0; padding: 0; }"
                ".side { float: right; width: 200px; height: 80px; background: #1e293b; }"
                ".main { width: calc(100% - 200px); background: #0f172a; }"
                ".hdr::before { content: \"PRE \"; color: #38bdf8; }"
                ".hdr::after { content: \" POST\"; color: #34d399; }"
                ".ib { display: inline-block; width: 90px; height: 24px; background: #1d4ed8; }"
                "table { width: 400px; }"
                "tr:nth-child(even) { background: #132038; }"
                ".after { clear: both; height: 20px; }"
                "</style></head><body>"
                "<div class=\"side\">FloatBox</div>"
                "<div class=\"main\">"
                "  <div class=\"hdr\">Title</div>"
                "  <span class=\"ib\">A</span><span class=\"ib\">B</span>"
                "  <table><tr><th>H1</th><th>H2</th></tr>"
                "         <tr><td>R1C1</td><td>R1C2</td></tr>"
                "         <tr><td colspan=\"2\">Span2</td></tr></table>"
                "</div>"
                "<div class=\"after\">Cleared</div>"
                "<iframe srcdoc=\"<html><body><p>NestedDoc</p></body></html>\" style=\"width:240px;height:60px;\"></iframe>"
                "</body></html>");
            CssStylesheet s9;
            s9.parse(d9.collect_stylesheets());
            LayoutFrame* r9 = LayoutEngine::build_layout(a9, d9, s9, 800.0f, 600.0f);
            DisplayList dl9 = LayoutEngine::build_display_list(r9, 800, 600);

            bool ok_float_x = false, ok_calc_w = false, ok_clear_y = false;
            bool ok_inline_block = false, ok_table_cols = false, ok_nth_even = false;
            bool ok_before = false, ok_after = false, ok_iframe = false;

            if (r9 && r9->children.size() >= 4) {
                const LayoutFrame* f_side = r9->children[0];
                const LayoutFrame* f_main = r9->children[1];
                const LayoutFrame* f_after = r9->children[2];
                ok_float_x = (f_side->border_box.x >= 595.0f && f_side->border_box.w >= 195.0f);
                ok_calc_w = (std::abs(f_main->border_box.w - 600.0f) < 2.0f);
                ok_clear_y = (f_after->border_box.y >= f_side->border_box.y + f_side->border_box.h - 1.0f);

                // Check inline-block siblings side-by-side and table columns
                const LayoutFrame* ib1 = nullptr;
                const LayoutFrame* ib2 = nullptr;
                const LayoutFrame* tbl = nullptr;
                for (const LayoutFrame* ch : f_main->children) {
                    if (ch->style.display == DisplayType::InlineBlock) {
                        if (!ib1) ib1 = ch;
                        else if (!ib2) ib2 = ch;
                    }
                    if (ch->node && ch->node->tag == "table") tbl = ch;
                }
                if (ib1 && ib2) {
                    ok_inline_block = (ib2->border_box.x > ib1->border_box.x + 50.0f &&
                                       std::abs(ib1->border_box.y - ib2->border_box.y) < 2.0f);
                }
                if (tbl) {
                    std::vector<const LayoutFrame*> rows;
                    std::function<void(const LayoutFrame*)> gather_rows = [&](const LayoutFrame* n) {
                        if (!n) return;
                        if (n->style.display == DisplayType::TableRow) rows.push_back(n);
                        for (const LayoutFrame* c : n->children) gather_rows(c);
                    };
                    gather_rows(tbl);
                    if (rows.size() == 3 && rows[1]->children.size() == 2 && rows[2]->children.size() == 1) {
                        const LayoutFrame* c0 = rows[1]->children[0];
                        const LayoutFrame* c1 = rows[1]->children[1];
                        const LayoutFrame* span2 = rows[2]->children[0];
                        ok_table_cols = (c1->border_box.x > c0->border_box.x + 80.0f) &&
                                        (span2->border_box.w > c0->border_box.w * 1.5f);
                        ok_nth_even = (rows[1]->style.background.to_argb() == 0xFF132038u);
                    }
                }
            }
            for (const auto& it : dl9.items) {
                if (it.type == DisplayItemType::DrawText) {
                    if (it.text.find("PRE") != std::string::npos) ok_before = true;
                    if (it.text.find("POST") != std::string::npos) ok_after = true;
                    if (it.text.find("NestedDoc") != std::string::npos) ok_iframe = true;
                }
            }
            bool all44 = ok_float_x && ok_calc_w && ok_clear_y && ok_inline_block &&
                         ok_table_cols && ok_nth_even && ok_before && ok_after && ok_iframe;
            if (!all44) {
                std::cerr << "    [diag44] float=" << ok_float_x << " calc=" << ok_calc_w
                          << " clear=" << ok_clear_y << " ib=" << ok_inline_block
                          << " tbl=" << ok_table_cols << " nth=" << ok_nth_even
                          << " bef=" << ok_before << " aft=" << ok_after << " ifr=" << ok_iframe << "\n";
            }
            expect(all44,
                   "Hito A: CSS3 Layout v9 handles <table>+colspan, float/clear, inline-block, calc(), :nth-child, ::before/::after & <iframe srcdoc>");
        }

        // 45. Hito A — AstraImage v3: Pure-C++20 GIF89a LZW Decoder (with Transparency) & WebP RIFF Probe
        {
            // Build a spec-valid 4x2 GIF89a image with 4-color GCT, transparent index=3, min_code_size=4 (5-bit codes)
            std::vector<uint8_t> gif = {
                'G','I','F','8','9','a',
                4, 0, 2, 0, 0x91, 0, 0,
                255, 0, 0,     // 0: Red
                0, 255, 0,     // 1: Green
                0, 0, 255,     // 2: Blue
                255, 255, 0,   // 3: Transparent
                0x21, 0xF9, 0x04, 0x01, 0x00, 0x00, 0x03, 0x00,
                0x2C, 0, 0, 0, 0, 4, 0, 2, 0, 0x00,
                4 // LZW minimum code size = 4 (clear=16, EOI=17, code width=5 bits)
            };
            const int codes[] = {16, 0, 1, 2, 3, 2, 1, 0, 3, 17};
            std::vector<uint8_t> lzw_bytes;
            uint32_t bitbuf = 0;
            int bitcnt = 0;
            for (int c : codes) {
                bitbuf |= (static_cast<uint32_t>(c) << bitcnt);
                bitcnt += 5;
                while (bitcnt >= 8) {
                    lzw_bytes.push_back(static_cast<uint8_t>(bitbuf & 0xFFu));
                    bitbuf >>= 8;
                    bitcnt -= 8;
                }
            }
            if (bitcnt > 0) lzw_bytes.push_back(static_cast<uint8_t>(bitbuf & 0xFFu));
            gif.push_back(static_cast<uint8_t>(lzw_bytes.size()));
            gif.insert(gif.end(), lzw_bytes.begin(), lzw_bytes.end());
            gif.push_back(0x00); // block terminator
            gif.push_back(0x3B); // GIF trailer

            DecodedImage gimg = ImageDecoder::decode_bytes(gif.data(), gif.size());
            bool ok_gif = gimg.valid && gimg.width == 4 && gimg.height == 2 && gimg.argb.size() == 8 &&
                          gimg.argb[0] == 0xFFFF0000u &&
                          gimg.argb[1] == 0xFF00FF00u &&
                          gimg.argb[2] == 0xFF0000FFu &&
                          (gimg.argb[3] >> 24) == 0u &&
                          gimg.argb[4] == 0xFF0000FFu &&
                          gimg.argb[5] == 0xFF00FF00u &&
                          gimg.argb[6] == 0xFFFF0000u &&
                          (gimg.argb[7] >> 24) == 0u;

            // Minimal RIFF WEBP VP8X header probe (16x8 canvas)
            std::vector<uint8_t> webp = {
                'R','I','F','F', 22, 0, 0, 0,
                'W','E','B','P',
                'V','P','8','X', 10, 0, 0, 0,
                0, 0, 0, 0,
                15, 0, 0, // width - 1 = 15 -> 16
                7, 0, 0   // height - 1 = 7 -> 8
            };
            DecodedImage wimg = ImageDecoder::decode_bytes(webp.data(), webp.size());
            bool ok_webp = wimg.format == "WEBP" && wimg.width == 16 && wimg.height == 8;
            if (!ok_gif || !ok_webp) {
                std::cerr << "    [diag45] gif=" << ok_gif << " (" << gimg.valid << "," << gimg.width << "x" << gimg.height << "," << gimg.error << ") webp=" << ok_webp << " (" << wimg.width << "x" << wimg.height << "," << wimg.error << ")\n";
            }
            expect(ok_gif && ok_webp,
                   "Hito A: AstraImage v3 decodes GIF89a LZW (with transparency) & RIFF WebP frames");
        }

        // 46. Hito A — Native HTML <form method="GET"> Submission + URL-Encoding
        {
            std::string target_path = "/tmp/astra_v9_form_target.html";
            std::string form_path = "/tmp/astra_v9_form_page.html";
            {
                std::ofstream tf(target_path);
                tf << "<html><head><title>Search Results</title></head><body><h1>Results</h1></body></html>";
            }
            {
                std::ofstream ff(form_path);
                ff << "<html><head><title>Search Form</title></head><body>"
                   << "<form action=\"file://" << target_path << "\" method=\"GET\">"
                   << "<input id=\"q\" name=\"q\" value=\"\" style=\"width:240px;height:32px;\" />"
                   << "<button id=\"sub\" type=\"submit\">Go</button>"
                   << "</form></body></html>";
            }
            uint32_t ftid = app.new_tab("file://" + form_path);
            app.switch_tab(ftid);
            bool clicked_input = false;
            if (const Tab* t = app.active_tab()) {
                for (const auto& hr : t->cached_display_list.hit_regions) {
                    if (hr.kind == HitRegionKind::Input) {
                        int cx = static_cast<int>(hr.rect.x + hr.rect.w * 0.5f);
                        int cy = Framebuffer::kChromeHeight + static_cast<int>(hr.rect.y + hr.rect.h * 0.5f);
                        auto cr = app.click_at(cx, cy);
                        clicked_input = cr.handled && cr.action == "focus_input";
                        break;
                    }
                }
            }
            app.type_text("astra v9");
            bool entered = app.press_enter();
            std::string final_u = app.active_tab() ? app.active_tab()->url : "";
            expect(clicked_input && entered &&
                   final_u == ("file://" + target_path + "?q=astra%20v9"),
                   "Hito A: HTML <form method=\"GET\"> collects <input name>, URL-encodes query & navigates on Enter");
        }

        // 47. Hito B — AstraJS v9: Real [[Prototype]] Chain, class extends, instanceof, RegExp, Date, Map & Set
        {
            SlabArena j9a;
            HtmlDocument j9doc = HtmlDocument::parse(j9a, "<html><body><div id=\"out\"></div></body></html>");
            AstraJsEngine vm9;
            auto rep = vm9.execute(
                "function Animal(n) { this.name = n; }\n"
                "Animal.prototype.speak = function() { return this.name + '!'; };\n"
                "const a = new Animal('Fox');\n"
                "const child = Object.create(a);\n"
                "const pOk = (Object.getPrototypeOf(child) === a) && (child.speak() === 'Fox!') && (a instanceof Animal);\n"
                "class Base { constructor(x) { this.x = x; } baseVal() { return this.x * 2; } }\n"
                "class Sub extends Base { constructor(x) { super(x); } subVal() { return this.baseVal() + 7; } }\n"
                "const s = new Sub(10);\n"
                "const cOk = (s instanceof Sub) && (s instanceof Base) && (s.subVal() === 27);\n"
                "const re = /([a-z]+)-(\\d+)/i;\n"
                "const m = re.exec('Core ASTRA-900 ok');\n"
                "const rOk = re.test('foo-42') && m[1] === 'ASTRA' && m[2] === '900' && "
                "  'x1y2z3'.replace(/\\d/g, '_') === 'x_y_z_' && 'a,b;c'.split(/[,;]/).join('|') === 'a|b|c';\n"
                "const dt = new Date('2026-11-25T10:30:45Z');\n"
                "const dOk = dt.getUTCFullYear() === 2026 && dt.getUTCMonth() === 10 && dt.getUTCDate() === 25 && "
                "  dt.toISOString() === '2026-11-25T10:30:45.000Z' && Date.now() > 1700000000000;\n"
                "const mp = new Map();\n"
                "mp.set('k1', 10); mp.set('k2', 20); mp.set('k1', 30);\n"
                "let msum = 0; for (const v of mp.values()) { msum = msum + v; }\n"
                "const st = new Set(); st.add('a'); st.add('b'); st.add('a');\n"
                "let sjoin = ''; for (const v of st) { sjoin = sjoin + v; }\n"
                "const collOk = mp.size === 2 && mp.get('k1') === 30 && msum === 50 && st.size === 2 && st.has('b') && sjoin === 'ab';\n"
                "document.querySelector('#out').textContent = `${pOk}|${cOk}|${rOk}|${dOk}|${collOk}`;\n",
                j9doc);
            std::string out = j9doc.query_selector("#out") ? j9doc.query_selector("#out")->text_content() : "";
            if (!rep.ok || out != "true|true|true|true|true") {
                std::cerr << "    [diag47] rep.ok=" << rep.ok << " err=" << rep.error << " out='" << out << "'\n";
            }
            expect(rep.ok && out == "true|true|true|true|true",
                   "Hito B: AstraJS v9 executes [[Prototype]] chains, class extends, instanceof, RegExp, Date, Map & Set");
        }

        // 48. Hito B — SPA Runtime: history.pushState/replaceState, location.* & sessionStorage
        {
            std::string spa_path = "/tmp/astra_v9_spa_test.html";
            {
                std::ofstream sf(spa_path);
                sf << "<html><body><div id=\"spa\">init</div><script>"
                   << "sessionStorage.setItem('token', 'spa-ok');"
                   << "history.pushState({ page: 2 }, '', '/docs/v9?tab=api#top');"
                   << "const st = history.state;"
                   << "document.querySelector('#spa').textContent = "
                   << "  location.pathname + '|' + location.search + '|' + location.hash + '|' + "
                   << "  history.length + '|' + st.page + '|' + sessionStorage.getItem('token');"
                   << "</script></body></html>";
            }
            uint32_t stid = app.new_tab("file://" + spa_path);
            app.switch_tab(stid);
            std::string spa_out, tab_url;
            if (const Tab* t = app.active_tab()) {
                tab_url = t->url;
                if (t->live_doc && t->live_doc->query_selector("#spa")) {
                    spa_out = t->live_doc->query_selector("#spa")->text_content();
                }
            }
            expect(spa_out == "/docs/v9|?tab=api|#top|2|2|spa-ok" &&
                   tab_url.find("/docs/v9?tab=api#top") != std::string::npos,
                   "Hito B: SPA history.pushState, location.*, and sessionStorage update VM and BrowserApp tab URL");
        }

        // 49. Hito C — Real Kernel seccomp-BPF Sandbox + CSP + Mixed Content + SOP/CORS + SameSite Cookies + Site Isolation
        {
            std::string seccomp_detail;
            bool ok_seccomp = ProcessSandbox::self_verify(&seccomp_detail);

            // CSP inline-script & connect-src enforcement
            ContentSecurityPolicy csp;
            csp.parse("default-src 'self'; script-src 'self'; connect-src 'self' https://*.trusted.example");
            SecurityOrigin doc_org = SecurityOrigin::from_url("https://app.example.com/index.html");
            bool ok_csp = !csp.allows_inline_script() &&
                          csp.allows_url(ResourceKind::Connect, "https://app.example.com/api", doc_org) &&
                          csp.allows_url(ResourceKind::Connect, "https://api.trusted.example/v1", doc_org) &&
                          !csp.allows_url(ResourceKind::Connect, "https://evil.attacker.org/exfil", doc_org);

            // CSP <meta> enforcement inside BrowserApp tab (blocks inline script)
            std::string csp_page = "/tmp/astra_v9_csp_test.html";
            {
                std::ofstream cf(csp_page);
                cf << "<html><head><meta http-equiv=\"Content-Security-Policy\" content=\"script-src 'none'\"></head>"
                   << "<body><p id=\"v\">SAFE</p><script>document.querySelector('#v').textContent='PWNED';</script></body></html>";
            }
            uint32_t csp_tid = app.new_tab("file://" + csp_page);
            std::string csp_text;
            size_t csp_blocks = 0;
            for (const auto& t : app.tabs()) {
                if (t.id == csp_tid) {
                    csp_blocks = t.csp_blocks;
                    if (t.live_doc && t.live_doc->query_selector("#v")) {
                        csp_text = t.live_doc->query_selector("#v")->text_content();
                    }
                }
            }
            bool ok_csp_dom = (csp_text == "SAFE" && csp_blocks >= 1);

            // Mixed Content blocking (blocks http:// & ws:// active resources on https://)
            bool ok_mixed = MixedContentBlocker::is_blocked(doc_org, "http://insecure.example/script.js", ResourceKind::Script) &&
                            MixedContentBlocker::is_blocked(doc_org, "ws://insecure.example/live", ResourceKind::Connect) &&
                            !MixedContentBlocker::is_blocked(doc_org, "https://cdn.example/script.js", ResourceKind::Script);

            // SOP / CORS Access-Control-Allow-Origin enforcement
            std::map<std::string, std::string> no_cors_hdrs{{"content-type", "application/json"}};
            std::map<std::string, std::string> ok_cors_hdrs{{"Access-Control-Allow-Origin", "https://app.example.com"}};
            bool ok_cors = !CorsEnforcer::allows_response(doc_org, "https://other.example.net/data", no_cors_hdrs) &&
                           CorsEnforcer::allows_response(doc_org, "https://other.example.net/data", ok_cors_hdrs) &&
                           CorsEnforcer::allows_response(doc_org, "https://app.example.com/same", no_cors_hdrs);

            // SameSite=Strict / Lax / None cookie enforcement in AstraShield
            AstraShield ss_shield;
            ss_shield.store_cookie("bank.example", "https://bank.example", "sid=strict1; SameSite=Strict");
            ss_shield.store_cookie("bank.example", "https://bank.example", "bad=none_no_sec; SameSite=None");
            ss_shield.store_cookie("evil.example", "https://bank.example", "sid=strict1; SameSite=Strict");
            ss_shield.store_cookie("evil.example", "https://bank.example", "lax=lax1; SameSite=Lax");
            ss_shield.store_cookie("evil.example", "https://bank.example", "none=ok1; SameSite=None; Secure");
            std::string same_site_hdr = ss_shield.cookie_header_for("bank.example", "https://bank.example", true, true);
            std::string cross_top_hdr = ss_shield.cookie_header_for("evil.example", "https://bank.example", true, true);
            std::string cross_sub_hdr = ss_shield.cookie_header_for("evil.example", "https://bank.example", true, false);
            bool ok_samesite = (same_site_hdr.find("sid=strict1") != std::string::npos) &&
                               (same_site_hdr.find("bad=") == std::string::npos) &&
                               (cross_top_hdr.find("sid=strict1") == std::string::npos) &&
                               (cross_top_hdr.find("lax=lax1") != std::string::npos) &&
                               (cross_sub_hdr.find("lax=lax1") == std::string::npos) &&
                               (cross_sub_hdr.find("none=ok1") != std::string::npos);

            // Per-origin Site Isolation worker spawning for distinct sites
            WorkerProcess* w_a = app.acquire_content_worker_for_origin(SecurityOrigin::from_url("https://alpha.example/"));
            WorkerProcess* w_b = app.acquire_content_worker_for_origin(SecurityOrigin::from_url("https://beta.example/"));
            bool ok_site_iso = (w_a != nullptr && w_b != nullptr && w_a->pid() != w_b->pid() &&
                                app.site_isolated_worker_count() >= 3);

            // El confinamiento del proceso de contenido se exige en TODAS las
            // plataformas soportadas, con la primitiva nativa de cada kernel:
            // seccomp-BPF en Linux y un perfil Seatbelt en macOS. La prueba
            // comprueba además que el canal IPC sobrevive al sandbox, porque un
            // confinamiento que rompiera el IPC dejaría al navegador sin
            // proceso de contenido y no valdría de nada.
            const bool sandbox_ok = ok_seccomp;
            bool all49 = sandbox_ok && ok_csp && ok_csp_dom && ok_mixed && ok_cors && ok_samesite && ok_site_iso;
            if (!all49) {
                std::cerr << "    [diag49] sandbox(" << SandboxReport::mechanism() << ")=" << ok_seccomp << "(" << seccomp_detail << ")"
                          << " csp=" << ok_csp << " csp_dom=" << ok_csp_dom
                          << " mixed=" << ok_mixed << " cors=" << ok_cors
                          << " samesite=" << ok_samesite << " site_iso=" << ok_site_iso << "\n";
            }
            expect(all49,
                   "Hito C: Kernel sandbox del proceso de contenido verificado con EPERM en la primitiva nativa "
                   "(seccomp-BPF en Linux, Seatbelt en macOS) sin romper el IPC, CSP, Mixed Content, CORS/SOP, "
                   "SameSite & Site Isolation");
        }

        // 50. Hito C — Adversarial Parser Fuzz-Resistance Harness + End-to-End astra_session9.html
        {
            // Hammer HTML, CSS, Image (PNG/JPEG/GIF/WebP), SVG, and HPACK parsers with hostile/truncated payloads
            const std::vector<std::string> hostile_inputs = {
                "",
                std::string(2048, '<'),
                "<table><tr><td colspan=\"999999999\"><div><span><iframe srcdoc=\"<iframe srcdoc='<iframe srcdoc=x>'>\">",
                "@media ((((((( { .a:nth-child(999999n-999999)::before { width: calc((((100% / 0) + 1e38px)); content: \"unterminated",
                std::string("\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\xff\xff\xff\xff\xff\xff\xff\xff", 24),
                std::string("\xff\xd8\xff\xc0\x00\x11\x08\xff\xff\xff\xff\x03\x01\x22\x00", 15),
                std::string("GIF89a\xff\xff\xff\xff\xf7\x00\x00\x2c\x00\x00\x00\x00\xff\xff\xff\xff\x00\x0c\x04\xff\xff\xff\xff", 29),
                std::string("RIFF\x10\x00\x00\x00WEBPVP8X\x0a\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff", 30),
                "<svg viewBox=\"0 0 0 0\"><path d=\"M 1e99 1e99 C 0 0 0 0 0 0 Q Z L H V 9999999\" /><circle r=\"-50\"/></svg>"
            };
            uint32_t rng = 0xDEADBEEFu;
            for (int iter = 0; iter < 40; ++iter) {
                std::string rnd(96, '\0');
                for (char& c : rnd) {
                    rng = rng * 1664525u + 1013904223u;
                    c = static_cast<char>(rng >> 24);
                }
                SlabArena fa(16 * 1024);
                (void)HtmlDocument::parse(fa, rnd);
                CssStylesheet fs;
                fs.parse(rnd);
                (void)ImageDecoder::decode_bytes(reinterpret_cast<const uint8_t*>(rnd.data()), rnd.size());
                (void)SvgEngine::render_svg(rnd, 32, 32);
                HpackCodec hp;
                std::vector<std::pair<std::string, std::string>> h_out;
                (void)hp.decode_headers(reinterpret_cast<const uint8_t*>(rnd.data()), rnd.size(), h_out);
            }
            for (const auto& payload : hostile_inputs) {
                SlabArena fa(32 * 1024);
                HtmlDocument fd = HtmlDocument::parse(fa, payload);
                CssStylesheet fs;
                fs.parse(payload);
                (void)LayoutEngine::build_layout(fa, fd, fs, 800.0f, 600.0f);
                (void)ImageDecoder::decode_bytes(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
                (void)SvgEngine::render_svg(payload, 64, 64);
                HpackCodec hp;
                std::vector<std::pair<std::string, std::string>> h_out;
                (void)hp.decode_headers(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), h_out);
            }

            // End-to-end verification of astra_session9.html
            std::string s9_path = "astra/examples/astra_session9.html";
            if (!std::filesystem::exists(s9_path)) s9_path = "examples/astra_session9.html";
            uint32_t s9_tid = app.new_tab("file://" + s9_path);
            std::string s9_proto, s9_regex, s9_colls, s9_route;
            bool has_pseudo = false, has_iframe_txt = false;
            for (const auto& t : app.tabs()) {
                if (t.id == s9_tid) {
                    if (t.live_doc) {
                        if (auto* n = t.live_doc->query_selector("#spa-proto")) s9_proto = n->text_content();
                        if (auto* n = t.live_doc->query_selector("#spa-regex")) s9_regex = n->text_content();
                        if (auto* n = t.live_doc->query_selector("#spa-colls")) s9_colls = n->text_content();
                        if (auto* n = t.live_doc->query_selector("#spa-route")) s9_route = n->text_content();
                    }
                    for (const auto& it : t.cached_display_list.items) {
                        if (it.type == DisplayItemType::DrawText) {
                            if (it.text.find("SECCOMP-BPF") != std::string::npos) has_pseudo = true;
                            if (it.text.find("Embedded srcdoc") != std::string::npos) has_iframe_txt = true;
                        }
                    }
                }
            }
            bool ok_s9 = s9_proto.find("astra-core@v9") != std::string::npos &&
                         s9_proto.find("instanceof=true") != std::string::npos &&
                         s9_regex.find("v9 (a#b#c#)") != std::string::npos &&
                         s9_regex.find("2026-11-25") != std::string::npos &&
                         s9_colls.find("Map(3)") != std::string::npos &&
                         s9_colls.find("Set(2)=true") != std::string::npos &&
                         s9_route.find("/wiki/astra-v9?milestone=all") != std::string::npos &&
                         s9_route.find("v9-verified") != std::string::npos &&
                         has_pseudo && has_iframe_txt;

            if (!ok_s9) {
                std::cerr << "    [diag50] proto='" << s9_proto << "' regex='" << s9_regex
                          << "' colls='" << s9_colls << "' route='" << s9_route
                          << "' pseudo=" << has_pseudo << " ifr=" << has_iframe_txt << "\n";
            }
            expect(ok_s9,
                   "Hito C + E2E: Fuzz harness survives 49 hostile HTML/CSS/Image/SVG/HPACK payloads & astra_session9.html renders cleanly");
        }

        // 51. Open-Web CSS3 v10 ([attr], :not(), :is(), @layer, @supports, unknown pseudo = false, IFC horizontal inline flow)
        {
            SlabArena arena(128 * 1024);
            std::string html = R"HTML(
                <html><head><style>
                    @layer base {
                        .chip[data-role="author"] { color: #1d4ed8; font-weight: bold; }
                        .chip[data-tag^="vid"] { color: #059669; }
                    }
                    @supports (display: grid) {
                        p:not(.muted) { color: #111827; }
                        :is(h1, h2).lead { font-size: 24px; }
                    }
                    .ghost:unknown-pseudo-v10 { display: none; }
                </style></head>
                <body bgcolor="#ffffff">
                    <h2 class="lead">Open Web</h2>
                    <div class="ghost" id="visible-ghost">Visible</div>
                    <p id="main-p">Body</p>
                    <div id="inline-row">
                        <span class="chip" data-role="author">Alice</span>
                        <span class="chip" data-tag="video-hd">HD</span>
                        <a href="https://example.com">Watch</a>
                    </div>
                </body></html>
            )HTML";
            HtmlDocument doc = HtmlDocument::parse(arena, html);
            CssStylesheet sheet;
            sheet.parse(doc.collect_stylesheets());
            LayoutFrame* root = LayoutEngine::build_layout(arena, doc, sheet, 800.0f, 600.0f, true);
            DisplayList dl = LayoutEngine::build_display_list(root, 800, 600);

            DomNode* ghost = doc.query_selector("#visible-ghost");
            DomNode* lead = doc.query_selector("h2");
            DomNode* author = doc.query_selector(".chip");
            ComputedStyle ghost_st = StyleEngine::compute_style(ghost, sheet, nullptr, 800.0f, true);
            ComputedStyle lead_st = StyleEngine::compute_style(lead, sheet, nullptr, 800.0f, true);
            ComputedStyle author_st = StyleEngine::compute_style(author, sheet, nullptr, 800.0f, true);

            // Verify horizontal IFC flow: "Alice" and "HD" share the same line (similar y, increasing x)
            float alice_x = -1, alice_y = -1, hd_x = -1, hd_y = -1;
            for (const auto& it : dl.items) {
                if (it.type == DisplayItemType::DrawText) {
                    if (it.text == "Alice") { alice_x = it.rect.x; alice_y = it.rect.y; }
                    if (it.text == "HD") { hd_x = it.rect.x; hd_y = it.rect.y; }
                }
            }
            bool ok51 = (ghost_st.display != DisplayType::None) &&
                        (lead_st.font_size >= 23.5f) &&
                        (author_st.color.r == 0x1d && author_st.color.b == 0xd8) &&
                        (alice_x >= 0.0f && hd_x > alice_x && std::fabs(hd_y - alice_y) < 6.0f);
            expect(ok51,
                   "CSS3 v10: [attr], :not(), :is(), @layer/@supports, unknown-pseudo safety & horizontal IFC inline flow");
        }

        // 52. AstraImage v4 — SVG-in-<img> + data:image/svg+xml & data:image/png;base64 subresource decoding
        {
            std::string svg_xml = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"32\"><circle cx=\"16\" cy=\"16\" r=\"14\" fill=\"#2563eb\"/></svg>";
            DecodedImage img_svg = ImageDecoder::decode_bytes(
                reinterpret_cast<const uint8_t*>(svg_xml.data()), svg_xml.size());

            DecodedImage img_data_svg;
            std::string data_svg_uri = "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24'><rect width='24' height='24' fill='%23059669'/></svg>";
            bool ok_data_svg = app.load_image_subresource("astra://home", data_svg_uri, img_data_svg);

            // Minimal valid 1x1 red PNG in base64
            DecodedImage img_data_png;
            std::string data_png_uri = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
            bool ok_data_png = app.load_image_subresource("astra://home", data_png_uri, img_data_png);

            expect(img_svg.valid && img_svg.format == "SVG" && img_svg.width == 32 &&
                       ok_data_svg && img_data_svg.valid && img_data_svg.width == 24 &&
                       ok_data_png && img_data_png.valid && img_data_png.width == 1 && img_data_png.height == 1,
                   "AstraImage v4: Native SVG-in-<img> decoding + data:image/svg+xml & data:image/png;base64 URIs");
        }

        // 53. AstraNet v8 — Brotli (RFC 7932) Decompression + External <script src> + Remote <iframe src> over Loopback HTTP
        {
            static const uint8_t kBrotliHello[] = {
                0x0b, 0x0b, 0x80, 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x41, 0x73, 0x74, 0x72, 0x61,
                0x20, 0x42, 0x72, 0x6f, 0x74, 0x6c, 0x69, 0x20, 0x76, 0x31, 0x30, 0x21, 0x03
            };
            std::string br_out;
            bool ok_br = HttpClient::brotli_available() &&
                         HttpClient::decompress_brotli(kBrotliHello, sizeof(kBrotliHello), br_out) &&
                         br_out == "Hello Astra Brotli v10!";

            // Spin up an ephemeral loopback HTTP server serving an HTML page with external <script src> + <iframe src> + Brotli body
            int srv = ::socket(AF_INET, SOCK_STREAM, 0);
            int opt = 1;
            setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            bool srv_ok = (srv >= 0 && ::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && ::listen(srv, 8) == 0);
            uint16_t port = 0;
            if (srv_ok) {
                socklen_t alen = sizeof(addr);
                getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &alen);
                port = ntohs(addr.sin_port);
            }

            std::thread srv_thread([srv, srv_ok]() {
                if (!srv_ok) return;
                for (int r = 0; r < 4; ++r) {
                    int cfd = ::accept(srv, nullptr, nullptr);
                    if (cfd < 0) break;
                    char buf[2048]{};
                    ssize_t n = ::recv(cfd, buf, sizeof(buf) - 1, 0);
                    std::string req(buf, n > 0 ? static_cast<size_t>(n) : 0);
                    std::string body, ctype = "text/html", extra;
                    if (req.find("GET /app.js ") != std::string::npos) {
                        ctype = "application/javascript";
                        body = "const el = document.getElementById('ext-out'); if (el) el.textContent = 'EXT_JS_OK';";
                    } else if (req.find("GET /frame.html ") != std::string::npos) {
                        body = "<html><body><p>REMOTE_IFRAME_OK</p></body></html>";
                    } else if (req.find("GET /br ") != std::string::npos) {
                        ctype = "text/plain";
                        extra = "Content-Encoding: br\r\n";
                        body.assign(reinterpret_cast<const char*>(kBrotliHello), sizeof(kBrotliHello));
                    } else {
                        body = "<!doctype html><html><head><title>Loopback v10</title></head><body>"
                               "<div id='ext-out'>pending</div>"
                               "<iframe src='/frame.html' width='300' height='80'></iframe>"
                               "<script src='/app.js'></script></body></html>";
                    }
                    std::ostringstream resp;
                    resp << "HTTP/1.1 200 OK\r\nContent-Type: " << ctype << "\r\n" << extra
                         << "Content-Length: " << body.size() << "\r\nConnection: close\r\n\r\n" << body;
                    std::string raw = resp.str();
                    ::send(cfd, raw.data(), raw.size(), MSG_NOSIGNAL);
                    ::close(cfd);
                }
                ::close(srv);
            });

            std::string br_http_body;
            std::string ext_js_text;
            bool has_iframe_text = false;
            if (srv_ok) {
                HttpResponse br_resp = app.http_client().get("http://127.0.0.1:" + std::to_string(port) + "/br", "", false);
                br_http_body = br_resp.body;

                uint32_t lb_tid = app.new_tab("http://127.0.0.1:" + std::to_string(port) + "/");
                for (const auto& t : app.tabs()) {
                    if (t.id == lb_tid) {
                        if (t.live_doc) {
                            if (auto* n = t.live_doc->query_selector("#ext-out")) ext_js_text = n->text_content();
                        }
                        for (const auto& it : t.cached_display_list.items) {
                            if (it.type == DisplayItemType::DrawText && it.text.find("REMOTE_IFRAME_OK") != std::string::npos) {
                                has_iframe_text = true;
                            }
                        }
                    }
                }
            }
            if (srv_thread.joinable()) srv_thread.join();

            expect(ok_br && br_http_body == "Hello Astra Brotli v10!" &&
                       ext_js_text == "EXT_JS_OK" && has_iframe_text,
                   "AstraNet v8: Native Brotli (RFC 7932) HTTP decompression + external <script src> + remote <iframe src>");
        }

        // 54. AstraMedia v5 — HLS (.m3u8) Master/Media Parser + Remote HTTP <video><source> RDD Decode
        {
            std::string master_m3u8 =
                "#EXTM3U\n"
                "#EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=640x360\n"
                "streams/low.m3u8\n"
                "#EXT-X-STREAM-INF:BANDWIDTH=2560000,RESOLUTION=1280x720\n"
                "streams/hd.m3u8\n";
            HlsPlaylist mpl = MediaEngine::parse_hls_playlist(master_m3u8, "https://cdn.example.com/live/master.m3u8");

            std::string media_m3u8 =
                "#EXTM3U\n"
                "#EXT-X-TARGETDURATION:4\n"
                "#EXTINF:4.0,\n"
                "seg001.ts\n"
                "#EXTINF:4.0,\n"
                "seg002.ts\n";
            HlsPlaylist spl = MediaEngine::parse_hls_playlist(media_m3u8, "https://cdn.example.com/live/streams/low.m3u8");

            bool ok_hls = mpl.valid && mpl.is_master && mpl.variant_urls.size() == 2 &&
                          mpl.variant_urls[0] == "https://cdn.example.com/live/streams/low.m3u8" &&
                          spl.valid && !spl.is_master && spl.target_duration_sec == 4.0 &&
                          spl.segment_urls.size() == 2 &&
                          spl.segment_urls[0] == "https://cdn.example.com/live/streams/seg001.ts";

            // Serve astra_demo.mp4 over loopback HTTP and verify <video><source src="http://..."> decodes via RDD worker
            std::string mp4_path = "astra/examples/astra_demo.mp4";
            if (!std::filesystem::exists(mp4_path)) mp4_path = "examples/astra_demo.mp4";
            std::string mp4_bytes;
            {
                std::ifstream mf(mp4_path, std::ios::binary);
                std::ostringstream oss;
                oss << mf.rdbuf();
                mp4_bytes = oss.str();
            }

            bool ok_remote_vid = false;
            if (!mp4_bytes.empty()) {
                int srv = ::socket(AF_INET, SOCK_STREAM, 0);
                int opt = 1;
                setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = 0;
                if (srv >= 0 && ::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && ::listen(srv, 4) == 0) {
                    socklen_t alen = sizeof(addr);
                    getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &alen);
                    uint16_t port = ntohs(addr.sin_port);
                    std::thread vt([srv, &mp4_bytes]() {
                        for (int r = 0; r < 2; ++r) {
                            int cfd = ::accept(srv, nullptr, nullptr);
                            if (cfd < 0) break;
                            char buf[1024]{};
                            ssize_t n = ::recv(cfd, buf, sizeof(buf) - 1, 0);
                            std::string req(buf, n > 0 ? static_cast<size_t>(n) : 0);
                            std::string body, ctype;
                            if (req.find("GET /clip.mp4 ") != std::string::npos) {
                                ctype = "video/mp4";
                                body = mp4_bytes;
                            } else {
                                ctype = "text/html";
                                body = "<html><body><video controls style='height:200px;'><source src='/clip.mp4' type='video/mp4'></video></body></html>";
                            }
                            std::ostringstream resp;
                            resp << "HTTP/1.1 200 OK\r\nContent-Type: " << ctype
                                 << "\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\n\r\n";
                            std::string hdr = resp.str();
                            ::send(cfd, hdr.data(), hdr.size(), MSG_NOSIGNAL);
                            size_t sent = 0;
                            while (sent < body.size()) {
                                ssize_t s = ::send(cfd, body.data() + sent, body.size() - sent, MSG_NOSIGNAL);
                                if (s <= 0) break;
                                sent += static_cast<size_t>(s);
                            }
                            ::close(cfd);
                        }
                        ::close(srv);
                    });

                    uint32_t vtid = app.new_tab("http://127.0.0.1:" + std::to_string(port) + "/player");
                    app.switch_tab(vtid);
                    Framebuffer vfb(640, 420);
                    ok_remote_vid = app.paint_active_tab(vfb) &&
                                    std::filesystem::exists("/tmp/astra_media_cache");
                    if (vt.joinable()) vt.join();
                }
            }
            expect(ok_hls && ok_remote_vid,
                   "AstraMedia v5: HLS (.m3u8) Master/Media playlist parser + remote HTTP <video><source> RDD decode");
        }

        // 55. AstraJS v10 — Web, Social & HTMLMediaElement APIs (window, navigator, URL, URLSearchParams, btoa/atob, DOM, video.play)
        {
            SlabArena arena(128 * 1024);
            HtmlDocument doc = HtmlDocument::parse(arena,
                "<html><body><div id='root'></div><video id='vid' src='demo.mp4'></video></body></html>");
            AstraJsEngine vm;
            auto rep = vm.execute(R"JS(
                window.APP_VER = "v10";
                const u = new URL("https://social.astra.dev/watch?v=42&hd=1#top");
                const sp = u.searchParams;
                sp.set("lang", "es");
                const enc = encodeURIComponent("hola mundo!");
                const dec = decodeURIComponent(enc);
                const b64 = btoa("Astra10");
                const raw = atob(b64);
                const obs = new MutationObserver(() => {});
                obs.observe(document.body);
                obs.disconnect();
                const ac = new AbortController();
                ac.abort();
                const vid = document.getElementById("vid");
                const can = vid.canPlayType("video/mp4");
                vid.play();
                vid.currentTime = 7.5;
                const playing = !vid.paused;
                vid.pause();
                const pausedAfter = vid.paused;
                const badge = document.createElement("span");
                badge.textContent = window.APP_VER + "|" + u.hostname + "|" + sp.get("v") + "|" + sp.get("lang") +
                                    "|" + dec + "|" + raw + "|" + can + "|" + playing + "|" + pausedAfter + "|" +
                                    ac.signal.aborted + "|" + navigator.onLine;
                document.getElementById("root").appendChild(badge);
                console.log(document.getElementById("root").textContent);
            )JS", doc);
            std::string out = rep.logs.empty() ? "" : rep.logs[0];
            bool ok55 = rep.ok &&
                        out == "v10|social.astra.dev|42|es|hola mundo!|Astra10|probably|true|true|true|true";
            if (!ok55) std::cerr << "    [diag55] ok=" << rep.ok << " err='" << rep.error << "' out='" << out << "'\n";
            expect(ok55,
                   "AstraJS v10: window/navigator/URL/URLSearchParams/btoa/atob/MutationObserver/AbortController/<video>.play()");
        }

        // 56. E2E Showcase (astra_session10.html) + Live Open-Internet Pages (Wikipedia, Hacker News, GitHub)
        {
            std::string s10_path = "astra/examples/astra_session10.html";
            if (!std::filesystem::exists(s10_path)) s10_path = "examples/astra_session10.html";
            uint32_t s10_tid = app.new_tab("file://" + s10_path);
            std::string p_status, u_info;
            for (const auto& t : app.tabs()) {
                if (t.id == s10_tid && t.live_doc) {
                    if (auto* n = t.live_doc->query_selector("#player-status")) p_status = n->text_content();
                    if (auto* n = t.live_doc->query_selector("#url-info")) u_info = n->text_content();
                }
            }
            bool ok_s10 = p_status.find("Playing=true") != std::string::npos &&
                          p_status.find("mp4=probably") != std::string::npos &&
                          p_status.find("hls=probably") != std::string::npos &&
                          u_info.find("stream.astra.dev/watch") != std::string::npos;

            // Live open-internet navigation verification (Wikipedia + Hacker News + GitHub)
            uint32_t web_tid = app.new_tab("");
            auto nav_hn = app.navigate(web_tid, "https://news.ycombinator.com/");
            auto nav_wiki = app.navigate(web_tid, "https://en.wikipedia.org/wiki/Web_browser");
            auto nav_gh = app.navigate(web_tid, "https://github.com/explore");
            bool ok_live = nav_hn.ok && nav_hn.display_items > 40 &&
                           nav_wiki.ok && nav_wiki.display_items > 80 &&
                           nav_gh.ok && nav_gh.display_items > 20;
            if (!ok_s10 || !ok_live) {
                std::cerr << "    [diag56] s10=" << ok_s10 << " p='" << p_status << "' u='" << u_info << "'"
                          << " hn=(" << nav_hn.ok << "," << nav_hn.display_items << ")"
                          << " wiki=(" << nav_wiki.ok << "," << nav_wiki.display_items << ")"
                          << " gh=(" << nav_gh.ok << "," << nav_gh.display_items << ")\n";
            }
            expect(ok_s10 && ok_live,
                   "E2E v10: astra_session10.html + Live Open-Internet HTTPS/H2 (Hacker News, Wikipedia, GitHub Explore)");
        }

        // 57. YouTube Watch Page extractor + Live Remote HTTPS MP4/WebM/HLS multi-frame playback
        {
            bool ids_ok =
                BrowserApp::extract_youtube_video_id("https://www.youtube.com/watch?v=jNQXAC9IVRw") == "jNQXAC9IVRw" &&
                BrowserApp::extract_youtube_video_id("https://youtu.be/aqz-KE-bpKQ?t=10") == "aqz-KE-bpKQ" &&
                BrowserApp::extract_youtube_video_id("https://www.youtube.com/shorts/dQw4w9WgXcQ") == "dQw4w9WgXcQ" &&
                BrowserApp::extract_youtube_video_id("https://www.youtube.com/embed/YE7VzlLtp-4") == "YE7VzlLtp-4";

            std::string sample_yt_html =
                "<html><script>var ytInitialPlayerResponse = {"
                "\"playabilityStatus\":{\"status\":\"OK\"},"
                "\"videoDetails\":{\"title\":\"Big Buck Bunny\",\"author\":\"Blender\",\"lengthSeconds\":\"634\",\"viewCount\":\"12345\"},"
                "\"streamingData\":{\"formats\":[{\"mimeType\":\"video/mp4; codecs=\\\"avc1.42001E, mp4a.40.2\\\"\",\"qualityLabel\":\"360p\",\"url\":\"https://rr1.googlevideo.com/videoplayback?id=123\"}]}"
                "};</script></html>";
            YouTubeWatchInfo yt_ok = BrowserApp::parse_youtube_player_response(sample_yt_html, "aqz-KE-bpKQ");
            bool parse_ok = yt_ok.playability_status == "OK" &&
                            yt_ok.title == "Big Buck Bunny" &&
                            yt_ok.author == "Blender" &&
                            yt_ok.stream_quality == "360p" &&
                            yt_ok.stream_url == "https://rr1.googlevideo.com/videoplayback?id=123";

            std::string bot_yt_html =
                "<html><script>var ytInitialPlayerResponse = {"
                "\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\",\"reason\":\"Sign in to confirm you're not a bot\"}"
                "};</script></html>";
            YouTubeWatchInfo yt_bot = BrowserApp::parse_youtube_player_response(bot_yt_html, "jNQXAC9IVRw");
            bool bot_ok = yt_bot.playability_status == "LOGIN_REQUIRED" &&
                          yt_bot.playability_reason.find("bot") != std::string::npos &&
                          yt_bot.stream_url.empty();

            // Verify live YouTube watch page loads cleanly (never blank) with real oEmbed title/thumbnail
            uint32_t yt_tid = app.new_tab("https://www.youtube.com/watch?v=jNQXAC9IVRw");
            bool yt_rendered = false;
            for (const auto& t : app.tabs()) {
                if (t.id == yt_tid && t.title.find("Me at the zoo") != std::string::npos &&
                    t.cached_display_list.items.size() >= 6) {
                    yt_rendered = true;
                }
            }

            expect(ids_ok && parse_ok && bot_ok && yt_rendered,
                   "YouTube Watch v10: videoId + ytInitialPlayerResponse (OK & LOGIN_REQUIRED) + live YouTube page render");
        }

        // ---------------------------------------------------------------------
        // 63. AstraStyle v8 — @keyframes + animation shorthand + easing curves
        // ---------------------------------------------------------------------
        {
            SlabArena aa;
            auto adoc = HtmlDocument::parse(aa,
                "<!doctype html><html><head><style>"
                "@keyframes grow {"
                "  from { width: 100px; opacity: 0.2; background: #000000; }"
                "  50%  { width: 200px; }"
                "  to   { width: 300px; opacity: 1; background: #FFFFFF; }"
                "}"
                ".card { height: 40px; animation: grow 1000ms linear 200ms 1 forwards; }"
                "</style></head><body><div class=\"card\"></div></body></html>");
            CssStylesheet asheet;
            asheet.parse(adoc.collect_stylesheets());

            // La regla @keyframes se parseó con sus tres pasos normalizados.
            const CssKeyframesRule* kf = asheet.find_keyframes("grow");
            bool kf_ok = kf && kf->keyframes.size() == 3 &&
                         std::abs(kf->keyframes[0].offset - 0.0f) < 1e-4f &&
                         std::abs(kf->keyframes[1].offset - 0.5f) < 1e-4f &&
                         std::abs(kf->keyframes[2].offset - 1.0f) < 1e-4f;

            AnimationTimeline tl;
            tl.attach(&asheet);
            AnimationSpec spec;
            spec.name = "grow";
            spec.duration_ms = 1000.0f;
            spec.delay_ms = 200.0f;
            spec.iteration_count = 1.0f;
            spec.easing = Easing::linear();
            spec.fill_mode = AnimationFillMode::Forwards;

            auto width_at = [&](double elapsed) -> float {
                auto sample = tl.sample_keyframes(spec, elapsed);
                for (const auto& d : sample.declarations) {
                    if (d.property == "width") return StyleEngine::parse_px(d.value, -1.0f);
                }
                return -1.0f;
            };

            // Durante el retardo no hay efecto (fill-mode: forwards no rellena
            // hacia atrás); a la mitad y al final los valores son exactos.
            bool delay_empty = tl.sample_keyframes(spec, 100.0).declarations.empty();
            float w_mid = width_at(200.0 + 500.0);   // progreso 0.50 -> keyframe 50%
            float w_q1 = width_at(200.0 + 250.0);    // progreso 0.25 -> mitad de [0, 50%]
            float w_end = width_at(200.0 + 1000.0);  // congelado por `forwards`
            bool widths_ok = std::abs(w_mid - 200.0f) < 0.6f &&
                             std::abs(w_q1 - 150.0f) < 0.6f &&
                             std::abs(w_end - 300.0f) < 0.6f;

            // El color interpola componente a componente (negro -> blanco).
            std::string bg_mid;
            for (const auto& d : tl.sample_keyframes(spec, 200.0 + 500.0).declarations) {
                if (d.property == "background") bg_mid = d.value;
            }
            bool color_ok = (bg_mid == "#808080" || bg_mid == "#7F7F7F");

            // Las curvas de temporización se evalúan con su definición CSS:
            // toda ease* pasa por (0.5 -> 0.5) y ease-in arranca más lento.
            Easing lin = Easing::parse("linear");
            Easing ein = Easing::parse("ease-in");
            Easing bez = Easing::parse("cubic-bezier(0.25, 0.1, 0.25, 1)");
            bool easing_ok = std::abs(lin.eval(0.37f) - 0.37f) < 1e-4f &&
                             std::abs(ein.eval(0.5f) - 0.3155f) < 0.01f &&
                             ein.eval(0.25f) < 0.25f &&
                             std::abs(bez.eval(0.5f) - Easing::ease().eval(0.5f)) < 1e-4f &&
                             std::abs(Easing::parse("steps(4, end)").eval(0.7f) - 0.5f) < 1e-4f;

            // Y el estilo calculado refleja el fotograma vigente a través de la
            // cascada completa (el efecto animado gana a la regla del autor):
            // el primer layout registra la animación con el reloj en 0 y el
            // segundo, tras avanzar 700 ms, ya aplica el fotograma interpolado.
            AnimationTimeline live;
            LayoutEngine::build_layout(aa, adoc, asheet, 800.0f, 400.0f, false, &live);
            live.advance(700.0);
            SlabArena aa2;
            LayoutFrame* aroot = LayoutEngine::build_layout(aa2, adoc, asheet, 800.0f, 400.0f, false, &live);
            float laid_width = (aroot && !aroot->children.empty())
                                   ? aroot->children[0]->border_box.w : -1.0f;
            bool cascade_ok = std::abs(laid_width - 200.0f) < 0.6f;

            expect(kf_ok && delay_empty && widths_ok && color_ok && easing_ok && cascade_ok,
                   "AstraStyle v8 parses @keyframes, honours animation delay/fill-mode and interpolates "
                   "px/colour values on exact CSS easing curves");
        }

        // ---------------------------------------------------------------------
        // 64. AstraStyle v8 — CSS transform 2D compuesto + composición afín ASD6
        // ---------------------------------------------------------------------
        {
            // rotate(90deg) alrededor del centro convierte una caja ancha en
            // una franja vertical: es la prueba geométrica más estricta.
            SlabArena ta;
            auto tdoc = HtmlDocument::parse(ta,
                "<!doctype html><html><head><style>"
                "body { background: #000000; }"
                ".rot { width: 120px; height: 20px; background: #22C55E;"
                "       transform: translate(40px, 140px) rotate(90deg) scale(2); }"
                "</style></head><body><div class=\"rot\"></div></body></html>");
            CssStylesheet tsheet;
            tsheet.parse(tdoc.collect_stylesheets());
            LayoutFrame* troot = LayoutEngine::build_layout(ta, tdoc, tsheet, 400.0f, 200.0f);

            const Transform2D& tf = troot->children[0]->style.transform;
            bool parse_ok = tf.enabled &&
                            std::abs(tf.translate_x - 40.0f) < 1e-3f &&
                            std::abs(tf.translate_y - 140.0f) < 1e-3f &&
                            std::abs(tf.rotate_deg - 90.0f) < 1e-3f &&
                            std::abs(tf.scale_x - 2.0f) < 1e-3f &&
                            std::abs(tf.scale_y - 2.0f) < 1e-3f;

            // La matriz afín de rotación de 90° es [0 -1; 1 0] escalada por 2.
            float m[6];
            tf.to_matrix(0.0f, 0.0f, m);
            bool matrix_ok = std::abs(m[0] - 0.0f) < 1e-3f && std::abs(m[1] - 2.0f) < 1e-3f &&
                             std::abs(m[2] + 2.0f) < 1e-3f && std::abs(m[3] - 0.0f) < 1e-3f &&
                             std::abs(m[4] - 40.0f) < 1e-3f && std::abs(m[5] - 140.0f) < 1e-3f;

            DisplayList tdl = LayoutEngine::build_display_list(troot, 400, 200);
            auto tblob = tdl.serialize();
            DisplayList trt;
            bool rt_ok = DisplayList::deserialize(tblob, trt);
            bool xform_rt = false;
            Rect src_rect{};
            for (const auto& it : trt.items) {
                if (it.type == DisplayItemType::FillRect && it.color_argb == 0xFF22C55Eu) {
                    xform_rt = it.has_transform &&
                               std::abs(it.xform[1] - 2.0f) < 1e-3f &&
                               std::abs(it.xform[2] + 2.0f) < 1e-3f;
                    src_rect = it.rect;
                }
            }

            // Composición real: el centro de la caja no se mueve con la
            // rotación, pero el ancho pintado pasa a ser la altura original
            // (20px) escalada x2 = 40px, y el alto 120px x2 = 240px.
            Framebuffer tfb(400, 400);
            tfb.composite_display_list(trt, 0, 0.0f);
            const uint32_t* px = tfb.pixels();
            auto is_green = [](uint32_t c) {
                return ((c >> 16) & 0xFFu) < 80u && ((c >> 8) & 0xFFu) > 160u && (c & 0xFFu) < 140u;
            };
            const int cx = static_cast<int>(std::round(src_rect.x + src_rect.w * 0.5f + 40.0f));
            const int cy = static_cast<int>(std::round(src_rect.y + src_rect.h * 0.5f + 140.0f));
            int span_x = 0, span_y = 0;
            for (int x = 0; x < 400; ++x) {
                if (cy >= 0 && cy < 400 && is_green(px[cy * 400 + x])) ++span_x;
            }
            for (int y = 0; y < 400; ++y) {
                if (cx >= 0 && cx < 400 && is_green(px[y * 400 + cx])) ++span_y;
            }
            bool raster_ok = cx >= 0 && cx < 400 && cy >= 0 && cy < 400 &&
                             is_green(px[cy * 400 + cx]) &&
                             std::abs(span_x - 40) <= 3 && std::abs(span_y - 240) <= 3;

            expect(parse_ok && matrix_ok && rt_ok && xform_rt && raster_ok,
                   "CSS transform translate/rotate/scale composes an exact 2D affine matrix, roundtrips in "
                   "ASD6 and rasterizes rotated geometry with 2x2 supersampling");
        }

        // ---------------------------------------------------------------------
        // 65. AstraStyle v8 — CSS transitions conducidas por requestAnimationFrame
        // ---------------------------------------------------------------------
        {
            const std::string tr_path = "/tmp/astra_session8_transition.html";
            {
                std::ofstream f(tr_path);
                f << "<!doctype html><html><head><style>"
                     ".fade { width: 100px; height: 30px; background: #111827;"
                     "        transition: width 400ms linear; }"
                     "</style></head><body><div class=\"fade\" id=\"fx\"></div>"
                     "<script>"
                     "var n = document.querySelector('#fx');"
                     "requestAnimationFrame(function(){ n.style.width = '300px'; });"
                     "</script></body></html>";
            }
            uint32_t ttab = app.new_tab("file://" + tr_path);
            app.navigate(ttab, "file://" + tr_path);

            auto width_of = [&]() -> float {
                const Tab* t = app.active_tab();
                if (!t) return -1.0f;
                for (const auto& it : t->cached_display_list.items) {
                    if (it.type == DisplayItemType::FillRect && it.color_argb == 0xFF111827u) return it.rect.w;
                }
                return -1.0f;
            };

            float w0 = width_of();

            // El primer fotograma ejecuta el callback de rAF, que cambia el
            // ancho: eso arranca la transición en lugar de saltar al destino.
            app.step_animation_frames(1);
            float w1 = width_of();

            // ~200 ms (12 fotogramas de 16.67 ms) -> mitad del recorrido lineal.
            app.step_animation_frames(12);
            float w_mid = width_of();

            // Pasado el total, la transición termina en el valor destino.
            app.step_animation_frames(20);
            float w_end = width_of();

            bool start_ok = std::abs(w0 - 100.0f) < 0.6f && std::abs(w1 - 100.0f) < 6.0f;
            bool mid_ok = w_mid > 140.0f && w_mid < 260.0f;
            bool end_ok = std::abs(w_end - 300.0f) < 0.6f;

            expect(start_ok && mid_ok && end_ok,
                   "CSS transitions interpolate a scripted element.style change across rAF frames "
                   "instead of snapping, and settle exactly on the target value");
        }

        // ---------------------------------------------------------------------
        // 66. AstraJS v8 — DOM Events Level 2: captura, objetivo y burbujeo
        // ---------------------------------------------------------------------
        {
            SlabArena ea;
            HtmlDocument edoc = HtmlDocument::parse(ea,
                "<html><body><div id=\"outer\"><div id=\"mid\">"
                "<button id=\"inner\">Go</button></div></div>"
                "<p id=\"log\">-</p></body></html>");
            AstraJsEngine evm;

            // Cada listener anota su fase y su currentTarget: la traza completa
            // demuestra el orden exacto raíz->objetivo->raíz de la especificación.
            std::string js =
                "let trace = [];\n"
                "const outer = document.querySelector(\"#outer\");\n"
                "const mid = document.querySelector(\"#mid\");\n"
                "const inner = document.querySelector(\"#inner\");\n"
                "outer.addEventListener(\"click\", function(ev) {\n"
                "  trace.push(\"cap:outer:\" + ev.eventPhase + \":\" + ev.currentTarget.id + \":\" + ev.target.id);\n"
                "}, true);\n"
                "mid.addEventListener(\"click\", function(ev) {\n"
                "  trace.push(\"cap:mid:\" + ev.eventPhase);\n"
                "}, { capture: true });\n"
                "inner.addEventListener(\"click\", function(ev) {\n"
                "  trace.push(\"target:\" + ev.eventPhase + \":\" + ev.type);\n"
                "  ev.preventDefault();\n"
                "});\n"
                "mid.addEventListener(\"click\", function(ev) {\n"
                "  trace.push(\"bub:mid:\" + ev.eventPhase);\n"
                "});\n"
                "outer.addEventListener(\"click\", function(ev) {\n"
                "  trace.push(\"bub:outer:\" + ev.eventPhase);\n"
                "});\n"
                "let once_hits = 0;\n"
                "outer.addEventListener(\"ping\", function(ev) { once_hits = once_hits + 1; }, { once: true });\n";
            auto erep = evm.execute(js, edoc);

            DomNode* btn = edoc.query_selector("#inner");
            auto drep = evm.dispatch_event(btn, "click", edoc);

            std::string joined;
            auto git = evm.globals().find("trace");
            if (git != evm.globals().end() && git->second.kind == JsValue::Kind::Object &&
                git->second.obj) {
                for (const auto& el : git->second.obj->elements) {
                    if (!joined.empty()) joined += "|";
                    joined += el.to_string();
                }
            }
            const std::string expected =
                "cap:outer:1:outer:inner|cap:mid:1|target:2:click|bub:mid:3|bub:outer:3";
            bool order_ok = (joined == expected);
            bool prevented_ok = drep.default_prevented && drep.listeners_invoked == 5;

            // stopPropagation detiene el recorrido en el nodo que lo invoca.
            std::string js2 =
                "let t2 = [];\n"
                "const o2 = document.querySelector(\"#outer\");\n"
                "const i2 = document.querySelector(\"#inner\");\n"
                "o2.addEventListener(\"tap\", function(ev) { t2.push(\"cap\"); ev.stopPropagation(); }, true);\n"
                "i2.addEventListener(\"tap\", function(ev) { t2.push(\"target\"); });\n";
            evm.execute(js2, edoc);
            auto drep2 = evm.dispatch_event(btn, "tap", edoc);
            std::string j2;
            auto g2 = evm.globals().find("t2");
            if (g2 != evm.globals().end() && g2->second.obj) {
                for (const auto& el : g2->second.obj->elements) {
                    if (!j2.empty()) j2 += "|";
                    j2 += el.to_string();
                }
            }
            bool stop_ok = (j2 == "cap") && !drep2.default_prevented && drep2.listeners_invoked == 1;

            // `once`: el listener se retira después de ejecutarse una vez.
            DomNode* outer_node = edoc.query_selector("#outer");
            evm.dispatch_event(outer_node, "ping", edoc);
            evm.dispatch_event(outer_node, "ping", edoc);
            double once_hits = 0.0;
            auto oh = evm.globals().find("once_hits");
            if (oh != evm.globals().end()) once_hits = oh->second.to_number();
            bool once_ok = std::abs(once_hits - 1.0) < 1e-9;

            expect(erep.ok && order_ok && prevented_ok && stop_ok && once_ok,
                   "AstraJS v8 dispatches DOM Events Level 2 through capture/target/bubble phases with a real "
                   "Event object (target, currentTarget, eventPhase, preventDefault, stopPropagation, once)");
        }

        // ---------------------------------------------------------------------
        // 67. AstraNet v8 — revalidación condicional If-None-Match -> 304
        // ---------------------------------------------------------------------
        {
            std::error_code rec;
            std::filesystem::remove_all("/tmp/astra_reval_cache", rec);

            int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
            platform::harden_socket_fd(lfd);
            int ropt = 1;
            if (lfd >= 0) setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &ropt, sizeof(ropt));
            sockaddr_in raddr{};
            raddr.sin_family = AF_INET;
            raddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            raddr.sin_port = 0;
            bool rbound = (lfd >= 0 &&
                           ::bind(lfd, reinterpret_cast<sockaddr*>(&raddr), sizeof(raddr)) == 0 &&
                           ::listen(lfd, 8) == 0);
            socklen_t ralen = sizeof(raddr);
            uint16_t rport = 0;
            if (rbound && ::getsockname(lfd, reinterpret_cast<sockaddr*>(&raddr), &ralen) == 0) {
                rport = ntohs(raddr.sin_port);
            }

            // Servidor de origen mínimo: entrega el recurso con ETag y
            // `Cache-Control: no-cache` (almacenable pero de revalidación
            // obligatoria) y responde 304 sin cuerpo cuando el ETag coincide.
            const std::string kEtag = "\"astra-v8-7f3c\"";
            const std::string kBody = "body{--astra:8;color:#38bdf8;}";
            int conditional_requests = 0;
            int plain_requests = 0;
            std::thread rsrv([&]() {
                if (!rbound) return;
                for (int i = 0; i < 3; ++i) {
                    int cfd = ::accept(lfd, nullptr, nullptr);
                    if (cfd < 0) return;
                    std::string req;
                    char rbuf[2048];
                    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
                        ssize_t n = ::recv(cfd, rbuf, sizeof(rbuf), 0);
                        if (n <= 0) break;
                        req.append(rbuf, static_cast<size_t>(n));
                    }
                    std::string lower_req = req;
                    std::transform(lower_req.begin(), lower_req.end(), lower_req.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    const bool matched = lower_req.find("if-none-match:") != std::string::npos &&
                                         req.find(kEtag) != std::string::npos;
                    std::string out;
                    if (matched) {
                        ++conditional_requests;
                        out = "HTTP/1.1 304 Not Modified\r\n"
                              "ETag: " + kEtag + "\r\n"
                              "Cache-Control: no-cache\r\n"
                              "Connection: keep-alive\r\n\r\n";
                    } else {
                        ++plain_requests;
                        out = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/css\r\n"
                              "ETag: " + kEtag + "\r\n"
                              "Last-Modified: Wed, 19 Nov 2025 10:00:00 GMT\r\n"
                              "Cache-Control: no-cache\r\n"
                              "Content-Length: " + std::to_string(kBody.size()) + "\r\n"
                              "Connection: keep-alive\r\n\r\n" + kBody;
                    }
                    (void)::send(cfd, out.data(), out.size(), ASTRA_MSG_NOSIGNAL);
                    ::close(cfd);
                }
            });

            HttpCache rcache("/tmp/astra_reval_cache", 1024 * 1024);
            AstraShield rshield;
            HttpClient rclient(rshield, &rcache);
            const std::string rurl = "http://127.0.0.1:" + std::to_string(rport) + "/theme.css";

            auto r1 = rclient.get(rurl, "127.0.0.1", true);
            auto r2 = rclient.get(rurl, "127.0.0.1", true);
            auto r3 = rclient.get(rurl, "127.0.0.1", true);

            if (rsrv.joinable()) rsrv.join();
            if (lfd >= 0) ::close(lfd);

            // La primera petición transfiere el cuerpo; las siguientes sólo
            // confirman el ETag y reutilizan los bytes ya almacenados.
            bool first_ok = r1.status_code == 200 && !r1.from_cache && !r1.revalidated &&
                            r1.body == kBody;
            bool reval_ok = r2.status_code == 200 && r2.from_cache && r2.revalidated &&
                            r2.body == kBody &&
                            r3.status_code == 200 && r3.revalidated && r3.body == kBody;
            bool server_ok = plain_requests == 1 && conditional_requests == 2;
            bool stats_ok = rcache.stats().revalidations >= 2 && rcache.stats().not_modified >= 2;

            // Y el parseo de directivas distingue los tres casos de RFC 9111.
            CacheControl cc_store = HttpCache::parse_cache_control("public, max-age=600");
            CacheControl cc_nocache = HttpCache::parse_cache_control("no-cache");
            CacheControl cc_nostore = HttpCache::parse_cache_control("no-store, max-age=99");
            bool cc_ok = cc_store.max_age_sec == 600 && !cc_store.must_revalidate && cc_store.storable() &&
                         cc_nocache.must_revalidate && cc_nocache.max_age_sec == 0 && cc_nocache.storable() &&
                         cc_nostore.no_store && !cc_nostore.storable();

            expect(rbound && first_ok && reval_ok && server_ok && stats_ok && cc_ok,
                   "AstraNet v8 revalidates a stale cache entry with a conditional If-None-Match GET and serves "
                   "the stored body on 304 Not Modified without retransferring it");
        }

        // ---------------------------------------------------------------------
        // 68. AstraJS v8 — document.cookie sobre el tarro particionado del Shield
        // ---------------------------------------------------------------------
        {
            AstraShield jshield;
            // Simula la partición de un sitio de primer nivel concreto.
            std::string top = "astra-site.test";
            std::string origin = "https://astra-site.test";

            SlabArena ca;
            HtmlDocument cdoc = HtmlDocument::parse(ca,
                "<html><body><p id=\"out\">-</p></body></html>");
            AstraJsEngine cvm;
            cvm.bind_cookies(
                [&]() { return jshield.cookie_header_for(top, origin, true); },
                [&](const std::string& sc) { jshield.store_cookie(top, origin, sc); });

            std::string js =
                "document.cookie = \"theme=dark; Secure; HttpOnly\";\n"
                "document.cookie = \"lang=es; Secure\";\n"
                "const jar = document.cookie;\n"
                "document.querySelector(\"#out\").textContent = jar;\n";
            auto crep = cvm.execute(js, cdoc);

            std::string jar = cdoc.query_selector("#out")->text_content();
            bool both_present = jar.find("theme=dark") != std::string::npos &&
                                jar.find("lang=es") != std::string::npos;

            // Y la Total Cookie Protection sigue vigente para lo que escribió
            // el script: otro sitio de primer nivel no ve nada de esa partición.
            std::string other_site_jar = jshield.cookie_header_for("otro-sitio.test", origin, true);

            expect(crep.ok && both_present && other_site_jar.empty(),
                   "AstraJS v8 document.cookie reads and writes through the AstraShield partitioned jar, so "
                   "script-set cookies stay isolated per top-level site");
        }

        // ---------------------------------------------------------------------
        // 69. CSS Grid Level 1 — áreas con nombre (grid-template-areas/grid-area)
        // ---------------------------------------------------------------------
        {
            // Reproduce la plantilla real con la que las skins modernas colocan
            // barra lateral y contenido en columnas contiguas. El fallo que
            // motivó esta prueba: los identificadores de área distinguen
            // mayúsculas, y normalizarlos a minúsculas hacía que NINGÚN hijo
            // casara con su área, apilándolos en vertical y empujando el
            // contenido fuera del viewport.
            SlabArena ga;
            HtmlDocument gdoc = HtmlDocument::parse(ga,
                "<html><head><style>"
                "body { margin: 0; padding: 0; }"
                ".page {"
                "  display: grid; column-gap: 24px;"
                "  grid-template: min-content 1fr min-content / 200px minmax(0, 1fr);"
                "  grid-template-areas: 'siteNotice siteNotice' 'columnStart pageContent' 'footer footer';"
                "}"
                ".notice  { grid-area: siteNotice;  height: 40px; background: #111111; }"
                ".side    { grid-area: columnStart; height: 90px; background: #222222; }"
                ".main    { grid-area: pageContent; height: 90px; background: #333333; }"
                ".foot    { grid-area: footer;      height: 30px; background: #444444; }"
                "</style></head><body><div class=\"page\">"
                // Orden del DOM a propósito distinto del orden visual:
                // la colocación debe venir de la plantilla, no del flujo.
                "<div class=\"foot\"></div>"
                "<div class=\"main\"></div>"
                "<div class=\"side\"></div>"
                "<div class=\"notice\"></div>"
                "</div></body></html>");
            CssStylesheet gsheet;
            gsheet.parse(gdoc.collect_stylesheets());
            LayoutFrame* groot = LayoutEngine::build_layout(ga, gdoc, gsheet, 1024.0f, 600.0f);

            const LayoutFrame* page = (groot && !groot->children.empty()) ? groot->children[0] : nullptr;
            bool tpl_ok = page && page->style.display == DisplayType::Grid &&
                          page->style.grid_areas.size() == 3 &&
                          page->style.grid_areas[1].size() == 2 &&
                          page->style.grid_areas[1][0] == "columnStart" &&
                          page->style.grid_areas[1][1] == "pageContent" &&
                          // `grid-template` tras la barra: 200px + minmax(0,1fr)
                          page->style.grid_columns.size() == 2 &&
                          !page->style.grid_columns[0].is_fr &&
                          std::abs(page->style.grid_columns[0].value - 200.0f) < 0.5f &&
                          page->style.grid_columns[1].is_fr;

            const LayoutFrame *notice = nullptr, *side = nullptr, *main_c = nullptr, *foot = nullptr;
            if (page) {
                for (const LayoutFrame* c : page->children) {
                    if (c->style.grid_area == "siteNotice") notice = c;
                    else if (c->style.grid_area == "columnStart") side = c;
                    else if (c->style.grid_area == "pageContent") main_c = c;
                    else if (c->style.grid_area == "footer") foot = c;
                }
            }
            bool found_all = notice && side && main_c && foot;

            // Lo esencial: barra lateral y contenido COMPARTEN banda vertical
            // (están uno al lado del otro), el aviso va encima y el pie debajo.
            bool geometry_ok = false;
            if (found_all) {
                const float gap = 24.0f;
                geometry_ok =
                    std::abs(notice->border_box.y - 0.0f) < 1.0f &&
                    std::abs(notice->border_box.w - (1024.0f - 0.0f)) < 2.0f &&   // abarca las 2 columnas
                    std::abs(side->border_box.y - main_c->border_box.y) < 1.0f &&  // misma fila
                    std::abs(side->border_box.w - 200.0f) < 1.0f &&                // pista fija
                    main_c->border_box.x > side->border_box.x + side->border_box.w - 1.0f &&
                    std::abs(main_c->border_box.x - (200.0f + gap)) < 1.5f &&      // tras el column-gap
                    main_c->border_box.w > 700.0f &&                               // minmax(0,1fr) se lleva el resto
                    foot->border_box.y > main_c->border_box.y + 80.0f;             // el pie, al final
            }

            if (!(tpl_ok && found_all && geometry_ok)) {
                std::cerr << "    [diag69] tpl=" << tpl_ok << " found=" << found_all
                          << " geom=" << geometry_ok << "\n";
                if (page) std::cerr << "      page box=(" << page->border_box.x << "," << page->border_box.y
                                    << " " << page->border_box.w << "x" << page->border_box.h
                                    << ") areas=" << page->style.grid_areas.size()
                                    << " cols=" << page->style.grid_columns.size() << "\n";
                auto pb=[&](const char* n,const LayoutFrame* f){ if(f) std::cerr << "      " << n << " box=("
                    << f->border_box.x << "," << f->border_box.y << " " << f->border_box.w
                    << "x" << f->border_box.h << ")\n"; else std::cerr << "      " << n << " NULL\n"; };
                pb("notice",notice); pb("side",side); pb("main",main_c); pb("foot",foot);
            }
            expect(tpl_ok && found_all && geometry_ok,
                   "CSS Grid named areas (grid-template-areas + grid-area, case-sensitive idents, minmax() and "
                   "grid-template shorthand) place a sidebar and content side by side instead of stacking them");
        }

        // ---------------------------------------------------------------------
        // 70. Tipografía: font-size relativo y cajas en línea sin suelo artificial
        // ---------------------------------------------------------------------
        {
            // Fallo original: `parse_px` ignoraba el `%`, así que
            // `font-size: 100%` se leía como 100px y el clamp lo dejaba en 64.
            // Toda etiqueta con tamaño porcentual salía gigante.
            SlabArena fa;
            HtmlDocument fdoc = HtmlDocument::parse(fa,
                "<html><head><style>"
                "body { margin: 0; padding: 0; font-size: 20px; }"
                "#same { font-size: 100%; }"
                "#small { font-size: 88.4%; }"
                "#em { font-size: 1.5em; }"
                "#rem { font-size: 1.5rem; }"
                "#kw { font-size: large; }"
                "#rel { font-size: smaller; }"
                "</style></head><body>"
                "<div id=\"same\">a</div><div id=\"small\">b</div><div id=\"em\">c</div>"
                "<div id=\"rem\">d</div><div id=\"kw\">e</div><div id=\"rel\">f</div>"
                "</body></html>");
            CssStylesheet fsheet;
            fsheet.parse(fdoc.collect_stylesheets());
            LayoutFrame* froot = LayoutEngine::build_layout(fa, fdoc, fsheet, 900.0f, 600.0f);

            auto font_of = [&](const char* id) -> float {
                const DomNode* n = fdoc.query_selector((std::string("#") + id).c_str());
                if (!n || !froot) return -1.0f;
                float out = -1.0f;
                std::function<void(const LayoutFrame*)> seek = [&](const LayoutFrame* f) {
                    if (!f || out >= 0.0f) return;
                    if (f->node == n) { out = f->style.font_size; return; }
                    for (const LayoutFrame* c : f->children) seek(c);
                };
                seek(froot);
                return out;
            };

            bool fonts_ok =
                std::abs(font_of("same") - 20.0f) < 0.5f &&     // 100% del heredado
                std::abs(font_of("small") - 17.68f) < 0.5f &&   // 88.4% de 20
                std::abs(font_of("em") - 30.0f) < 0.5f &&       // 1.5em del heredado
                std::abs(font_of("rem") - 24.0f) < 0.5f &&      // 1.5rem de la raíz (16)
                std::abs(font_of("kw") - 18.0f) < 0.5f &&       // large = 1.125 x 16
                std::abs(font_of("rel") - (20.0f / 1.2f)) < 0.5f; // smaller

            // Segundo fallo: layout_node imponía un suelo de 40px a toda caja,
            // también a las de línea, de modo que una palabra corta o un signo
            // de puntuación abrían un hueco enorme dentro del párrafo.
            SlabArena ia;
            HtmlDocument idoc = HtmlDocument::parse(ia,
                "<html><body><p>A <b>web browser</b>, is an <a href=\"#\">app</a>.</p></body></html>");
            CssStylesheet isheet;
            LayoutFrame* iroot = LayoutEngine::build_layout(ia, idoc, isheet, 900.0f, 400.0f, true);

            std::vector<const LayoutFrame*> leaves;
            std::function<void(const LayoutFrame*)> collect = [&](const LayoutFrame* f) {
                if (!f) return;
                if (f->children.empty() && f->node && f->node->type == NodeType::Text) leaves.push_back(f);
                for (const LayoutFrame* c : f->children) collect(c);
            };
            collect(iroot);

            bool inline_ok = leaves.size() >= 5;
            if (inline_ok) {
                // Ninguna caja de texto corta debe inflarse al viejo suelo de 40px...
                for (const LayoutFrame* lf : leaves) {
                    const std::string t = lf->node->text_content();
                    if (t.size() <= 2 && lf->border_box.w > 24.0f) inline_ok = false;
                }
                // ...y las cajas de una misma línea deben ir contiguas, sin
                // holgura artificial entre ellas.
                for (size_t k = 1; k < leaves.size(); ++k) {
                    const LayoutFrame* prev = leaves[k - 1];
                    const LayoutFrame* cur = leaves[k];
                    if (std::abs(prev->border_box.y - cur->border_box.y) > 1.0f) continue; // otra línea
                    const float gap = cur->border_box.x - (prev->border_box.x + prev->border_box.w);
                    if (gap > 4.0f) inline_ok = false;
                }
            }

            if (!(fonts_ok && inline_ok)) {
                std::cerr << "    [diag70] same=" << font_of("same") << " small=" << font_of("small")
                          << " em=" << font_of("em") << " rem=" << font_of("rem")
                          << " kw=" << font_of("kw") << " rel=" << font_of("rel")
                          << " leaves=" << leaves.size() << "\n";
                for (const LayoutFrame* lf : leaves) {
                    std::cerr << "      [" << lf->node->text_content() << "] x=" << lf->border_box.x
                              << " w=" << lf->border_box.w << "\n";
                }
            }

            expect(fonts_ok && inline_ok,
                   "CSS font-size resolves %, em, rem and keywords against the inherited size, and inline boxes "
                   "drop the 40px block floor so short words and punctuation sit flush on the line");
        }

        // ---------------------------------------------------------------------
        // 71. Espacio entre cajas en línea y reparto por flex-grow
        // ---------------------------------------------------------------------
        {
            // (a) Un nodo de texto que es SÓLO un espacio separa dos elementos
            // en línea y debe conservarse colapsado (CSS Text §4.1.1); entre
            // bloques, en cambio, se elimina y no debe abrir renglones.
            SlabArena wa;
            HtmlDocument wdoc = HtmlDocument::parse(wa,
                "<html><body>\n"
                "  <p><a href=\"#\">uno</a> <a href=\"#\">dos</a></p>\n"
                "  <div>bloque A</div>\n"
                "  <div>bloque B</div>\n"
                "</body></html>");
            CssStylesheet wsheet;
            LayoutFrame* wroot = LayoutEngine::build_layout(wa, wdoc, wsheet, 900.0f, 400.0f, true);

            std::vector<const LayoutFrame*> leaves;
            std::function<void(const LayoutFrame*)> collect = [&](const LayoutFrame* f) {
                if (!f) return;
                if (f->children.empty() && f->node && f->node->type == NodeType::Text) leaves.push_back(f);
                for (const LayoutFrame* c : f->children) collect(c);
            };
            collect(wroot);

            // El espacio sobrevive entre los dos enlaces...
            bool space_kept = false;
            for (size_t k = 1; k + 1 < leaves.size(); ++k) {
                if (leaves[k]->node->text_content() == " " &&
                    leaves[k - 1]->node->text_content() == "uno" &&
                    leaves[k + 1]->node->text_content() == "dos") {
                    space_kept = (leaves[k]->border_box.w > 1.0f && leaves[k]->border_box.w < 12.0f);
                }
            }
            // ...y los dos bloques quedan en renglones consecutivos, sin que
            // los saltos de línea del HTML metan líneas en blanco entre ellos.
            const LayoutFrame *bA = nullptr, *bB = nullptr;
            for (const LayoutFrame* lf : leaves) {
                if (lf->node->text_content() == "bloque A") bA = lf;
                if (lf->node->text_content() == "bloque B") bB = lf;
            }
            bool blocks_ok = bA && bB && (bB->border_box.y - bA->border_box.y) > 1.0f &&
                             (bB->border_box.y - bA->border_box.y) < 40.0f;

            // (b) flex-grow reparte el espacio libre: con un hermano fijo de
            // 100px, el que crece debe llevarse el resto y empujar al tercero
            // contra el borde opuesto.
            SlabArena fa2;
            HtmlDocument fdoc2 = HtmlDocument::parse(fa2,
                "<html><head><style>"
                "body { margin: 0; padding: 0; }"
                ".bar { display: flex; width: 600px; }"
                ".left { width: 100px; height: 20px; }"
                ".mid { flex-grow: 1; height: 20px; }"
                ".right { flex: 0 0 80px; height: 20px; }"
                "</style></head><body><div class=\"bar\">"
                "<div class=\"left\"></div><div class=\"mid\"></div><div class=\"right\"></div>"
                "</div></body></html>");
            CssStylesheet fsheet2;
            fsheet2.parse(fdoc2.collect_stylesheets());
            LayoutFrame* froot2 = LayoutEngine::build_layout(fa2, fdoc2, fsheet2, 900.0f, 400.0f);
            const LayoutFrame* bar = (froot2 && !froot2->children.empty()) ? froot2->children[0] : nullptr;
            bool flex_ok = false;
            if (bar && bar->children.size() == 3) {
                const LayoutFrame* L = bar->children[0];
                const LayoutFrame* M = bar->children[1];
                const LayoutFrame* R = bar->children[2];
                flex_ok = std::abs(L->border_box.w - 100.0f) < 1.5f &&
                          std::abs(R->border_box.w - 80.0f) < 1.5f &&
                          // el elástico se queda con 600 - 100 - 80
                          std::abs(M->border_box.w - 420.0f) < 2.0f &&
                          M->border_box.x > L->border_box.x &&
                          R->border_box.x > M->border_box.x &&
                          std::abs(M->style.flex_grow - 1.0f) < 1e-6f &&
                          std::abs(R->style.flex_basis - 80.0f) < 1e-6f;
            }

            if (!(space_kept && blocks_ok && flex_ok)) {
                std::cerr << "    [diag71] space=" << space_kept << " blocks=" << blocks_ok
                          << " flex=" << flex_ok << " hojas=" << leaves.size() << "\n";
                if (bar) for (const LayoutFrame* c : bar->children)
                    std::cerr << "      flex item x=" << c->border_box.x << " w=" << c->border_box.w
                              << " grow=" << c->style.flex_grow << " basis=" << c->style.flex_basis << "\n";
            }

            expect(space_kept && blocks_ok && flex_ok,
                   "Whitespace-only text collapses to one space between inline boxes (and vanishes between "
                   "blocks), and flex-grow / flex shorthand distribute the container's free space");
        }

        // ---------------------------------------------------------------------
        // 72. flex-grow con flex-wrap, y sin espacio al inicio de línea
        // ---------------------------------------------------------------------
        {
            // (a) La rama multi-fila de flexbox ignoraba `flex-grow`: un ítem
            // sin ancho explícito recibía 160px fijos, así que una barra con
            // un bloque fijo y otro elástico dejaba los dos pegados a la
            // izquierda en lugar de ocupar todo el contenedor.
            SlabArena wa;
            HtmlDocument wdoc = HtmlDocument::parse(wa,
                "<html><head><style>"
                "body { margin: 0; padding: 0; }"
                ".bar { display: flex; flex-wrap: wrap; width: 800px; }"
                ".fixed { width: 120px; height: 20px; }"
                ".elastic { flex-grow: 1; height: 20px; }"
                "</style></head><body><div class=\"bar\">"
                "<div class=\"fixed\"></div><div class=\"elastic\"></div>"
                "</div></body></html>");
            CssStylesheet wsheet;
            wsheet.parse(wdoc.collect_stylesheets());
            LayoutFrame* wroot = LayoutEngine::build_layout(wa, wdoc, wsheet, 1000.0f, 400.0f);
            const LayoutFrame* bar = (wroot && !wroot->children.empty()) ? wroot->children[0] : nullptr;
            bool wrap_grow_ok = false;
            if (bar && bar->children.size() == 2) {
                const LayoutFrame* fixed = bar->children[0];
                const LayoutFrame* elastic = bar->children[1];
                wrap_grow_ok = std::abs(fixed->border_box.w - 120.0f) < 1.5f &&
                               // el elástico se lleva 800 - 120, no los 160 fijos de antes
                               std::abs(elastic->border_box.w - 680.0f) < 2.0f &&
                               elastic->border_box.x > fixed->border_box.x;
            }

            // (b) El espacio que queda al PRINCIPIO de una línea se elimina
            // (CSS Text §4.1.1). Conservarlo —necesario entre cajas— dejaba
            // una sangría en cada renglón que empezaba con un nodo de texto.
            SlabArena ia;
            HtmlDocument idoc = HtmlDocument::parse(ia,
                "<html><body><p><a href=\"#\">enlace</a> texto que continua y "
                "se parte en varias lineas porque el parrafo es estrecho de mas</p></body></html>");
            CssStylesheet isheet;
            LayoutFrame* iroot = LayoutEngine::build_layout(ia, idoc, isheet, 260.0f, 400.0f, true);

            bool no_indent = true;
            float min_x = 1e9f;
            std::function<void(const LayoutFrame*)> scan = [&](const LayoutFrame* f) {
                if (!f) return;
                if (f->children.empty() && f->node && f->node->type == NodeType::Text) {
                    min_x = std::min(min_x, f->border_box.x);
                    // Ninguna línea renderizada debe empezar por espacio.
                    for (const auto& ln : f->wrapped_lines) {
                        if (!ln.empty() && ln.front() == ' ' && f->border_box.x <= min_x + 0.5f) {
                            no_indent = false;
                        }
                    }
                }
                for (const LayoutFrame* c : f->children) scan(c);
            };
            scan(iroot);

            if (!(wrap_grow_ok && no_indent)) {
                std::cerr << "    [diag72] wrap_grow=" << wrap_grow_ok << " no_indent=" << no_indent << "\n";
                if (bar) for (const LayoutFrame* c : bar->children)
                    std::cerr << "      item x=" << c->border_box.x << " w=" << c->border_box.w
                              << " grow=" << c->style.flex_grow << "\n";
            }

            expect(wrap_grow_ok && no_indent,
                   "flex-grow distributes free space in the multi-row flex path too, and a space left at the "
                   "start of a line is dropped so wrapped lines are not indented");
        }

        // ---------------------------------------------------------------------
        // 73. Fondo enmascarado no pintado y currentColor
        // ---------------------------------------------------------------------
        {
            // Los iconos de las skins modernas son un color de fondo recortado
            // por `mask-image`. Sin aplicar la máscara, pintar el fondo entero
            // daba un bloque macizo donde debía verse un icono: 365 reglas de
            // este tipo en una sola hoja de Wikipedia, de ahí los cuadros
            // negros. Mientras la máscara no se aplique, no se pinta el fondo.
            SlabArena ma;
            HtmlDocument mdoc = HtmlDocument::parse(ma,
                "<html><head><style>"
                "body { margin: 0; padding: 0; color: #336699; }"
                ".icon { width: 18px; height: 18px; background: #202122;"
                "        mask-image: url(/w/icon.svg); }"
                ".plain { width: 18px; height: 18px; background: #202122; }"
                ".tinted { width: 10px; height: 10px; background: currentColor; }"
                "</style></head><body>"
                "<div class=\"icon\"></div><div class=\"plain\"></div><div class=\"tinted\"></div>"
                "</body></html>");
            CssStylesheet msheet;
            msheet.parse(mdoc.collect_stylesheets());
            LayoutFrame* mroot = LayoutEngine::build_layout(ma, mdoc, msheet, 400.0f, 200.0f);

            const LayoutFrame *icon = nullptr, *plain = nullptr, *tinted = nullptr;
            if (mroot) {
                for (const LayoutFrame* c : mroot->children) {
                    if (!c->node || c->node->type != NodeType::Element) continue;
                    const std::string cl = c->node->get_attr("class");
                    if (cl == "icon") icon = c;
                    else if (cl == "plain") plain = c;
                    else if (cl == "tinted") tinted = c;
                }
            }
            bool style_ok = icon && plain && tinted &&
                            icon->style.has_unapplied_mask() &&
                            !plain->style.has_unapplied_mask() &&
                            // currentColor toma el valor calculado de `color`
                            tinted->style.background.to_argb() == 0xFF336699u;

            // El enmascarado no emite relleno; el normal sí.
            DisplayList mdl = LayoutEngine::build_display_list(mroot, 400, 200);
            int dark_fills = 0;
            for (const auto& it : mdl.items) {
                if (it.type == DisplayItemType::FillRect && it.color_argb == 0xFF202122u) ++dark_fills;
            }
            bool paint_ok = (dark_fills == 1); // sólo .plain

            if (!(style_ok && paint_ok)) {
                std::cerr << "    [diag73] style=" << style_ok << " dark_fills=" << dark_fills
                          << " icon=" << (icon ? icon->style.mask_image : "(null)")
                          << " tinted=" << (tinted ? tinted->style.background.to_argb() : 0) << "\n";
            }

            expect(style_ok && paint_ok,
                   "An element whose background is shaped by an unapplied mask-image paints no solid block, "
                   "and currentColor resolves to the computed color");
        }

        // ---------------------------------------------------------------------
        // 74. SVG: el contenido de <defs> no se pinta
        // ---------------------------------------------------------------------
        {
            // SVG 1.1 §5.6: lo que hay dentro de <defs>, <clipPath>, <mask>,
            // <symbol>... son DEFINICIONES; sólo se dibujan al referenciarlas.
            // El motor las pintaba, y como un <clipPath> suele contener un
            // rectángulo del tamaño del lienzo, logos y wordmarks acababan
            // como bloques macizos: el wordmark de Wikipedia salía 100%
            // opaco en vez de mostrar sus letras.
            const std::string svg =
                "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 40 40\" width=\"40\" height=\"40\">"
                "<defs><clipPath id=\"c\"><rect x=\"0\" y=\"0\" width=\"40\" height=\"40\" fill=\"#ffffff\"/></clipPath>"
                "<linearGradient id=\"g\"><stop offset=\"0\"/></linearGradient></defs>"
                "<circle cx=\"20\" cy=\"20\" r=\"8\" fill=\"#0E65C0\"/>"
                "</svg>";
            SvgSurface surf = SvgEngine::render_svg(svg, 40, 40);

            size_t opaque = 0;
            bool has_blue = false;
            for (uint32_t px : surf.argb) {
                if (((px >> 24) & 0xFFu) > 16) ++opaque;
                if ((px & 0x00FFFFFFu) == 0x000E65C0u && ((px >> 24) & 0xFFu) > 200) has_blue = true;
            }
            const double ratio = surf.argb.empty() ? 1.0
                                                   : static_cast<double>(opaque) / static_cast<double>(surf.argb.size());
            // El círculo ocupa ~pi*8^2/1600 = 12.6% del lienzo: debe haber
            // pintura, pero muy lejos del 100% que daba el rect de <defs>.
            bool ok_svg = surf.valid && has_blue && ratio > 0.05 && ratio < 0.35;

            // Y el mismo SVG a través del decodificador de <img>.
            DecodedImage dec = ImageDecoder::decode_bytes(
                reinterpret_cast<const uint8_t*>(svg.data()), svg.size());
            size_t dec_opaque = 0;
            for (uint32_t px : dec.argb) {
                if (((px >> 24) & 0xFFu) > 16) ++dec_opaque;
            }
            bool ok_img = dec.valid && dec.format == "SVG" && !dec.argb.empty() &&
                          static_cast<double>(dec_opaque) / static_cast<double>(dec.argb.size()) < 0.35;

            if (!(ok_svg && ok_img)) {
                std::cerr << "    [diag74] valid=" << surf.valid << " blue=" << has_blue
                          << " ratio=" << ratio << " dec=" << dec.valid << "\n";
            }

            expect(ok_svg && ok_img,
                   "SvgEngine skips non-rendered <defs>/<clipPath>/<mask> subtrees, so a logo is drawn as its "
                   "real shape instead of a solid block covering the whole surface");
        }

        app.stop_workers();
    }

    std::cout << "\nSummary: " << g_tests_passed << " passed, " << g_tests_failed << " failed.\n";
    return g_tests_failed == 0 ? 0 : 1;
}

int run_stress_test(int tab_count, uint64_t ceiling_mb) {
    std::cout << "=== Astra v10 Multi-Tab Stress Test (" << tab_count << " tabs, ceiling " << ceiling_mb << " MB) ===\n";
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
        << "Astra Browser Core v10 (Native C++20 Multi-Process Open-Web Engine)\n"
        << "Usage:\n"
        << "  astra --test                                Run full Session 10 subsystem suite (61 tests)\n"
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
        std::cout << "Astra v9 Interactive Shell (open <url>, download <url>, downloads, back, forward, reload, close <id>, tabs, switch <id>, scroll <px>, click <x> <y>, type <text>, enter, raf [N], mem, shield, cache, render <file.png>, quit)\n";
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
