#include "net/http_client.hpp"
#include "net/http2_client.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <netdb.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

namespace astra {

namespace {

// Dynamic OpenSSL 3.x ABI loader with strict certificate + hostname verification + ALPN + TLS version
struct OpenSslApi {
    void* ssl_lib = nullptr;
    void* crypto_lib = nullptr;
    bool loaded = false;

    using TLS_client_method_fn = const void* (*)();
    using SSL_CTX_new_fn = void* (*)(const void*);
    using SSL_CTX_free_fn = void (*)(void*);
    using SSL_CTX_set_default_verify_paths_fn = int (*)(void*);
    using SSL_CTX_set_verify_fn = void (*)(void*, int, void*);
    using SSL_CTX_set_alpn_protos_fn = int (*)(void*, const unsigned char*, unsigned int);
    using SSL_new_fn = void* (*)(void*);
    using SSL_free_fn = void (*)(void*);
    using SSL_set_fd_fn = int (*)(void*, int);
    using SSL_ctrl_fn = long (*)(void*, int, long, void*);
    using SSL_set1_host_fn = int (*)(void*, const char*);
    using SSL_get_verify_result_fn = long (*)(const void*);
    using SSL_get_version_fn = const char* (*)(const void*);
    using SSL_get0_alpn_selected_fn = void (*)(const void*, const unsigned char**, unsigned int*);
    using SSL_connect_fn = int (*)(void*);
    using SSL_write_fn = int (*)(void*, const void*, int);
    using SSL_read_fn = int (*)(void*, void*, int);
    using SSL_shutdown_fn = int (*)(void*);

    TLS_client_method_fn TLS_client_method = nullptr;
    SSL_CTX_new_fn SSL_CTX_new = nullptr;
    SSL_CTX_free_fn SSL_CTX_free = nullptr;
    SSL_CTX_set_default_verify_paths_fn SSL_CTX_set_default_verify_paths = nullptr;
    SSL_CTX_set_verify_fn SSL_CTX_set_verify = nullptr;
    SSL_CTX_set_alpn_protos_fn SSL_CTX_set_alpn_protos = nullptr;
    SSL_new_fn SSL_new = nullptr;
    SSL_free_fn SSL_free = nullptr;
    SSL_set_fd_fn SSL_set_fd = nullptr;
    SSL_ctrl_fn SSL_ctrl = nullptr;
    SSL_set1_host_fn SSL_set1_host = nullptr;
    SSL_get_verify_result_fn SSL_get_verify_result = nullptr;
    SSL_get_version_fn SSL_get_version = nullptr;
    SSL_get0_alpn_selected_fn SSL_get0_alpn_selected = nullptr;
    SSL_connect_fn SSL_connect = nullptr;
    SSL_write_fn SSL_write = nullptr;
    SSL_read_fn SSL_read = nullptr;
    SSL_shutdown_fn SSL_shutdown = nullptr;

    static constexpr int kSslCtrlSetTlsextHostname = 55;
    static constexpr long kTlsextNametypeHostName = 0;
    static constexpr int kSslVerifyPeer = 0x01;

    OpenSslApi() {
        crypto_lib = dlopen("libcrypto.so.3", RTLD_LAZY | RTLD_GLOBAL);
        if (!crypto_lib) crypto_lib = dlopen("libcrypto.so.1.1", RTLD_LAZY | RTLD_GLOBAL);
        ssl_lib = dlopen("libssl.so.3", RTLD_LAZY);
        if (!ssl_lib) ssl_lib = dlopen("libssl.so.1.1", RTLD_LAZY);
        if (!ssl_lib) return;

        TLS_client_method = reinterpret_cast<TLS_client_method_fn>(dlsym(ssl_lib, "TLS_client_method"));
        SSL_CTX_new = reinterpret_cast<SSL_CTX_new_fn>(dlsym(ssl_lib, "SSL_CTX_new"));
        SSL_CTX_free = reinterpret_cast<SSL_CTX_free_fn>(dlsym(ssl_lib, "SSL_CTX_free"));
        SSL_CTX_set_default_verify_paths = reinterpret_cast<SSL_CTX_set_default_verify_paths_fn>(
            dlsym(ssl_lib, "SSL_CTX_set_default_verify_paths"));
        SSL_CTX_set_verify = reinterpret_cast<SSL_CTX_set_verify_fn>(dlsym(ssl_lib, "SSL_CTX_set_verify"));
        SSL_CTX_set_alpn_protos = reinterpret_cast<SSL_CTX_set_alpn_protos_fn>(
            dlsym(ssl_lib, "SSL_CTX_set_alpn_protos"));
        SSL_new = reinterpret_cast<SSL_new_fn>(dlsym(ssl_lib, "SSL_new"));
        SSL_free = reinterpret_cast<SSL_free_fn>(dlsym(ssl_lib, "SSL_free"));
        SSL_set_fd = reinterpret_cast<SSL_set_fd_fn>(dlsym(ssl_lib, "SSL_set_fd"));
        SSL_ctrl = reinterpret_cast<SSL_ctrl_fn>(dlsym(ssl_lib, "SSL_ctrl"));
        SSL_set1_host = reinterpret_cast<SSL_set1_host_fn>(dlsym(ssl_lib, "SSL_set1_host"));
        SSL_get_verify_result = reinterpret_cast<SSL_get_verify_result_fn>(dlsym(ssl_lib, "SSL_get_verify_result"));
        SSL_get_version = reinterpret_cast<SSL_get_version_fn>(dlsym(ssl_lib, "SSL_get_version"));
        SSL_get0_alpn_selected = reinterpret_cast<SSL_get0_alpn_selected_fn>(
            dlsym(ssl_lib, "SSL_get0_alpn_selected"));
        SSL_connect = reinterpret_cast<SSL_connect_fn>(dlsym(ssl_lib, "SSL_connect"));
        SSL_write = reinterpret_cast<SSL_write_fn>(dlsym(ssl_lib, "SSL_write"));
        SSL_read = reinterpret_cast<SSL_read_fn>(dlsym(ssl_lib, "SSL_read"));
        SSL_shutdown = reinterpret_cast<SSL_shutdown_fn>(dlsym(ssl_lib, "SSL_shutdown"));

        loaded = TLS_client_method && SSL_CTX_new && SSL_CTX_free && SSL_new &&
                 SSL_free && SSL_set_fd && SSL_ctrl && SSL_connect &&
                 SSL_write && SSL_read && SSL_shutdown;
    }
};

OpenSslApi& openssl() {
    static OpenSslApi api;
    return api;
}

struct ZlibStream {
    const uint8_t* next_in;
    unsigned int avail_in;
    unsigned long total_in;
    uint8_t* next_out;
    unsigned int avail_out;
    unsigned long total_out;
    const char* msg;
    void* state;
    void* zalloc;
    void* zfree;
    void* opaque;
    int data_type;
    unsigned long adler;
    unsigned long reserved;
};

struct ZlibApi {
    void* lib = nullptr;
    bool loaded = false;

    using inflateInit2_fn = int (*)(ZlibStream*, int, const char*, int);
    using inflate_fn = int (*)(ZlibStream*, int);
    using inflateEnd_fn = int (*)(ZlibStream*);
    using zlibVersion_fn = const char* (*)();

    inflateInit2_fn inflateInit2_ = nullptr;
    inflate_fn inflate = nullptr;
    inflateEnd_fn inflateEnd = nullptr;
    zlibVersion_fn zlibVersion = nullptr;

    ZlibApi() {
        lib = dlopen("libz.so.1", RTLD_LAZY);
        if (!lib) lib = dlopen("libz.so", RTLD_LAZY);
        if (!lib) return;
        inflateInit2_ = reinterpret_cast<inflateInit2_fn>(dlsym(lib, "inflateInit2_"));
        inflate = reinterpret_cast<inflate_fn>(dlsym(lib, "inflate"));
        inflateEnd = reinterpret_cast<inflateEnd_fn>(dlsym(lib, "inflateEnd"));
        zlibVersion = reinterpret_cast<zlibVersion_fn>(dlsym(lib, "zlibVersion"));
        loaded = inflateInit2_ && inflate && inflateEnd && zlibVersion;
    }
};

ZlibApi& zlib_api() {
    static ZlibApi api;
    return api;
}

struct BrotliApi {
    void* lib = nullptr;
    bool loaded = false;

    using CreateInstance_fn = void* (*)(void*, void*, void*);
    using DestroyInstance_fn = void (*)(void*);
    // BrotliDecoderResult: 0=ERROR, 1=SUCCESS, 2=NEEDS_MORE_INPUT, 3=NEEDS_MORE_OUTPUT
    using DecompressStream_fn = int (*)(void*, size_t*, const uint8_t**, size_t*, uint8_t**, size_t*);

    CreateInstance_fn create_instance = nullptr;
    DestroyInstance_fn destroy_instance = nullptr;
    DecompressStream_fn decompress_stream = nullptr;

    BrotliApi() {
        lib = dlopen("libbrotlidec.so.1", RTLD_LAZY);
        if (!lib) lib = dlopen("libbrotlidec.so", RTLD_LAZY);
        if (!lib) return;
        create_instance = reinterpret_cast<CreateInstance_fn>(dlsym(lib, "BrotliDecoderCreateInstance"));
        destroy_instance = reinterpret_cast<DestroyInstance_fn>(dlsym(lib, "BrotliDecoderDestroyInstance"));
        decompress_stream = reinterpret_cast<DecompressStream_fn>(dlsym(lib, "BrotliDecoderDecompressStream"));
        loaded = create_instance && destroy_instance && decompress_stream;
    }
};

BrotliApi& brotli_api() {
    static BrotliApi api;
    return api;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

std::string decode_chunked(const std::string& raw) {
    std::string out;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t line_end = raw.find("\r\n", pos);
        if (line_end == std::string::npos) break;
        std::string hex_str = raw.substr(pos, line_end - pos);
        size_t semi = hex_str.find(';');
        if (semi != std::string::npos) hex_str.resize(semi);
        size_t chunk_size = std::strtoul(hex_str.c_str(), nullptr, 16);
        pos = line_end + 2;
        if (chunk_size == 0) break;
        if (pos + chunk_size > raw.size()) {
            out.append(raw.data() + pos, raw.size() - pos);
            break;
        }
        out.append(raw.data() + pos, chunk_size);
        pos += chunk_size + 2;
    }
    return out;
}

int connect_tcp(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        return -1;
    }

    int fd = -1;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        timeval tv{};
        tv.tv_sec = 8;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

} // namespace

bool HttpClient::tls_available() {
    return openssl().loaded;
}

bool HttpClient::brotli_available() {
    return brotli_api().loaded;
}

bool HttpClient::decompress_brotli(const uint8_t* data, size_t len, std::string& out) {
    auto& br = brotli_api();
    if (!br.loaded || !data || len == 0) return false;
    void* state = br.create_instance(nullptr, nullptr, nullptr);
    if (!state) return false;

    out.clear();
    size_t avail_in = len;
    const uint8_t* next_in = data;
    uint8_t buf[32768];
    bool ok = false;
    constexpr size_t kMaxOut = 32ULL * 1024 * 1024;

    for (;;) {
        size_t avail_out = sizeof(buf);
        uint8_t* next_out = buf;
        size_t total_out = 0;
        int res = br.decompress_stream(state, &avail_in, &next_in, &avail_out, &next_out, &total_out);
        size_t produced = sizeof(buf) - avail_out;
        if (produced > 0) {
            if (out.size() + produced > kMaxOut) {
                ok = false;
                break;
            }
            out.append(reinterpret_cast<const char*>(buf), produced);
        }
        if (res == 1) { // BROTLI_DECODER_RESULT_SUCCESS
            ok = true;
            break;
        }
        if (res == 3) { // BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT
            continue;
        }
        // ERROR (0) or NEEDS_MORE_INPUT (2) with no more data
        ok = (res == 2 && !out.empty());
        break;
    }

    br.destroy_instance(state);
    return ok;
}

std::string HttpClient::resolve_relative_url(const std::string& base_url, const std::string& rel) {
    if (rel.empty()) return base_url;
    if (rel.rfind("http://", 0) == 0 || rel.rfind("https://", 0) == 0 ||
        rel.rfind("file://", 0) == 0 || rel.rfind("astra://", 0) == 0) {
        return rel;
    }
    ParsedUrl base = parse_url(base_url);
    if (!base.valid) return rel;
    if (rel.rfind("//", 0) == 0) {
        return base.scheme + ":" + rel;
    }
    if (base.scheme == "file") {
        if (rel[0] == '/') return "file://" + rel;
        size_t slash = base.path.rfind('/');
        std::string dir = (slash == std::string::npos) ? "" : base.path.substr(0, slash + 1);
        return "file://" + dir + rel;
    }
    if (rel[0] == '/') {
        return base.origin() + rel;
    }
    size_t slash = base.path.rfind('/');
    std::string dir = (slash == std::string::npos) ? "/" : base.path.substr(0, slash + 1);
    return base.origin() + dir + rel;
}

bool HttpClient::decompress_gzip_or_zlib(const uint8_t* data, size_t len, std::string& out) {
    auto& z = zlib_api();
    if (!z.loaded || !data || len == 0) return false;

    ZlibStream strm{};
    strm.next_in = data;
    strm.avail_in = static_cast<unsigned int>(len);

    int rc = z.inflateInit2_(&strm, 47, z.zlibVersion(), static_cast<int>(sizeof(ZlibStream)));
    if (rc != 0) {
        strm = ZlibStream{};
        strm.next_in = data;
        strm.avail_in = static_cast<unsigned int>(len);
        rc = z.inflateInit2_(&strm, -15, z.zlibVersion(), static_cast<int>(sizeof(ZlibStream)));
        if (rc != 0) return false;
    }

    out.clear();
    uint8_t chunk[16384];
    int status = 0;
    do {
        strm.next_out = chunk;
        strm.avail_out = sizeof(chunk);
        status = z.inflate(&strm, 0);
        if (status != 0 && status != 1) {
            z.inflateEnd(&strm);
            return false;
        }
        size_t have = sizeof(chunk) - strm.avail_out;
        out.append(reinterpret_cast<const char*>(chunk), have);
        if (out.size() > 32 * 1024 * 1024) break;
    } while (status != 1 && strm.avail_out == 0);

    z.inflateEnd(&strm);
    return status == 1 || !out.empty();
}

HttpClient::HttpClient(AstraShield& shield, HttpCache* cache)
    : shield_(shield), cache_(cache) {}

HttpClient::~HttpClient() {
    std::lock_guard<std::mutex> lk(pool_mu_);
    for (auto& c : conn_pool_) close_conn(c);
    conn_pool_.clear();
}

void HttpClient::close_conn(PooledConn& conn) {
    auto& ssl_api = openssl();
    if (conn.ssl && ssl_api.loaded) {
        ssl_api.SSL_shutdown(conn.ssl);
        ssl_api.SSL_free(conn.ssl);
        conn.ssl = nullptr;
    }
    if (conn.ctx && ssl_api.loaded) {
        ssl_api.SSL_CTX_free(conn.ctx);
        conn.ctx = nullptr;
    }
    if (conn.fd >= 0) {
        ::close(conn.fd);
        conn.fd = -1;
    }
}

bool HttpClient::take_pooled(const std::string& key, PooledConn& out) {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    std::lock_guard<std::mutex> lk(pool_mu_);
    for (auto it = conn_pool_.begin(); it != conn_pool_.end();) {
        if (now - it->last_used_sec > 15) {
            close_conn(*it);
            it = conn_pool_.erase(it);
        } else if (it->key == key) {
            out = std::move(*it);
            conn_pool_.erase(it);
            return true;
        } else {
            ++it;
        }
    }
    return false;
}

void HttpClient::return_pooled(PooledConn&& conn) {
    if (conn.fd < 0) return;
    conn.last_used_sec = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
    std::lock_guard<std::mutex> lk(pool_mu_);
    while (conn_pool_.size() >= 8) {
        close_conn(conn_pool_.front());
        conn_pool_.erase(conn_pool_.begin());
    }
    conn_pool_.push_back(std::move(conn));
}

HttpResponse HttpClient::get(const std::string& url,
                             const std::string& top_level_site,
                             bool is_top_level_navigation,
                             int max_redirects) {
    std::string current_url = url;
    std::string top_site = top_level_site;

    for (int hop = 0; hop <= max_redirects; ++hop) {
        ParsedUrl prelim = parse_url(current_url);
        if (top_site.empty() && prelim.valid) {
            top_site = prelim.registrable_domain();
        }

        // Step 1: AstraShield inspection (thread-safe)
        PrivacyDecision pdec;
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            pdec = shield_.inspect_request(current_url, top_site, is_top_level_navigation);
        }
        if (pdec.blocked) {
            HttpResponse resp;
            resp.final_url = pdec.sanitized_url;
            resp.blocked_by_privacy = true;
            resp.privacy_decision = pdec;
            resp.error = pdec.reason;
            return resp;
        }
        current_url = pdec.sanitized_url;

        // Step 2: Extension Declarative Net Request (DNR)
        std::vector<std::pair<std::string, std::string>> ext_headers;
        if (dnr_hook_) {
            DnrHookResult dnr;
            {
                std::lock_guard<std::mutex> lk(state_mu_);
                dnr = dnr_hook_(current_url, top_site);
            }
            if (dnr.block) {
                HttpResponse resp;
                resp.final_url = current_url;
                resp.blocked_by_extension = true;
                resp.privacy_decision = pdec;
                resp.error = "Blocked by WebExtension DNR: " + dnr.rule_source;
                return resp;
            }
            if (!dnr.redirect_url.empty() && dnr.redirect_url != current_url) {
                current_url = dnr.redirect_url;
                continue;
            }
            ext_headers = std::move(dnr.add_headers);
        }

        ParsedUrl parsed = parse_url(current_url);
        if (!parsed.valid) {
            HttpResponse resp;
            resp.error = "Invalid URL: " + current_url;
            return resp;
        }
        std::string canonical_url = parsed.rebuild();

        // Step 3: Check bounded RAM/Disk HttpCache before opening any socket
        if (cache_) {
            CachedResource cached;
            bool hit = false;
            {
                std::lock_guard<std::mutex> lk(state_mu_);
                hit = cache_->lookup(canonical_url, cached);
            }
            if (hit) {
                HttpResponse resp;
                resp.status_code = 200;
                resp.final_url = canonical_url;
                resp.content_type = cached.content_type;
                resp.body = std::move(cached.body);
                resp.from_cache = true;
                resp.tls_verified = (canonical_url.rfind("https://", 0) == 0);
                resp.privacy_decision = pdec;
                return resp;
            }
        }

        HttpResponse resp = perform_single_request("GET", parsed, top_site, ext_headers, "", "");
        resp.privacy_decision = pdec;

        if ((resp.status_code == 301 || resp.status_code == 302 ||
             resp.status_code == 303 || resp.status_code == 307 || resp.status_code == 308) &&
            resp.headers.count("location") > 0 && hop < max_redirects) {
            current_url = resolve_relative_url(current_url, resp.headers["location"]);
            continue;
        }

        // Step 4: Populate bounded HttpCache on 200 OK
        if (cache_ && resp.status_code == 200 && !resp.body.empty()) {
            std::string cc = resp.headers.count("cache-control") ? resp.headers["cache-control"] : "";
            int64_t max_age = HttpCache::parse_max_age(cc);
            if (max_age > 0) {
                CachedResource cr;
                cr.url = resp.final_url;
                cr.content_type = resp.content_type;
                cr.etag = resp.headers.count("etag") ? resp.headers["etag"] : "";
                cr.body = resp.body;
                cr.max_age_sec = max_age;
                std::lock_guard<std::mutex> lk(state_mu_);
                cache_->store(cr);
            }
        }

        return resp;
    }

    HttpResponse err;
    err.error = "Too many redirects";
    return err;
}

HttpResponse HttpClient::request(const std::string& method,
                                 const std::string& url,
                                 const std::string& body,
                                 const std::vector<std::pair<std::string, std::string>>& extra_headers,
                                 const std::string& content_type,
                                 const std::string& top_level_site) {
    std::string upper_m = method.empty() ? "GET" : method;
    std::transform(upper_m.begin(), upper_m.end(), upper_m.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    ParsedUrl prelim = parse_url(url);
    std::string top_site = top_level_site.empty() && prelim.valid ? prelim.registrable_domain() : top_level_site;

    PrivacyDecision pdec;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        pdec = shield_.inspect_request(url, top_site, false);
    }
    if (pdec.blocked) {
        HttpResponse resp;
        resp.final_url = pdec.sanitized_url;
        resp.blocked_by_privacy = true;
        resp.privacy_decision = pdec;
        resp.error = pdec.reason;
        return resp;
    }

    std::vector<std::pair<std::string, std::string>> merged_headers = extra_headers;
    std::string effective_ct = content_type;
    for (const auto& [hk, hv] : extra_headers) {
        std::string lk = hk;
        std::transform(lk.begin(), lk.end(), lk.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lk == "content-type" && !hv.empty()) effective_ct = hv;
    }

    if (dnr_hook_) {
        DnrHookResult dnr;
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            dnr = dnr_hook_(pdec.sanitized_url, top_site);
        }
        if (dnr.block) {
            HttpResponse resp;
            resp.final_url = pdec.sanitized_url;
            resp.blocked_by_extension = true;
            resp.privacy_decision = pdec;
            resp.error = "Blocked by WebExtension DNR: " + dnr.rule_source;
            return resp;
        }
        for (const auto& h : dnr.add_headers) merged_headers.push_back(h);
    }

    ParsedUrl parsed = parse_url(pdec.sanitized_url);
    if (!parsed.valid) {
        HttpResponse resp;
        resp.error = "Invalid URL: " + url;
        return resp;
    }
    HttpResponse resp = perform_single_request(upper_m, parsed, top_site, merged_headers, body, effective_ct);
    resp.privacy_decision = pdec;
    return resp;
}

HttpResponse HttpClient::post(const std::string& url,
                              const std::string& body,
                              const std::string& content_type,
                              const std::string& top_level_site) {
    return request("POST", url, body, {}, content_type, top_level_site);
}

std::vector<HttpResponse> HttpClient::fetch_batch(const std::vector<std::string>& urls,
                                                  const std::string& top_level_site,
                                                  size_t max_threads) {
    std::vector<HttpResponse> results(urls.size());
    if (urls.empty()) return results;

    std::vector<bool> done(urls.size(), false);

    // Phase 1: Same-origin HTTP/2 Multi-Stream Multiplexing over a single TLS 1.3 socket
    if (http2_enabled_ && urls.size() >= 2 && tls_available()) {
        struct PendingItem {
            size_t idx = 0;
            ParsedUrl parsed;
            std::string top_site;
            PrivacyDecision pdec;
            std::vector<std::pair<std::string, std::string>> extra_hdrs;
        };
        std::map<std::string, std::vector<PendingItem>> origin_groups;

        for (size_t i = 0; i < urls.size(); ++i) {
            ParsedUrl prelim = parse_url(urls[i]);
            std::string top_site = top_level_site.empty() && prelim.valid
                                       ? prelim.registrable_domain()
                                       : top_level_site;
            PrivacyDecision pdec;
            {
                std::lock_guard<std::mutex> lk(state_mu_);
                pdec = shield_.inspect_request(urls[i], top_site, false);
            }
            if (pdec.blocked) {
                results[i].final_url = pdec.sanitized_url;
                results[i].blocked_by_privacy = true;
                results[i].privacy_decision = pdec;
                results[i].error = pdec.reason;
                done[i] = true;
                continue;
            }
            std::vector<std::pair<std::string, std::string>> ext_hdrs;
            if (dnr_hook_) {
                DnrHookResult dnr;
                {
                    std::lock_guard<std::mutex> lk(state_mu_);
                    dnr = dnr_hook_(pdec.sanitized_url, top_site);
                }
                if (dnr.block) {
                    results[i].final_url = pdec.sanitized_url;
                    results[i].blocked_by_extension = true;
                    results[i].privacy_decision = pdec;
                    results[i].error = "Blocked by WebExtension DNR: " + dnr.rule_source;
                    done[i] = true;
                    continue;
                }
                if (!dnr.redirect_url.empty()) continue; // Let regular get() handle redirects
                ext_hdrs = std::move(dnr.add_headers);
            }
            if (cache_) {
                CachedResource cached;
                bool hit = false;
                {
                    std::lock_guard<std::mutex> lk(state_mu_);
                    hit = cache_->lookup(pdec.sanitized_url, cached);
                }
                if (hit) {
                    results[i].status_code = 200;
                    results[i].final_url = pdec.sanitized_url;
                    results[i].content_type = cached.content_type;
                    results[i].body = std::move(cached.body);
                    results[i].from_cache = true;
                    results[i].tls_verified = (pdec.sanitized_url.rfind("https://", 0) == 0);
                    results[i].privacy_decision = pdec;
                    done[i] = true;
                    continue;
                }
            }
            ParsedUrl pu = parse_url(pdec.sanitized_url);
            if (pu.valid && pu.scheme == "https") {
                uint16_t port = pu.port ? pu.port : 443;
                std::string gkey = pu.host + ":" + std::to_string(port);
                origin_groups[gkey].push_back(PendingItem{i, std::move(pu), std::move(top_site), pdec, std::move(ext_hdrs)});
            }
        }

        auto& ssl_api = openssl();
        for (auto& [gkey, items] : origin_groups) {
            if (items.size() < 2) continue;
            const std::string& host = items[0].parsed.host;
            uint16_t port = items[0].parsed.port ? items[0].parsed.port : 443;

            PooledConn conn;
            conn.key = gkey + ":tls";
            conn.fd = connect_tcp(host, port);
            if (conn.fd < 0) continue;

            conn.ctx = ssl_api.SSL_CTX_new(ssl_api.TLS_client_method());
            if (conn.ctx && strict_tls_ && ssl_api.SSL_CTX_set_default_verify_paths && ssl_api.SSL_CTX_set_verify) {
                ssl_api.SSL_CTX_set_default_verify_paths(conn.ctx);
                ssl_api.SSL_CTX_set_verify(conn.ctx, OpenSslApi::kSslVerifyPeer, nullptr);
            }
            if (conn.ctx && ssl_api.SSL_CTX_set_alpn_protos) {
                static const unsigned char kAlpnH2[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
                ssl_api.SSL_CTX_set_alpn_protos(conn.ctx, kAlpnH2, sizeof(kAlpnH2));
            }
            conn.ssl = conn.ctx ? ssl_api.SSL_new(conn.ctx) : nullptr;
            if (!conn.ctx || !conn.ssl) {
                close_conn(conn);
                continue;
            }
            ssl_api.SSL_set_fd(conn.ssl, conn.fd);
            ssl_api.SSL_ctrl(conn.ssl, OpenSslApi::kSslCtrlSetTlsextHostname,
                             OpenSslApi::kTlsextNametypeHostName,
                             const_cast<char*>(host.c_str()));
            if (strict_tls_ && ssl_api.SSL_set1_host) {
                ssl_api.SSL_set1_host(conn.ssl, host.c_str());
            }
            if (ssl_api.SSL_connect(conn.ssl) != 1) {
                close_conn(conn);
                continue;
            }
            if (ssl_api.SSL_get_verify_result) conn.tls_verified = (ssl_api.SSL_get_verify_result(conn.ssl) == 0);
            if (ssl_api.SSL_get_version) {
                const char* ver = ssl_api.SSL_get_version(conn.ssl);
                if (ver) conn.tls_version = ver;
            }
            if (ssl_api.SSL_get0_alpn_selected) {
                const unsigned char* proto = nullptr;
                unsigned int plen = 0;
                ssl_api.SSL_get0_alpn_selected(conn.ssl, &proto, &plen);
                if (proto && plen > 0) conn.alpn.assign(reinterpret_cast<const char*>(proto), plen);
            }

            if (conn.alpn != "h2") {
                // Server chose HTTP/1.1 — save socket in keep-alive pool and fall through to Phase 2
                return_pooled(std::move(conn));
                continue;
            }

            std::vector<Http2RequestSpec> specs;
            specs.reserve(items.size());
            std::vector<std::pair<std::string, std::string>> hardened;
            {
                std::lock_guard<std::mutex> lk(state_mu_);
                hardened = shield_.hardened_headers();
            }

            for (const auto& item : items) {
                Http2RequestSpec sp;
                sp.method = "GET";
                sp.scheme = "https";
                sp.authority = item.parsed.host;
                sp.path = item.parsed.path.empty() ? "/" : item.parsed.path;
                if (!item.parsed.query.empty()) sp.path += "?" + item.parsed.query;
                for (const auto& kv : hardened) sp.extra_headers.push_back(kv);
                for (const auto& kv : item.extra_hdrs) sp.extra_headers.push_back(kv);
                std::string cookies;
                {
                    std::lock_guard<std::mutex> lk(state_mu_);
                    cookies = shield_.cookie_header_for(item.top_site, item.parsed.origin(), true);
                }
                if (!cookies.empty()) sp.extra_headers.emplace_back("cookie", cookies);
                specs.push_back(std::move(sp));
            }

            auto h2_resps = Http2Client::execute_multiplexed_batch(
                [&](uint8_t* b, size_t l) { return ssl_api.SSL_read(conn.ssl, b, static_cast<int>(l)); },
                [&](const uint8_t* b, size_t l) { return ssl_api.SSL_write(conn.ssl, b, static_cast<int>(l)); },
                specs);

            close_conn(conn);

            for (size_t k = 0; k < items.size() && k < h2_resps.size(); ++k) {
                auto& h2 = h2_resps[k];
                if (!h2.ok || (h2.status_code >= 300 && h2.status_code < 400)) {
                    continue; // Fall back to regular get() for redirects or errors
                }
                const auto& item = items[k];
                HttpResponse& r = results[item.idx];
                r.final_url = item.parsed.rebuild();
                r.status_code = h2.status_code;
                r.headers = std::move(h2.headers);
                r.tls_verified = conn.tls_verified;
                r.tls_version = conn.tls_version;
                r.alpn_protocol = "h2";
                r.used_http2 = true;
                r.multiplexed_h2 = true;
                r.privacy_decision = item.pdec;
                http2_requests_.fetch_add(1);
                http2_multiplexed_streams_.fetch_add(1);

                for (const auto& sc : h2.set_cookies) {
                    std::lock_guard<std::mutex> lk(state_mu_);
                    shield_.store_cookie(item.top_site, item.parsed.origin(), sc);
                }
                std::string body_raw = std::move(h2.body);
                if (r.headers.count("content-encoding")) {
                    std::string enc = to_lower(r.headers["content-encoding"]);
                    if (enc.find("br") != std::string::npos) {
                        std::string inflated;
                        if (decompress_brotli(reinterpret_cast<const uint8_t*>(body_raw.data()),
                                              body_raw.size(), inflated)) {
                            body_raw = std::move(inflated);
                        }
                    } else if (enc.find("gzip") != std::string::npos || enc.find("deflate") != std::string::npos) {
                        std::string inflated;
                        if (decompress_gzip_or_zlib(reinterpret_cast<const uint8_t*>(body_raw.data()),
                                                    body_raw.size(), inflated)) {
                            body_raw = std::move(inflated);
                        }
                    }
                }
                r.content_type = r.headers.count("content-type") ? r.headers["content-type"] : "text/html";
                r.body = std::move(body_raw);

                if (cache_ && r.status_code == 200 && !r.body.empty()) {
                    std::string cc = r.headers.count("cache-control") ? to_lower(r.headers["cache-control"]) : "";
                    if (cc.find("no-store") == std::string::npos) {
                        CachedResource cr;
                        cr.url = r.final_url;
                        cr.content_type = r.content_type;
                        cr.etag = r.headers.count("etag") ? r.headers["etag"] : "";
                        cr.body = r.body;
                        cr.max_age_sec = 300;
                        std::lock_guard<std::mutex> lk(state_mu_);
                        cache_->store(cr);
                    }
                }
                done[item.idx] = true;
            }
        }
    }

    // Phase 2: Bounded worker pool (max 4 threads) for any remaining non-multiplexed URLs
    std::vector<size_t> remaining_indices;
    for (size_t i = 0; i < urls.size(); ++i) {
        if (!done[i]) remaining_indices.push_back(i);
    }
    if (remaining_indices.empty()) return results;

    size_t workers = std::clamp<size_t>(max_threads, 1, std::min<size_t>(4, remaining_indices.size()));
    if (workers == 1) {
        for (size_t idx : remaining_indices) {
            results[idx] = get(urls[idx], top_level_site, false);
        }
        return results;
    }

    std::atomic<size_t> next_idx{0};
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t w = 0; w < workers; ++w) {
        pool.emplace_back([&]() {
            for (;;) {
                size_t k = next_idx.fetch_add(1);
                if (k >= remaining_indices.size()) break;
                size_t idx = remaining_indices[k];
                results[idx] = get(urls[idx], top_level_site, false);
            }
        });
    }
    for (auto& th : pool) th.join();
    return results;
}

HttpResponse HttpClient::perform_single_request(
    const std::string& method,
    const ParsedUrl& parsed,
    const std::string& top_level_site,
    const std::vector<std::pair<std::string, std::string>>& extra_headers,
    const std::string& req_body,
    const std::string& req_content_type) {
    HttpResponse resp;
    resp.final_url = parsed.rebuild();

    bool use_tls = (parsed.scheme == "https");
    uint16_t port = parsed.port ? parsed.port : (use_tls ? 443 : 80);
    std::string pool_key = parsed.host + ":" + std::to_string(port) + (use_tls ? ":tls" : ":tcp");

    std::vector<std::pair<std::string, std::string>> hardened;
    std::string cookies;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        hardened = shield_.hardened_headers();
        cookies = shield_.cookie_header_for(top_level_site, parsed.origin(), use_tls);
    }

    std::string target = parsed.path.empty() ? "/" : parsed.path;
    if (!parsed.query.empty()) target += "?" + parsed.query;

    auto build_http1_req = [&](bool keep_alive) {
        std::ostringstream req;
        req << method << " " << target << " HTTP/1.1\r\n";
        req << "Host: " << parsed.host << "\r\n";
        req << "Accept: text/html,application/xhtml+xml,text/css,image/webp,image/svg+xml,image/jpeg,image/png,*/*;q=0.8\r\n";
        req << "Accept-Encoding: " << (brotli_available() ? "gzip, deflate, br" : "gzip, deflate") << "\r\n";
        req << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
        for (const auto& [k, v] : hardened) req << k << ": " << v << "\r\n";
        for (const auto& [k, v] : extra_headers) req << k << ": " << v << "\r\n";
        if (!cookies.empty()) req << "Cookie: " << cookies << "\r\n";
        if (!req_body.empty() || method == "POST") {
            req << "Content-Type: " << (req_content_type.empty() ? "application/octet-stream" : req_content_type) << "\r\n";
            req << "Content-Length: " << req_body.size() << "\r\n";
        }
        req << "\r\n";
        if (!req_body.empty()) req << req_body;
        return req.str();
    };

    // Helper: read an HTTP/1.1 response honoring Content-Length / chunked boundary so keep-alive sockets can be reused!
    auto read_http1_stream = [&](const std::function<int(char*, size_t)>& rd, std::string& raw, bool& can_reuse) -> bool {
        raw.clear();
        can_reuse = false;
        char buf[8192];
        size_t hdr_end = std::string::npos;
        size_t content_len = 0;
        bool has_cl = false;
        bool is_chunked = false;
        bool server_close = false;

        for (;;) {
            int n = rd(buf, sizeof(buf));
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));
            if (hdr_end == std::string::npos) {
                hdr_end = raw.find("\r\n\r\n");
                if (hdr_end != std::string::npos) {
                    std::string hb = to_lower(raw.substr(0, hdr_end));
                    size_t clp = hb.find("\r\ncontent-length:");
                    if (clp != std::string::npos) {
                        size_t val_start = clp + 17;
                        size_t val_end = hb.find("\r\n", val_start);
                        std::string cls = trim(hb.substr(val_start, val_end == std::string::npos ? std::string::npos : val_end - val_start));
                        content_len = std::strtoul(cls.c_str(), nullptr, 10);
                        has_cl = true;
                    }
                    if (hb.find("transfer-encoding: chunked") != std::string::npos) is_chunked = true;
                    if (hb.find("connection: close") != std::string::npos) server_close = true;
                }
            }
            if (hdr_end != std::string::npos) {
                size_t body_got = raw.size() - (hdr_end + 4);
                if (has_cl && body_got >= content_len) {
                    raw.resize(hdr_end + 4 + content_len);
                    can_reuse = !server_close;
                    return true;
                }
                if (is_chunked && raw.find("\r\n0\r\n\r\n", hdr_end) != std::string::npos) {
                    can_reuse = !server_close;
                    return true;
                }
            }
            if (raw.size() > 16 * 1024 * 1024) break;
        }
        return !raw.empty();
    };

    PooledConn conn;
    bool from_pool = take_pooled(pool_key, conn);
    if (!from_pool) {
        conn.key = pool_key;
        conn.fd = connect_tcp(parsed.host, port);
        if (conn.fd < 0) {
            resp.error = "TCP connection failed to " + parsed.host + ":" + std::to_string(port);
            return resp;
        }
        if (use_tls) {
            auto& ssl_api = openssl();
            if (!ssl_api.loaded) {
                close_conn(conn);
                resp.error = "TLS unavailable: libssl.so.3 not found";
                return resp;
            }
            conn.ctx = ssl_api.SSL_CTX_new(ssl_api.TLS_client_method());
            if (conn.ctx && strict_tls_ && ssl_api.SSL_CTX_set_default_verify_paths && ssl_api.SSL_CTX_set_verify) {
                ssl_api.SSL_CTX_set_default_verify_paths(conn.ctx);
                ssl_api.SSL_CTX_set_verify(conn.ctx, OpenSslApi::kSslVerifyPeer, nullptr);
            }
            if (conn.ctx && ssl_api.SSL_CTX_set_alpn_protos) {
                if (http2_enabled_) {
                    static const unsigned char kAlpnH2[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
                    ssl_api.SSL_CTX_set_alpn_protos(conn.ctx, kAlpnH2, sizeof(kAlpnH2));
                } else {
                    static const unsigned char kAlpn11[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
                    ssl_api.SSL_CTX_set_alpn_protos(conn.ctx, kAlpn11, sizeof(kAlpn11));
                }
            }
            conn.ssl = conn.ctx ? ssl_api.SSL_new(conn.ctx) : nullptr;
            if (!conn.ctx || !conn.ssl) {
                close_conn(conn);
                resp.error = "Failed to initialize OpenSSL context";
                return resp;
            }
            ssl_api.SSL_set_fd(conn.ssl, conn.fd);
            ssl_api.SSL_ctrl(conn.ssl, OpenSslApi::kSslCtrlSetTlsextHostname,
                             OpenSslApi::kTlsextNametypeHostName,
                             const_cast<char*>(parsed.host.c_str()));
            if (strict_tls_ && ssl_api.SSL_set1_host) {
                ssl_api.SSL_set1_host(conn.ssl, parsed.host.c_str());
            }
            if (ssl_api.SSL_connect(conn.ssl) != 1) {
                close_conn(conn);
                resp.error = "TLS handshake or certificate verification failed for " + parsed.host;
                return resp;
            }
            if (ssl_api.SSL_get_verify_result) conn.tls_verified = (ssl_api.SSL_get_verify_result(conn.ssl) == 0);
            if (ssl_api.SSL_get_version) {
                const char* ver = ssl_api.SSL_get_version(conn.ssl);
                if (ver) conn.tls_version = ver;
            }
            if (ssl_api.SSL_get0_alpn_selected) {
                const unsigned char* proto = nullptr;
                unsigned int plen = 0;
                ssl_api.SSL_get0_alpn_selected(conn.ssl, &proto, &plen);
                if (proto && plen > 0) conn.alpn.assign(reinterpret_cast<const char*>(proto), plen);
            }
        }
    } else {
        resp.reused_connection = true;
        keepalive_reuses_.fetch_add(1);
    }

    resp.tls_verified = conn.tls_verified;
    resp.tls_version = conn.tls_version;
    resp.alpn_protocol = conn.alpn;

    // Fast path: Native Binary HTTP/2 when server negotiated ALPN "h2"
    if (use_tls && conn.alpn == "h2") {
        auto& ssl_api = openssl();
        std::vector<std::pair<std::string, std::string>> h2_hdrs;
        for (const auto& kv : hardened) h2_hdrs.push_back(kv);
        for (const auto& kv : extra_headers) h2_hdrs.push_back(kv);
        if (!cookies.empty()) h2_hdrs.emplace_back("cookie", cookies);
        if (!req_body.empty() || method == "POST") {
            h2_hdrs.emplace_back("content-type", req_content_type.empty() ? "application/octet-stream" : req_content_type);
            h2_hdrs.emplace_back("content-length", std::to_string(req_body.size()));
        }

        auto h2 = Http2Client::execute_request(
            [&](uint8_t* b, size_t l) { return ssl_api.SSL_read(conn.ssl, b, static_cast<int>(l)); },
            [&](const uint8_t* b, size_t l) { return ssl_api.SSL_write(conn.ssl, b, static_cast<int>(l)); },
            method, "https", parsed.host, target, h2_hdrs, req_body);

        close_conn(conn);
        if (!h2.ok) {
            resp.error = h2.error;
            return resp;
        }
        resp.used_http2 = true;
        http2_requests_.fetch_add(1);
        resp.status_code = h2.status_code;
        resp.headers = std::move(h2.headers);
        for (const auto& sc : h2.set_cookies) {
            std::lock_guard<std::mutex> lk(state_mu_);
            shield_.store_cookie(top_level_site, parsed.origin(), sc);
        }
        std::string body_raw = std::move(h2.body);
        if (resp.headers.count("content-encoding")) {
            std::string enc = to_lower(resp.headers["content-encoding"]);
            if (enc.find("br") != std::string::npos) {
                std::string inflated;
                if (decompress_brotli(reinterpret_cast<const uint8_t*>(body_raw.data()),
                                      body_raw.size(), inflated)) {
                    body_raw = std::move(inflated);
                }
            } else if (enc.find("gzip") != std::string::npos || enc.find("deflate") != std::string::npos) {
                std::string inflated;
                if (decompress_gzip_or_zlib(reinterpret_cast<const uint8_t*>(body_raw.data()),
                                            body_raw.size(), inflated)) {
                    body_raw = std::move(inflated);
                }
            }
        }
        resp.content_type = resp.headers.count("content-type") ? resp.headers["content-type"] : "text/html";
        resp.body = std::move(body_raw);
        return resp;
    }

    // HTTP/1.1 path (with keep-alive socket pooling & automatic stale-socket retry)
    std::string req_str = build_http1_req(true);
    std::string raw_response;
    bool can_reuse = false;

    auto send_and_read = [&]() -> bool {
        if (use_tls) {
            auto& ssl_api = openssl();
            size_t sent = 0;
            while (sent < req_str.size()) {
                int n = ssl_api.SSL_write(conn.ssl, req_str.data() + sent, static_cast<int>(req_str.size() - sent));
                if (n <= 0) return false;
                sent += static_cast<size_t>(n);
            }
            return read_http1_stream(
                [&](char* b, size_t l) { return ssl_api.SSL_read(conn.ssl, b, static_cast<int>(l)); },
                raw_response, can_reuse);
        } else {
            size_t sent = 0;
            while (sent < req_str.size()) {
                ssize_t n = ::send(conn.fd, req_str.data() + sent, req_str.size() - sent, MSG_NOSIGNAL);
                if (n <= 0) return false;
                sent += static_cast<size_t>(n);
            }
            return read_http1_stream(
                [&](char* b, size_t l) { return static_cast<int>(::recv(conn.fd, b, l, 0)); },
                raw_response, can_reuse);
        }
    };

    bool io_ok = send_and_read();
    if (!io_ok && from_pool) {
        // Stale pooled keep-alive connection: close and retry once fresh
        close_conn(conn);
        return perform_single_request(method, parsed, top_level_site, extra_headers, req_body, req_content_type);
    }

    if (can_reuse) return_pooled(std::move(conn));
    else close_conn(conn);

    size_t hdr_end = raw_response.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        resp.error = "Malformed HTTP response from " + parsed.host;
        return resp;
    }

    std::string hdr_block = raw_response.substr(0, hdr_end);
    std::string body_raw = raw_response.substr(hdr_end + 4);

    std::istringstream hss(hdr_block);
    std::string status_line;
    if (std::getline(hss, status_line)) {
        std::istringstream sl(status_line);
        std::string http_ver;
        sl >> http_ver >> resp.status_code;
    }

    std::string line;
    while (std::getline(hss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = to_lower(trim(line.substr(0, colon)));
        std::string val = trim(line.substr(colon + 1));
        if (key == "set-cookie") {
            std::lock_guard<std::mutex> lk(state_mu_);
            shield_.store_cookie(top_level_site, parsed.origin(), val);
        } else {
            resp.headers[key] = val;
        }
    }

    if (resp.headers.count("transfer-encoding") &&
        to_lower(resp.headers["transfer-encoding"]).find("chunked") != std::string::npos) {
        body_raw = decode_chunked(body_raw);
    }

    if (resp.headers.count("content-encoding")) {
        std::string enc = to_lower(resp.headers["content-encoding"]);
        if (enc.find("br") != std::string::npos) {
            std::string inflated;
            if (decompress_brotli(reinterpret_cast<const uint8_t*>(body_raw.data()),
                                  body_raw.size(), inflated)) {
                body_raw = std::move(inflated);
            }
        } else if (enc.find("gzip") != std::string::npos || enc.find("deflate") != std::string::npos) {
            std::string inflated;
            if (decompress_gzip_or_zlib(reinterpret_cast<const uint8_t*>(body_raw.data()),
                                        body_raw.size(), inflated)) {
                body_raw = std::move(inflated);
            }
        }
    }

    resp.content_type = resp.headers.count("content-type") ? resp.headers["content-type"] : "text/html";
    resp.body = std::move(body_raw);
    return resp;
}

} // namespace astra
