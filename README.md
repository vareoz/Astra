---
language:
  - es
  - en
license: mpl-2.0
tags:
  - browser-engine
  - cpp20
  - native-desktop
  - privacy
  - webextensions
  - astra-browser
---

# Astra Browser Core (Native C++20 Multi-Process Engine)

Navegador web de escritorio construido desde cero en **C++20 puro**, inspirado en la arquitectura multiproceso de Mozilla Firefox (e10s / WebRender / Necko / RDD / nsPresArena), **sin Electron**, **sin Chromium** y con un techo estricto de memoria **≤ 2 GB RAM**.

## Módulos principales

- **Core & Memoria (`src/core/`)**: `SlabArena` por pestaña basado en `mmap`/`munmap`/`madvise(MADV_DONTNEED)`, `MemoryGovernor` con lectura real de RSS en `/proc/<pid>/statm` y deshidratación LRU de pestañas, parser JSON RFC 8259.
- **IPC Multiproceso (`src/ipc/`)**: Aislamiento real mediante `fork()` y sockets binarios `AF_UNIX` (`Main`, `Content`, `RDD Media`).
- **Privacidad (`src/privacy/`)**: `AstraShield` con Total Cookie Protection particionado por `(top_level_site, origin)`, bloqueador Trie de rastreadores, limpieza de parámetros UTM/FBCLID, modo HTTPS-Only y cabeceras endurecidas (`Sec-GPC`).
- **Red y Caché (`src/net/`)**: `AstraNet v3` con HTTP/1.1 (`GET` + `POST`), verificación estricta de certificados TLS 1.3 + negociación **ALPN** (`dlopen libssl.so.3`), descompresión `gzip`/`deflate` (`libz.so.1`), `HttpCache` acotado thread-safe (RAM + Disco) y descarga paralela acotada en 4 hilos (`HttpClient::fetch_batch`) para subrecursos CSS e imágenes remotas.
- **Motor Web (`src/dom/`, `src/style/`, `src/layout/`)**: Parser HTML5, cascada CSS3 con Flexbox + **CSS Grid** (`display: grid`, `repeat(N, 1fr)`, `gap`), posicionamiento avanzado **`position: relative | absolute | fixed`**, capas **`z-index`**, **`opacity`**, sombras suaves **`box-shadow`** (`DropShadow`), campos interactivos (`<input>`/`<textarea>`), Display List binario serializable **`ASD3`** y regiones interactivas (`HitRegion`) inmunes al scroll para elementos `fixed`.
- **JavaScript y Extensiones (`src/js/`, `src/ext/`)**: `AstraJS v4` (compilador a bytecode + VM de pila + GC Mark-and-Sweep + operador ternario `? :` + Event Loop `Microtask -> Timer` + **`Promise`** real con `.then`/`.catch`/`.finally`/`Promise.resolve`/`reject`/`all` + `fetch(url, options)` encadenable como Promise + `localStorage` **persistente en disco por origen** con tope de 1 MiB + `chrome.storage.local` / `browser.storage.local` + `Array.join`/`map` y `String.split`) y runtime multi-extensión Manifest V3 simultáneo con Declarative Net Request (DNR) combinado.
- **Gráficos, Chrome v4, Tipografía, 3D, Documentos y Video/Audio (`src/gfx/`, `src/docs/`, `src/media/`)**:
  - **Chrome de escritorio v4**: Omnibox editable con foco, cursor y navegación por Enter, historial por pestaña (`<` Atrás / `>` Adelante / `R` Recargar), botón `[+]` nueva pestaña, chip `[x]` cerrar pestaña y botón `ASTRA` -> `astra://home`, con truncado UTF-8 seguro.
  - `AstraFont`: tipografía vectorial TrueType UTF-8 con anti-aliasing vía `dlopen("libfreetype.so.6")` (DejaVuSans + JetBrainsMono) y caché LRU acotada a 4 MiB (con fallback bitmap 8×8).
  - `AstraImage v2`: decodificador **JPEG Baseline Sequential DCT** en C++20 puro (YCbCr 4:4:4/4:2:2/4:2:0, IDCT 8×8, cero `libjpeg`) + decodificador nativo PNG/BMP/PPM.
  - Compositor ARGB32 con scroll APZ, `DropShadow` suave, capas `fixed_viewport` y ventana X11 interactiva (ratón + teclado `XLookupString`), motor 3D propio `Astra3D` (Z-buffer, normales suaves Blinn-Phong, shaders procedurales, `.obj`), visor nativo PDF (`FlateDecode`) + Markdown, y **`AstraMedia v3`** con barra scrubber interactiva de `<video>` (búsqueda directa por clic al frame RDD), sondeo de pistas de audio MP4/WAV y decodificador PCM `s16le` acotado a 4 MiB con cálculo RMS real.

## Compilación y uso (Linux, cero dependencias de compilación)

```bash
make -j4
./build/astra --test
./build/astra --stress-tabs 150 --cap 2048
./build/astra --ext examples/astra-privacy-guard --ext examples/astra-reader-boost --url astra://home --out home.png
./build/astra --ext examples/astra-privacy-guard --ext examples/astra-reader-boost --url file://examples/astra_session4.html --scroll 240 --click 430,382 --out session4_seek.png
./build/astra --url astra://home --click 320,60 --type "https://example.com" --enter --out nav.png
./build/astra --url astra://3d --wireframe --out studio_3d.png
./build/astra --url astra://home --gui   # Ventana interactiva nativa X11 (Linux con DISPLAY)
```

## Resultados verificados (Sesión 4)

- **Compilación**: `g++ -std=c++20 -O2 -Wall -Wextra -pthread -ldl` — **0 errores, 0 warnings**.
- **Suite de verificación (`--test`)**: **27 / 27 pruebas aprobadas** (incluyendo todas las 21 pruebas previas más Chrome v4 Omnibox/Historial/Tabs, CSS `position: absolute/fixed` + `z-index` + `box-shadow` + `opacity` + `ASD3`, `AstraNet v3` TLS 1.3 ALPN + `fetch_batch` paralelo en 4 hilos, `AstraJS v4` `Promise` microtasks FIFO + `localStorage` persistente en disco, aislamiento multi-extensión MV3 simultáneo con 2 extensiones + `chrome.storage.local`, y `AstraMedia v3` WAV/PCM con cálculo RMS real + búsqueda directa en scrubber de video).
- **Prueba de estrés de 150 pestañas (`--stress-tabs 150 --cap 2048`)**:
  - 150 pestañas creadas en **835.1 ms** (**5.6 ms/pestaña** con tipografía vectorial TrueType y motor v4).
  - **RAM total real (Linux `/proc` RSS sumando los 3 procesos)**: **21.89 MB / 2048 MB** (`main`: 12.89 MB + `content`: 6.16 MB + `rdd-media`: 2.84 MB — ~1.07% del techo de 2 GB).
  - Rehidratación de pestañas deshidratadas (Tab #1): **OK**.

