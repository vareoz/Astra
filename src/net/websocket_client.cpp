#include "net/websocket_client.hpp"

#include "core/platform.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace astra {

namespace {

struct WsTlsApi {
    using SSL_CTX_new_fn = void* (*)(const void*);
    using TLS_client_method_fn = const void* (*)();
    using SSL_CTX_free_fn = void (*)(void*);
    using SSL_CTX_set_default_verify_paths_fn = int (*)(void*);
    using SSL_CTX_set_verify_fn = void (*)(void*, int, void*);
    using SSL_new_fn = void* (*)(void*);
    using SSL_free_fn = void (*)(void*);
    using SSL_set_fd_fn = int (*)(void*, int);
    using SSL_ctrl_fn = long (*)(void*, int, long, void*);
    using SSL_set1_host_fn = int (*)(void*, const char*);
    using SSL_connect_fn = int (*)(void*);
    using SSL_read_fn = int (*)(void*, void*, int);
    using SSL_write_fn = int (*)(void*, const void*, int);
    using SSL_shutdown_fn = int (*)(void*);
    using SSL_get_verify_result_fn = long (*)(const void*);

    bool loaded = false;
    void* handle = nullptr;
    SSL_CTX_new_fn SSL_CTX_new = nullptr;
    TLS_client_method_fn TLS_client_method = nullptr;
    SSL_CTX_free_fn SSL_CTX_free = nullptr;
    SSL_CTX_set_default_verify_paths_fn SSL_CTX_set_default_verify_paths = nullptr;
    SSL_CTX_set_verify_fn SSL_CTX_set_verify = nullptr;
    SSL_new_fn SSL_new = nullptr;
    SSL_free_fn SSL_free = nullptr;
    SSL_set_fd_fn SSL_set_fd = nullptr;
    SSL_ctrl_fn SSL_ctrl = nullptr;
    SSL_set1_host_fn SSL_set1_host = nullptr;
    SSL_connect_fn SSL_connect = nullptr;
    SSL_read_fn SSL_read = nullptr;
    SSL_write_fn SSL_write = nullptr;
    SSL_shutdown_fn SSL_shutdown = nullptr;
    SSL_get_verify_result_fn SSL_get_verify_result = nullptr;

    static WsTlsApi& instance() {
        static WsTlsApi api;
        static bool init = false;
        if (!init) {
            init = true;
            for (const char* name : platform::libssl_names()) {
                api.handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
                if (api.handle) break;
            }
            if (api.handle) {
                api.SSL_CTX_new = reinterpret_cast<SSL_CTX_new_fn>(dlsym(api.handle, "SSL_CTX_new"));
                api.TLS_client_method = reinterpret_cast<TLS_client_method_fn>(dlsym(api.handle, "TLS_client_method"));
                api.SSL_CTX_free = reinterpret_cast<SSL_CTX_free_fn>(dlsym(api.handle, "SSL_CTX_free"));
                api.SSL_CTX_set_default_verify_paths = reinterpret_cast<SSL_CTX_set_default_verify_paths_fn>(dlsym(api.handle, "SSL_CTX_set_default_verify_paths"));
                api.SSL_CTX_set_verify = reinterpret_cast<SSL_CTX_set_verify_fn>(dlsym(api.handle, "SSL_CTX_set_verify"));
                api.SSL_new = reinterpret_cast<SSL_new_fn>(dlsym(api.handle, "SSL_new"));
                api.SSL_free = reinterpret_cast<SSL_free_fn>(dlsym(api.handle, "SSL_free"));
                api.SSL_set_fd = reinterpret_cast<SSL_set_fd_fn>(dlsym(api.handle, "SSL_set_fd"));
                api.SSL_ctrl = reinterpret_cast<SSL_ctrl_fn>(dlsym(api.handle, "SSL_ctrl"));
                api.SSL_set1_host = reinterpret_cast<SSL_set1_host_fn>(dlsym(api.handle, "SSL_set1_host"));
                api.SSL_connect = reinterpret_cast<SSL_connect_fn>(dlsym(api.handle, "SSL_connect"));
                api.SSL_read = reinterpret_cast<SSL_read_fn>(dlsym(api.handle, "SSL_read"));
                api.SSL_write = reinterpret_cast<SSL_write_fn>(dlsym(api.handle, "SSL_write"));
                api.SSL_shutdown = reinterpret_cast<SSL_shutdown_fn>(dlsym(api.handle, "SSL_shutdown"));
                api.SSL_get_verify_result = reinterpret_cast<SSL_get_verify_result_fn>(dlsym(api.handle, "SSL_get_verify_result"));
                api.loaded = (api.SSL_CTX_new && api.TLS_client_method && api.SSL_new &&
                              api.SSL_set_fd && api.SSL_connect && api.SSL_read && api.SSL_write);
            }
        }
        return api;
    }
};

struct ParsedWsUrl {
    bool valid = false;
    bool secure = false;
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
};

ParsedWsUrl parse_ws_url(std::string_view url) {
    ParsedWsUrl out;
    size_t pos = 0;
    if (url.rfind("wss://", 0) == 0) {
        out.secure = true;
        out.port = 443;
        pos = 6;
    } else if (url.rfind("ws://", 0) == 0) {
        out.secure = false;
        out.port = 80;
        pos = 5;
    } else {
        return out;
    }

    size_t slash = url.find('/', pos);
    std::string_view authority = (slash == std::string_view::npos) ? url.substr(pos) : url.substr(pos, slash - pos);
    out.path = (slash == std::string_view::npos) ? "/" : std::string(url.substr(slash));
    if (authority.empty()) return out;

    size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        out.host = std::string(authority.substr(0, colon));
        int p = std::atoi(std::string(authority.substr(colon + 1)).c_str());
        if (p <= 0 || p > 65535) return out;
        out.port = static_cast<uint16_t>(p);
    } else {
        out.host = std::string(authority);
    }
    out.valid = !out.host.empty();
    return out;
}

std::string to_lower_copy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim_copy(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

uint32_t rol32(uint32_t v, unsigned bits) {
    return (v << bits) | (v >> (32u - bits));
}

} // namespace

void WebSocketClient::sha1(const uint8_t* data, size_t len, uint8_t out_digest[20]) {
    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;

    std::vector<uint8_t> msg(data, data + len);
    uint64_t bit_len = static_cast<uint64_t>(len) * 8ULL;
    msg.push_back(0x80u);
    while ((msg.size() % 64) != 56) msg.push_back(0x00u);
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFu));
    }

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[off + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(msg[off + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = rol32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol32(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out_digest[i * 4] = static_cast<uint8_t>((hs[i] >> 24) & 0xFFu);
        out_digest[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFFu);
        out_digest[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFFu);
        out_digest[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFFu);
    }
}

std::string WebSocketClient::base64_encode(const uint8_t* data, size_t len) {
    static constexpr char kTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTab[(n >> 18) & 63]);
        out.push_back(kTab[(n >> 12) & 63]);
        out.push_back((i + 1 < len) ? kTab[(n >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? kTab[n & 63] : '=');
    }
    return out;
}

std::string WebSocketClient::compute_accept_key(std::string_view sec_websocket_key) {
    static constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined;
    combined.reserve(sec_websocket_key.size() + kGuid.size());
    combined.append(sec_websocket_key);
    combined.append(kGuid);
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(combined.data()), combined.size(), digest);
    return base64_encode(digest, 20);
}

WebSocketClient::~WebSocketClient() {
    if (open_) close(1001, "going_away");
    if (ssl_) {
        auto& api = WsTlsApi::instance();
        if (api.SSL_shutdown) api.SSL_shutdown(ssl_);
        if (api.SSL_free) api.SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ssl_ctx_) {
        auto& api = WsTlsApi::instance();
        if (api.SSL_CTX_free) api.SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool WebSocketClient::write_all(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = 0;
        if (use_tls_ && ssl_) {
            n = WsTlsApi::instance().SSL_write(ssl_, data + sent, static_cast<int>(len - sent));
        } else {
            n = ::send(fd_, data + sent, len - sent, ASTRA_MSG_NOSIGNAL);
        }
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool WebSocketClient::read_exact(uint8_t* data, size_t len, int timeout_ms) {
    size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(100, timeout_ms));
    while (got < len) {
        if (!use_tls_) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            int rem = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            pollfd pfd{fd_, POLLIN, 0};
            int pr = ::poll(&pfd, 1, std::max(1, rem));
            if (pr <= 0) return false;
        }
        ssize_t n = 0;
        if (use_tls_ && ssl_) {
            n = WsTlsApi::instance().SSL_read(ssl_, data + got, static_cast<int>(len - got));
        } else {
            n = ::recv(fd_, data + got, len - got, 0);
        }
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool WebSocketClient::connect(const std::string& url, int timeout_ms, std::string* error_out) {
    if (open_) close(1000, "reconnect");
    url_ = url;
    tls_verified_ = false;
    use_tls_ = false;

    ParsedWsUrl u = parse_ws_url(url);
    if (!u.valid) {
        if (error_out) *error_out = "Invalid WebSocket URL (expected ws:// or wss://)";
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string port_str = std::to_string(u.port);
    if (getaddrinfo(u.host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        if (error_out) *error_out = "DNS resolution failed for " + u.host;
        return false;
    }

    int fd = -1;
    for (addrinfo* rp = res; rp; rp = rp->ai_next) {
        fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        platform::harden_socket_fd(fd);
        if (fd < 0) continue;
        timeval tv{};
        tv.tv_sec = std::max(1, timeout_ms / 1000);
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        if (error_out) *error_out = "TCP connect failed to " + u.host + ":" + port_str;
        return false;
    }
    fd_ = fd;

    if (u.secure) {
        auto& api = WsTlsApi::instance();
        if (!api.loaded) {
            if (error_out) *error_out = "OpenSSL libssl not available for wss://";
            ::close(fd_); fd_ = -1;
            return false;
        }
        ssl_ctx_ = api.SSL_CTX_new(api.TLS_client_method());
        if (!ssl_ctx_) {
            if (error_out) *error_out = "SSL_CTX_new failed";
            ::close(fd_); fd_ = -1;
            return false;
        }
        if (api.SSL_CTX_set_default_verify_paths) api.SSL_CTX_set_default_verify_paths(ssl_ctx_);
        if (api.SSL_CTX_set_verify) api.SSL_CTX_set_verify(ssl_ctx_, 1 /* SSL_VERIFY_PEER */, nullptr);
        ssl_ = api.SSL_new(ssl_ctx_);
        if (!ssl_) {
            if (error_out) *error_out = "SSL_new failed";
            api.SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr;
            ::close(fd_); fd_ = -1;
            return false;
        }
        api.SSL_set_fd(ssl_, fd_);
        // SNI (SSL_CTRL_SET_TLSEXT_HOSTNAME = 55, TLSEXT_NAMETYPE_host_name = 0)
        if (api.SSL_ctrl) api.SSL_ctrl(ssl_, 55, 0, const_cast<char*>(u.host.c_str()));
        if (api.SSL_set1_host) api.SSL_set1_host(ssl_, u.host.c_str());
        if (api.SSL_connect(ssl_) != 1) {
            if (error_out) *error_out = "TLS handshake failed for wss://" + u.host;
            api.SSL_free(ssl_); ssl_ = nullptr;
            api.SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr;
            ::close(fd_); fd_ = -1;
            return false;
        }
        use_tls_ = true;
        tls_verified_ = (api.SSL_get_verify_result && api.SSL_get_verify_result(ssl_) == 0);
    }

    // Generate 16-byte Sec-WebSocket-Key using steady_clock entropy + xorshift64*
    mask_rng_ ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    auto next_u64 = [&]() -> uint64_t {
        uint64_t x = mask_rng_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        mask_rng_ = x;
        return x * 2685821657736338717ULL;
    };
    uint8_t nonce[16];
    uint64_t r0 = next_u64(), r1 = next_u64();
    std::memcpy(nonce, &r0, 8);
    std::memcpy(nonce + 8, &r1, 8);
    std::string ws_key = base64_encode(nonce, 16);
    std::string expected_accept = compute_accept_key(ws_key);

    std::string host_hdr = u.host;
    if ((!u.secure && u.port != 80) || (u.secure && u.port != 443)) {
        host_hdr += ":" + std::to_string(u.port);
    }

    std::string req =
        "GET " + u.path + " HTTP/1.1\r\n"
        "Host: " + host_hdr + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + ws_key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "User-Agent: AstraBrowser/7.0 (Native C++20; RFC6455)\r\n\r\n";

    if (!write_all(reinterpret_cast<const uint8_t*>(req.data()), req.size())) {
        if (error_out) *error_out = "Failed to send WebSocket Upgrade request";
        return false;
    }

    // Read HTTP 101 Switching Protocols header byte-by-byte until \r\n\r\n
    std::string hdr;
    hdr.reserve(512);
    uint8_t b = 0;
    while (hdr.size() < 8192) {
        if (!read_exact(&b, 1, timeout_ms)) {
            if (error_out) *error_out = "Timeout reading WebSocket Upgrade response";
            return false;
        }
        hdr.push_back(static_cast<char>(b));
        if (hdr.size() >= 4 && hdr.compare(hdr.size() - 4, 4, "\r\n\r\n") == 0) break;
    }

    if (hdr.find(" 101 ") == std::string::npos) {
        if (error_out) *error_out = "Server did not upgrade to HTTP 101 Switching Protocols";
        return false;
    }

    // Validate Sec-WebSocket-Accept header strictly per RFC 6455 Section 4.1
    bool accept_ok = false;
    size_t pos = 0;
    while (pos < hdr.size()) {
        size_t eol = hdr.find("\r\n", pos);
        if (eol == std::string::npos) break;
        std::string_view line(hdr.data() + pos, eol - pos);
        size_t col = line.find(':');
        if (col != std::string_view::npos) {
            std::string k = to_lower_copy(trim_copy(line.substr(0, col)));
            std::string v = trim_copy(line.substr(col + 1));
            if (k == "sec-websocket-accept" && v == expected_accept) {
                accept_ok = true;
            }
        }
        pos = eol + 2;
    }

    if (!accept_ok) {
        if (error_out) *error_out = "RFC 6455 Sec-WebSocket-Accept mismatch";
        return false;
    }

    open_ = true;
    return true;
}

bool WebSocketClient::send_frame(WsOpcode opcode, const uint8_t* payload, size_t len) {
    if (!open_ || len > kMaxFrameBytes) return false;

    uint64_t x = mask_rng_ ^ 0x9E3779B97F4A7C15ULL;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    mask_rng_ = x;
    uint32_t m32 = static_cast<uint32_t>((x * 2685821657736338717ULL) >> 32);
    uint8_t mask[4] = {
        static_cast<uint8_t>((m32 >> 24) & 0xFFu),
        static_cast<uint8_t>((m32 >> 16) & 0xFFu),
        static_cast<uint8_t>((m32 >> 8) & 0xFFu),
        static_cast<uint8_t>(m32 & 0xFFu)
    };

    std::vector<uint8_t> frame;
    frame.reserve(14 + len);
    frame.push_back(static_cast<uint8_t>(0x80u | static_cast<uint8_t>(opcode))); // FIN=1
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(0x80u | len)); // MASK=1
    } else if (len <= 0xFFFFu) {
        frame.push_back(static_cast<uint8_t>(0x80u | 126u));
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFFu));
        frame.push_back(static_cast<uint8_t>(len & 0xFFu));
    } else {
        frame.push_back(static_cast<uint8_t>(0x80u | 127u));
        uint64_t l64 = static_cast<uint64_t>(len);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((l64 >> (i * 8)) & 0xFFu));
        }
    }
    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < len; ++i) {
        frame.push_back(payload[i] ^ mask[i & 3u]);
    }
    return write_all(frame.data(), frame.size());
}

bool WebSocketClient::send_text(std::string_view text, std::string* error_out) {
    bool ok = send_frame(WsOpcode::Text, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    if (!ok && error_out) *error_out = "WebSocket send_text failed";
    return ok;
}

bool WebSocketClient::send_binary(std::string_view data, std::string* error_out) {
    bool ok = send_frame(WsOpcode::Binary, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    if (!ok && error_out) *error_out = "WebSocket send_binary failed";
    return ok;
}

bool WebSocketClient::send_ping(std::string_view data, std::string* error_out) {
    bool ok = send_frame(WsOpcode::Ping, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    if (!ok && error_out) *error_out = "WebSocket send_ping failed";
    return ok;
}

WsMessage WebSocketClient::recv(int timeout_ms) {
    WsMessage msg;
    if (!open_) {
        msg.error = "WebSocket is not open";
        return msg;
    }

    for (int control_hops = 0; control_hops < 8; ++control_hops) {
        uint8_t hdr[2];
        if (!read_exact(hdr, 2, timeout_ms)) {
            msg.error = "WebSocket read timeout or closed";
            return msg;
        }

        WsOpcode op = static_cast<WsOpcode>(hdr[0] & 0x0Fu);
        bool masked = (hdr[1] & 0x80u) != 0;
        uint64_t plen = static_cast<uint64_t>(hdr[1] & 0x7Fu);

        if (plen == 126) {
            uint8_t ext[2];
            if (!read_exact(ext, 2, timeout_ms)) { msg.error = "Truncated 16-bit length"; return msg; }
            plen = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (plen == 127) {
            uint8_t ext[8];
            if (!read_exact(ext, 8, timeout_ms)) { msg.error = "Truncated 64-bit length"; return msg; }
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | ext[i];
        }

        if (plen > kMaxFrameBytes) {
            msg.error = "WebSocket frame exceeds 4 MiB ceiling";
            close(1009, "message_too_big");
            return msg;
        }

        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked && !read_exact(mask, 4, timeout_ms)) {
            msg.error = "Truncated mask key";
            return msg;
        }

        std::string payload(static_cast<size_t>(plen), '\0');
        if (plen > 0 && !read_exact(reinterpret_cast<uint8_t*>(payload.data()), static_cast<size_t>(plen), timeout_ms)) {
            msg.error = "Truncated WebSocket payload";
            return msg;
        }
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i & 3u]);
            }
        }

        if (op == WsOpcode::Ping) {
            send_frame(WsOpcode::Pong, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
            continue;
        }
        if (op == WsOpcode::Pong) {
            msg.ok = true;
            msg.opcode = WsOpcode::Pong;
            msg.payload = std::move(payload);
            return msg;
        }
        if (op == WsOpcode::Close) {
            open_ = false;
            msg.ok = false;
            msg.opcode = WsOpcode::Close;
            msg.payload = std::move(payload);
            msg.error = "Peer closed WebSocket";
            return msg;
        }

        msg.ok = true;
        msg.opcode = op;
        msg.payload = std::move(payload);
        return msg;
    }

    msg.error = "Too many consecutive control frames";
    return msg;
}

void WebSocketClient::close(uint16_t status_code, std::string_view reason) {
    if (!open_) return;
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>((status_code >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(status_code & 0xFFu));
    for (char c : reason) {
        if (body.size() >= 125) break;
        body.push_back(static_cast<uint8_t>(c));
    }
    send_frame(WsOpcode::Close, body.data(), body.size());
    open_ = false;
    if (ssl_) {
        auto& api = WsTlsApi::instance();
        if (api.SSL_shutdown) api.SSL_shutdown(ssl_);
        if (api.SSL_free) api.SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ssl_ctx_) {
        auto& api = WsTlsApi::instance();
        if (api.SSL_CTX_free) api.SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace astra
