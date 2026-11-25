#include "ipc/sandbox.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

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

} // namespace

SandboxReport ProcessSandbox::apply(SandboxProfile profile) {
    SandboxReport rep;
#ifdef __linux__
    // 1. POSIX resource limits (no core dumps, bounded file descriptors)
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
    rep.rlimits_applied = ok_core || ok_fd;

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
            rep.seccomp_active = true;
            rep.detail = (profile == SandboxProfile::ContentWorker)
                             ? "seccomp-bpf(ContentWorker: no-exec,no-net,no-ptrace,no-fork) + PR_SET_NO_NEW_PRIVS"
                             : "seccomp-bpf(RddMediaWorker: no-net,no-ptrace,no-admin) + PR_SET_NO_NEW_PRIVS";
        } else {
            rep.detail = std::string("PR_SET_SECCOMP failed: ") + std::strerror(errno);
        }
    } else {
        rep.detail = std::string("PR_SET_NO_NEW_PRIVS failed: ") + std::strerror(errno);
    }
#else
    (void)profile;
    rep.detail = "non-linux platform";
#endif
    return rep;
}

bool ProcessSandbox::self_verify_seccomp(std::string* detail_out) {
#ifdef __linux__
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        auto rep = apply(SandboxProfile::ContentWorker);
        if (!rep.no_new_privs || !rep.seccomp_active) _exit(2);

        // Attempt 1: raw network socket must fail with EPERM
        errno = 0;
        int sfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sfd >= 0 || errno != EPERM) {
            if (sfd >= 0) ::close(sfd);
            _exit(3);
        }

        // Attempt 2: execve must fail with EPERM
        errno = 0;
        char* const argv[] = {const_cast<char*>("/bin/true"), nullptr};
        char* const envp[] = {nullptr};
        int er = ::execve("/bin/true", argv, envp);
        if (er == 0 || errno != EPERM) _exit(4);

        // Attempt 3: ptrace must fail with EPERM
        errno = 0;
        long pr = ::syscall(SYS_ptrace, 0, 0, 0, 0);
        if (pr == 0 || errno != EPERM) _exit(5);

        _exit(0);
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return false;
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (detail_out) {
        if (ok) {
            *detail_out = "seccomp-BPF verified: socket(AF_INET)=EPERM, execve()=EPERM, ptrace()=EPERM";
        } else {
            *detail_out = "seccomp-BPF verification exit=" + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
    }
    return ok;
#else
    if (detail_out) *detail_out = "non-linux";
    return false;
#endif
}

} // namespace astra
