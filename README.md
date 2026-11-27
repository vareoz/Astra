# Astra Browser Core

**Un navegador de escritorio construido desde cero en C++20 puro.** Sin Chromium. Sin WebKit. Sin Gecko. Sin Electron. Sin V8, sin Blink, sin Skia.

28 240 líneas de C++20 en 52 ficheros que arrancan tres procesos, hablan HTTP/2 sobre TLS 1.3, parsean HTML y CSS, compilan JavaScript a bytecode, lo ejecutan en una VM de pila con recolector de basura, maquetan el resultado con Flexbox y Grid, lo rasterizan en software con anti-aliasing y lo pintan en una ventana — y renderizan Wikipedia, Hacker News y GitHub de forma reconocible.

```
82 / 82 pruebas aprobadas   ·   0 warnings   ·   150 pestañas en 642 ms con 27 MB de RSS
```

> ### 📄 Lee [**HANDOFF.md**](HANDOFF.md) primero
>
> Contiene el inventario completo de lo que está construido **y, sobre todo, el inventario honesto de lo que falta para llegar al nivel de Chrome**: por capas, con las ausencias concretas y una estimación de esfuerzo que suma entre 16 y 27 años-persona. Si solo vas a leer un documento de este repositorio, que sea ese.

---

## Arquitectura

```
┌──────────────┐   AF_UNIX    ┌───────────────┐
│ Main Process │◄────────────►│ Content Worker│  sandbox: seccomp-BPF (Linux)
│  Chrome UI   │              │ DOM/CSS/JS/LY │           Seatbelt (macOS)
│  Net / Cache │   AF_UNIX    ├───────────────┤
│  MemGovernor │◄────────────►│  RDD Media    │  sandbox: sin red
└──────────────┘              └───────────────┘
```

Multiproceso real con `fork()` y canal IPC binario sobre `socketpair(AF_UNIX)`, inspirado en la arquitectura e10s / WebRender / Necko / RDD de Firefox. Arena de memoria `mmap` por pestaña con devolución inmediata de páginas al kernel, deshidratación LRU y un techo duro de 2 GB que el `MemoryGovernor` vigila leyendo el RSS **real** del sistema operativo.

El árbol es portable: todas las divergencias de sistema operativo viven en un único encabezado, [`src/core/platform.hpp`](src/core/platform.hpp). Las mismas fuentes compilan en **Linux (glibc)** y **macOS/Darwin** sin `#ifdef` dispersos.

## Compilación y uso

Cero dependencias de compilación. Las bibliotecas opcionales (OpenSSL, zlib, Brotli, FreeType, libwebp, libX11, ALSA) se cargan con `dlopen()` en tiempo de ejecución; si falta alguna, ese subsistema degrada de forma explícita.

```bash
make -j8
```

```bash
./build/astra --test                        # suite de 82 pruebas
```

```bash
./build/astra --stress-tabs 150 --cap 2048  # estrés de memoria
```

```bash
./build/astra --url https://en.wikipedia.org/wiki/Web_browser --out wiki.png
```

```bash
./build/astra --url astra://home --gui      # ventana X11 interactiva (Linux con DISPLAY)
```

## Módulos

| Módulo | Líneas | Qué hace |
|---|---:|---|
| [`src/core/`](src/core) | 1 009 | Arena `mmap`, gobernador de memoria, JSON RFC 8259, portabilidad POSIX |
| [`src/ipc/`](src/ipc) | 772 | `fork()`, canal `AF_UNIX`, site isolation, `ProcessSandbox` |
| [`src/privacy/`](src/privacy) | 878 | CSP 2/3, SOP, CORS, mixed content, cookies particionadas |
| [`src/net/`](src/net) | 3 246 | HTTP/1.1, HTTP/2 + HPACK, WebSocket RFC 6455, TLS 1.3, caché RFC 9111, Brotli |
| [`src/dom/`](src/dom) | 578 | Parser HTML5, DOM dinámico |
| [`src/style/`](src/style) | 3 345 | Cascada CSS3, `calc()`, Grid, Flexbox, `@keyframes`, `transition`, `transform` |
| [`src/layout/`](src/layout) | 1 957 | Bloque, línea, tabla, float, Grid, Flex |
| [`src/js/`](src/js) | 4 445 | `AstraJS`: compilador a bytecode, VM de pila, GC mark-and-sweep |
| [`src/ext/`](src/ext) | 290 | Runtime multi-extensión Manifest V3 |
| [`src/gfx/`](src/gfx) | 3 997 | Rasterizador, SVG, JPEG/PNG/GIF/WebP/BMP, FreeType, Astra3D |
| [`src/docs/`](src/docs) | 372 | Visor PDF y Markdown |
| [`src/media/`](src/media) | 874 | Audio/vídeo, HLS, ALSA |
| [`src/browser/`](src/browser) | 2 604 | Omnibox, pestañas, historial, descargas con SHA-256 |
| [`src/main.cpp`](src/main.cpp) | 3 873 | CLI y suite de verificación |

## Qué sabe hacer

**Red** — HTTP/2 multi-stream real (RFC 7540 + HPACK/Huffman RFC 7541) sobre un solo socket TLS 1.3 con ALPN y verificación estricta de certificados; HTTP/1.1 con pool Keep-Alive; WebSocket RFC 6455 (`ws://` y `wss://`); revalidación condicional RFC 9111 (`304` servido desde caché sin retransferir el cuerpo); `gzip`, `deflate`, Brotli; REST completo con cabeceras personalizadas.

**Seguridad y privacidad** — CSP Nivel 2/3 desde cabecera y `<meta>`; Same-Origin Policy; CORS; bloqueo de contenido mixto activo; Total Cookie Protection particionada por `(top_level_site, origin)` con `SameSite=Strict|Lax|None` aplicado de verdad; bloqueador Trie de rastreadores; limpieza de UTM/FBCLID; HTTPS-Only; Site Isolation por eTLD+1; y un **sandbox verificado a nivel de kernel** (`seccomp-BPF` en Linux, perfil Seatbelt en macOS) que comprueba `connect()=EPERM`, `bind()=EPERM`, `execve()=EPERM` **y que el canal IPC sigue vivo** como control positivo.

**CSS** — Flexbox Level 1, Grid Level 1 con áreas con nombre y `minmax()`, Animations Level 1 con `@keyframes` por propiedad, Transitions Level 1, Transforms Level 1 como matriz afín de pintado (que, conforme a la especificación, no altera la maquetación), `cubic-bezier()` por Newton-Raphson, variables CSS, `calc()` recursivo, `@media`, pseudo-clases y pseudo-elementos, `float`/`clear`, `inline-block`, tablas con `colspan`, `position`, `z-index`, `opacity`, `box-shadow`, `overflow: hidden`, `mask-image` compositada de verdad y `background-image`.

**JavaScript (`AstraJS v10`)** — Compilador a bytecode, VM de pila, GC mark-and-sweep. Cadena `[[Prototype]]` real con `class … extends` y `super()`; `async`/`await`, `Promise`, closures; `RegExp`, `Date`, `Map`, `Set` con `for..of`; destructuring, spread, template literals; DOM Events Level 2 completo (ruta de propagación congelada, captura/objetivo/burbujeo, `stopImmediatePropagation`, `{capture, once}`); `fetch()`; `localStorage` y `sessionStorage`; `history.pushState`; `<canvas>` 2D con `requestAnimationFrame`.

**Gráficos** — Rasterizador propio con composición alpha, gradientes, sombras y composición afín por píxel con muestreo inverso y supermuestreo 2×2. Parser y rasterizador **SVG** propios. Decodificadores nativos en C++20 puro de **JPEG baseline DCT**, **PNG**, **GIF87a/89a con LZW**, BMP y PPM. Tipografía TrueType UTF-8 anti-aliased con caché LRU.

**Chrome de escritorio** — Omnibox editable, pestañas, historial, gestor de descargas nativo (`astra://downloads`) con verificación SHA-256 (FIPS 180-4), ventana X11 interactiva con scroll APZ, extensiones Manifest V3, visor de PDF y Markdown.

## Resultados medidos

```
$ ./build/astra --test
Summary: 82 passed, 0 failed.

$ ./build/astra --stress-tabs 150 --cap 2048
  Tabs created:       150          Total elapsed:  642.3 ms (4.3 ms/tab)
  Active/Warm tabs:   8            Total OS RSS:   27.02 MB / 2048 MB
  Dehydrated tabs:    142            main        15.75 MB
  Rehydrate Tab #1:   OK             content      6.89 MB
                                     rdd-media    4.38 MB
```

| Sitio | Resultado |
|---|---|
| `en.wikipedia.org` | Tres columnas (índice, artículo con ficha ilustrada, panel de apariencia), logotipo desde su SVG remoto, iconos desde sus máscaras SVG |
| `news.ycombinator.com` | Legible y reconocible, 457 display items |
| `github.com/explore` | Rejilla de tres columnas con las proporciones reales (25/50/25) |

Fuzzing: 49 cargas hostiles o corruptas en HTML, CSS, PNG, JPEG, GIF, WebP, SVG y HPACK → 0 fallos, 0 cuelgues.

## Qué NO sabe hacer

Para que nadie se lleve a engaño, los titulares — la lista completa y razonada está en [HANDOFF.md](HANDOFF.md#parte-ii--lo-que-falta-para-ser-un-navegador-real-al-nivel-de-chrome):

- **Sin JIT.** El JavaScript de una página corre entre 10× y 100× más lento que en V8.
- **Sin módulos ES, generadores ni `Proxy`.** Vue 3 no arranca; nada empaquetado como ESM carga.
- **Sin Web Workers, Service Workers, IndexedDB, Shadow DOM ni WebAssembly.**
- **Sin GPU ni composición en hilo.** Todo el pintado es software, en CPU, en el hilo principal.
- **Sin bidi ni conformado de texto complejo.** El árabe y el hebreo se renderizan al revés; el devanagari es ilegible.
- **Sin árbol de accesibilidad.** Ningún lector de pantalla puede usar Astra.
- **Sin códecs de vídeo ni audio.** No hay H.264, VP9, AV1, AAC ni Opus. En macOS no hay salida de audio en absoluto.
- **Sin HTTP/3.**
- **Rendimiento estructural:** `github.com` tarda unos 45 s y consume unos 492 MB; Chrome hace lo mismo en menos de 2 s.
- **`RegExp` se apoya en `std::regex`**, que es vulnerable a ReDoS con patrones de páginas no confiables. Es el agujero de seguridad más concreto del árbol.

Chromium ronda los 40 millones de líneas. Astra es el 0,07 % de ese tamaño, y ese cociente es la forma más honesta de resumir la distancia.

## Documentación

- [**HANDOFF.md**](HANDOFF.md) — estado completo y hoja de ruta hacia la paridad con Chrome
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — diseño interno
- [`examples/`](examples) — nueve páginas de demostración y dos extensiones MV3

## Licencia

[MPL-2.0](https://www.mozilla.org/en-US/MPL/2.0/)
