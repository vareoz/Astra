#pragma once

#include <string>

namespace astra {

enum class SandboxProfile {
    ContentWorker,  // Strict: blocks execve, fork, ptrace, network sockets, mount, chmod/chown
    RddMediaWorker  // Media decode: blocks network sockets, ptrace, mount, chmod/chown (allows helper decode)
};

struct SandboxReport {
    bool no_new_privs = false;
    bool rlimits_applied = false;
    bool seccomp_active = false;
    std::string detail;
};

// AstraSandbox v1: Real Linux kernel process sandboxing via PR_SET_NO_NEW_PRIVS,
// POSIX setrlimit guards, and seccomp-BPF syscall filters (SECCOMP_MODE_FILTER).
class ProcessSandbox {
public:
    static SandboxReport apply(SandboxProfile profile);

    // Spawns an ephemeral test child, engages ContentWorker seccomp-BPF, and verifies
    // at kernel level that blocked syscalls (execve & socket) fail with EPERM.
    static bool self_verify_seccomp(std::string* detail_out = nullptr);
};

} // namespace astra
