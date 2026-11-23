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
```
