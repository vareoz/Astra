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

El árbol de código es portable: todas las divergencias de sistema operativo están concentradas en `src/core/platform.hpp`, de modo que las mismas fuentes compilan en **Linux (glibc)** y en **macOS/Darwin**. Las primitivas que solo existen en un kernel (el sandbox `seccomp-BPF`) degradan de forma segura y explícita fuera de él.

## Módulos principales

- **Core, Memoria y Portabilidad (`src/core/`)**: `SlabArena` por pestaña basado en `mmap`/`munmap` con devolución inmediata de páginas al kernel (`MADV_DONTNEED` en Linux, `MADV_FREE` en Darwin), `MemoryGovernor` con lectura **real** de RSS por proceso (`/proc/<pid>/statm` en Linux, `proc_pid_rusage`/`proc_pidinfo` en macOS) y deshidratación LRU de pestañas, parser JSON RFC 8259 y **capa de portabilidad POSIX `platform.hpp`** (banderas de socket, semántica de `madvise`, nombres de biblioteca para `dlopen`).
- **IPC Multiproceso y Sandbox de Kernel (`src/ipc/`)**: Aislamiento real mediante `fork()`, sockets binarios `AF_UNIX` (`Main`, `Content`, `RDD Media`), **Site Isolation por eTLD+1** (procesos de contenido dedicados por origen) y **sandbox real de kernel Linux (`ProcessSandbox`) con `PR_SET_NO_NEW_PRIVS`, `setrlimit` y filtro `seccomp-BPF`** (`SECCOMP_MODE_FILTER`) bloqueando con `EPERM` las syscalls `socket`/`connect`/`bind`/`listen`/`accept`/`ptrace`/`mount`/`chmod`/`chown` y (en `ContentWorker`) `execve`/`execveat`/`fork`/`vfork`/`kill`.
- **Seguridad Web y Privacidad (`src/privacy/`)**:
  - **Motor de Políticas Web (`security_policy.*`)**: `SecurityOrigin` (Same-Origin Policy y clave `site_key()` eTLD+1), **`ContentSecurityPolicy` (CSP Nivel 2/3)** desde cabecera HTTP y `<meta http-equiv="Content-Security-Policy">` (control de `script-src`, `style-src`, `connect-src`, `default-src`, `'self'`, `'none'`, `'unsafe-inline'`, wildcards `*.dominio`), **`MixedContentBlocker`** (bloqueo de contenido activo `http://`/`ws://` desde documentos `https://`) y **`CorsEnforcer`** (`Access-Control-Allow-Origin`).
  - **`AstraShield`**: Total Cookie Protection particionado por `(top_level_site, origin)` con **aplicación real de `SameSite=Strict | Lax | None`** (rechazo de `SameSite=None` sin `Secure`), bloqueador Trie de rastreadores, limpieza de parámetros UTM/FBCLID, modo HTTPS-Only y cabeceras endurecidas (`Sec-GPC`).
- **Red y Caché (`src/net/`)**: `AstraNet v8` con **revalidación condicional RFC 9111 (`If-None-Match` / `If-Modified-Since` → `304 Not Modified` servido desde la caché acotada sin retransferir el cuerpo, con directivas `max-age` / `no-cache` / `must-revalidate` / `no-store`)**, **Brotli (RFC 7932)**, métodos REST completos (`GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD`) y cabeceras personalizadas en `HttpClient::request()` y `fetch()`, cliente nativo WebSocket RFC 6455 (`ws://` y `wss://` vía TLS 1.3 `dlopen`), multiplexación real multi-stream HTTP/2 (RFC 7540 + RFC 7541 HPACK/Huffman) sobre un único socket TLS 1.3 ALPN (`h2`), pool acotado Keep-Alive HTTP/1.1, verificación estricta de certificados (`dlopen libssl.so.3`), `gzip`/`deflate` (`libz.so.1`) y `HttpCache` acotado thread-safe (RAM + Disco).
- **Motor Web (`src/dom/`, `src/style/`, `src/layout/`)**: Parser HTML5 con DOM dinámico (`innerHTML`, `classList`, `removeAttribute`, `<canvas>`, `<svg>`, **`<table>`/`<thead>`/`<tbody>`/`<tr>`/`<th>`/`<td>` con `colspan` acotado**, **`<iframe srcdoc>` recursivo acotado** y **envío nativo de formularios `<form action="..." method="GET|POST">`**), cascada CSS3 v9 con **evaluador recursivo `calc(...)` (`px`, `%`, `vw`, `vh`, `rem`, `em`)**, **`:nth-child(an+b | odd | even | N)`**, **pseudo-elementos `::before`/`::after` (`content: "..."`)**, **`display: inline-block` real con salto horizontal**, **`float: left | right` y `clear: left | right | both`**, pseudo-clases (`:first-child`, `:last-child`, `:focus`, `:hover`), `flex-wrap: wrap`, `border-left`, **animaciones `@keyframes` con listas de fotogramas por propiedad y atajo `animation` (duración, retardo, iteraciones, `infinite`, `alternate`, `forwards`)**, **transiciones `transition` interpoladas sobre valores calculados**, **curvas de temporización exactas (`linear`, `ease*`, `cubic-bezier()`, `steps()`) resueltas por Newton-Raphson con respaldo de bisección**, **`transform` 2D completo (`translate`/`scale`/`rotate`) resuelto a matriz afín de pintado que, conforme a CSS Transforms Level 1, NO altera la maquetación**, variables CSS (`--*`, `var()`), combinador `>`, Flexbox, CSS Grid, `@media`, `box-sizing`, `linear-gradient(...)`, `position: relative | absolute | fixed`, `z-index`, `opacity`, `box-shadow`, recorte `overflow: hidden` y formato binario **`ASD6`** (matriz afín por item).
- **JavaScript y Extensiones (`src/js/`, `src/ext/`)**: `AstraJS v10` (compilador a bytecode + VM de pila + GC Mark-and-Sweep + **cadena `[[Prototype]]` real (`Object.create`/`getPrototypeOf`/`setPrototypeOf`, `fn.prototype`, `class Sub extends Base` + `super()`/`super.method()`, `instanceof`)** + **`RegExp` nativo (`/pat/flags`, `.test()`, `.exec()`, `String.match`/`replace(/g)`/`split(re)`)** + **`Date` nativo (ISO-8601, `Date.now()`, getters UTC, `.toISOString()`)** + **`Map` y `Set` nativos con iteración `for..of`** + **APIs SPA (`history.pushState`/`replaceState`/`back`/`forward`/`state`/`length`, `location.*`, `sessionStorage`)** + **DOM Events Level 2 (ruta de propagación congelada, fases de captura/objetivo/burbujeo, objeto `Event` con `type`/`target`/`currentTarget`/`eventPhase`, `preventDefault()`, `stopPropagation()`, `stopImmediatePropagation()`, opciones `{capture, once}`, `removeEventListener` y `dispatchEvent`)** + **`document.cookie` enganchado al tarro particionado de AstraShield** + destructuring ES6, parámetros por defecto, spread, shorthand, `Object.*`, `Array`/`String` stdlib, `fetch()`, clases ES6, template literals, `element.style.*`, `new WebSocket()`, `try`/`catch`/`finally`/`throw`, `requestAnimationFrame`, `<canvas>` 2D, `async`/`await`, closures, `Promise`, `localStorage`) y runtime multi-extensión Manifest V3 simultáneo con DNR combinado.
- **Gráficos, SVG, Chrome v4, Descargas, Tipografía, 3D, Documentos y Video/Audio (`src/gfx/`, `src/docs/`, `src/media/`, `src/browser/`)**:
  - **`AstraImage v3` (`src/gfx/image_decoder.*`)**: decodificador **GIF87a/GIF89a LZW** en C++20 puro (GCT/LCT, transparencia, entrelazado) + soporte **WebP** (`RIFF WEBP` con `dlopen("libwebp.so.7"/"libwebp.so.6")` + sonda de cabecera VP8X/VP8L/VP8) + **JPEG Baseline Sequential DCT** puro + PNG/BMP/PPM nativos.
  - **`AstraSVG v1` (`src/gfx/svg_engine.*`)**: parser y rasterizador vectorial nativo en C++20 puro para `<svg>` inline con supersampling 2x2 anti-aliased (límite 512×512).
  - **Chrome de escritorio v4 + Gestor de Descargas Nativo (`astra://downloads`)**: Omnibox editable sincronizado con `history.pushState`, historial (`<` / `>` / `R`), pestañas `[+]` / `[x]`, verificación nativa **SHA-256 (FIPS 180-4)** y registro `ledger.json`.
  - `AstraFont`: tipografía vectorial TrueType UTF-8 anti-aliased vía `dlopen("libfreetype.so.6")` (caché LRU 4 MiB).
  - **Compositor v10 (`ASD6`)**: **composición afín por píxel con muestreo inverso y supermuestreo 2x2 para `transform`**, composición alpha de `<svg>`, gradientes direccionales CSS, `border-left`, `text-align`, `<canvas>` 2D, recorte estricto `overflow: hidden`, scroll APZ, `DropShadow`, `fixed_viewport`, X11 interactivo, `Astra3D`, PDF/Markdown y `AstraMedia v4` con ALSA (`dlopen("libasound.so.2")`).

## Compilación y uso (Linux y macOS, cero dependencias de compilación)

```bash
make -j4
./build/astra --test
./build/astra --stress-tabs 150 --cap 2048
./build/astra --url https://example.com
./build/astra --ext examples/astra-privacy-guard --ext examples/astra-reader-boost --url astra://home --out home.png
./build/astra --url file://examples/astra_session9.html --out session9.png
./build/astra --url file://examples/astra_session8.html --out session8.png
./build/astra --url file://examples/astra_session7.html --out session7.png
./build/astra --url file://examples/astra_session6.html --raf-step 4 --out session6.png
./build/astra --download file://examples/astra_data.json --download https://example.com --url astra://downloads --out downloads.png
./build/astra --url astra://home --gui   # Ventana interactiva nativa X11 (Linux con DISPLAY)
```

## Publicación en Hugging Face Hub

```bash
export HF_TOKEN="<tu_token_write_de_hugging_face>"
export HF_REPO_ID="thevareoz/astra-browser-core"
bash upload_to_huggingface.sh "Release Astra Browser Core v9 (Hito A Layout + Hito B AstraJS v9 SPA + Hito C Seccomp-BPF/CSP/CORS)"
```

## Resultados verificados (fusión v10 + v8, macOS 27.2 arm64)

Esta versión fusiona dos líneas de desarrollo que partían ambas de la v7: la rama de apertura a la web real (SVG, stdlib y prototipos de JS, tablas/floats/`calc()`, GIF/WebP, Brotli, HLS, sandbox y políticas de seguridad) y la rama de portabilidad y animación (capa POSIX, `@keyframes`/`transition`/`transform` con `ASD6`, eventos DOM nivel 2 y revalidación HTTP condicional).

- **Compilación**: `g++ -std=c++20 -O2 -Wall -Wextra -pthread` en macOS 27.2 arm64 (Apple clang 21) — **0 errores, 0 warnings**.
- **Suite de verificación (`--test`)**: **68 / 68 pruebas aprobadas** (43 de la v7 + 19 de la rama de web abierta + 6 de la rama de animación y portabilidad).
- **Prueba de estrés de 150 pestañas (`--stress-tabs 150 --cap 2048`)**:
  - 150 pestañas creadas en **620.6 ms** (**4.1 ms/pestaña**).
  - **RAM total real (suma del RSS de los 3 procesos)**: **19.95 MB / 2048 MB** (`main`: 14.81 MB + `content`: 4.05 MB + `rdd-media`: 1.09 MB — ~0.97 % del techo de 2 GB).
  - Rehidratación de pestañas deshidratadas (Tab #1): **OK**.
- **Renderizado E2E**: las nueve páginas de `examples/` (sesiones 3 a 10) se renderizan sin errores.

### Dos salvedades honestas sobre esta verificación

1. **El sandbox `seccomp-BPF` no se ejerce en macOS.** Es una primitiva del kernel de Linux; `ProcessSandbox` informa `non-linux` y degrada sin bloquear. La prueba de seguridad exige el filtro donde existe y, fuera de él, que el resto de la política (CSP, Mixed Content, CORS/SOP, `SameSite`, Site Isolation) siga aplicándose — eso es lo verificado aquí. **En macOS el proceso de contenido corre sin sandbox**; falta un perfil equivalente con `sandbox_init`.
2. **`RegExp` se apoya en `std::regex`.** Los patrones llegan desde JS de páginas no confiables, y `std::regex` es conocido por desbordar la pila y por explosión exponencial ante patrones patológicos. El harness de fuzzing actual no cubre esa superficie.

## Resultados verificados (Sesión 9)

- **Compilación**: `g++ -std=c++20 -O2 -Wall -Wextra -pthread -ldl` — **0 errores, 0 warnings**.
- **Suite de verificación (`--test`)**: **55 / 55 pruebas aprobadas** (48 previas + 7 nuevas pruebas cubriendo los 3 hitos completos):
  - **Hito A (Layout web real, imágenes y formularios)**: `<table>` con `colspan`, `float: left|right` + `clear: both`, `display: inline-block`, `calc(100% - 200px)`, `:nth-child(even)`, `::before`/`::after`, `<iframe srcdoc>`, decodificador nativo GIF89a LZW con transparencia + WebP RIFF, y envío de `<form method="GET">` con codificación URL.
  - **Hito B (Profundidad de motor JS y SPAs)**: cadena `[[Prototype]]` (`Object.create`/`getPrototypeOf`), `class Sub extends Base` + `super()`, `instanceof`, `RegExp` (`/pat/flags`, `.test`, `.exec`, `replace(/g)`, `split(re)`), `Date` (ISO-8601, UTC, `.toISOString()`), `Map`/`Set` con `for..of`, y runtime SPA (`history.pushState`, `location.*`, `sessionStorage`).
  - **Hito C (Seguridad real y fuzzing)**: verificación a nivel de kernel de **`seccomp-BPF`** (`socket(AF_INET)=EPERM`, `execve()=EPERM`, `ptrace()=EPERM`), bloqueo **CSP** (`<meta>` y cabecera), **Mixed Content**, **SOP/CORS**, cookies **`SameSite=Strict|Lax|None`**, **Site Isolation** por eTLD+1, y **harness de fuzzing** sobre 49 payloads hostiles/corruptos en HTML, CSS, PNG/JPEG/GIF/WebP, SVG y HPACK (0 fallos/cuelgues) + renderizado E2E de `examples/astra_session9.html`.
- **Prueba de estrés de 150 pestañas (`--stress-tabs 150 --cap 2048`)**:
  - 150 pestañas creadas en **952.3 ms** (**6.3 ms/pestaña**).
  - **RAM total real (Linux `/proc` RSS sumando Main + Content sandboxed + RDD Media)**: **26.41 MB / 2048 MB** (`main`: 13.86 MB + `content`: 8.02 MB + `rdd-media`: 4.53 MB — ~1.29% del techo de 2 GB).
  - Rehidratación de pestañas deshidratadas (Tab #1): **OK**.



