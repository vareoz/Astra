#pragma once

#include <string>

namespace astra {

enum class SandboxProfile {
    ContentWorker,  // Strict: blocks execve, fork, ptrace, network sockets, mount, chmod/chown
    RddMediaWorker  // Media decode: blocks network sockets, ptrace, mount, chmod/chown (allows helper decode)
};

struct SandboxReport {
    bool no_new_privs = false;   // Linux: PR_SET_NO_NEW_PRIVS (no equivalente en Darwin)
    bool rlimits_applied = false;
    bool filter_active = false;  // seccomp-BPF (Linux) o perfil Seatbelt (Darwin) activo
    std::string detail;

    // Nombre del mecanismo que confina realmente al proceso en esta plataforma.
    static const char* mechanism();
};

// AstraSandbox v2: confinamiento real del proceso con la primitiva nativa de
// cada kernel, detrás de una única interfaz.
//
//   Linux  — PR_SET_NO_NEW_PRIVS + setrlimit + filtro seccomp-BPF
//            (SECCOMP_MODE_FILTER) que devuelve EPERM en las syscalls vetadas.
//   Darwin — setrlimit + perfil Seatbelt compilado con sandbox_init(SBPL):
//            `(deny network*)` corta connect/bind, `(deny process-exec)` corta
//            execve y `(deny process-fork)` impide engendrar procesos nuevos.
//
// En ambas plataformas el canal IPC `socketpair(AF_UNIX)` que ya está abierto
// sigue funcionando: lo que se veta es abrir rutas NUEVAS hacia el exterior.
class ProcessSandbox {
public:
    static SandboxReport apply(SandboxProfile profile);

    // Engendra un hijo efímero, le aplica el perfil ContentWorker y verifica a
    // nivel de kernel que las operaciones vetadas fallan con EPERM y que el
    // canal IPC sigue vivo (control positivo). Devuelve true solo si ambas
    // cosas se cumplen.
    static bool self_verify(std::string* detail_out = nullptr);
};

} // namespace astra
