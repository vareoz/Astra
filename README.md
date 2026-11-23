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
- **Red y Caché (`src/net/`)**: `AstraNet` con HTTP/1.1, verificación estricta de certificados TLS 1.3 (`dlopen libssl.so.3`), descompresión `gzip`/`deflate` (`libz.so.1`) y `HttpCache` acotado (RAM + Disco).
- **Motor Web (`src/dom/`, `src/style/`, `src/layout/`)**: Parser HTML5, cascada CSS3 con Flexbox + **CSS Grid** (`display: grid`, `grid-template-columns`, `repeat(N, 1fr)`, `fr`/`px`, `gap`), tipografía monoespaciada, campos de formulario interactivos (`<input>`/`<textarea>` con foco y escritura por teclado), Display List binario serializable (`ASD2`) y regiones interactivas (`HitRegion`).
- **JavaScript y Extensiones (`src/js/`, `src/ext/`)**: `AstraJS v3` (compilador a bytecode + VM de pila + GC Mark-and-Sweep + Event Loop de microtasks/timers + eventos DOM `click`/`input`/`submit` + `fetch(url)` integrado con `AstraShield`/`HttpCache`/TLS + `JSON.parse`/`JSON.stringify` + `localStorage` + métodos de `String`) y runtime de WebExtensions Manifest V3 con Declarative Net Request (DNR).
- **Gráficos, Tipografía, 3D, Documentos y Video (`src/gfx/`, `src/docs/`, `src/media/`)**:
  - `AstraFont`: tipografía vectorial TrueType UTF-8 con anti-aliasing vía `dlopen("libfreetype.so.6")` (DejaVuSans + JetBrainsMono) y caché LRU acotada a 4 MiB (con fallback bitmap 8×8).
  - `AstraImage v2`: decodificador **JPEG Baseline Sequential DCT** en C++20 puro (YCbCr 4:4:4/4:2:2/4:2:0, IDCT 8×8, cero `libjpeg`) + decodificador nativo PNG/BMP/PPM.
  - Compositor ARGB32 con scroll APZ y ventana X11 interactiva (ratón + teclado `XLookupString`), motor 3D propio `Astra3D` (Z-buffer, normales suaves Blinn-Phong, shaders procedurales, `.obj`), visor nativo PDF (`FlateDecode`) + Markdown, y decodificador de video real MP4 H.264/Y4M en proceso RDD.

## Compilación y uso (Linux, cero dependencias de compilación)

```bash
make -j4
./build/astra --test
./build/astra --stress-tabs 150 --cap 2048
./build/astra --ext examples/astra-privacy-guard --url astra://home --out home.png
./build/astra --url file://examples/astra_session3.html --click 200,235 --type "Hola Astra v3" --enter --out session3_after.png
./build/astra --url astra://3d --wireframe --out studio_3d.png
./build/astra --url astra://home --gui   # Ventana interactiva nativa X11 (Linux con DISPLAY)
```

## Resultados verificados (Sesión 3)

- **Compilación**: `g++ -std=c++20 -O2 -Wall -Wextra -pthread -ldl` — **0 errores, 0 warnings**.
- **Suite de verificación (`--test`)**: **21 / 21 pruebas aprobadas** (SlabArena `munmap`/`madvise`, JSON RFC 8259, AstraShield + Total Cookie Protection, `HttpCache` RAM+Disco con evicción LRU, decodificador nativo PNG, DisplayList binario `ASD2` + `HitRegions`, `AstraJS v2/v3` Bytecode VM + GC Mark-and-Sweep + Event Loop `Microtask -> Timer` + eventos DOM `click`/`input`/`submit` + `fetch()`/`JSON`/`localStorage`, `Astra3D v2` shaders suaves Blinn-Phong, visor nativo PDF, `AstraMedia v2` MP4 H.264 multiframe acotado a 16 MiB, IPC multiproceso `Main + Content + RDD`, verificación estricta TLS con CA del sistema + cache hit, `AstraFont` TrueType UTF-8 + caché LRU 4 MiB, `AstraImage v2` JPEG DCT puro, CSS Grid multicolumna/multifila, foco + escritura + Enter en `<input>`).
- **Prueba de estrés de 150 pestañas (`--stress-tabs 150 --cap 2048`)**:
  - 150 pestañas creadas en **~808 ms** (**~5.4 ms/pestaña** con tipografía vectorial TrueType).
  - **RAM total real (Linux `/proc` RSS sumando los 3 procesos)**: **~21.6 MB / 2048 MB** (`main` + `content` + `rdd-media`, ~1% del techo de 2 GB).
  - Rehidratación de pestañas deshidratadas (Tab #1): **OK**.

