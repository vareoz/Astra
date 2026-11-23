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
- **Motor Web (`src/dom/`, `src/style/`, `src/layout/`)**: Parser HTML5, cascada CSS3 con Flexbox, Display List binario serializable (`ASD2`) y regiones interactivas (`HitRegion`).
- **JavaScript y Extensiones (`src/js/`, `src/ext/`)**: `AstraJS v2` (compilador a bytecode + VM de pila + GC Mark-and-Sweep + Event Loop de microtasks/timers + eventos DOM) y runtime de WebExtensions Manifest V3 con Declarative Net Request (DNR).
- **Gráficos, 3D, Documentos y Video (`src/gfx/`, `src/docs/`, `src/media/`)**: Compositor ARGB32 con scroll APZ y ventana X11 interactiva, decodificador nativo PNG/BMP/PPM, motor 3D propio `Astra3D` (Z-buffer, normales suaves, shaders procedurales, `.obj`), visor nativo PDF (`FlateDecode`) + Markdown, y decodificador de video real MP4/Y4M en proceso RDD.

## Compilación y uso (Linux, cero dependencias de compilación)

```bash
make -j4
./build/astra --test
./build/astra --stress-tabs 150 --cap 2048
./build/astra --ext examples/astra-privacy-guard --url astra://home --out home.png
./build/astra --url file://examples/astra_interactive.html --click 300,245 --out after_click.png
./build/astra --url astra://3d --wireframe --out studio_3d.png
./build/astra --url astra://home --gui   # Ventana interactiva nativa X11 (Linux con DISPLAY)
```

## Resultados verificados (Sesión 2)

- **Compilación**: `g++ -std=c++20 -O2 -Wall -Wextra -pthread -ldl` — **0 errores, 0 warnings**.
- **Suite de verificación (`--test`)**: **16 / 16 pruebas aprobadas** (SlabArena `munmap`/`madvise`, JSON RFC 8259, AstraShield + Total Cookie Protection, `HttpCache` RAM+Disco con evicción LRU, decodificador nativo PNG, DisplayList binario `ASD2` + `HitRegions`, `AstraJS v2` Bytecode VM + GC Mark-and-Sweep + Event Loop `Microtask -> Timer` + eventos DOM `click`, `Astra3D v2` shaders suaves Blinn-Phong, visor nativo PDF, `AstraMedia v2` MP4 H.264 multiframe acotado a 16 MiB, IPC multiproceso `Main + Content + RDD`, verificación estricta TLS con CA del sistema + cache hit).
- **Prueba de estrés de 150 pestañas (`--stress-tabs 150 --cap 2048`)**:
  - 150 pestañas creadas en **367.4 ms** (**2.4 ms/pestaña**).
  - **RAM total real (Linux `/proc` RSS sumando los 3 procesos)**: **14.58 MB / 2048 MB** (`main`: 10.94 MB, `content`: 2.51 MB, `rdd-media`: 1.13 MB).
  - Rehidratación de pestañas deshidratadas (Tab #1): **OK**.

