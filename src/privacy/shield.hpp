#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace astra {

enum class TrackerCategory {
    None,
    Advertising,
    Analytics,
    Fingerprinting,
    Cryptomining,
    SocialTracker
};

struct ParsedUrl {
    std::string scheme;
    std::string host;
    uint16_t port = 0;
    std::string path;
    std::string query;
    std::string fragment;
    bool valid = false;

    std::string origin() const;
    std::string registrable_domain() const;
    std::string rebuild() const;
};

ParsedUrl parse_url(std::string_view raw);

enum class SameSiteMode {
    Lax,
    Strict,
    None
};

struct CookieEntry {
    std::string name;
    std::string value;
    std::string domain;
    std::string path = "/";
    bool secure = false;
    bool http_only = false;
    SameSiteMode same_site = SameSiteMode::Lax;
};

struct PrivacyDecision {
    bool blocked = false;
    TrackerCategory category = TrackerCategory::None;
    std::string reason;
    std::string sanitized_url;
    bool upgraded_to_https = false;
    size_t params_stripped = 0;
};

struct ShieldStats {
    uint64_t requests_inspected = 0;
    uint64_t trackers_blocked = 0;
    uint64_t params_stripped = 0;
    uint64_t https_upgrades = 0;
    uint64_t cookies_partitioned = 0;
};

// Reverse-label domain suffix trie for O(host_len) tracker matching.
class DomainSuffixTrie {
public:
    DomainSuffixTrie();
    void insert(std::string_view domain, TrackerCategory category);
    TrackerCategory match(std::string_view host) const;
    size_t rule_count() const { return rule_count_; }

private:
    struct Node {
        TrackerCategory category = TrackerCategory::None;
        std::unordered_map<std::string, std::unique_ptr<Node>> children;
    };
    std::unique_ptr<Node> root_;
    size_t rule_count_ = 0;
};

// AstraShield: Firefox Enhanced Tracking Protection + Total Cookie Protection
// + Fingerprint Hardening + HTTPS-Only Mode.
class AstraShield {
public:
    AstraShield();

    void set_strict_mode(bool enabled) { strict_mode_ = enabled; }
    void set_https_only(bool enabled) { https_only_ = enabled; }
    void set_fingerprint_hardening(bool enabled) { fingerprint_hardening_ = enabled; }

    void add_tracker_domain(std::string_view domain, TrackerCategory cat);
    void allowlist_site(std::string_view top_level_site);

    PrivacyDecision inspect_request(std::string_view raw_url,
                                    std::string_view top_level_site,
                                    bool is_top_level_navigation = false);

    // Total Cookie Protection: cookies are isolated inside (top_level_site, origin)
    // partition keys so third-party embeds cannot share state across sites.
    void store_cookie(std::string_view top_level_site,
                      std::string_view request_origin,
                      std::string_view set_cookie_header);

    std::string cookie_header_for(std::string_view top_level_site,
                                  std::string_view request_origin,
                                  bool is_https,
                                  bool is_top_level_get = true) const;

    // Deterministic per-partition canvas fingerprint noise seed.
    uint64_t canvas_noise_seed(std::string_view top_level_site) const;

    // Hardened privacy headers (normalized UA, Sec-GPC: 1, DNT: 1).
    std::vector<std::pair<std::string, std::string>> hardened_headers() const;

    const ShieldStats& stats() const { return stats_; }
    size_t blocklist_size() const { return trie_.rule_count(); }

private:
    std::string strip_tracking_params(std::string_view query, size_t& stripped_count) const;

    DomainSuffixTrie trie_;
    bool strict_mode_ = true;
    bool https_only_ = true;
    bool fingerprint_hardening_ = true;
    uint64_t session_salt_ = 0xA57A2026C0FFEEULL;
    std::unordered_map<std::string, bool> allowlist_;
    // Partition key: "<top_level_site>|<origin>" -> cookie name -> entry
    std::map<std::string, std::map<std::string, CookieEntry>> cookie_jars_;
    ShieldStats stats_;
};

const char* tracker_category_name(TrackerCategory c);

} // namespace astra
