#pragma once

// =============================================================================
// AstraPlatform — POSIX portability layer (Session 8)
//
// Astra Browser Core nació como motor nativo para Linux (e10s + /proc + ALSA +
// X11 + dlopen de .so). Esta capa concentra TODAS las divergencias de sistema
// operativo en un único encabezado para que el mismo árbol de código compile y
// pase la suite completa de verificación tanto en Linux (glibc) como en
// macOS/Darwin (arm64 y x86_64), sin #ifdef dispersos por los 22 módulos.
//
// Cubre:
//   - Banderas de socket ausentes en Darwin (SOCK_CLOEXEC, MSG_NOSIGNAL).
//   - Semántica de devolución de páginas al kernel (MADV_DONTNEED vs MADV_FREE).
//   - Lectura real de RSS/VSIZE por proceso (/proc/<pid>/statm vs libproc).
//   - Nombres de bibliotecas dinámicas para dlopen (.so.N vs .N.dylib).
// =============================================================================

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

#if defined(__APPLE__)
#include <fcntl.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#endif

// -----------------------------------------------------------------------------
// Banderas de socket
// -----------------------------------------------------------------------------
// Darwin no define SOCK_CLOEXEC ni MSG_NOSIGNAL: el close-on-exec se aplica con
// fcntl(FD_CLOEXEC) y la supresión de SIGPIPE con setsockopt(SO_NOSIGPIPE).
#if !defined(SOCK_CLOEXEC)
#define ASTRA_SOCK_CLOEXEC 0
#else
#define ASTRA_SOCK_CLOEXEC SOCK_CLOEXEC
#endif

#if !defined(MSG_NOSIGNAL)
#define ASTRA_MSG_NOSIGNAL 0
#else
#define ASTRA_MSG_NOSIGNAL MSG_NOSIGNAL
#endif

// Devolución inmediata de páginas anónimas al kernel. En Linux MADV_DONTNEED
// descarta las páginas al instante (el RSS cae en la siguiente lectura de
// /proc); en Darwin ese flag no libera memoria anónima, el equivalente real es
// MADV_FREE (el kernel reclama bajo presión y el RSS deja de contabilizarlas).
#if defined(__APPLE__)
#define ASTRA_MADV_DONTNEED MADV_FREE
#else
#define ASTRA_MADV_DONTNEED MADV_DONTNEED
#endif

namespace astra::platform {

// Nombre legible de la plataforma de compilación (se imprime en la suite).
inline const char* os_name() {
#if defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "posix";
#endif
}

// Aplica close-on-exec y supresión de SIGPIPE a un descriptor de socket recién
// creado, de forma equivalente en ambas plataformas.
inline void harden_socket_fd(int fd) {
    if (fd < 0) return;
#if defined(__APPLE__)
    int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    int on = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

// Devuelve páginas anónimas al kernel con la semántica nativa de cada sistema.
inline void release_pages(void* base, size_t length) {
    if (!base || length == 0) return;
    (void)::madvise(base, length, ASTRA_MADV_DONTNEED);
}

// -----------------------------------------------------------------------------
// RSS / VSIZE reales por proceso
// -----------------------------------------------------------------------------
// Linux: /proc/<pid>/statm (páginas: size, resident, ...).
// Darwin: proc_pid_rusage(RUSAGE_INFO_V2).ri_resident_size y
//         proc_pidinfo(PROC_PIDTASKINFO).pti_virtual_size, ambos accesibles
//         para procesos del mismo usuario sin task_for_pid privilegiado.
struct ProcessMemory {
    uint64_t rss_bytes = 0;
    uint64_t vsize_bytes = 0;
    bool valid = false;
};

inline ProcessMemory query_process_memory(pid_t pid) {
    ProcessMemory out;
#if defined(__APPLE__)
    rusage_info_current ri{};
    if (::proc_pid_rusage(pid, RUSAGE_INFO_CURRENT, reinterpret_cast<rusage_info_t*>(&ri)) == 0) {
        out.rss_bytes = static_cast<uint64_t>(ri.ri_resident_size);
        out.valid = out.rss_bytes > 0;
    }
    struct proc_taskinfo pti{};
    int n = ::proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &pti, sizeof(pti));
    if (n == static_cast<int>(sizeof(pti))) {
        out.vsize_bytes = static_cast<uint64_t>(pti.pti_virtual_size);
        if (out.rss_bytes == 0) {
            out.rss_bytes = static_cast<uint64_t>(pti.pti_resident_size);
            out.valid = out.rss_bytes > 0;
        }
    }
#else
    static const uint64_t page_size = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
    // Lectura directa sin <fstream> para no depender de iostreams aquí.
    std::string path = "/proc/" + std::to_string(static_cast<long long>(pid)) + "/statm";
    FILE* f = ::fopen(path.c_str(), "r");
    if (f) {
        unsigned long long size_pages = 0, resident_pages = 0;
        if (::fscanf(f, "%llu %llu", &size_pages, &resident_pages) == 2) {
            out.vsize_bytes = static_cast<uint64_t>(size_pages) * page_size;
            out.rss_bytes = static_cast<uint64_t>(resident_pages) * page_size;
            out.valid = true;
        }
        ::fclose(f);
    }
#endif
    return out;
}

// -----------------------------------------------------------------------------
// Nombres de bibliotecas dinámicas para dlopen
// -----------------------------------------------------------------------------
// Cada subsistema (TLS, zlib, FreeType, X11, audio) intenta en orden los
// candidatos de su plataforma; si ninguno existe el módulo degrada con
// elegancia (igual que antes en Linux cuando faltaba la .so).
struct LibraryCandidates {
    const char* const* names;
    size_t count;

    const char* const* begin() const { return names; }
    const char* const* end() const { return names + count; }
};

#define ASTRA_LIB_LIST(ident, ...)                                             \
    inline LibraryCandidates ident() {                                         \
        static const char* const kNames[] = {__VA_ARGS__};                     \
        return LibraryCandidates{kNames, sizeof(kNames) / sizeof(kNames[0])};  \
    }

#if defined(__APPLE__)
// En Darwin los nombres desnudos (libcrypto.3.dylib) resuelven contra el stub
// no soportado del caché de dyld de Apple ("loading libcrypto in an unsafe
// way"), que abortaría el proceso al primer uso real de TLS. Por eso se
// intentan primero las rutas absolutas de OpenSSL 3 instalado (Homebrew en
// Apple Silicon e Intel, y MacPorts) y solo al final los nombres genéricos.
ASTRA_LIB_LIST(libcrypto_names,
               "/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib",
               "/usr/local/opt/openssl@3/lib/libcrypto.3.dylib",
               "/opt/local/lib/libcrypto.3.dylib",
               "libcrypto.3.dylib")
ASTRA_LIB_LIST(libssl_names,
               "/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib",
               "/usr/local/opt/openssl@3/lib/libssl.3.dylib",
               "/opt/local/lib/libssl.3.dylib",
               "libssl.3.dylib")
ASTRA_LIB_LIST(libz_names, "libz.1.dylib", "libz.dylib")
ASTRA_LIB_LIST(libfreetype_names,
               "/opt/homebrew/opt/freetype/lib/libfreetype.6.dylib",
               "/usr/local/opt/freetype/lib/libfreetype.6.dylib",
               "/opt/local/lib/libfreetype.6.dylib",
               "/opt/X11/lib/libfreetype.6.dylib",
               "libfreetype.6.dylib")
ASTRA_LIB_LIST(libx11_names, "/opt/X11/lib/libX11.6.dylib", "libX11.6.dylib")
ASTRA_LIB_LIST(libaudio_names, "libasound.2.dylib")
ASTRA_LIB_LIST(libbrotlidec_names,
               "/opt/homebrew/lib/libbrotlidec.1.dylib",
               "/usr/local/lib/libbrotlidec.1.dylib",
               "/opt/local/lib/libbrotlidec.1.dylib",
               "libbrotlidec.1.dylib", "libbrotlidec.dylib")
ASTRA_LIB_LIST(libwebp_names,
               "/opt/homebrew/lib/libwebp.7.dylib",
               "/usr/local/lib/libwebp.7.dylib",
               "/opt/local/lib/libwebp.7.dylib",
               "libwebp.7.dylib", "libwebp.dylib")
#else
ASTRA_LIB_LIST(libcrypto_names, "libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so")
ASTRA_LIB_LIST(libssl_names, "libssl.so.3", "libssl.so.1.1", "libssl.so")
ASTRA_LIB_LIST(libz_names, "libz.so.1", "libz.so")
ASTRA_LIB_LIST(libfreetype_names, "libfreetype.so.6", "libfreetype.so")
ASTRA_LIB_LIST(libx11_names, "libX11.so.6", "libX11.so")
ASTRA_LIB_LIST(libaudio_names, "libasound.so.2", "libasound.so")
ASTRA_LIB_LIST(libbrotlidec_names, "libbrotlidec.so.1", "libbrotlidec.so")
ASTRA_LIB_LIST(libwebp_names, "libwebp.so.7", "libwebp.so.6", "libwebp.so")
#endif

#undef ASTRA_LIB_LIST

} // namespace astra::platform
