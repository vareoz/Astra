#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace astra {

enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};

struct WsMessage {
    bool ok = false;
    WsOpcode opcode = WsOpcode::Text;
    std::string payload;
    std::string error;
};

// AstraNet v6: Native RFC 6455 WebSocket Client (ws:// and wss:// via TLS 1.3 dlopen)
// Zero compile-time external dependencies. Pure C++20 SHA-1 + Base64 handshake & frame codec.
class WebSocketClient {
public:
    static constexpr size_t kMaxFrameBytes = 4 * 1024 * 1024; // 4 MiB hard cap

    WebSocketClient() = default;
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // Connect to ws://host[:port]/path or wss://host[:port]/path
    bool connect(const std::string& url, int timeout_ms = 5000, std::string* error_out = nullptr);
    bool is_open() const { return open_; }
    const std::string& url() const { return url_; }
    bool tls_verified() const { return tls_verified_; }

    // Send RFC 6455 masked client frames
    bool send_text(std::string_view text, std::string* error_out = nullptr);
    bool send_binary(std::string_view data, std::string* error_out = nullptr);
    bool send_ping(std::string_view data = "", std::string* error_out = nullptr);

    // Receive next application frame (automatically replies to Ping with Pong and handles Close)
    WsMessage recv(int timeout_ms = 3000);

    // Graceful RFC 6455 Close handshake
    void close(uint16_t status_code = 1000, std::string_view reason = "normal");

    // Exposed RFC 6455 helpers (used by unit tests & server verification)
    static std::string compute_accept_key(std::string_view sec_websocket_key);
    static std::string base64_encode(const uint8_t* data, size_t len);
    static void sha1(const uint8_t* data, size_t len, uint8_t out_digest[20]);

private:
    bool write_all(const uint8_t* data, size_t len);
    bool read_exact(uint8_t* data, size_t len, int timeout_ms);
    bool send_frame(WsOpcode opcode, const uint8_t* payload, size_t len);

    int fd_ = -1;
    void* ssl_ctx_ = nullptr;
    void* ssl_ = nullptr;
    bool use_tls_ = false;
    bool tls_verified_ = false;
    bool open_ = false;
    std::string url_;
    uint64_t mask_rng_ = 0xA57A9E71C3B50741ULL;
};

} // namespace astra
