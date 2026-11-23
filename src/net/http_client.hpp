#pragma once

#include "net/http_cache.hpp"
#include "privacy/shield.hpp"

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
    std::string alpn_protocol; // e.g. "http/1.1"
    bool blocked_by_privacy = false;
    bool blocked_by_extension = false;
    bool from_cache = false;
    bool tls_verified = false;
    PrivacyDecision privacy_decision;
};

struct DnrHookResult {
    bool block = false;
    std::string redirect_url;
    std::vector<std::pair<std::string, std::string>> add_headers;
    std::string rule_source;
};

// AstraNet v3: Native HTTP/1.1 (GET & POST) + Strict-Verified TLS 1.3 + ALPN (dlopen libssl.so.3)
// + gzip/deflate (dlopen libz.so.1) + bounded RAM/Disk HttpCache + AstraShield + DNR
// + bounded multi-threaded parallel subresource batch fetcher.
class HttpClient {
public:
    explicit HttpClient(AstraShield& shield, HttpCache* cache = nullptr);

    using DnrCallback = std::function<DnrHookResult(const std::string& url, const std::string& top_site)>;
    void set_dnr_hook(DnrCallback cb) { dnr_hook_ = std::move(cb); }
    void set_strict_tls(bool enabled) { strict_tls_ = enabled; }
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

    static bool decompress_gzip_or_zlib(const uint8_t* data, size_t len, std::string& out);
    static bool tls_available();
    static std::string resolve_relative_url(const std::string& base_url, const std::string& rel);

private:
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
    mutable std::mutex state_mu_;
};

} // namespace astra
