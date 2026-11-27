# Astra Browser Core — Documento de relevo (HANDOFF)

**Fecha**: 27 de noviembre de 2026
**Rama**: `main` · **Plataforma de verificación**: macOS 27.2 arm64 (Apple clang 21) y Linux glibc
**Estado de la suite**: **82 / 82 pruebas aprobadas, 0 errores, 0 warnings**

Este documento tiene dos mitades y conviene leerlas en orden:

1. **[Lo que hay](#parte-i--lo-que-hay)** — qué está construido, medido y verificado, con los números reales.
2. **[Lo que falta para ser Chrome](#parte-ii--lo-que-falta-para-ser-un-navegador-real-al-nivel-de-chrome)** — el inventario honesto de las ausencias, por capas, con una estimación de esfuerzo.

La segunda mitad es la importante. Astra renderiza Wikipedia, Hacker News y GitHub de forma reconocible, pero **la distancia hasta Chrome no es incremental: es de dos órdenes de magnitud**. Este documento existe para que nadie se equivoque sobre eso.

---

# Parte I — Lo que hay

## 1. Qué es Astra

Un motor de navegador de escritorio escrito **desde cero en C++20 puro**: sin Chromium, sin WebKit, sin Gecko, sin Electron, sin Skia, sin V8, sin Blink. El único código de terceros que entra lo hace por `dlopen()` en tiempo de ejecución y es opcional: OpenSSL (TLS), zlib, Brotli, FreeType, libwebp, libX11, ALSA. Si falta cualquiera de ellos, el subsistema correspondiente degrada de forma explícita en vez de romper.

La arquitectura imita a Firefox (e10s / WebRender / Necko / RDD / nsPresArena) en lo esencial: **multiproceso real con `fork()`**, canal IPC binario sobre `socketpair(AF_UNIX)`, arena de memoria por pestaña, y un techo duro de **2 GB de RAM** que el `MemoryGovernor` vigila leyendo el RSS real del sistema operativo.

```
┌──────────────┐   AF_UNIX    ┌───────────────┐
│ Main Process │◄────────────►│ Content Worker│  (sandbox: seccomp-BPF / Seatbelt)
│  Chrome UI   │              │ DOM/CSS/JS/LY │
│  Net / Cache │   AF_UNIX    ├───────────────┤
│  MemGovernor │◄────────────►│  RDD Media    │  (sandbox: sin red)
└──────────────┘              └───────────────┘
```

## 2. Tamaño y forma del código

| Módulo | Líneas | Ficheros | Responsabilidad |
|---|---:|---:|---|
| `src/core/` | 1 009 | 7 | Arena `mmap`, `MemoryGovernor`, JSON RFC 8259, **capa de portabilidad POSIX** |
| `src/ipc/` | 772 | 4 | `fork()`, canal binario `AF_UNIX`, Site Isolation, `ProcessSandbox` |
| `src/privacy/` | 878 | 4 | CSP 2/3, SOP, CORS, Mixed Content, Total Cookie Protection, `SameSite` |
| `src/net/` | 3 246 | 8 | HTTP/1.1 + HTTP/2 (HPACK), WebSocket RFC 6455, TLS 1.3, caché RFC 9111, Brotli |
| `src/dom/` | 578 | 2 | Parser HTML5, DOM dinámico |
| `src/style/` | 3 345 | 4 | Cascada CSS3, `calc()`, Grid, Flexbox, `@keyframes`, `transition`, `transform` |
| `src/layout/` | 1 957 | 2 | Maquetación de bloques, línea, tabla, float, Grid, Flex |
| `src/js/` | 4 445 | 2 | Compilador a bytecode + VM de pila + GC Mark-and-Sweep |
| `src/ext/` | 290 | 2 | Runtime multi-extensión Manifest V3 |
| `src/gfx/` | 3 997 | 10 | Rasterizador, SVG, JPEG/PNG/GIF/WebP/BMP, FreeType, Astra3D |
| `src/docs/` | 372 | 2 | Visor PDF y Markdown |
| `src/media/` | 874 | 2 | Audio/vídeo, HLS, ALSA |
| `src/browser/` | 2 604 | 2 | Chrome de escritorio: Omnibox, pestañas, historial, descargas |
| `src/main.cpp` | 3 873 | 1 | Entrada, CLI y **suite de 82 pruebas** |
| **Total** | **28 240** | **52** | |

Para comparar: **Chromium ronda los 40 millones de líneas**. Astra es el 0,07 % de ese tamaño. Ese cociente es la forma más honesta de resumir todo este documento.

## 3. Resultados medidos (no estimados)

### Compilación
`g++ -std=c++20 -O2 -Wall -Wextra -pthread` → **0 errores, 0 warnings** en macOS arm64 y Linux x86-64. Las divergencias de sistema operativo están concentradas en un único encabezado, `src/core/platform.hpp` (banderas de socket, semántica de `madvise`, lectura de RSS, nombres de biblioteca para `dlopen`); no hay `#ifdef` dispersos por los módulos.

### Suite de verificación — `./build/astra --test`
**82 / 82 aprobadas.** No son pruebas de humo: cada una comprueba una propiedad concreta del motor contra la especificación (por ejemplo, que `line-height` cambie de verdad la altura maquetada del bloque, o que el sandbox devuelva `EPERM` en `connect()` mientras el canal IPC sigue vivo como control positivo).

### Estrés de 150 pestañas — `./build/astra --stress-tabs 150 --cap 2048`
```
Tabs created:       150          Total elapsed:  642.3 ms (4.3 ms/tab)
Active/Warm tabs:   8            Total OS RSS:   27.02 MB / 2048 MB
Dehydrated tabs:    142            main        15.75 MB
Rehydrate Tab #1:   OK             content      6.89 MB
                                   rdd-media    4.38 MB
```
27 MB para 150 pestañas (1,3 % del techo) es un resultado real, pero hay que leerlo con la salvedad del apartado 6: la mayoría de esas pestañas están deshidratadas y ninguna ejecuta un sitio moderno con megabytes de JavaScript.

### Sandbox verificado a nivel de kernel
`ProcessSandbox::self_verify()` engendra un hijo efímero, le aplica el perfil `ContentWorker` y comprueba en el kernel que `connect()`, `bind()` y `execve()` devuelven `EPERM` **y que el `socketpair(AF_UNIX)` del IPC sigue funcionando**. Lo segundo es tan importante como lo primero: un sandbox que rompiera el IPC dejaría al navegador sin proceso de contenido.

- **Linux**: `PR_SET_NO_NEW_PRIVS` + `setrlimit` + filtro **`seccomp-BPF`** (`SECCOMP_MODE_FILTER`).
- **macOS**: `setrlimit` + perfil **Seatbelt** compilado con `sandbox_init` (SBPL): `(deny network*)`, `(deny process-exec)`, `(deny process-fork)`.

### Fuzzing
Harness sobre **49 cargas hostiles o corruptas** en HTML, CSS, PNG, JPEG, GIF, WebP, SVG y HPACK: **0 fallos, 0 cuelgues**. Cobertura modesta, pero real.

### Web abierta
| Sitio | Resultado |
|---|---|
| `en.wikipedia.org` | Tres columnas (índice, artículo con ficha ilustrada, panel de apariencia), cabecera repartida, logotipo vectorial desde su SVG remoto, iconos de interfaz rasterizados desde sus máscaras SVG |
| `news.ycombinator.com` | Legible y reconocible, 457 display items |
| `github.com/explore` | Rejilla de tres columnas con las proporciones reales (25/50/25), avatares junto a los nombres |
| `examples/*.html` | Las nueve páginas de demostración (sesiones 3–10) renderizan sin errores |

## 4. Capacidades por capa

**Red** — HTTP/1.1 con pool Keep-Alive acotado; **HTTP/2 multi-stream real** (RFC 7540 + HPACK/Huffman RFC 7541) sobre un solo socket; TLS 1.3 con verificación estricta de certificados vía `dlopen`; WebSocket RFC 6455 (`ws://` y `wss://`); **revalidación condicional RFC 9111** (`If-None-Match` / `If-Modified-Since` → `304` servido desde caché sin retransferir el cuerpo, honrando `max-age` / `no-cache` / `must-revalidate` / `no-store`); `gzip`, `deflate` y **Brotli**; métodos REST completos con cabeceras personalizadas.

**Seguridad y privacidad** — Same-Origin Policy con clave eTLD+1; **CSP Nivel 2/3** desde cabecera y `<meta>`; bloqueo de contenido mixto activo; CORS; **Total Cookie Protection** particionada por `(top_level_site, origin)` con aplicación real de `SameSite=Strict|Lax|None` (y rechazo de `None` sin `Secure`); bloqueador Trie de rastreadores; limpieza de UTM/FBCLID; HTTPS-Only; `Sec-GPC`; **Site Isolation por eTLD+1** con proceso de contenido dedicado por origen.

**CSS** — Cascada CSS3 con variables (`--*`, `var()`), `@media`, combinador `>`, pseudo-clases (`:first-child`, `:last-child`, `:nth-child(an+b)`, `:focus`, `:hover`), pseudo-elementos `::before`/`::after`, evaluador recursivo `calc()` (`px`, `%`, `vw`, `vh`, `rem`, `em`). **Flexbox Level 1** con `flex-grow`/`flex-basis`/`flex-wrap` y reparto de espacio libre también en el camino multi-fila. **Grid Level 1** con áreas con nombre, atajo `grid-template`, `minmax()`, `min-content`, `column-gap`/`row-gap` y dimensionado de pistas por fases. **Animations Level 1** (`@keyframes` con listas de fotogramas por propiedad) y **Transitions Level 1**, con curvas exactas (`cubic-bezier()` por Newton-Raphson con respaldo de bisección, `steps()`). **Transforms Level 1** resuelto a matriz afín de **pintado** que, conforme a la especificación, no altera la maquetación. `box-sizing`, `linear-gradient()`, `position` (`relative`/`absolute`/`fixed`), `z-index`, `opacity`, `box-shadow`, `overflow: hidden`, `float`/`clear`, `inline-block`, tablas con `colspan`, `mask-image` compositada de verdad (tinte a través del canal alfa con muestreo bilineal) y `background-image`.

**JavaScript (`AstraJS v10`)** — Compilador a bytecode, VM de pila y **GC Mark-and-Sweep**. Cadena `[[Prototype]]` real (`Object.create`, `getPrototypeOf`/`setPrototypeOf`, `fn.prototype`, `class … extends` con `super()`, `instanceof`); `async`/`await`, `Promise`, closures; `RegExp`, `Date`, `Map`, `Set` con `for..of`; destructuring, spread, parámetros por defecto, shorthand, template literals; `Array`/`String`/`Object` stdlib; `fetch()`; `localStorage` persistente y `sessionStorage`; **DOM Events Level 2** completo (ruta de propagación congelada, captura/objetivo/burbujeo, `preventDefault`, `stopPropagation`, `stopImmediatePropagation`, `{capture, once}`, `dispatchEvent`); APIs de SPA (`history.pushState`/`replaceState`/`back`/`forward`, `location.*`); `document.cookie` enganchado al tarro particionado; `<canvas>` 2D con `requestAnimationFrame`; `new WebSocket()`; `try`/`catch`/`finally`/`throw`.

**Gráficos** — Rasterizador propio con composición alpha, gradientes, sombras, recorte estricto y **composición afín por píxel con muestreo inverso y supermuestreo 2×2**. **SVG** propio (parser y rasterizador vectorial con AA 2×2, respetando la exclusión de subárboles no renderizables de SVG 1.1 §5.6). Decodificadores nativos en C++20 puro de **JPEG baseline DCT**, **PNG**, **GIF87a/89a con LZW** (transparencia y entrelazado), BMP y PPM; WebP vía `dlopen`. Tipografía TrueType UTF-8 anti-aliased por FreeType con caché LRU de 4 MiB. Formato binario de display list **`ASD6`** (matriz afín por item).

**Chrome y plataforma** — Omnibox editable sincronizada con `history.pushState`, pestañas `[+]`/`[x]`, historial, **gestor de descargas nativo** (`astra://downloads`) con verificación **SHA-256 (FIPS 180-4)** y `ledger.json`, ventana interactiva nativa **X11** con scroll APZ, runtime **multi-extensión Manifest V3** simultáneo con DNR combinado, visor de PDF y Markdown, audio/vídeo con HLS y ALSA.

## 5. Historial de sesiones

| Commit | Sesión | Contenido |
|---|---|---|
| `ef28ed2` · `cae32d7` | 1–2 | Arena, IPC multiproceso, gobernador de memoria, 16 pruebas |
| `336a9be` | 3 | TrueType UTF-8, JPEG DCT puro, Grid inicial, formularios, `fetch()` |
| `d78e3fd` | 4 | Chrome v4 (Omnibox/historial/pestañas), `position`/`z-index`/`box-shadow`, TLS 1.3 ALPN, Promises, MV3 |
| `070294b` | 5 | HTTP/2 + HPACK, `async`/`await`, `@media`, descargas SHA-256 |
| `6023bcd` | 6 | Multiplexación H2, Canvas 2D + rAF, `box-sizing`, `linear-gradient` |
| `3e92b27` | 7 | WebSocket RFC 6455, variables CSS, Flexbox, clases ES6, `element.style` |
| `2614223` | 8 | **Portabilidad POSIX**, CSS Animations/Transitions/`transform` (`ASD6`), DOM Events L2, revalidación HTTP |
| `322b2e5` | 8–10 (rama paralela) | SVG, stdlib y prototipos JS, tablas/floats/`calc()`, GIF/WebP, Brotli, HLS, seccomp, CSP |
| `55d2779` | — | **Fusión de las dos ramas** |
| `0f0d94b` | — | Sandbox real en macOS (perfil Seatbelt) |
| `323b86f` … `06a2236` | — | Campaña de paridad visual contra un navegador de referencia: Grid con áreas, tipografía relativa, texto en línea, `flex-grow`, máscaras de iconos, `<defs>`, parser de declaraciones, `margin` de `<body>`, `line-height` |

La última tanda de commits se hizo con una metodología que merece quedar escrita, porque es la que funcionó: **medir, no adivinar**. Se instrumentó el motor con diagnósticos activables por variable de entorno y se comparó contra los estilos calculados de un navegador real. Varios "fallos" resultaron ser comportamiento correcto (el globo de Wikipedia es `display:none` en su hoja; los grupos de radio del panel de apariencia los construye JavaScript y no están en el HTML servido). Y varios números exactos delataron la causa de inmediato: `413 = 1240 × 25 %` y `103 = 413 × 25 %` probaron que el porcentaje se aplicaba dos veces; un ancho de exactamente 64 px delató el techo del clamp; una opacidad del 100 % delató que se estaba pintando `<defs>`.

## 6. Salvedades honestas sobre la verificación

Estas cuatro no son defectos pendientes de arreglar, son **límites de lo que las mediciones de arriba demuestran**. Conviene no confundirlos.

1. **El sandbox de macOS usa API obsoleta y es lista de denegación, no de permiso.** `sandbox_init` está deprecada desde macOS 10.8, pero sigue siendo la única vía de confinar un binario sin empaquetar ni firmar. El perfil parte de `(allow default)` y veta operaciones concretas; es confinamiento real, pero más débil que una lista blanca. Darwin tampoco tiene equivalente a `PR_SET_NO_NEW_PRIVS`.
2. **`RegExp` se apoya en `std::regex`.** Los patrones llegan desde JavaScript de páginas no confiables, y `std::regex` es conocido por desbordar la pila y por explosión exponencial ante patrones patológicos (ReDoS). El harness de fuzzing actual **no cubre esa superficie**. Es el agujero de seguridad más concreto y accionable del árbol.
3. **Las cifras de memoria se miden con pestañas deshidratadas.** De las 150, 142 están deshidratadas y 8 activas. Ninguna ejecuta un sitio moderno con megabytes de JavaScript. Los 27 MB son ciertos para esa carga, no para 150 pestañas de Gmail.
4. **"Renderiza Wikipedia" significa reconocible, no idéntico.** La estructura, la tipografía, los iconos y el logotipo salen bien. No salen: las partes de la interfaz que el sitio construye con JavaScript cargado por su `ResourceLoader`, ni los detalles que dependen de hojas de estilo inyectadas dinámicamente.

---

# Parte II — Lo que falta para ser un navegador real al nivel de Chrome

## Cómo leer esta parte

Las ausencias están agrupadas en tres niveles por **consecuencia**, no por dificultad:

- **Nivel 1 — Bloqueantes.** Sin esto, la mayoría de la web moderna no funciona, no solo se ve distinta. Un sitio que dependa de uno de estos puntos queda en blanco o roto.
- **Nivel 2 — Paridad funcional.** El sitio funciona, pero mal: lento, incompleto, inaccesible o visualmente distinto.
- **Nivel 3 — Paridad de producto.** Lo que distingue a un motor que funciona de un navegador que la gente instala.

Las estimaciones de esfuerzo están en **meses-persona de ingeniería senior** y son deliberadamente conservadoras. Suman más de lo que parece: esa es la conclusión.

---

## Nivel 1 — Bloqueantes

### 1.1 Motor de JavaScript: profundidad y velocidad

`AstraJS` es un compilador a bytecode con VM de pila y GC mark-and-sweep. Es un logro real en 4 445 líneas, y **no es comparable a V8**.

**Ausente del lenguaje:**

| Característica | Por qué bloquea |
|---|---|
| **Generadores e `iterador`/`yield`** | Base de innumerables bibliotecas; `for..of` sobre iterables propios no funciona |
| **Módulos ES (`import`/`export`)** | **El empaquetado de la web moderna entero.** Sin esto no se carga una aplicación con Vite, Webpack en modo ESM, o `<script type="module">` |
| **`Proxy` y `Reflect`** | Vue 3 entero es reactividad basada en `Proxy`. Sin esto, Vue 3 no arranca |
| **`Symbol` completo, `WeakMap`/`WeakRef`/`FinalizationRegistry`** | Patrones de privacidad e integración con el GC |
| **`BigInt`, `Atomics`, `SharedArrayBuffer`** | Criptografía, WASM multihilo |
| **Getters/setters en objetos literales, campos privados `#x`, decoradores** | Sintaxis de clase moderna |
| **`label:`/`continue label`, `switch` con fallthrough completo, coma como operador, `with`** | Esquinas que el código minificado usa de verdad |
| **Etiquetas de plantilla (`` tag`...` ``), `??=`, `||=`, `&&=`, encadenamiento opcional `?.`** | Sintaxis cotidiana desde 2020 |

**Ausente del rendimiento:** no hay **JIT**. V8 tiene Ignition (intérprete), Sparkplug (baseline), Maglev y TurboFan (optimizador), con inline caches, feedback de tipos, deoptimización y escape analysis. La diferencia de velocidad en código caliente es de **10× a 100×**. El GC es mark-and-sweep de parada total: Chrome usa Orinoco, un GC generacional, concurrente e incremental; una pausa de 200 ms en cada recolección es la diferencia entre una interfaz fluida y una que tartamudea.

**Ausente de la seguridad:** `std::regex` por `RegExp` (ver salvedad 2 de la Parte I) y ninguna mitigación tipo pointer compression, sandbox de heap o CFI.

**Esfuerzo**: completar el lenguaje, **12–18 meses-persona**. Un JIT de un solo nivel que compita con Sparkplug, **18–30 meses-persona** más. Un optimizador tipo TurboFan es un proyecto de equipo de varios años; no es realista para este árbol.

### 1.2 Web APIs ausentes

Sin estas, clases enteras de aplicación no existen:

- **Web Workers** y `MessageChannel` — cualquier aplicación que haga trabajo fuera del hilo principal.
- **Service Workers** — **el modelo entero de aplicación offline, PWAs e interceptación de red.**
- **IndexedDB** — la única base de datos del lado del cliente con capacidad real. `localStorage` no la sustituye.
- **Shadow DOM, Custom Elements, HTML Templates** — Web Components. Sin esto, bibliotecas como Lit o cualquier design system basado en componentes nativos no renderizan.
- **Fetch Streams, `ReadableStream`/`WritableStream`/`TransformStream`** — descarga progresiva, respuestas en streaming de modelos de lenguaje.
- **`WebAssembly`** — todo el software compilado que corre en la web.
- **`IntersectionObserver`, `ResizeObserver`, `MutationObserver`** — carga diferida, interfaces reactivas al tamaño, integración con frameworks.
- **WebRTC** — videollamadas.
- **WebGL / WebGPU** — gráficos acelerados, mapas, visualizaciones, juegos.
- **Web Crypto API (`crypto.subtle`)** — autenticación, cifrado en cliente.
- **Clipboard API, File System Access API, Notifications, Push, Geolocation, Web Bluetooth/USB/Serial, WebAuthn, Payment Request, Media Capture, Web Audio, Gamepad, Wake Lock, Credential Management** — el catálogo que la gente da por supuesto.
- **`structuredClone`, `BroadcastChannel`, `EventSource` (SSE), `AbortController` completo, `URLPattern`, `Intl.*`** — infraestructura cotidiana.

Solo `Intl` (formato de fechas, números, monedas, plurales y colación para cientos de locales, con ICU detrás) es un proyecto de varios meses-persona por sí solo.

**Esfuerzo**: **24–40 meses-persona** para un subconjunto que cubra la web moderna de verdad; el catálogo completo no lo tiene ningún navegador fuera de los tres grandes.

### 1.3 Composición y aceleración por GPU

Todo el pintado de Astra es **software, en CPU, sin hilos**. Chrome tiene:

- Un **proceso de GPU** aislado, con Skia sobre Vulkan, Metal o D3D.
- **Composición en hilo** (`cc`): el desplazamiento y las animaciones de `transform`/`opacity` corren en un hilo del compositor y **no bloquean** ni JavaScript ni la maquetación. Esto es lo que hace que una página siga respondiendo al scroll mientras el hilo principal está ocupado.
- **Capas de composición** promovidas por `will-change`, `transform: translateZ(0)`, vídeo, canvas.
- **Rasterizado en tiles**, invalidación parcial y **tiles de desplazamiento especulativo**.
- Pintado y rasterizado **fuera del hilo principal**.

Sin esto, el desplazamiento de una página compleja es una recomposición completa en CPU. Es la diferencia perceptual más grande de toda esta lista: se nota en el primer segundo de uso.

**Esfuerzo**: **18–30 meses-persona**, y obliga a reescribir el compositor y el modelo de invalidación.

### 1.4 Maquetación: lo que falta del modelo de caja

El motor hace bloque, línea, Flexbox, Grid y tabla. Falta:

- **Dimensionado intrínseco real** (`min-content`, `max-content`, `fit-content` como ciudadanos de primera clase en todo el motor, no casos especiales).
- **Modos de escritura** (`writing-mode: vertical-rl`, `direction: rtl`) y **bidi (UAX #9)**. Sin bidi, el árabe y el hebreo **se renderizan al revés**. No es un detalle estético: deja fuera a cientos de millones de personas.
- **Conformado de texto complejo (HarfBuzz)**: ligaduras, kerning contextual, marcas combinantes, escrituras índicas y árabes donde la forma del glifo depende del contexto. Sin shaping, el devanagari y el árabe son ilegibles.
- **Posicionamiento sticky** (`position: sticky`), **contención** (`contain`), **consultas de contenedor** (`@container`), **`aspect-ratio`**, **multi-columna**, **saltos de página/región**.
- **Flexbox y Grid completos**: `align-content`, `place-items`, `order`, líneas de rejilla implícitas, `grid-auto-flow: dense`, subgrid.
- **Contextos de apilamiento completos**, `mix-blend-mode`, `filter`, `backdrop-filter`, `clip-path`, `mask` completo.
- **`overflow: scroll` con barras reales**, scroll anidado, `scroll-snap`, `overscroll-behavior`.
- **Reflow incremental**: Astra remaqueta el documento. Chrome invalida subárboles. En una página grande, la diferencia es de milisegundos a cientos de milisegundos por cada cambio de estilo.

**Esfuerzo**: **18–30 meses-persona**. Solo bidi + shaping son 6–10.

---

## Nivel 2 — Paridad funcional

### 2.1 Rendimiento

El dato más revelador de toda la campaña de pruebas: **`github.com` tardó unos 45 segundos y consumió unos 492 MB**. Chrome hace la misma página en **menos de 2 segundos**. Un factor de ~25× en tiempo y de ~10× en memoria sobre una sola página.

Las causas son estructurales, no micro-optimizaciones pendientes:
- Remaquetación completa en vez de invalidación incremental.
- Pintado en CPU sin tiles ni hilos.
- Sin JIT en el JavaScript de la página.
- Sin caché de estilo calculado entre fotogramas.
- Sin parser HTML especulativo ni precarga (`preload scanner`).

Chrome además tiene: precarga y precalentamiento de conexiones, **Back/Forward Cache**, prerenderizado, división de código del propio navegador, compilación perezosa de scripts, caché de bytecode en disco. Nada de eso existe aquí.

**Esfuerzo**: **12–24 meses-persona**, entrelazado con 1.1 y 1.3.

### 2.2 Red

- **HTTP/3 y QUIC** — ausente. Una parte creciente de la web lo negocia por defecto.
- **DNS**: no hay resolutor propio, ni **DNS sobre HTTPS/TLS**, ni caché de DNS, ni Happy Eyeballs (RFC 8305) para IPv4/IPv6.
- **TLS**: funciona vía OpenSSL por `dlopen`, pero sin **grapado OCSP**, sin **Certificate Transparency**, sin **HSTS preload list**, sin fijación de claves, sin reanudación de sesión ni **0-RTT**.
- **Caché**: la caché RFC 9111 es correcta pero simple. Falta caché de disco con desalojo por presión, **caché particionada por sitio** (si no, es un vector de seguimiento), `Vary` completo, y rangos (`Range`) para reanudar descargas.
- **Proxies**, PAC, autenticación de proxy, cliente SOCKS: ausentes.
- **Cookies**: falta el **Public Suffix List real** (ahora es una heurística eTLD+1), atributos `Partitioned`/CHIPS, y caducidad con poda por presión.

**Esfuerzo**: **12–18 meses-persona**. QUIC + HTTP/3 solo, 6–9.

### 2.3 Multimedia

- **Vídeo**: no hay decodificación de **H.264, H.265, VP8, VP9 ni AV1**. Sin eso no hay YouTube, ni Netflix, ni un `<video>` cualquiera de la web.
- **Audio**: **AAC, Opus, Vorbis, MP3** ausentes. En macOS no hay salida de audio en absoluto: falta el respaldo **CoreAudio** (solo hay ALSA, es decir, Linux).
- **Imágenes**: falta **AVIF** y **JPEG XL**; el PNG no soporta APNG; el JPEG es solo baseline (sin progresivo).
- **Media Source Extensions (MSE)** y **Encrypted Media Extensions (EME/Widevine)**: ausentes. Sin MSE no hay streaming adaptativo real; sin EME no hay contenido con DRM — y Widevine no se puede implementar, se licencia.
- **Web Audio API**, captura de cámara y micrófono, **Picture-in-Picture**, subtítulos **WebVTT**: ausentes.

**Esfuerzo**: **12–20 meses-persona** apoyándose en decodificadores de terceros (FFmpeg/libvpx/dav1d). Escribirlos desde cero no es realista y tampoco es deseable.

### 2.4 Accesibilidad

**Esta es la ausencia más grave que no se ve en una captura de pantalla.**

No hay **árbol de accesibilidad**. Eso significa: ningún lector de pantalla puede usar Astra. Ni VoiceOver, ni NVDA, ni JAWS, ni Orca. Falta todo el aparato:

- Árbol de accesibilidad derivado del DOM con cálculo de rol, nombre y descripción accesibles (AccName).
- Puentes a las API de plataforma: **IAccessible2/UIA** en Windows, **NSAccessibility** en macOS, **AT-SPI2** en Linux.
- **ARIA** completo (roles, estados, propiedades, regiones vivas).
- Navegación completa por teclado con orden de foco, trampas de foco y anillo de foco visible.
- Zoom de página y de texto, modo de alto contraste, respeto a `prefers-reduced-motion` y `prefers-contrast`, inversión inteligente de colores.
- Modo caret browsing, búsqueda en página.

En muchas jurisdicciones esto además es un requisito legal, no una opción.

**Esfuerzo**: **9–15 meses-persona**.

### 2.5 Seguridad

Lo que hay (CSP, SOP, CORS, mixed content, cookies particionadas, site isolation, sandbox) es un cimiento respetable. Lo que falta para ser defendible:

- **Sandbox de lista de permiso**, no de denegación, en ambas plataformas. En macOS, además, migrar de `sandbox_init` (obsoleta) a un App Sandbox con entitlements, lo que exige empaquetar y firmar.
- **Site Isolation completa**: un proceso por origen con **Cross-Origin Read Blocking**, políticas COOP/COEP/CORP y aislamiento de recursos. Lo que hay es isolation por eTLD+1, que es más grueso.
- **Sandbox de GPU y de red** en procesos propios (hoy no existen esos procesos).
- **Sustituir `std::regex`** por un motor de expresiones regulares con límites de retroceso y tiempo. Es la tarea de seguridad más urgente y más acotada del árbol.
- **Fuzzing continuo** de todos los parsers escritos a mano (HTML, CSS, JPEG, PNG, GIF, SVG, HPACK, HTTP/2, WebSocket) con cobertura guiada (libFuzzer/AFL++) e integración tipo ClusterFuzz. Un harness de 49 payloads es un comienzo, no una red.
- **Mitigaciones de memoria**: CFI, Control Flow Guard, asignador endurecido, pointer authentication en arm64, MiraclePtr o equivalente. El árbol es C++ con punteros crudos; los errores de memoria son la clase de vulnerabilidad dominante en navegadores.
- **Safe Browsing** o equivalente, aviso de descargas peligrosas, indicadores de seguridad en la interfaz, bloqueo de descargas de contenido mixto.
- **Actualizaciones automáticas firmadas**. Sin esto, un navegador con una vulnerabilidad conocida se queda vulnerable para siempre. Para un navegador que se distribuya de verdad, es tan obligatorio como el sandbox.
- **Gestor de contraseñas** con almacenamiento cifrado por el llavero del sistema.

**Esfuerzo**: **18–30 meses-persona**, y una parte es proceso continuo, no un hito que se cierra.

---

## Nivel 3 — Paridad de producto

### 3.1 Plataforma y ventanas

Hoy la ventana interactiva es **X11 por `dlopen`**: eso es Linux, y además la pila heredada. Falta:

- **Windowing nativo**: **Cocoa/AppKit** en macOS, **Win32/Direct3D** en Windows, **Wayland** en Linux moderno.
- **Pantallas HiDPI** con factores de escala fraccionarios y por monitor.
- **IME** (métodos de entrada) para chino, japonés y coreano. Sin IME, esos idiomas no se pueden escribir.
- Integración de escritorio: arrastrar y soltar, portapapeles del sistema con varios formatos, menús nativos, pestañas arrastrables entre ventanas, impresión con diálogo nativo y vista previa, modo pantalla completa, atajos de teclado por plataforma, asociaciones de archivo y de protocolo, navegador por defecto.
- **Empaquetado y firma**: `.app` notarizado en macOS, instalador firmado en Windows, `.deb`/`.rpm`/Flatpak/Snap en Linux.

**Esfuerzo**: **12–20 meses-persona** por las tres plataformas.

### 3.2 Funciones de navegador que la gente espera

Autocompletado de formularios y direcciones, autocompletado de la Omnibox con sugerencias y búsqueda, **sincronización** de pestañas/marcadores/historial entre dispositivos, perfiles y **navegación privada** real, grupos de pestañas, pestañas fijadas, árbol de historial, gestor de marcadores, **DevTools** (inspector, consola, red, rendimiento, depurador de JavaScript — por sí solo es un producto), traducción de páginas, modo lector, captura de pantalla de página completa, lectura en voz alta, gestión de permisos por sitio, **API completa de WebExtensions** (lo que hay cubre un subconjunto de MV3), **Back/Forward Cache**, restauración de sesión tras un cierre inesperado, informes de fallo, telemetría con consentimiento.

**Esfuerzo**: **24–40 meses-persona**. DevTools solo, 9–15.

### 3.3 Conformidad y pruebas

Chrome corre **millones** de pruebas: Web Platform Tests, Blink layout tests, pruebas unitarias, de rendimiento, de referencia visual, de fuzzing continuo. Astra tiene **82 pruebas escritas a mano**, que son buenas pero cubren lo que se construyó, no lo que la web exige.

Lo que haría medible el progreso:
- **Integrar Web Platform Tests** y publicar el porcentaje de aprobación. Es la única métrica honesta y comparable que existe. Hoy no sabemos si Astra pasa el 2 % o el 8 % de WPT, y **ese número debería ser el primer indicador del proyecto**.
- Pruebas de referencia visual contra capturas de navegadores reales, automatizadas.
- Pruebas de rendimiento con regresión detectada (Speedometer, JetStream, MotionMark).
- Fuzzing continuo en integración continua.
- Matriz de plataformas en CI (macOS arm64/x86-64, Linux, Windows).

**Esfuerzo**: **6–12 meses-persona** para la infraestructura; después es continuo.

---

## Suma y conclusión

| Nivel | Área | Meses-persona |
|---|---|---:|
| 1 | Motor JS: lenguaje completo + JIT de un nivel | 30–48 |
| 1 | Web APIs (Workers, Service Worker, IndexedDB, Shadow DOM, WASM, observers…) | 24–40 |
| 1 | Composición en GPU y en hilo | 18–30 |
| 1 | Maquetación: bidi, shaping, intrínseco, reflow incremental | 18–30 |
| 2 | Rendimiento estructural | 12–24 |
| 2 | Red: HTTP/3, DNS, TLS completo, caché | 12–18 |
| 2 | Multimedia: códecs, MSE/EME, CoreAudio, AVIF | 12–20 |
| 2 | Accesibilidad | 9–15 |
| 2 | Seguridad defendible | 18–30 |
| 3 | Plataforma: Cocoa/Win32/Wayland, HiDPI, IME, empaquetado | 12–20 |
| 3 | Funciones de producto, incluido DevTools | 24–40 |
| 3 | Conformidad y pruebas (WPT) | 6–12 |
| | **Total** | **195–327** |

**Entre 16 y 27 años-persona.** Con un equipo de diez ingenieros senior dedicados, entre **2 y 3 años**. Para contexto: Chrome tiene del orden de **2 000 ingenieros**, 17 años de historia y un linaje de código que arranca en KHTML en 1998.

**Lo que esto no quiere decir.** Astra no es un juguete. 28 240 líneas que renderizan Wikipedia, Hacker News y GitHub de forma reconocible, con multiproceso real, sandbox verificado a nivel de kernel, HTTP/2 multiplexado, TLS 1.3, un motor de JavaScript propio con GC, rasterizador y SVG propios, 82 pruebas verdes y 27 MB para 150 pestañas, es un trabajo de ingeniería serio y un recurso educativo poco común: casi nadie puede leer un navegador entero en un fin de semana.

**Lo que sí quiere decir.** "Navegador funcional" y "navegador al nivel de Chrome" son dos cosas distintas, separadas por dos órdenes de magnitud. Astra está en la primera categoría para una parte de la web y no está en la segunda, y ningún atajo cambia eso.

## Si hay que elegir qué hacer a continuación

En este orden, porque es el orden en que más desbloquean por unidad de trabajo:

1. **Integrar Web Platform Tests y publicar el porcentaje.** Barato, y sin él se está trabajando a ciegas. Debería ser la próxima tarea del proyecto, antes que cualquier característica.
2. **Sustituir `std::regex`.** Acotado, y cierra el agujero de seguridad más concreto del árbol.
3. **Módulos ES (`import`/`export`).** Es la llave de la web empaquetada moderna; sin ellos, el techo de sitios que pueden funcionar está puesto en 2015.
4. **Respaldo CoreAudio.** Pequeño, y hoy macOS — la plataforma donde se está desarrollando — no tiene sonido.
5. **Dimensionado intrínseco de primera clase.** Es la raíz de la mayoría de las diferencias visuales que quedaron en la campaña de paridad.
6. **Composición en hilo.** La inversión grande con el mayor efecto perceptible: es lo que convierte el desplazamiento de tartamudeo en fluidez.

---

## Cómo compilar y ejecutar

```bash
make -j8                                  # cero dependencias de compilación
./build/astra --test                       # 82 pruebas
./build/astra --stress-tabs 150 --cap 2048 # estrés de memoria
./build/astra --url https://example.com
./build/astra --url file://examples/astra_session9.html --out session9.png
./build/astra --url astra://home --gui     # ventana X11 interactiva (Linux con DISPLAY)
```

## Mapa del árbol

```
src/core/      arena mmap, gobernador de memoria, JSON, platform.hpp (portabilidad)
src/ipc/       fork(), canal AF_UNIX, site isolation, ProcessSandbox
src/privacy/   CSP, SOP, CORS, mixed content, cookies particionadas, shield
src/net/       HTTP/1.1, HTTP/2+HPACK, WebSocket, TLS 1.3, caché RFC 9111, Brotli
src/dom/       parser HTML5, DOM dinámico
src/style/     cascada CSS3, calc(), Grid, Flexbox, animaciones, transiciones
src/layout/    bloque, línea, tabla, float, Grid, Flex
src/js/        AstraJS: compilador a bytecode, VM de pila, GC mark-and-sweep
src/ext/       runtime multi-extensión Manifest V3
src/gfx/       rasterizador, SVG, JPEG/PNG/GIF/WebP/BMP, FreeType, Astra3D
src/docs/      visor PDF y Markdown
src/media/     audio/vídeo, HLS, ALSA
src/browser/   Omnibox, pestañas, historial, descargas SHA-256
src/main.cpp   CLI y suite de 82 pruebas
docs/          ARCHITECTURE.md
examples/      nueve páginas de demostración y dos extensiones MV3
```

## Licencia

MPL-2.0.
