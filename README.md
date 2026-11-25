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
- **Red y Caché (`src/net/`)**: `AstraNet v6` con **cliente nativo WebSocket RFC 6455 (`ws://` y `wss://` vía TLS 1.3 `dlopen`, handshake SHA-1/Base64, masking obligatorio, Ping/Pong automático y límite estricto de 4 MiB por trama)**, **multiplexación real multi-stream HTTP/2 (RFC 7540 + RFC 7541 HPACK/Huffman) sobre un único socket TLS 1.3 ALPN (`h2`)**, pool acotado Keep-Alive HTTP/1.1, verificación estricta de certificados (`dlopen libssl.so.3`), `gzip`/`deflate` (`libz.so.1`) y `HttpCache` acotado thread-safe (RAM + Disco).
- **Motor Web (`src/dom/`, `src/style/`, `src/layout/`)**: Parser HTML5 con DOM dinámico (`innerHTML`, `classList`, `removeAttribute`, `<canvas>`), cascada CSS3 v7 con **variables CSS (`--*`, `var()` con fallback y herencia `:root`)**, **combinador de hijo directo (`>`)**, **Flexbox completo (`justify-content: center | flex-end | space-between`, `align-items: center | flex-end`, `flex-shrink`)**, **`text-align: left | center | right`**, **`min-width`/`max-width`**, `box-sizing: border-box | content-box`, `linear-gradient(...)` direccional, `@media (min-width / max-width)`, CSS Grid, `position: relative | absolute | fixed`, `z-index`, `opacity`, `box-shadow`, recorte de píxeles `overflow: hidden` y formato binario **`ASD5`**.
- **JavaScript y Extensiones (`src/js/`, `src/ext/`)**: `AstraJS v7` (compilador a bytecode + VM de pila + GC Mark-and-Sweep + **clases ES6 `class` / `constructor` / `this` / `new`** + **template literals `` `...${expr}...` ``** + **proxy dinámico `element.style.*` camelCase↔kebab-case** + **API nativa `new WebSocket(url)` con `send`/`ping`/`recv`/`close`/`onmessage`/`onclose`** + `try`/`catch`/`finally`/`throw` multi-frame + `for..of` + `requestAnimationFrame` + HTML5 `<canvas>` 2D API + `async`/`await` + arrow functions + closures mutables + `Promise` + `fetch` + `localStorage`) y runtime multi-extensión Manifest V3 simultáneo con DNR combinado.
- **Gráficos, Chrome v4, Descargas, Tipografía, 3D, Documentos y Video/Audio (`src/gfx/`, `src/docs/`, `src/media/`, `src/browser/`)**:
  - **Chrome de escritorio v4 + Gestor de Descargas Nativo (`astra://downloads`)**: Omnibox editable, historial (`<` / `>` / `R`), pestañas `[+]` / `[x]`, descarga filtrada por `AstraShield` + MV3 DNR con verificación nativa **SHA-256 (FIPS 180-4)** y registro `ledger.json`.
  - `AstraFont`: tipografía vectorial TrueType UTF-8 anti-aliased vía `dlopen("libfreetype.so.6")` (caché LRU 4 MiB).
  - `AstraImage v2`: decodificador **JPEG Baseline Sequential DCT** en C++20 puro + PNG/BMP/PPM nativos.
  - **Compositor v7 (`ASD5`)**: rasterización direccional de gradientes CSS por píxel, alineación `text-align`, blitting de `<canvas>` 2D, recorte estricto `overflow: hidden`, scroll APZ, `DropShadow`, capas `fixed_viewport`, bucle `requestAnimationFrame` en ventana X11 interactiva, motor 3D propio `Astra3D`, visor nativo PDF/Markdown y `AstraMedia v4` con salida Linux ALSA (`dlopen("libasound.so.2")`) y visualizador de onda PCM.

## Compilación y uso (Linux, cero dependencias de compilación)

```bash
make -j4
./build/astra --test
./build/astra --stress-tabs 150 --cap 2048
./build/astra --url https://example.com
./build/astra --ext examples/astra-privacy-guard --ext examples/astra-reader-boost --url astra://home --out home.png
./build/astra --url file://examples/astra_session7.html --out session7.png
./build/astra --url file://examples/astra_session6.html --raf-step 4 --out session6.png
./build/astra --download file://examples/astra_data.json --download https://example.com --url astra://downloads --out downloads.png
./build/astra --url astra://home --gui   # Ventana interactiva nativa X11 (Linux con DISPLAY)
```

## Publicación en Hugging Face Hub

```bash
export HF_TOKEN="<tu_token_write_de_hugging_face>"
export HF_REPO_ID="thevareoz/astra-browser-core"
bash upload_to_huggingface.sh "Release Astra Browser Core v7"
```

## Resultados verificados (Sesión 7)

- **Compilación**: `g++ -std=c++20 -O2 -Wall -Wextra -pthread -ldl` — **0 errores, 0 warnings**.
- **Suite de verificación (`--test`)**: **43 / 43 pruebas aprobadas** (38 previas + cliente nativo WebSocket RFC 6455 con servidor echo loopback y `new WebSocket()` en JS, variables CSS `--*`/`var()` con `:root` y combinador `>`, Flexbox `justify-content: space-between` + `align-items: center` + `text-align` + `min/max-width`, clases ES6 `class`/`constructor`/`this`/`new` + template literals, y proxy dinámico `element.style.*` reflejado en LayoutEngine).
- **Prueba de estrés de 150 pestañas (`--stress-tabs 150 --cap 2048`)**:
  - 150 pestañas creadas en **843.8 ms** (**5.6 ms/pestaña**).
  - **RAM total real (Linux `/proc` RSS sumando los 3 procesos)**: **27.86 MB / 2048 MB** (`main`: 14.43 MB + `content`: 8.93 MB + `rdd-media`: 4.50 MB — ~1.36% del techo de 2 GB).
  - Rehidratación de pestañas deshidratadas (Tab #1): **OK**.


