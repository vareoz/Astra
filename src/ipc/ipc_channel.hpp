#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace astra {

// Firefox IPDL-inspired binary framed IPC protocol over POSIX AF_UNIX sockets.
enum class IpcMsgType : uint32_t {
    Ping = 1,
    Pong = 2,
    RenderHtml = 10,          // Main -> Content: viewport + URL + HTML + CSS
    DisplayListBlob = 11,     // Content -> Main: serialized binary Display List + title + arena stats
    MemoryReportReq = 20,     // Main -> Worker
    MemoryReportResp = 21,    // Worker -> Main: pid + rss_bytes + arena_bytes
    DecodeVideoReq = 30,      // Main -> RDD Media: file path + target frame
    DecodeVideoResp = 31,     // RDD Media -> Main: width + height + RGBA frame bytes
    Shutdown = 99
};

struct IpcMessage {
    IpcMsgType type = IpcMsgType::Ping;
    uint32_t request_id = 0;
    std::vector<uint8_t> payload;
};

class IpcChannel {
public:
    IpcChannel() = default;
    explicit IpcChannel(int fd);
    ~IpcChannel();

    IpcChannel(const IpcChannel&) = delete;
    IpcChannel& operator=(const IpcChannel&) = delete;
    IpcChannel(IpcChannel&& other) noexcept;
    IpcChannel& operator=(IpcChannel&& other) noexcept;

    bool valid() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    void close();

    bool send(const IpcMessage& msg) const;
    bool recv(IpcMessage& out) const;

    static bool create_pair(IpcChannel& parent_end, IpcChannel& child_end);

private:
    int fd_ = -1;
};

// Spawns an isolated child worker process (Content or RDD Media) connected via
// a bidirectional binary IPC channel.
class WorkerProcess {
public:
    WorkerProcess() = default;
    ~WorkerProcess();

    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;
    WorkerProcess(WorkerProcess&& other) noexcept;
    WorkerProcess& operator=(WorkerProcess&& other) noexcept;

    bool spawn(const std::string& role, const std::function<void(IpcChannel&)>& worker_main);
    void stop();

    bool alive() const { return pid_ > 0 && channel_.valid(); }
    pid_t pid() const { return pid_; }
    const std::string& role() const { return role_; }
    IpcChannel& channel() { return channel_; }

private:
    pid_t pid_ = -1;
    std::string role_;
    IpcChannel channel_;
};

// Binary serialization helpers for IPC payloads
void write_u32(std::vector<uint8_t>& buf, uint32_t v);
void write_u64(std::vector<uint8_t>& buf, uint64_t v);
void write_f32(std::vector<uint8_t>& buf, float v);
void write_str(std::vector<uint8_t>& buf, const std::string& s);

bool read_u32(const std::vector<uint8_t>& buf, size_t& off, uint32_t& out);
bool read_u64(const std::vector<uint8_t>& buf, size_t& off, uint64_t& out);
bool read_f32(const std::vector<uint8_t>& buf, size_t& off, float& out);
bool read_str(const std::vector<uint8_t>& buf, size_t& off, std::string& out);

} // namespace astra
