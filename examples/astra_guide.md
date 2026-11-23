# Astra Browser — Guia de Arquitectura Nativa

> Motor de navegador de escritorio construido desde cero en C++20, inspirado en la arquitectura multiproceso de Mozilla Firefox, sin Electron y con techo estricto de 2 GB de RAM.

## Pilares del Proyecto

- Separacion multiproceso real con fork() y sockets binarios AF_UNIX (Main + Content + RDD Media).
- Asignador SlabArena por pestana inspirado en nsPresArena con devolucion instantanea de paginas al kernel Linux via madvise(MADV_DONTNEED) y munmap.
- Gobernador de memoria con lectura real de RSS en /proc/<pid>/statm y deshidratacion automatica LRU de pestanas en segundo plano.
- Privacidad AstraShield: Total Cookie Protection particionada por (sitio_raiz, origen), bloqueador Trie de rastreadores, limpieza de parametros UTM/FBCLID y modo HTTPS-Only.
- Runtime de WebExtensions Manifest V3 con reglas Declarative Net Request (DNR), inyeccion de content scripts y almacenamiento aislado.
- Motor 3D nativo Astra3D, visor nativo PDF/Markdown AstraDocs y decodificador de video real AstraMedia.

```cpp
// Ejemplo del pipeline de renderizado multiproceso de Astra:
// Content Process -> SlabArena DOM/CSS/Layout -> Binary DisplayList (IPC) -> Compositor
```
