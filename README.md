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
- **Red y Caché (`src/net/`)**: `AstraNet v4` con **HTTP/2 binario nativo (RFC 7540 + RFC 7541 HPACK tabla estática/dinámica + decodificador canónico Huffman Apéndice B)** negociado sobre TLS 1.3 ALPN (`h2`), pool acotado de conexiones persistentes HTTP/1.1 Keep-Alive (máx. 8 sockets, TTL 15s), verificación estricta de certificados (`dlopen libssl.so.3`), descompresión `gzip`/`deflate` (`libz.so.1`), `HttpCache` acotado thread-safe (RAM + Disco) y descarga paralela acotada en 4 hilos (`HttpClient::fetch_batch`).
- **Motor Web (`src/dom/`, `src/style/`, `src/layout/`)**: Parser HTML5 con DOM dinámico (`innerHTML` getter/setter, `classList`, `removeAttribute`), cascada CSS3 con **`@media (min-width / max-width)`**, Flexbox, **CSS Grid**, `position: relative | absolute | fixed`, `z-index`, `opacity`, `box-shadow`, recorte de píxeles estilo WebRender **`overflow: hidden`** serializado por ítem en **`ASD4`** (compatible con `ASD3`), `<input>`/`<textarea>`, `<audio>` y `<a download>`.
- **JavaScript y Extensiones (`src/js/`, `src/ext/`)**: `AstraJS v5` (compilador a bytecode + VM de pila + GC Mark-and-Sweep + **`async`/`await`** + **arrow functions (`=>`)** + **closures léxicos compartidos y mutables** + `Promise` `.then`/`.catch`/`.finally`/`all` + `fetch(url, options)` + `localStorage` persistente en disco + DOM dinámico `innerHTML`/`classList`/`setAttribute`/`removeAttribute`) y runtime multi-extensión Manifest V3 simultáneo con Declarative Net Request (DNR) combinado.
- **Gráficos, Chrome v4, Descargas, Tipografía, 3D, Documentos y Video/Audio (`src/gfx/`, `src/docs/`, `src/media/`, `src/browser/`)**:
  - **Chrome de escritorio v4 + Gestor de Descargas Nativo (`astra://downloads`)**: Omnibox editable, historial (`<` / `>` / `R`), pestañas `[+]` / `[x]`, descarga de archivos filtrada por `AstraShield` + MV3 DNR hacia `/tmp/astra_downloads/` con verificación nativa **SHA-256 (FIPS 180-4)** y registro persistente `ledger.json`.
  - `AstraFont`: tipografía vectorial TrueType UTF-8 con anti-aliasing vía `dlopen("libfreetype.so.6")` y caché LRU acotada a 4 MiB.
  - `AstraImage v2`: decodificador **JPEG Baseline Sequential DCT** en C++20 puro + PNG/BMP/PPM nativos.
  - Compositor ARGB32 con recorte estricto `overflow: hidden` por ítem, scroll APZ, `DropShadow` suave, capas `fixed_viewport` y ventana X11 interactiva, motor 3D propio `Astra3D`, visor nativo PDF (`FlateDecode`) + Markdown, y **`AstraMedia v4`** con salida de audio nativa Linux **ALSA** (`dlopen("libasound.so.2")`), analizador de picos de forma de onda PCM y reproductor interactivo `<audio>` con visualizador de barras + `<video>` con barra scrubber RDD.

## Compilación y uso (Linux, cero dependencias de compilación)

```bash
make -j4
./build/astra --test
./build/astra --stress-tabs 150 --cap 2048
./build/astra --url https://example.com
./build/astra --ext examples/astra-privacy-guard --ext examples/astra-reader-boost --url astra://home --out home.png
./build/astra --url file://examples/astra_session5.html --out session5.png
./build/astra --download file://examples/astra_data.json --download https://example.com --url astra://downloads --out downloads.png
./build/astra --url astra://home --gui   # Ventana interactiva nativa X11 (Linux con DISPLAY)
```

## Resultados verificados (Sesión 5)

- **Compilación**: `g++ -std=c++20 -O2 -Wall -Wextra -pthread -ldl` — **0 errores, 0 warnings**.
- **Suite de verificación (`--test`)**: **33 / 33 pruebas aprobadas** (27 previas + HTTP/2 RFC 7540/7541 HPACK+Huffman y tráfico real `h2` sobre TLS 1.3 ALPN, `AstraJS v5` `async`/`await` + arrow functions + closures mutables, DOM dinámico `innerHTML`/`classList`/`removeAttribute`, `@media` + recorte estricto de píxeles `overflow: hidden` en `ASD4`, Gestor de Descargas Nativo + SHA-256 + `astra://downloads`, y `AstraMedia v4` analizador de waveform PCM + sondeo ALSA vía `dlopen`).
- **Navegación HTTPS real sobre HTTP/2 (`./build/astra --url https://example.com`)**: `Protocol: h2`, `TLS Verified: YES (OS CA Store)`, **75.68 ms**, **21.79 MB RSS**.
- **Prueba de estrés de 150 pestañas (`--stress-tabs 150 --cap 2048`)**:
  - 150 pestañas creadas en **847.7 ms** (**5.7 ms/pestaña**).
  - **RAM total real (Linux `/proc` RSS sumando los 3 procesos)**: **27.13 MB / 2048 MB** (`main`: 14.59 MB + `content`: 8.49 MB + `rdd-media`: 4.05 MB — ~1.32% del techo de 2 GB).
  - Rehidratación de pestañas deshidratadas (Tab #1): **OK**.

