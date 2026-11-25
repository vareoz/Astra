# Astra Browser — Native Multi-Process Architecture

## Architectural Comparison: Mozilla Firefox → Astra (100% Original C++20)

After inspecting the Mozilla Firefox source tree (`ipc/docs/processes.md`, `layout/docs/LayoutOverview.md`, `gfx/docs/RenderingOverview.md`, `memory/build/`, `netwerk/`, `toolkit/components/extensions/`, `toolkit/components/antitracking/`), Astra adopts the strongest systems-level ideas from Gecko/Servo/WebRender while stripping away 25 years of legacy bloat and introducing a **strict ≤ 2 GB Hard Memory Ceiling** from day one:

| Firefox Subsystem | Astra Subsystem (`astra/src/`) | Key Innovation in Astra |
|---|---|---|
| `ipc/glue`, `ipc/ipdl`, e10s (`Main`, `Content`, `RDD`, `GPU`) | `ipc/ipc_channel.*`, `browser/browser_app.*` | Clean binary length-prefixed `AF_UNIX` IPC over `fork()` processes (`Main`, `Content`, `RDD Media`) with zero XPCOM overhead. |
| `memory/build/Arena.h`, `nsPresArena` (frame poisoning) | `core/arena.*`, `core/memory_governor.*`, `core/platform.hpp` | Per-tab `mmap` Slab Arenas with poison-on-free (`0xDEADBEEF`) + immediate OS page return (`madvise(MADV_DONTNEED)` en Linux, `MADV_FREE` en Darwin) + real per-process RSS Governor (`/proc/<pid>/statm` en Linux, `proc_pid_rusage`/`proc_pidinfo` en macOS) enforcing a hard 2 GB ceiling via automatic LRU Tab Dehydration. |
| `netwerk/` (Necko HTTP/TLS/Cache/Cookie) | `net/http_client.*`, `net/http_cache.*` | Zero-build-dependency POSIX socket + TLS 1.3 y gzip/deflate por `dlopen` (`libssl.so.3` / `libssl.3.dylib`, `libz.so.1` / `libz.1.dylib`), wired directly into AstraShield & Extension DNR hooks before any socket opens, con **revalidación condicional RFC 9111** (`If-None-Match` / `If-Modified-Since` → `304 Not Modified` servido desde la caché acotada sin retransferir el cuerpo). |
| `toolkit/components/antitracking/` | `privacy/shield.*` | Total Cookie Protection via `(top_level_site, origin)` partitioned jars, suffix-Trie anti-tracker engine, URL tracking parameter stripper (`utm_*`, `fbclid`, `gclid`), HTTPS-Only upgrade, and fingerprint defense. |
| `toolkit/components/extensions/` (`ExtensionDNR`, `Conduits`) | `ext/extension_runtime.*`, `js/astra_js.*` | Manifest V3 loader, permission checks, Declarative Net Request (DNR) rule engine, isolated storage, content script matching, and embedded AstraJS VM with DOM + `astra.*` bindings. |
| `parser/html/`, `dom/` | `dom/html_parser.*`, `js/astra_js.*` | Arena-allocated HTML5 tokenizer & DOM tree builder (zero per-node `malloc` heap fragmentation) + **DOM Events Level 2**: ruta de propagación congelada, fases de captura/objetivo/burbujeo, objeto `Event` con `target`/`currentTarget`/`eventPhase`, `preventDefault()`, `stopPropagation()`, `stopImmediatePropagation()` y listeners `once`. |
| `layout/style/`, `layout/generic/` (`nsIFrame`) | `style/css_engine.*`, `style/animation_engine.*`, `layout/layout_engine.*` | CSS3 cascade + specificity + inheritance + Box/Flexbox/Grid layout producing a self-contained **Serialized Binary Display List** (`ASD6`, mirroring WebRender Display Lists) for cross-process transport, con **CSS Animations Level 1** (`@keyframes` con listas de fotogramas por propiedad, `animation` con delay/iteraciones/dirección/fill-mode), **CSS Transitions Level 1** y `transform` 2D resuelto a matriz afín por item. |
| `gfx/wr/`, `gfx/2d/`, `widget/` | `gfx/rasterizer.*`, `gfx/astra3d.*` | Software 2D ARGB32 compositor with rounded-rect AA, TrueType vector glyphs (`AstraFont`), native PNG encoder, native X11 desktop window (`dlopen`), **composición afín por píxel con muestreo inverso y supermuestreo 2x2** para `transform`, AND native zero-dep 3D Z-buffer/Phong engine (`Astra3D`). |
| `pdf.js` (JS-based in Firefox) | `docs/doc_viewer.*` | Native C++20 PDF parser (`FlateDecode` streams, `BT/ET/Tj/TJ/Td` text operators, pagination) + native Markdown compiler — no JS PDF overhead. |
| `dom/media/`, `RDD` | `media/media_engine.*` | Native ISOBMFF/MP4 & WebM/EBML container parser, SIMD-friendly YUV420p→RGBA converter, frame clock, and isolated RDD media decoding process. |

## Memory Budget Architecture (≤ 2 GB Hard Cap Under Stress)

1. **Per-Tab Slab Arenas**: Every tab's DOM nodes, computed styles, and Frame Tree live inside dedicated `mmap` slabs. Closing or dehydrating a tab calls `madvise(MADV_DONTNEED)` / `munmap`, returning 100% of pages to the kernel immediately.
2. **Continuous RSS Telemetry**: `MemoryGovernor` reads the exact kernel resident set size of Main + every worker process through each OS's native API — `/proc/<pid>/statm` on Linux and `proc_pid_rusage(RUSAGE_INFO_CURRENT)` / `proc_pidinfo(PROC_PIDTASKINFO)` (libproc) on macOS. No estimation, no heuristics.
3. **3-Tier Watermarks**:
   - `< 70%` of ceiling: Normal operation (active + warm tabs).
   - `70% – 85%` (Soft Watermark): Flush decoded image/video surface caches on background tabs and compact arena slabs.
   - `> 85%` (Hard Watermark): Automatically **dehydrate** LRU background tabs into compact serialized snapshots (URL, scroll offset, title, form state, compressed HTML) — shrinking a 20 MB live tab to ~8–40 KB — and transparently **rehydrate** on tab switch.

## Capa de portabilidad POSIX (Session 8)

Astra nació como motor nativo para Linux. A partir de la sesión 8 **las mismas fuentes compilan en Linux (glibc) y en macOS/Darwin**, sin `#ifdef` repartidos por los 23 módulos: todas las divergencias viven en `src/core/platform.hpp`. La suite completa (49/49) se verificó en macOS 27.2 arm64; las rutas de Linux se conservan intactas detrás de la misma capa.

| Divergencia | Linux | macOS / Darwin |
|---|---|---|
| `socketpair` close-on-exec | `SOCK_CLOEXEC` | `fcntl(FD_CLOEXEC)` tras crear el par |
| Supresión de `SIGPIPE` al escribir | `send(..., MSG_NOSIGNAL)` | `setsockopt(SO_NOSIGPIPE)` |
| Devolución de páginas anónimas | `madvise(MADV_DONTNEED)` | `madvise(MADV_FREE)` |
| RSS / VSIZE por proceso | `/proc/<pid>/statm` | `proc_pid_rusage` + `proc_pidinfo` (libproc) |
| Bibliotecas por `dlopen` | `libssl.so.3`, `libz.so.1`, `libfreetype.so.6`, `libX11.so.6`, `libasound.so.2` | rutas absolutas de OpenSSL 3 / FreeType (Homebrew o MacPorts), `libz.1.dylib`, `/opt/X11` |
| Enlazado de `dl*()` | `-ldl` | parte de `libSystem` (sin `-ldl`) |

Dos detalles importantes del puerto a macOS:

- **Nunca se carga `/usr/lib/libcrypto.dylib`.** El stub que Apple publica en el caché de dyld imprime `loading libcrypto in an unsafe way` y **aborta el proceso** desde su inicializador. Por eso la lista de candidatos de Darwin empieza por las rutas absolutas de OpenSSL 3 instalado y deja los nombres genéricos al final.
- **Las respuestas sin cuerpo se cortan por código de estado.** `1xx`, `204` y `304` nunca llevan cuerpo (RFC 9110 §6.4.1) aunque no declaren `Content-Length`; sin ese corte la lectura de un `304` de revalidación se bloquearía esperando el cierre de la conexión.

El `Makefile` detecta el sistema con `uname -s`, ajusta el enlazado y genera dependencias de cabeceras con `-MMD -MP`, de modo que tocar `platform.hpp` recompila lo que corresponde.
