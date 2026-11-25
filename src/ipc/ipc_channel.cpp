#include "ipc/ipc_channel.hpp"

#include "core/platform.hpp"

#include <cerrno>
#include <cstring>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace astra {

namespace {
bool write_exact(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, ASTRA_MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool read_exact(int fd, void* data, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(data);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}
} // namespace

IpcChannel::IpcChannel(int fd) : fd_(fd) {}

IpcChannel::~IpcChannel() { close(); }

IpcChannel::IpcChannel(IpcChannel&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

IpcChannel& IpcChannel::operator=(IpcChannel&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void IpcChannel::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool IpcChannel::create_pair(IpcChannel& parent_end, IpcChannel& child_end) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | ASTRA_SOCK_CLOEXEC, 0, fds) != 0) {
        return false;
    }
    // Darwin no admite SOCK_CLOEXEC en socketpair(): se aplica FD_CLOEXEC y
    // SO_NOSIGPIPE a posteriori para que ambos extremos del canal se comporten
    // igual que en Linux (el fork() de los procesos hijos conserva el fd, pero
    // un exec() posterior no lo heredaría).
    platform::harden_socket_fd(fds[0]);
    platform::harden_socket_fd(fds[1]);
    parent_end = IpcChannel(fds[0]);
    child_end = IpcChannel(fds[1]);
    return true;
}

bool IpcChannel::send(const IpcMessage& msg) const {
    if (fd_ < 0) return false;
    uint32_t header[3] = {
        static_cast<uint32_t>(msg.type),
        msg.request_id,
        static_cast<uint32_t>(msg.payload.size())
    };
    if (!write_exact(fd_, header, sizeof(header))) return false;
    if (!msg.payload.empty()) {
        if (!write_exact(fd_, msg.payload.data(), msg.payload.size())) return false;
    }
    return true;
}

bool IpcChannel::recv(IpcMessage& out) const {
    if (fd_ < 0) return false;
    uint32_t header[3] = {0, 0, 0};
    if (!read_exact(fd_, header, sizeof(header))) return false;
    out.type = static_cast<IpcMsgType>(header[0]);
    out.request_id = header[1];
    uint32_t payload_len = header[2];
    if (payload_len > 128u * 1024u * 1024u) return false; // 128 MiB safety guard
    out.payload.resize(payload_len);
    if (payload_len > 0) {
        if (!read_exact(fd_, out.payload.data(), payload_len)) return false;
    }
    return true;
}

WorkerProcess::~WorkerProcess() { stop(); }

WorkerProcess::WorkerProcess(WorkerProcess&& other) noexcept
    : pid_(other.pid_), role_(std::move(other.role_)), channel_(std::move(other.channel_)) {
    other.pid_ = -1;
}

WorkerProcess& WorkerProcess::operator=(WorkerProcess&& other) noexcept {
    if (this != &other) {
        stop();
        pid_ = other.pid_;
        role_ = std::move(other.role_);
        channel_ = std::move(other.channel_);
        other.pid_ = -1;
    }
    return *this;
}

bool WorkerProcess::spawn(const std::string& role, const std::function<void(IpcChannel&)>& worker_main) {
    stop();
    IpcChannel parent_end, child_end;
    if (!IpcChannel::create_pair(parent_end, child_end)) return false;

    pid_t pid = ::fork();
    if (pid < 0) return false;

    if (pid == 0) {
        // Child worker process
        parent_end.close();
        worker_main(child_end);
        child_end.close();
        _exit(0);
    }

    // Parent process
    child_end.close();
    pid_ = pid;
    role_ = role;
    channel_ = std::move(parent_end);
    return true;
}

void WorkerProcess::stop() {
    if (pid_ > 0) {
        if (channel_.valid()) {
            IpcMessage bye;
            bye.type = IpcMsgType::Shutdown;
            channel_.send(bye);
            channel_.close();
        }
        int status = 0;
        pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == 0) {
            usleep(20000);
            r = ::waitpid(pid_, &status, WNOHANG);
            if (r == 0) {
                ::kill(pid_, SIGTERM);
                ::waitpid(pid_, &status, 0);
            }
        }
        pid_ = -1;
    } else {
        channel_.close();
    }
}

void write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    size_t pos = buf.size();
    buf.resize(pos + 4);
    std::memcpy(buf.data() + pos, &v, 4);
}

void write_u64(std::vector<uint8_t>& buf, uint64_t v) {
    size_t pos = buf.size();
    buf.resize(pos + 8);
    std::memcpy(buf.data() + pos, &v, 8);
}

void write_f32(std::vector<uint8_t>& buf, float v) {
    size_t pos = buf.size();
    buf.resize(pos + 4);
    std::memcpy(buf.data() + pos, &v, 4);
}

void write_str(std::vector<uint8_t>& buf, const std::string& s) {
    write_u32(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

bool read_u32(const std::vector<uint8_t>& buf, size_t& off, uint32_t& out) {
    if (off + 4 > buf.size()) return false;
    std::memcpy(&out, buf.data() + off, 4);
    off += 4;
    return true;
}

bool read_u64(const std::vector<uint8_t>& buf, size_t& off, uint64_t& out) {
    if (off + 8 > buf.size()) return false;
    std::memcpy(&out, buf.data() + off, 8);
    off += 8;
    return true;
}

bool read_f32(const std::vector<uint8_t>& buf, size_t& off, float& out) {
    if (off + 4 > buf.size()) return false;
    std::memcpy(&out, buf.data() + off, 4);
    off += 4;
    return true;
}

bool read_str(const std::vector<uint8_t>& buf, size_t& off, std::string& out) {
    uint32_t len = 0;
    if (!read_u32(buf, off, len)) return false;
    if (off + len > buf.size()) return false;
    out.assign(reinterpret_cast<const char*>(buf.data() + off), len);
    off += len;
    return true;
}

} // namespace astra
