# Astra Browser — Native Multi-Process Architecture

## Architectural Comparison: Mozilla Firefox → Astra (100% Original C++20)

After inspecting the Mozilla Firefox source tree (`ipc/docs/processes.md`, `layout/docs/LayoutOverview.md`, `gfx/docs/RenderingOverview.md`, `memory/build/`, `netwerk/`, `toolkit/components/extensions/`, `toolkit/components/antitracking/`), Astra adopts the strongest systems-level ideas from Gecko/Servo/WebRender while stripping away 25 years of legacy bloat and introducing a **strict ≤ 2 GB Hard Memory Ceiling** from day one:

| Firefox Subsystem | Astra Subsystem (`astra/src/`) | Key Innovation in Astra |
|---|---|---|
| `ipc/glue`, `ipc/ipdl`, e10s (`Main`, `Content`, `RDD`, `GPU`) | `ipc/ipc_channel.*`, `browser/browser_app.*` | Clean binary length-prefixed `AF_UNIX` IPC over `fork()` processes (`Main`, `Content`, `RDD Media`) with zero XPCOM overhead. |
| `memory/build/Arena.h`, `nsPresArena` (frame poisoning) | `core/arena.*`, `core/memory_governor.*` | Per-tab `mmap` Slab Arenas with poison-on-free (`0xDEADBEEF`) + `madvise(MADV_DONTNEED)` immediate OS page return + real `/proc/<pid>/statm` RSS Governor enforcing a hard 2 GB ceiling via automatic LRU Tab Dehydration. |
| `netwerk/` (Necko HTTP/TLS/Cache/Cookie) | `net/http_client.*` | Zero-build-dependency POSIX socket + `dlopen("libssl.so.3")` TLS 1.3 + `dlopen("libz.so.1")` gzip/deflate client, wired directly into AstraShield & Extension DNR hooks before any socket opens. |
| `toolkit/components/antitracking/` | `privacy/shield.*` | Total Cookie Protection via `(top_level_site, origin)` partitioned jars, suffix-Trie anti-tracker engine, URL tracking parameter stripper (`utm_*`, `fbclid`, `gclid`), HTTPS-Only upgrade, and fingerprint defense. |
| `toolkit/components/extensions/` (`ExtensionDNR`, `Conduits`) | `ext/extension_runtime.*`, `js/astra_js.*` | Manifest V3 loader, permission checks, Declarative Net Request (DNR) rule engine, isolated storage, content script matching, and embedded AstraJS VM with DOM + `astra.*` bindings. |
| `parser/html/`, `dom/` | `dom/html_parser.*` | Arena-allocated HTML5 tokenizer & DOM tree builder (zero per-node `malloc` heap fragmentation). |
| `layout/style/`, `layout/generic/` (`nsIFrame`) | `style/css_engine.*`, `layout/layout_engine.*` | CSS3 cascade + specificity + inheritance + Box/Flexbox layout producing a self-contained **Serialized Binary Display List** (mirroring WebRender Display Lists) for cross-process transport. |
| `gfx/wr/`, `gfx/2d/`, `widget/` | `gfx/rasterizer.*`, `gfx/astra3d.*` | Software 2D ARGB32 compositor with rounded-rect AA, crisp bitmap glyph renderer (ASCII + Latin-1), native PNG encoder, native X11 desktop window (`dlopen("libX11.so.6")`), AND native zero-dep 3D Z-buffer/Phong engine (`Astra3D`). |
| `pdf.js` (JS-based in Firefox) | `docs/doc_viewer.*` | Native C++20 PDF parser (`FlateDecode` streams, `BT/ET/Tj/TJ/Td` text operators, pagination) + native Markdown compiler — no JS PDF overhead. |
| `dom/media/`, `RDD` | `media/media_engine.*` | Native ISOBMFF/MP4 & WebM/EBML container parser, SIMD-friendly YUV420p→RGBA converter, frame clock, and isolated RDD media decoding process. |

## Memory Budget Architecture (≤ 2 GB Hard Cap Under Stress)

1. **Per-Tab Slab Arenas**: Every tab's DOM nodes, computed styles, and Frame Tree live inside dedicated `mmap` slabs. Closing or dehydrating a tab calls `madvise(MADV_DONTNEED)` / `munmap`, returning 100% of pages to the kernel immediately.
2. **Continuous RSS Telemetry**: `MemoryGovernor` reads exact kernel resident set size from `/proc/<pid>/statm` across Main + all worker processes.
3. **3-Tier Watermarks**:
   - `< 70%` of ceiling: Normal operation (active + warm tabs).
   - `70% – 85%` (Soft Watermark): Flush decoded image/video surface caches on background tabs and compact arena slabs.
   - `> 85%` (Hard Watermark): Automatically **dehydrate** LRU background tabs into compact serialized snapshots (URL, scroll offset, title, form state, compressed HTML) — shrinking a 20 MB live tab to ~8–40 KB — and transparently **rehydrate** on tab switch.
