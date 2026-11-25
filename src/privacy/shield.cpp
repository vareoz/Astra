#include "privacy/shield.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace astra {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string> split_labels_reversed(std::string_view host) {
    std::vector<std::string> labels;
    std::string lower = to_lower(host);
    size_t start = 0;
    while (start <= lower.size()) {
        size_t dot = lower.find('.', start);
        if (dot == std::string::npos) {
            if (start < lower.size()) labels.push_back(lower.substr(start));
            break;
        }
        if (dot > start) labels.push_back(lower.substr(start, dot - start));
        start = dot + 1;
    }
    std::reverse(labels.begin(), labels.end());
    return labels;
}

const std::unordered_set<std::string>& tracking_query_params() {
    static const std::unordered_set<std::string> kParams = {
        "utm_source", "utm_medium", "utm_campaign", "utm_term", "utm_content",
        "utm_id", "fbclid", "gclid", "dclid", "msclkid", "mc_cid", "mc_eid",
        "igshid", "yclid", "_hsenc", "_hsmi", "ref_src", "vero_id", "oly_anon_id"
    };
    return kParams;
}

} // namespace

std::string ParsedUrl::origin() const {
    if (!valid) return {};
    std::string out = scheme + "://" + host;
    bool default_port = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
    if (port != 0 && !default_port) {
        out += ":" + std::to_string(port);
    }
    return out;
}

std::string ParsedUrl::registrable_domain() const {
    if (host.empty()) return {};
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        if (dot == std::string::npos) {
            parts.push_back(host.substr(start));
            break;
        }
        parts.push_back(host.substr(start, dot - start));
        start = dot + 1;
    }
    if (parts.size() <= 2) return host;
    return parts[parts.size() - 2] + "." + parts[parts.size() - 1];
}

std::string ParsedUrl::rebuild() const {
    if (!valid) return {};
    std::string out = scheme + "://";
    if (!host.empty()) {
        out += host;
        bool default_port = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
        if (port != 0 && !default_port) {
            out += ":" + std::to_string(port);
        }
    }
    out += path.empty() ? "/" : path;
    if (!query.empty()) out += "?" + query;
    if (!fragment.empty()) out += "#" + fragment;
    return out;
}

ParsedUrl parse_url(std::string_view raw) {
    ParsedUrl u;
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.remove_prefix(1);
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.remove_suffix(1);
    if (raw.empty()) return u;

    size_t scheme_end = raw.find("://");
    if (scheme_end == std::string_view::npos) return u;
    u.scheme = to_lower(raw.substr(0, scheme_end));
    std::string_view rest = raw.substr(scheme_end + 3);

    size_t frag_pos = rest.find('#');
    if (frag_pos != std::string_view::npos) {
        u.fragment = std::string(rest.substr(frag_pos + 1));
        rest = rest.substr(0, frag_pos);
    }

    size_t query_pos = rest.find('?');
    if (query_pos != std::string_view::npos) {
        u.query = std::string(rest.substr(query_pos + 1));
        rest = rest.substr(0, query_pos);
    }

    if (u.scheme == "file" || u.scheme == "astra") {
        u.path = std::string(rest);
        u.valid = true;
        return u;
    }

    size_t slash_pos = rest.find('/');
    std::string_view authority = (slash_pos == std::string_view::npos) ? rest : rest.substr(0, slash_pos);
    u.path = (slash_pos == std::string_view::npos) ? "/" : std::string(rest.substr(slash_pos));

    size_t colon_pos = authority.rfind(':');
    if (colon_pos != std::string_view::npos) {
        u.host = to_lower(authority.substr(0, colon_pos));
        int p = std::atoi(std::string(authority.substr(colon_pos + 1)).c_str());
        u.port = (p > 0 && p <= 65535) ? static_cast<uint16_t>(p) : 0;
    } else {
        u.host = to_lower(authority);
        u.port = (u.scheme == "https") ? 443 : (u.scheme == "http" ? 80 : 0);
    }

    u.valid = !u.host.empty();
    return u;
}

DomainSuffixTrie::DomainSuffixTrie() : root_(std::make_unique<Node>()) {}

void DomainSuffixTrie::insert(std::string_view domain, TrackerCategory category) {
    auto labels = split_labels_reversed(domain);
    if (labels.empty()) return;
    Node* cur = root_.get();
    for (const auto& label : labels) {
        auto& next = cur->children[label];
        if (!next) next = std::make_unique<Node>();
        cur = next.get();
    }
    if (cur->category == TrackerCategory::None) ++rule_count_;
    cur->category = category;
}

TrackerCategory DomainSuffixTrie::match(std::string_view host) const {
    auto labels = split_labels_reversed(host);
    const Node* cur = root_.get();
    TrackerCategory best = TrackerCategory::None;
    for (const auto& label : labels) {
        auto it = cur->children.find(label);
        if (it == cur->children.end()) break;
        cur = it->second.get();
        if (cur->category != TrackerCategory::None) {
            best = cur->category;
        }
    }
    return best;
}

AstraShield::AstraShield() {
    // Built-in Disconnect/EasyPrivacy-inspired curated tracker ruleset
    const std::pair<const char*, TrackerCategory> builtin_rules[] = {
        {"doubleclick.net", TrackerCategory::Advertising},
        {"googlesyndication.com", TrackerCategory::Advertising},
        {"googleadservices.com", TrackerCategory::Advertising},
        {"adnxs.com", TrackerCategory::Advertising},
        {"taboola.com", TrackerCategory::Advertising},
        {"outbrain.com", TrackerCategory::Advertising},
        {"criteo.com", TrackerCategory::Advertising},
        {"pubmatic.com", TrackerCategory::Advertising},
        {"rubiconproject.com", TrackerCategory::Advertising},
        {"amazon-adsystem.com", TrackerCategory::Advertising},
        {"google-analytics.com", TrackerCategory::Analytics},
        {"googletagmanager.com", TrackerCategory::Analytics},
        {"hotjar.com", TrackerCategory::Analytics},
        {"mixpanel.com", TrackerCategory::Analytics},
        {"segment.io", TrackerCategory::Analytics},
        {"amplitude.com", TrackerCategory::Analytics},
        {"scorecardresearch.com", TrackerCategory::Analytics},
        {"quantserve.com", TrackerCategory::Analytics},
        {"clarity.ms", TrackerCategory::Analytics},
        {"fingerprintjs.com", TrackerCategory::Fingerprinting},
        {"fpjs.io", TrackerCategory::Fingerprinting},
        {"iovation.com", TrackerCategory::Fingerprinting},
        {"threatmetrix.com", TrackerCategory::Fingerprinting},
        {"coinhive.com", TrackerCategory::Cryptomining},
        {"coin-hive.com", TrackerCategory::Cryptomining},
        {"cryptoloot.pro", TrackerCategory::Cryptomining},
        {"connect.facebook.net", TrackerCategory::SocialTracker},
        {"pixel.facebook.com", TrackerCategory::SocialTracker},
        {"ads.tiktok.com", TrackerCategory::SocialTracker},
        {"analytics.tiktok.com", TrackerCategory::SocialTracker},
        {"bat.bing.com", TrackerCategory::Analytics},
        {"px.ads.linkedin.com", TrackerCategory::SocialTracker}
    };
    for (const auto& [domain, cat] : builtin_rules) {
        trie_.insert(domain, cat);
    }
}

void AstraShield::add_tracker_domain(std::string_view domain, TrackerCategory cat) {
    trie_.insert(domain, cat);
}

void AstraShield::allowlist_site(std::string_view top_level_site) {
    allowlist_[to_lower(top_level_site)] = true;
}

std::string AstraShield::strip_tracking_params(std::string_view query, size_t& stripped_count) const {
    if (query.empty()) return {};
    const auto& blocked = tracking_query_params();
    std::string out;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        std::string_view pair = (amp == std::string_view::npos) ? query.substr(start) : query.substr(start, amp - start);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            std::string key = to_lower(eq == std::string_view::npos ? pair : pair.substr(0, eq));
            if (blocked.count(key) > 0 || key.rfind("utm_", 0) == 0) {
                ++stripped_count;
            } else {
                if (!out.empty()) out.push_back('&');
                out.append(pair);
            }
        }
        if (amp == std::string_view::npos) break;
        start = amp + 1;
    }
    return out;
}

PrivacyDecision AstraShield::inspect_request(std::string_view raw_url,
                                             std::string_view top_level_site,
                                             bool is_top_level_navigation) {
    ++stats_.requests_inspected;
    PrivacyDecision dec;
    dec.sanitized_url = std::string(raw_url);

    ParsedUrl parsed = parse_url(raw_url);
    if (!parsed.valid || parsed.scheme == "file" || parsed.scheme == "astra") {
        return dec;
    }

    std::string site_key = to_lower(top_level_site);
    bool allowlisted = !site_key.empty() && allowlist_.count(site_key) > 0;

    // 1. HTTPS-Only Mode upgrade (skip localhost / 127.0.0.1)
    if (https_only_ && parsed.scheme == "http" &&
        parsed.host != "localhost" && parsed.host != "127.0.0.1") {
        parsed.scheme = "https";
        if (parsed.port == 80) parsed.port = 443;
        dec.upgraded_to_https = true;
        ++stats_.https_upgrades;
    }

    // 2. Strip tracking query parameters
    if (!allowlisted && !parsed.query.empty()) {
        parsed.query = strip_tracking_params(parsed.query, dec.params_stripped);
        stats_.params_stripped += dec.params_stripped;
    }
    dec.sanitized_url = parsed.rebuild();

    // 3. Anti-Tracking classification
    if (!allowlisted && strict_mode_) {
        TrackerCategory cat = trie_.match(parsed.host);
        if (cat != TrackerCategory::None) {
            bool is_third_party = !site_key.empty() && parsed.registrable_domain() != site_key;
            if (!is_top_level_navigation || is_third_party ||
                cat == TrackerCategory::Cryptomining || cat == TrackerCategory::Fingerprinting) {
                dec.blocked = true;
                dec.category = cat;
                dec.reason = std::string("Blocked by AstraShield (") + tracker_category_name(cat) + ")";
                ++stats_.trackers_blocked;
            }
        }
    }

    return dec;
}

void AstraShield::store_cookie(std::string_view top_level_site,
                               std::string_view request_origin,
                               std::string_view set_cookie_header) {
    size_t semi = set_cookie_header.find(';');
    std::string_view kv = (semi == std::string_view::npos) ? set_cookie_header : set_cookie_header.substr(0, semi);
    size_t eq = kv.find('=');
    if (eq == std::string_view::npos) return;

    CookieEntry entry;
    entry.name = std::string(kv.substr(0, eq));
    entry.value = std::string(kv.substr(eq + 1));
    while (!entry.name.empty() && std::isspace(static_cast<unsigned char>(entry.name.front()))) entry.name.erase(entry.name.begin());
    while (!entry.name.empty() && std::isspace(static_cast<unsigned char>(entry.name.back()))) entry.name.pop_back();

    std::string lower_hdr = to_lower(set_cookie_header);
    entry.secure = lower_hdr.find("secure") != std::string::npos;
    entry.http_only = lower_hdr.find("httponly") != std::string::npos;
    entry.same_site = SameSiteMode::Lax; // RFC 6265bis default
    if (lower_hdr.find("samesite=strict") != std::string::npos) {
        entry.same_site = SameSiteMode::Strict;
    } else if (lower_hdr.find("samesite=none") != std::string::npos) {
        if (!entry.secure) return; // Reject insecure SameSite=None cookies per spec
        entry.same_site = SameSiteMode::None;
    } else if (lower_hdr.find("samesite=lax") != std::string::npos) {
        entry.same_site = SameSiteMode::Lax;
    }
    entry.domain = std::string(request_origin);

    // Total Cookie Protection partition key: (top_level_site, request_origin)
    std::string partition_key = to_lower(top_level_site) + "|" + to_lower(request_origin);
    cookie_jars_[partition_key][entry.name] = std::move(entry);
    ++stats_.cookies_partitioned;
}

std::string AstraShield::cookie_header_for(std::string_view top_level_site,
                                           std::string_view request_origin,
                                           bool is_https,
                                           bool is_top_level_get) const {
    std::string partition_key = to_lower(top_level_site) + "|" + to_lower(request_origin);
    auto it = cookie_jars_.find(partition_key);
    if (it == cookie_jars_.end()) return {};

    ParsedUrl req_u = parse_url(request_origin);
    std::string req_site = req_u.valid ? req_u.registrable_domain() : to_lower(request_origin);
    std::string top_site = to_lower(top_level_site);
    bool is_cross_site = !top_site.empty() && !req_site.empty() && (top_site != req_site);

    std::string out;
    for (const auto& [name, entry] : it->second) {
        if (entry.secure && !is_https) continue;
        if (is_cross_site) {
            if (entry.same_site == SameSiteMode::Strict) continue;
            if (entry.same_site == SameSiteMode::Lax && !is_top_level_get) continue;
        }
        if (!out.empty()) out += "; ";
        out += name + "=" + entry.value;
    }
    return out;
}

uint64_t AstraShield::canvas_noise_seed(std::string_view top_level_site) const {
    uint64_t h = session_salt_ ^ 14695981039346656037ULL;
    for (char c : top_level_site) {
        h ^= static_cast<uint64_t>(static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c))));
        h *= 1099511628211ULL;
    }
    return h;
}

std::vector<std::pair<std::string, std::string>> AstraShield::hardened_headers() const {
    if (!fingerprint_hardening_) {
        return {{"User-Agent", "Astra/1.0 (Linux x86_64)"}};
    }
    return {
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64; rv:133.0) Gecko/20100101 Firefox/133.0"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Sec-GPC", "1"},
        {"DNT", "1"}
    };
}

const char* tracker_category_name(TrackerCategory c) {
    switch (c) {
        case TrackerCategory::None: return "none";
        case TrackerCategory::Advertising: return "advertising";
        case TrackerCategory::Analytics: return "analytics";
        case TrackerCategory::Fingerprinting: return "fingerprinting";
        case TrackerCategory::Cryptomining: return "cryptomining";
        case TrackerCategory::SocialTracker: return "social-tracker";
    }
    return "unknown";
}

} // namespace astra
