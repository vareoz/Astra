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

Desde la **Sesión 8** el árbol de código es portable: todas las divergencias de sistema operativo están concentradas en `src/core/platform.hpp`, de modo que las mismas fuentes compilan en **Linux (glibc)** y en **macOS/Darwin**. La verificación de la Sesión 8 (49/49 pruebas) se ejecutó en **macOS 27.2 arm64**; las rutas de Linux (`/proc`, `MADV_DONTNEED`, `SOCK_CLOEXEC`, `.so`) se conservan intactas tras la capa de portabilidad, pero no se han vuelto a ejecutar en esta sesión.

## Módulos principales

- **Core, Memoria y Portabilidad (`src/core/`)**: `SlabArena` por pestaña basado en `mmap`/`munmap` con devolución inmediata de páginas al kernel (`MADV_DONTNEED` en Linux, `MADV_FREE` en Darwin), `MemoryGovernor` con lectura **real** de RSS por proceso (`/proc/<pid>/statm` en Linux, `proc_pid_rusage`/`proc_pidinfo` en macOS) y deshidratación LRU de pestañas, parser JSON RFC 8259 y **capa de portabilidad POSIX `platform.hpp`** (banderas de socket, semántica de `madvise`, nombres de bibliotecas para `dlopen`).
- **IPC Multiproceso (`src/ipc/`)**: Aislamiento real mediante `fork()` y sockets binarios `AF_UNIX` (`Main`, `Content`, `RDD Media`).
- **Privacidad (`src/privacy/`)**: `AstraShield` con Total Cookie Protection particionado por `(top_level_site, origin)`, bloqueador Trie de rastreadores, limpieza de parámetros UTM/FBCLID, modo HTTPS-Only y cabeceras endurecidas (`Sec-GPC`).
- **Red y Caché (`src/net/`)**: `AstraNet v8` con **revalidación condicional RFC 9111 (`If-None-Match` / `If-Modified-Since` → `304 Not Modified` servido desde la caché acotada sin retransferir el cuerpo, con directivas `max-age` / `no-cache` / `must-revalidate` / `no-store`)**, cliente nativo WebSocket RFC 6455 (`ws://` y `wss://` vía TLS 1.3 `dlopen`, handshake SHA-1/Base64, masking obligatorio, Ping/Pong automático y límite estricto de 4 MiB por trama), multiplexación real multi-stream HTTP/2 (RFC 7540 + RFC 7541 HPACK/Huffman) sobre un único socket TLS 1.3 ALPN (`h2`), pool acotado Keep-Alive HTTP/1.1, verificación estricta de certificados, `gzip`/`deflate` y `HttpCache` acotado thread-safe (RAM + Disco).
- **Motor Web (`src/dom/`, `src/style/`, `src/layout/`)**: Parser HTML5 con DOM dinámico (`innerHTML`, `classList`, `removeAttribute`, `<canvas>`), cascada CSS3 v8 con **animaciones `@keyframes` (listas de fotogramas por propiedad, `animation` con duración/retardo/iteraciones/`infinite`/`alternate`/`forwards`)**, **transiciones (`transition`) interpoladas sobre valores calculados**, **curvas de temporización exactas (`linear`, `ease`, `ease-in`, `ease-out`, `ease-in-out`, `cubic-bezier()`, `steps()`) resueltas por Newton-Raphson + bisección**, **`transform` 2D (`translate`/`translateX`/`translateY`/`scale`/`scaleX`/`scaleY`/`rotate`) compuesto en matriz afín**, variables CSS (`--*`, `var()` con fallback y herencia `:root`), combinador de hijo directo (`>`), Flexbox completo, `text-align`, `min-width`/`max-width`, `box-sizing`, `linear-gradient(...)` direccional, `@media`, CSS Grid, `position`, `z-index`, `opacity`, `box-shadow`, recorte `overflow: hidden` y formato binario **`ASD6`**.
- **JavaScript y Extensiones (`src/js/`, `src/ext/`)**: `AstraJS v8` (compilador a bytecode + VM de pila + GC Mark-and-Sweep + **DOM Events Level 2: fases de captura/objetivo/burbujeo sobre una ruta congelada, objeto `Event` real con `type`/`target`/`currentTarget`/`eventPhase`, `preventDefault()`, `stopPropagation()`, `stopImmediatePropagation()`, opciones `{capture, once}`, `removeEventListener` y `dispatchEvent`** + **`document.cookie` enganchado al tarro particionado de AstraShield** + clases ES6 + template literals + proxy dinámico `element.style.*` + API nativa `new WebSocket(url)` + `try`/`catch`/`finally`/`throw` + `for..of` + `requestAnimationFrame` + `<canvas>` 2D + `async`/`await` + arrow functions + closures + `Promise` + `fetch` + `localStorage`) y runtime multi-extensión Manifest V3 simultáneo con DNR combinado.
- **Gráficos, Chrome v4, Descargas, Tipografía, 3D, Documentos y Video/Audio (`src/gfx/`, `src/docs/`, `src/media/`, `src/browser/`)**:
  - **Chrome de escritorio v4 + Gestor de Descargas Nativo (`astra://downloads`)**: Omnibox editable, historial (`<` / `>` / `R`), pestañas `[+]` / `[x]`, descarga filtrada por `AstraShield` + MV3 DNR con verificación nativa **SHA-256 (FIPS 180-4)** y registro `ledger.json`.
  - `AstraFont`: tipografía vectorial TrueType UTF-8 anti-aliased vía `dlopen("libfreetype.so.6")` (caché LRU 4 MiB).
  - `AstraImage v2`: decodificador **JPEG Baseline Sequential DCT** en C++20 puro + PNG/BMP/PPM nativos.
  - **Compositor v8 (`ASD6`)**: **composición afín por píxel con muestreo inverso y supermuestreo 2x2 para `transform`**, rasterización direccional de gradientes CSS, alineación `text-align`, blitting de `<canvas>` 2D, recorte estricto `overflow: hidden`, scroll APZ, `DropShadow`, capas `fixed_viewport`, bucle `requestAnimationFrame` en ventana X11 interactiva, motor 3D propio `Astra3D`, visor nativo PDF/Markdown y `AstraMedia v4` con salida de audio por `dlopen` y visualizador de onda PCM.

## Compilación y uso (Linux y macOS, cero dependencias de compilación)

```bash
make -j4
./build/astra --test
./build/astra --stress-tabs 150 --cap 2048
./build/astra --url https://example.com
./build/astra --ext examples/astra-privacy-guard --ext examples/astra-reader-boost --url astra://home --out home.png
./build/astra --url file://examples/astra_session8.html --raf-step 30 --out session8.png
./build/astra --url file://examples/astra_session7.html --out session7.png
./build/astra --url file://examples/astra_session6.html --raf-step 4 --out session6.png
./build/astra --download file://examples/astra_data.json --download https://example.com --url astra://downloads --out downloads.png
./build/astra --url astra://home --gui   # Ventana interactiva nativa X11 (Linux con DISPLAY, o macOS con XQuartz)
```

## Publicación en Hugging Face Hub

```bash
export HF_TOKEN="<tu_token_write_de_hugging_face>"
export HF_REPO_ID="thevareoz/astra-browser-core"
bash upload_to_huggingface.sh "Release Astra Browser Core v8"
```

## Resultados verificados (Sesión 8, macOS 27.2 arm64)

- **Compilación**: `g++ -std=c++20 -O2 -Wall -Wextra -pthread` en macOS 27.2 arm64 (Apple clang 21) — **0 errores, 0 warnings**.
- **Suite de verificación (`--test`)**: **49 / 49 pruebas aprobadas** (43 previas + 6 nuevas):
  - `@keyframes` con listas de fotogramas por propiedad, `animation` con retardo y `fill-mode`, interpolación exacta de px y color, y curvas `linear` / `ease-in` / `cubic-bezier()` / `steps()`.
  - `transform: translate/rotate/scale` compuesto en matriz afín exacta, ida y vuelta por `ASD6` y rasterización de geometría rotada con supermuestreo 2x2.
  - Transiciones CSS que interpolan un cambio de `element.style` a lo largo de fotogramas de `requestAnimationFrame` en vez de saltar, asentándose exactamente en el valor destino.
  - DOM Events Level 2: orden exacto captura → objetivo → burbujeo, `preventDefault()`, `stopPropagation()` y listeners `once`.
  - `AstraNet v8`: GET condicional `If-None-Match` contra un servidor de origen real en loopback y cuerpo servido desde caché al recibir `304 Not Modified`.
  - `document.cookie` leyendo y escribiendo en el tarro particionado, con la Total Cookie Protection intacta frente a otro sitio de primer nivel.
- **RSS real por proceso**: verificado con la API nativa de cada sistema (`/proc/<pid>/statm` en Linux, `libproc` en macOS).
- **Prueba de estrés de 150 pestañas (`--stress-tabs 150 --cap 2048`)** en macOS arm64:
  - 150 pestañas creadas en **633.8 ms** (**4.2 ms/pestaña**).
  - **RAM total real (suma del RSS de los 3 procesos)**: **19.61 MB / 2048 MB** (`main`: 14.47 MB + `content`: 4.05 MB + `rdd-media`: 1.09 MB — ~0.96 % del techo de 2 GB).
  - Rehidratación de pestañas deshidratadas (Tab #1): **OK**.

## Resultados verificados (Sesión 7, Linux)

- **Compilación**: `g++ -std=c++20 -O2 -Wall -Wextra -pthread -ldl` — **0 errores, 0 warnings**.
- **Suite de verificación (`--test`)**: **43 / 43 pruebas aprobadas** (38 previas + cliente nativo WebSocket RFC 6455 con servidor echo loopback y `new WebSocket()` en JS, variables CSS `--*`/`var()` con `:root` y combinador `>`, Flexbox `justify-content: space-between` + `align-items: center` + `text-align` + `min/max-width`, clases ES6 `class`/`constructor`/`this`/`new` + template literals, y proxy dinámico `element.style.*` reflejado en LayoutEngine).
- **Prueba de estrés de 150 pestañas (`--stress-tabs 150 --cap 2048`)**:
  - 150 pestañas creadas en **843.8 ms** (**5.6 ms/pestaña**).
  - **RAM total real (Linux `/proc` RSS sumando los 3 procesos)**: **27.86 MB / 2048 MB** (`main`: 14.43 MB + `content`: 8.93 MB + `rdd-media`: 4.50 MB — ~1.36% del techo de 2 GB).
  - Rehidratación de pestañas deshidratadas (Tab #1): **OK**.


