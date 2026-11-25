#pragma once

#include "net/http_cache.hpp"
#include "privacy/shield.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace astra {

struct HttpResponse {
    int status_code = 0;
    std::string final_url;
    std::string content_type;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string error;
    std::string tls_version;   // e.g. "TLSv1.3"
    std::string alpn_protocol; // e.g. "h2" or "http/1.1"
    bool blocked_by_privacy = false;
    bool blocked_by_extension = false;
    bool from_cache = false;
    bool tls_verified = false;
    bool used_http2 = false;
    bool multiplexed_h2 = false;
    bool reused_connection = false;
    // Session 8: la respuesta se sirvió desde la caché tras confirmar con el
    // servidor mediante un GET condicional que devolvió 304 Not Modified.
    bool revalidated = false;
    PrivacyDecision privacy_decision;
};

struct DnrHookResult {
    bool block = false;
    std::string redirect_url;
    std::vector<std::pair<std::string, std::string>> add_headers;
    std::string rule_source;
};

// AstraNet v4: Native Binary HTTP/2 (RFC 7540 + HPACK/Huffman) + HTTP/1.1 Keep-Alive Pool
// + Strict-Verified TLS 1.3 + ALPN (h2, http/1.1 via dlopen libssl.so.3)
// + gzip/deflate (dlopen libz.so.1) + bounded RAM/Disk HttpCache + AstraShield + DNR
// + bounded multi-threaded parallel subresource batch fetcher.
class HttpClient {
public:
    explicit HttpClient(AstraShield& shield, HttpCache* cache = nullptr);
    ~HttpClient();

    using DnrCallback = std::function<DnrHookResult(const std::string& url, const std::string& top_site)>;
    void set_dnr_hook(DnrCallback cb) { dnr_hook_ = std::move(cb); }
    void set_strict_tls(bool enabled) { strict_tls_ = enabled; }
    void set_http2_enabled(bool enabled) { http2_enabled_ = enabled; }
    void set_cache(HttpCache* cache) { cache_ = cache; }

    HttpResponse get(const std::string& url,
                     const std::string& top_level_site = "",
                     bool is_top_level_navigation = true,
                     int max_redirects = 5);

    HttpResponse post(const std::string& url,
                      const std::string& body,
                      const std::string& content_type = "application/json",
                      const std::string& top_level_site = "");

    // Fetch multiple subresources concurrently using a bounded worker pool (default max 4 threads).
    std::vector<HttpResponse> fetch_batch(const std::vector<std::string>& urls,
                                          const std::string& top_level_site = "",
                                          size_t max_threads = 4);

    size_t http2_requests() const { return http2_requests_.load(); }
    size_t http2_multiplexed_streams() const { return http2_multiplexed_streams_.load(); }
    size_t keepalive_reuses() const { return keepalive_reuses_.load(); }

    static bool decompress_gzip_or_zlib(const uint8_t* data, size_t len, std::string& out);
    static bool tls_available();
    static std::string resolve_relative_url(const std::string& base_url, const std::string& rel);

private:
    struct PooledConn {
        std::string key; // host:port:tls
        int fd = -1;
        void* ctx = nullptr;
        void* ssl = nullptr;
        std::string tls_version;
        std::string alpn;
        bool tls_verified = false;
        int64_t last_used_sec = 0;
    };

    bool take_pooled(const std::string& key, PooledConn& out);
    void return_pooled(PooledConn&& conn);
    void close_conn(PooledConn& conn);

    HttpResponse perform_single_request(const std::string& method,
                                        const ParsedUrl& parsed,
                                        const std::string& top_level_site,
                                        const std::vector<std::pair<std::string, std::string>>& extra_headers,
                                        const std::string& req_body,
                                        const std::string& req_content_type);

    AstraShield& shield_;
    HttpCache* cache_ = nullptr;
    DnrCallback dnr_hook_;
    bool strict_tls_ = true;
    bool http2_enabled_ = true;
    std::atomic<size_t> http2_requests_{0};
    std::atomic<size_t> http2_multiplexed_streams_{0};
    std::atomic<size_t> keepalive_reuses_{0};
    mutable std::mutex state_mu_;
    std::mutex pool_mu_;
    std::vector<PooledConn> conn_pool_;
};

} // namespace astra
