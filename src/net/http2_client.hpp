#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace astra {

// RFC 7540 HTTP/2 frame types
enum class Http2FrameType : uint8_t {
    Data = 0x0,
    Headers = 0x1,
    Priority = 0x2,
    RstStream = 0x3,
    Settings = 0x4,
    PushPromise = 0x5,
    Ping = 0x6,
    GoAway = 0x7,
    WindowUpdate = 0x8,
    Continuation = 0x9
};

struct Http2Frame {
    Http2FrameType type = Http2FrameType::Data;
    uint8_t flags = 0;
    uint32_t stream_id = 0;
    std::vector<uint8_t> payload;
};

struct Http2Response {
    bool ok = false;
    int status_code = 0;
    std::map<std::string, std::string> headers;
    std::vector<std::string> set_cookies;
    std::string body;
    std::string error;
    size_t frames_received = 0;
};

// RFC 7541 HPACK encoder/decoder with static table (61 entries),
// dynamic table (4096 bytes), and full Appendix B Huffman decoder.
class HpackCodec {
public:
    explicit HpackCodec(size_t max_dynamic_bytes = 4096)
        : max_dynamic_bytes_(max_dynamic_bytes) {}

    std::vector<uint8_t> encode_headers(const std::vector<std::pair<std::string, std::string>>& headers);
    bool decode_headers(const uint8_t* data, size_t len,
                        std::vector<std::pair<std::string, std::string>>& out_headers);

    static bool huffman_decode(const uint8_t* data, size_t len, std::string& out);

private:
    struct DynEntry {
        std::string name;
        std::string value;
        size_t size() const { return name.size() + value.size() + 32; }
    };

    void add_dynamic(const std::string& name, const std::string& value);
    bool lookup_index(size_t idx, std::string& name, std::string& value) const;

    std::vector<DynEntry> dynamic_;
    size_t dynamic_bytes_ = 0;
    size_t max_dynamic_bytes_ = 4096;
};

struct Http2RequestSpec {
    std::string method = "GET";
    std::string scheme = "https";
    std::string authority;
    std::string path = "/";
    std::vector<std::pair<std::string, std::string>> extra_headers;
    std::string body;
};

// AstraNet v5 Native Binary HTTP/2 Client (RFC 7540 Multi-Stream Multiplexing + RFC 7541 HPACK)
class Http2Client {
public:
    using ReadFn = std::function<int(uint8_t* buf, size_t len)>;
    using WriteFn = std::function<int(const uint8_t* buf, size_t len)>;

    static std::vector<uint8_t> pack_frame(const Http2Frame& frame);
    static bool unpack_frame(const uint8_t* buf, size_t len, Http2Frame& out, size_t& consumed);

    // Execute a single HTTP/2 request/response exchange on an already-connected TLS ALPN "h2" stream.
    static Http2Response execute_request(
        const ReadFn& read_fn,
        const WriteFn& write_fn,
        const std::string& method,
        const std::string& scheme,
        const std::string& authority,
        const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& extra_headers,
        const std::string& req_body);

    // Multiplex N concurrent HTTP/2 streams (stream IDs 1, 3, 5, ...) over a single TLS ALPN "h2" connection.
    static std::vector<Http2Response> execute_multiplexed_batch(
        const ReadFn& read_fn,
        const WriteFn& write_fn,
        const std::vector<Http2RequestSpec>& requests);
};

} // namespace astra
