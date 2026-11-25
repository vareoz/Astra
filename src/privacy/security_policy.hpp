#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace astra {

struct SecurityOrigin {
    std::string scheme;
    std::string host;
    uint16_t port = 0;
    bool opaque = false;

    static SecurityOrigin from_url(std::string_view url);
    bool same_origin(const SecurityOrigin& other) const;
    bool same_site(const SecurityOrigin& other) const;
    bool is_loopback() const;
    bool is_potentially_trustworthy() const;
    std::string serialize() const;
    std::string site_key() const; // eTLD+1 isolation key for Site Isolation
};

enum class ResourceKind {
    Script,
    Style,
    Image,
    Connect,
    Frame,
    Media,
    Font,
    Document
};

// W3C Content Security Policy (CSP Level 2/3) parser and evaluator.
class ContentSecurityPolicy {
public:
    void parse(std::string_view policy_header);
    void clear() { directives_.clear(); active_ = false; }
    bool active() const { return active_; }

    bool allows_inline_script() const;
    bool allows_inline_style() const;
    bool allows_url(ResourceKind kind,
                    std::string_view target_url,
                    const SecurityOrigin& doc_origin,
                    std::string* reason_out = nullptr) const;

    const std::unordered_map<std::string, std::vector<std::string>>& directives() const {
        return directives_;
    }

private:
    const std::vector<std::string>* effective_sources(ResourceKind kind) const;
    static bool source_matches(const std::string& expr,
                               const SecurityOrigin& target,
                               const SecurityOrigin& doc_origin,
                               std::string_view raw_url);

    bool active_ = false;
    std::unordered_map<std::string, std::vector<std::string>> directives_;
};

// W3C Mixed Content Blocker: blocks insecure http:// active subresources on https:// documents.
class MixedContentBlocker {
public:
    static bool is_blocked(const SecurityOrigin& doc_origin,
                           std::string_view target_url,
                           ResourceKind kind,
                           std::string* reason_out = nullptr);
};

// Same-Origin Policy & CORS Access-Control-Allow-Origin enforcer for fetch()/XHR/DOM.
class CorsEnforcer {
public:
    static bool allows_response(const SecurityOrigin& doc_origin,
                                std::string_view target_url,
                                const std::map<std::string, std::string>& resp_headers,
                                std::string* reason_out = nullptr);
};

} // namespace astra
