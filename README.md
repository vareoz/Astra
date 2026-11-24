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
- **Red y Caché (`src/net/`)**: `AstraNet v5` con **multiplexación real multi-stream HTTP/2 (RFC 7540 + RFC 7541 HPACK/Huffman) sobre un único socket TLS 1.3 ALPN (`h2`)** (streams impares `1, 3, 5, ...` con control de flujo por conexión/stream, `SETTINGS`, `WINDOW_UPDATE`, `RST_STREAM` y `GOAWAY`) integrada en `HttpClient::fetch_batch`, pool acotado Keep-Alive HTTP/1.1, verificación estricta de certificados (`dlopen libssl.so.3`), `gzip`/`deflate` (`libz.so.1`) y `HttpCache` acotado thread-safe (RAM + Disco).
- **Motor Web (`src/dom/`, `src/style/`, `src/layout/`)**: Parser HTML5 con DOM dinámico (`innerHTML`, `classList`, `removeAttribute`, `<canvas>`), cascada CSS3 v6 con **`box-sizing: border-box | content-box`**, **`linear-gradient(...)` direccional**, `@media (min-width / max-width)`, Flexbox, CSS Grid, `position: relative | absolute | fixed`, `z-index`, `opacity`, `box-shadow`, recorte de píxeles `overflow: hidden` y formato binario **`ASD5`** (`FillGradient` + `EmbedCanvas`, retrocompatible con `ASD4`/`ASD3`).
- **JavaScript y Extensiones (`src/js/`, `src/ext/`)**: `AstraJS v6` (compilador a bytecode + VM de pila + GC Mark-and-Sweep + **`try`/`catch`/`finally`/`throw`** con desenrollado multi-frame + bucles **`for..of`** sobre Arrays y Strings + **`requestAnimationFrame`** + **HTML5 `<canvas>` 2D API** con superficie acotada ARGB32 de máx. 1024×1024 + `async`/`await` + arrow functions + closures mutables + `Promise` + `fetch` + `localStorage`) y runtime multi-extensión Manifest V3 simultáneo con DNR combinado.
- **Gráficos, Chrome v4, Descargas, Tipografía, 3D, Documentos y Video/Audio (`src/gfx/`, `src/docs/`, `src/media/`, `src/browser/`)**:
  - **Chrome de escritorio v4 + Gestor de Descargas Nativo (`astra://downloads`)**: Omnibox editable, historial (`<` / `>` / `R`), pestañas `[+]` / `[x]`, descarga filtrada por `AstraShield` + MV3 DNR con verificación nativa **SHA-256 (FIPS 180-4)** y registro `ledger.json`.
  - `AstraFont`: tipografía vectorial TrueType UTF-8 anti-aliased vía `dlopen("libfreetype.so.6")` (caché LRU 4 MiB).
  - `AstraImage v2`: decodificador **JPEG Baseline Sequential DCT** en C++20 puro + PNG/BMP/PPM nativos.
  - **Compositor v6 (`ASD5`)**: rasterización direccional de gradientes CSS por píxel, blitting de `<canvas>` 2D, recorte estricto `overflow: hidden`, scroll APZ, `DropShadow`, capas `fixed_viewport`, bucle `requestAnimationFrame` en ventana X11 interactiva, motor 3D propio `Astra3D`, visor nativo PDF/Markdown y `AstraMedia v4` con salida Linux ALSA (`dlopen("libasound.so.2")`) y visualizador de onda PCM.

## Compilación y uso (Linux, cero dependencias de compilación)

```bash
make -j4
./build/astra --test
./build/astra --stress-tabs 150 --cap 2048
./build/astra --url https://example.com
./build/astra --ext examples/astra-privacy-guard --ext examples/astra-reader-boost --url astra://home --out home.png
./build/astra --url file://examples/astra_session6.html --raf-step 4 --out session6.png
./build/astra --download file://examples/astra_data.json --download https://example.com --url astra://downloads --out downloads.png
./build/astra --url astra://home --gui   # Ventana interactiva nativa X11 (Linux con DISPLAY)
```

## Resultados verificados (Sesión 6)

- **Compilación**: `g++ -std=c++20 -O2 -Wall -Wextra -pthread -ldl` — **0 errores, 0 warnings**.
- **Suite de verificación (`--test`)**: **38 / 38 pruebas aprobadas** (33 previas + multiplexación real de 3 streams HTTP/2 sobre un solo socket TLS 1.3 ALPN `h2`, `AstraJS v6` `try`/`catch`/`finally`/`throw` multi-frame + `for..of` sobre Arrays y Strings, HTML5 `<canvas>` 2D API + cola `requestAnimationFrame`, CSS `box-sizing: border-box` vs `content-box`, y `linear-gradient(90deg)` con serialización `ASD5` e interpolación RGB por píxel).
- **Prueba de estrés de 150 pestañas (`--stress-tabs 150 --cap 2048`)**:
  - 150 pestañas creadas en **959.9 ms** (**6.4 ms/pestaña**).
  - **RAM total real (Linux `/proc` RSS sumando los 3 procesos)**: **27.75 MB / 2048 MB** (`main`: 14.37 MB + `content`: 8.89 MB + `rdd-media`: 4.48 MB — ~1.35% del techo de 2 GB).
  - Rehidratación de pestañas deshidratadas (Tab #1): **OK**.


