#include "privacy/security_policy.hpp"
#include "privacy/shield.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace astra {

namespace {

std::string to_lower_str(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim_ws(std::string_view s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

uint16_t default_port_for(const std::string& scheme) {
    if (scheme == "http" || scheme == "ws") return 80;
    if (scheme == "https" || scheme == "wss") return 443;
    return 0;
}

} // namespace

SecurityOrigin SecurityOrigin::from_url(std::string_view url) {
    SecurityOrigin o;
    ParsedUrl p = parse_url(url);
    if (!p.valid || p.scheme == "data" || p.scheme == "javascript" || p.scheme == "about") {
        o.opaque = true;
        o.scheme = p.scheme;
        return o;
    }
    o.scheme = to_lower_str(p.scheme);
    o.host = to_lower_str(p.host);
    o.port = p.port ? p.port : default_port_for(o.scheme);
    if (o.scheme == "file" || o.scheme == "astra") {
        o.opaque = false;
        o.port = 0;
    }
    return o;
}

bool SecurityOrigin::same_origin(const SecurityOrigin& other) const {
    if (opaque || other.opaque) return false;
    return scheme == other.scheme && host == other.host && port == other.port;
}

bool SecurityOrigin::same_site(const SecurityOrigin& other) const {
    if (opaque || other.opaque) return false;
    if (scheme == "file" && other.scheme == "file") return true;
    if (scheme != other.scheme) return false;
    return site_key() == other.site_key();
}

bool SecurityOrigin::is_loopback() const {
    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

bool SecurityOrigin::is_potentially_trustworthy() const {
    if (opaque) return false;
    return scheme == "https" || scheme == "wss" || scheme == "file" || scheme == "astra" || is_loopback();
}

std::string SecurityOrigin::serialize() const {
    if (opaque) return "null";
    if (scheme == "file") return "file://";
    if (scheme == "astra") return "astra://" + host;
    std::string out = scheme + "://" + host;
    uint16_t def = default_port_for(scheme);
    if (port != 0 && port != def) out += ":" + std::to_string(port);
    return out;
}

std::string SecurityOrigin::site_key() const {
    if (opaque) return "opaque";
    if (scheme == "file" || scheme == "astra") return scheme + "://" + host;
    ParsedUrl tmp;
    tmp.scheme = scheme;
    tmp.host = host;
    tmp.port = port;
    tmp.valid = true;
    std::string reg = tmp.registrable_domain();
    return scheme + "://" + (reg.empty() ? host : reg);
}

void ContentSecurityPolicy::parse(std::string_view policy_header) {
    size_t pos = 0;
    while (pos < policy_header.size()) {
        size_t semi = policy_header.find(';', pos);
        std::string_view part = (semi == std::string_view::npos)
                                    ? policy_header.substr(pos)
                                    : policy_header.substr(pos, semi - pos);
        std::string trimmed = trim_ws(part);
        if (!trimmed.empty()) {
            std::istringstream iss(trimmed);
            std::string directive;
            if (iss >> directive) {
                directive = to_lower_str(directive);
                std::vector<std::string> tokens;
                std::string tok;
                while (iss >> tok) tokens.push_back(to_lower_str(tok));
                // First occurrence of a directive wins per W3C CSP spec
                if (directives_.find(directive) == directives_.end()) {
                    directives_[directive] = std::move(tokens);
                    active_ = true;
                }
            }
        }
        if (semi == std::string_view::npos) break;
        pos = semi + 1;
    }
}

const std::vector<std::string>* ContentSecurityPolicy::effective_sources(ResourceKind kind) const {
    const char* name = "default-src";
    switch (kind) {
        case ResourceKind::Script:   name = "script-src"; break;
        case ResourceKind::Style:    name = "style-src"; break;
        case ResourceKind::Image:    name = "img-src"; break;
        case ResourceKind::Connect:  name = "connect-src"; break;
        case ResourceKind::Frame:    name = "frame-src"; break;
        case ResourceKind::Media:    name = "media-src"; break;
        case ResourceKind::Font:     name = "font-src"; break;
        case ResourceKind::Document: name = "default-src"; break;
    }
    auto it = directives_.find(name);
    if (it != directives_.end()) return &it->second;
    auto def = directives_.find("default-src");
    if (def != directives_.end()) return &def->second;
    return nullptr;
}

bool ContentSecurityPolicy::allows_inline_script() const {
    if (!active_) return true;
    const auto* srcs = effective_sources(ResourceKind::Script);
    if (!srcs) return true;
    for (const auto& s : *srcs) {
        if (s == "'unsafe-inline'") return true;
    }
    return false;
}

bool ContentSecurityPolicy::allows_inline_style() const {
    if (!active_) return true;
    const auto* srcs = effective_sources(ResourceKind::Style);
    if (!srcs) return true;
    for (const auto& s : *srcs) {
        if (s == "'unsafe-inline'") return true;
    }
    return false;
}

bool ContentSecurityPolicy::source_matches(const std::string& expr,
                                           const SecurityOrigin& target,
                                           const SecurityOrigin& doc_origin,
                                           std::string_view raw_url) {
    if (expr == "'none'") return false;
    if (expr == "*") return !target.opaque && (target.scheme == "http" || target.scheme == "https" || target.scheme == "ws" || target.scheme == "wss");
    if (expr == "'self'") return target.same_origin(doc_origin);
    // Scheme source e.g. "https:", "data:"
    if (!expr.empty() && expr.back() == ':') {
        std::string want_scheme = expr.substr(0, expr.size() - 1);
        if (want_scheme == "data" && to_lower_str(raw_url.substr(0, 5)) == "data:") return true;
        return target.scheme == want_scheme;
    }
    // Host / origin source (with optional scheme and wildcard *.domain)
    std::string e = expr;
    std::string req_scheme;
    size_t sp = e.find("://");
    if (sp != std::string::npos) {
        req_scheme = e.substr(0, sp);
        e = e.substr(sp + 3);
    }
    size_t slash = e.find('/');
    if (slash != std::string::npos) e = e.substr(0, slash);
    uint16_t req_port = 0;
    size_t colon = e.find(':');
    if (colon != std::string::npos) {
        req_port = static_cast<uint16_t>(std::atoi(e.substr(colon + 1).c_str()));
        e = e.substr(0, colon);
    }
    if (!req_scheme.empty() && target.scheme != req_scheme) return false;
    if (req_port != 0 && target.port != req_port) return false;
    if (e.size() > 2 && e[0] == '*' && e[1] == '.') {
        std::string suffix = e.substr(1); // ".example.com"
        return target.host.size() > suffix.size() &&
               target.host.compare(target.host.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    return target.host == e;
}

bool ContentSecurityPolicy::allows_url(ResourceKind kind,
                                       std::string_view target_url,
                                       const SecurityOrigin& doc_origin,
                                       std::string* reason_out) const {
    if (!active_) return true;
    const auto* srcs = effective_sources(kind);
    if (!srcs) return true;
    SecurityOrigin target = SecurityOrigin::from_url(target_url);
    for (const auto& s : *srcs) {
        if (s == "'none'") {
            if (reason_out) *reason_out = "Blocked by CSP ('none')";
            return false;
        }
        if (source_matches(s, target, doc_origin, target_url)) return true;
    }
    if (reason_out) {
        *reason_out = "Blocked by Content-Security-Policy for " + std::string(target_url);
    }
    return false;
}

bool MixedContentBlocker::is_blocked(const SecurityOrigin& doc_origin,
                                     std::string_view target_url,
                                     ResourceKind kind,
                                     std::string* reason_out) {
    if (doc_origin.scheme != "https") return false;
    SecurityOrigin target = SecurityOrigin::from_url(target_url);
    if (target.is_loopback()) return false;
    bool insecure_net = (target.scheme == "http" || target.scheme == "ws");
    if (!insecure_net) return false;
    // Block all active mixed content (scripts, stylesheets, fetch/XHR/WS, iframes, fonts)
    bool is_active = (kind == ResourceKind::Script || kind == ResourceKind::Style ||
                      kind == ResourceKind::Connect || kind == ResourceKind::Frame ||
                      kind == ResourceKind::Font);
    if (is_active) {
        if (reason_out) {
            *reason_out = "Mixed Content blocked: insecure " + target.scheme + ":// resource from https:// document";
        }
        return true;
    }
    return false;
}

bool CorsEnforcer::allows_response(const SecurityOrigin& doc_origin,
                                   std::string_view target_url,
                                   const std::map<std::string, std::string>& resp_headers,
                                   std::string* reason_out) {
    // Local file:// and internal astra:// tools bypass network CORS unless explicitly remote
    if (doc_origin.scheme == "file" || doc_origin.scheme == "astra" || doc_origin.opaque) return true;
    SecurityOrigin target = SecurityOrigin::from_url(target_url);
    if (target.same_origin(doc_origin)) return true;

    std::string acao;
    for (const auto& [k, v] : resp_headers) {
        if (to_lower_str(k) == "access-control-allow-origin") {
            acao = trim_ws(v);
            break;
        }
    }
    if (acao == "*" || to_lower_str(acao) == to_lower_str(doc_origin.serialize())) {
        return true;
    }
    if (reason_out) {
        *reason_out = "CORS policy blocked cross-origin read from " + doc_origin.serialize() +
                      " to " + target.serialize() + " (missing Access-Control-Allow-Origin)";
    }
    return false;
}

} // namespace astra
