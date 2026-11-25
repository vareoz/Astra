#include "ipc/sandbox.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__APPLE__)
#include <sandbox.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace astra {

namespace {

#ifdef __linux__
uint32_t current_audit_arch() {
#if defined(__x86_64__)
    return AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    return AUDIT_ARCH_AARCH64;
#elif defined(__i386__)
    return AUDIT_ARCH_I386;
#elif defined(__arm__)
    return AUDIT_ARCH_ARM;
#else
    return 0;
#endif
}

std::vector<sock_filter> build_bpf_program(SandboxProfile profile) {
    std::vector<uint32_t> denied_syscalls;
    auto deny = [&](long nr) {
        if (nr >= 0) denied_syscalls.push_back(static_cast<uint32_t>(nr));
    };

    // Network isolation: neither Content nor RDD workers may open raw sockets
#ifdef SYS_socket
    deny(SYS_socket);
#endif
#ifdef SYS_connect
    deny(SYS_connect);
#endif
#ifdef SYS_bind
    deny(SYS_bind);
#endif
#ifdef SYS_listen
    deny(SYS_listen);
#endif
#ifdef SYS_accept
    deny(SYS_accept);
#endif
#ifdef SYS_accept4
    deny(SYS_accept4);
#endif
    // Privilege / debugging / filesystem-admin lockdown
#ifdef SYS_ptrace
    deny(SYS_ptrace);
#endif
#ifdef SYS_process_vm_readv
    deny(SYS_process_vm_readv);
#endif
#ifdef SYS_process_vm_writev
    deny(SYS_process_vm_writev);
#endif
#ifdef SYS_mount
    deny(SYS_mount);
#endif
#ifdef SYS_umount2
    deny(SYS_umount2);
#endif
#ifdef SYS_chroot
    deny(SYS_chroot);
#endif
#ifdef SYS_pivot_root
    deny(SYS_pivot_root);
#endif
#ifdef SYS_chmod
    deny(SYS_chmod);
#endif
#ifdef SYS_fchmod
    deny(SYS_fchmod);
#endif
#ifdef SYS_chown
    deny(SYS_chown);
#endif
#ifdef SYS_lchown
    deny(SYS_lchown);
#endif
#ifdef SYS_fchown
    deny(SYS_fchown);
#endif

    // Strict Content Worker profile additionally blocks all exec/process creation
    if (profile == SandboxProfile::ContentWorker) {
#ifdef SYS_execve
        deny(SYS_execve);
#endif
#ifdef SYS_execveat
        deny(SYS_execveat);
#endif
#ifdef SYS_fork
        deny(SYS_fork);
#endif
#ifdef SYS_vfork
        deny(SYS_vfork);
#endif
#ifdef SYS_kill
        deny(SYS_kill);
#endif
    }

    std::vector<sock_filter> prog;
    uint32_t arch = current_audit_arch();
    if (arch != 0) {
        // Load seccomp_data.arch and verify architecture
        prog.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<uint32_t>(offsetof(seccomp_data, arch))));
        prog.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, arch, 1, 0));
        prog.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
    }

    // Load seccomp_data.nr
    prog.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<uint32_t>(offsetof(seccomp_data, nr))));

    for (uint32_t nr : denied_syscalls) {
        prog.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1));
        prog.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
    }

    // Default allow for standard worker memory/IPC syscalls
    prog.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    return prog;
}
#endif

#if defined(__APPLE__) || defined(__linux__)
// Guardas POSIX comunes: sin volcados de memoria y con un techo de descriptores
// acorde al papel del worker. Son portables y no dependen del kernel.
bool apply_posix_rlimits(SandboxProfile profile) {
    rlimit core_lim{0, 0};
    bool ok_core = (::setrlimit(RLIMIT_CORE, &core_lim) == 0);
    rlimit nofile_cur{};
    bool ok_fd = false;
    if (::getrlimit(RLIMIT_NOFILE, &nofile_cur) == 0) {
        rlim_t target = (profile == SandboxProfile::ContentWorker) ? 64 : 128;
        if (nofile_cur.rlim_cur > target) {
            rlimit nl{target, std::min(nofile_cur.rlim_max, target)};
            ok_fd = (::setrlimit(RLIMIT_NOFILE, &nl) == 0);
        } else {
            ok_fd = true;
        }
    }
    return ok_core || ok_fd;
}
#endif

#if defined(__APPLE__)
// Perfiles Seatbelt (SBPL). Se parte de `(allow default)` y se vetan las
// operaciones peligrosas, igual que el filtro seccomp de Linux deniega una
// lista de syscalls en vez de permitir solo una lista blanca. Verificado
// empíricamente: `network*` corta connect()/bind() (socket() sigue creando el
// descriptor, porque Seatbelt arbitra en la conexión, no en la creación),
// `process-exec` corta execve() y `process-fork` impide engendrar procesos.
const char* sbpl_profile(SandboxProfile profile) {
    switch (profile) {
        case SandboxProfile::ContentWorker:
            // El proceso de contenido no necesita red propia (la sirve el
            // proceso principal por IPC), ni ejecutar binarios, ni ramificarse.
            return "(version 1)"
                   "(allow default)"
                   "(deny network*)"
                   "(deny process-exec)"
                   "(deny process-fork)";
        case SandboxProfile::RddMediaWorker:
            // El worker de medios sí puede lanzar el decodificador auxiliar,
            // pero tampoco debe alcanzar la red por su cuenta.
            return "(version 1)"
                   "(allow default)"
                   "(deny network*)";
    }
    return "(version 1)(allow default)(deny network*)";
}

// sandbox_init está marcada como obsoleta desde macOS 10.8 pero sigue siendo
// la única vía para confinar un binario sin empaquetar ni firmar, y es la que
// usaron históricamente los navegadores. El aviso se silencia solo aquí.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
bool engage_seatbelt(SandboxProfile profile, std::string& detail) {
    char* err = nullptr;
    const int rc = ::sandbox_init(sbpl_profile(profile), 0, &err);
    if (rc != 0) {
        detail = std::string("sandbox_init failed: ") + (err ? err : "unknown");
        if (err) ::sandbox_free_error(err);
        return false;
    }
    if (err) ::sandbox_free_error(err);
    detail = (profile == SandboxProfile::ContentWorker)
                 ? "seatbelt(ContentWorker: no-net,no-exec,no-fork) + setrlimit"
                 : "seatbelt(RddMediaWorker: no-net) + setrlimit";
    return true;
}
#pragma clang diagnostic pop
#endif

} // namespace

const char* SandboxReport::mechanism() {
#if defined(__linux__)
    return "seccomp-BPF";
#elif defined(__APPLE__)
    return "seatbelt";
#else
    return "none";
#endif
}

SandboxReport ProcessSandbox::apply(SandboxProfile profile) {
    SandboxReport rep;
#ifdef __linux__
    // 1. POSIX resource limits (no core dumps, bounded file descriptors)
    rep.rlimits_applied = apply_posix_rlimits(profile);

    // 2. Irrevocably drop ability to gain privileges via setuid/caps
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0) {
        rep.no_new_privs = true;
    }

    // 3. Install kernel seccomp-BPF filter
    if (rep.no_new_privs) {
        auto filter = build_bpf_program(profile);
        sock_fprog fprog{};
        fprog.len = static_cast<unsigned short>(filter.size());
        fprog.filter = filter.data();
        if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog) == 0) {
            rep.filter_active = true;
            rep.detail = (profile == SandboxProfile::ContentWorker)
                             ? "seccomp-bpf(ContentWorker: no-exec,no-net,no-ptrace,no-fork) + PR_SET_NO_NEW_PRIVS"
                             : "seccomp-bpf(RddMediaWorker: no-net,no-ptrace,no-admin) + PR_SET_NO_NEW_PRIVS";
        } else {
            rep.detail = std::string("PR_SET_SECCOMP failed: ") + std::strerror(errno);
        }
    } else {
        rep.detail = std::string("PR_SET_NO_NEW_PRIVS failed: ") + std::strerror(errno);
    }
#elif defined(__APPLE__)
    // 1. Mismas guardas POSIX que en Linux.
    rep.rlimits_applied = apply_posix_rlimits(profile);

    // 2. Darwin no tiene equivalente a PR_SET_NO_NEW_PRIVS: el perfil Seatbelt
    //    se hereda a través de fork() y no es revocable desde el propio
    //    proceso, que es la garantía que aquí importa.
    rep.no_new_privs = false;

    // 3. Compilar y activar el perfil Seatbelt del kernel.
    std::string detail;
    if (engage_seatbelt(profile, detail)) {
        rep.filter_active = true;
    }
    rep.detail = std::move(detail);
#else
    (void)profile;
    rep.detail = "unsupported platform (no kernel sandbox)";
#endif
    return rep;
}

bool ProcessSandbox::self_verify(std::string* detail_out) {
#if defined(__linux__) || defined(__APPLE__)
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // Se resuelve ANTES de confinar: una vez dentro del sandbox incluso
        // consultar el permiso de ejecución está vetado, y un ENOENT no
        // demostraría nada sobre el confinamiento.
        const char* exec_target = nullptr;
        for (const char* cand : {"/bin/true", "/usr/bin/true", "/bin/echo"}) {
            if (::access(cand, X_OK) == 0) {
                exec_target = cand;
                break;
            }
        }

        auto rep = apply(SandboxProfile::ContentWorker);
        if (!rep.filter_active) _exit(2);
#ifdef __linux__
        if (!rep.no_new_privs) _exit(2);

        // Linux veta la syscall entera: socket() ya falla.
        errno = 0;
        int sfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sfd >= 0 || errno != EPERM) {
            if (sfd >= 0) ::close(sfd);
            _exit(3);
        }
#else
        // Darwin arbitra en la conexión, no en la creación del descriptor: el
        // socket se crea pero no puede alcanzar nada.
        int sfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sfd < 0) _exit(3);
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(80);
        ::inet_pton(AF_INET, "93.184.216.34", &dst.sin_addr);
        errno = 0;
        int cr = ::connect(sfd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        ::close(sfd);
        if (cr == 0 || errno != EPERM) _exit(3);

        // bind() tampoco: el worker no puede abrir un puerto de escucha.
        int bfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (bfd >= 0) {
            sockaddr_in lo{};
            lo.sin_family = AF_INET;
            lo.sin_port = htons(0);
            lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            errno = 0;
            int br = ::bind(bfd, reinterpret_cast<sockaddr*>(&lo), sizeof(lo));
            ::close(bfd);
            if (br == 0 || errno != EPERM) _exit(6);
        }
#endif

        // Común a ambos: ejecutar un binario nuevo debe fallar con EPERM.
        if (!exec_target) _exit(9); // sin binario de referencia la prueba no es concluyente
        errno = 0;
        char* const argv[] = {const_cast<char*>(exec_target), nullptr};
        char* const envp[] = {nullptr};
        int er = ::execve(exec_target, argv, envp);
        if (er == 0 || errno != EPERM) _exit(4);

#ifdef __linux__
        errno = 0;
        long pr = ::syscall(SYS_ptrace, 0, 0, 0, 0);
        if (pr == 0 || errno != EPERM) _exit(5);
#endif

        // CONTROL POSITIVO: el canal IPC debe seguir vivo. Un sandbox que
        // también rompiera esto dejaría al navegador sin proceso de contenido,
        // así que la prueba no vale de nada sin comprobarlo.
        int sp[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) _exit(7);
        const char ping[] = "astra";
        char buf[sizeof(ping)] = {0};
        if (::send(sp[0], ping, sizeof(ping), 0) != static_cast<ssize_t>(sizeof(ping))) _exit(8);
        if (::recv(sp[1], buf, sizeof(buf), 0) != static_cast<ssize_t>(sizeof(ping))) _exit(8);
        if (std::strcmp(buf, ping) != 0) _exit(8);

        _exit(0);
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return false;
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (detail_out) {
        if (ok) {
#ifdef __linux__
            *detail_out = "seccomp-BPF verified: socket(AF_INET)=EPERM, execve()=EPERM, ptrace()=EPERM, AF_UNIX IPC alive";
#else
            *detail_out = "seatbelt verified: connect()=EPERM, bind()=EPERM, execve()=EPERM, AF_UNIX IPC alive";
#endif
        } else {
            *detail_out = std::string(SandboxReport::mechanism()) + " verification exit=" +
                          std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
    }
    return ok;
#else
    if (detail_out) *detail_out = "unsupported platform (no kernel sandbox)";
    return false;
#endif
}
} // namespace astra
