#include "ext/extension_runtime.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace astra {

namespace {

bool read_file_text(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream oss;
    oss << f.rdbuf();
    out = oss.str();
    return true;
}

bool glob_match(std::string_view pat, std::string_view str) {
    size_t p = 0, s = 0, star = std::string_view::npos, match = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == str[s] || pat[p] == '?')) {
            ++p; ++s;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            match = s;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            s = ++match;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

} // namespace

bool ExtensionRuntime::match_pattern(const std::string& pattern, const std::string& url) {
    if (pattern == "<all_urls>") return true;
    return glob_match(pattern, url);
}

bool ExtensionRuntime::load_extension_dir(const std::string& dir_path, std::string* error) {
    std::string manifest_raw;
    std::string manifest_path = dir_path + "/manifest.json";
    if (!read_file_text(manifest_path, manifest_raw)) {
        if (error) *error = "Cannot open " + manifest_path;
        return false;
    }

    JsonValue root;
    std::string parse_err;
    if (!JsonValue::parse(manifest_raw, root, &parse_err) || !root.is_object()) {
        if (error) *error = "Invalid manifest.json: " + parse_err;
        return false;
    }

    LoadedExtension ext;
    ext.root_dir = dir_path;
    ext.manifest_version = static_cast<int>(root.get_number("manifest_version", 3));
    ext.name = root.get_string("name", "Unnamed Extension");
    ext.version = root.get_string("version", "1.0.0");
    ext.id = root.get_string("id", ext.name + "@" + ext.version);

    if (const JsonValue* perms = root.get("permissions"); perms && perms->is_array()) {
        for (const auto& p : perms->as_array()) {
            if (p.is_string()) ext.permissions.insert(p.as_string());
        }
    }

    // Load Manifest V3 declarative_net_request rule_resources
    if (ext.permissions.count("declarativeNetRequest") > 0) {
        if (const JsonValue* dnr = root.get("declarative_net_request"); dnr && dnr->is_object()) {
            if (const JsonValue* res = dnr->get("rule_resources"); res && res->is_array()) {
                for (const auto& r : res->as_array()) {
                    if (!r.get_bool("enabled", true)) continue;
                    std::string rel_path = r.get_string("path");
                    if (rel_path.empty()) continue;
                    std::string rules_raw;
                    if (!read_file_text(dir_path + "/" + rel_path, rules_raw)) continue;
                    JsonValue rules_json;
                    if (!JsonValue::parse(rules_raw, rules_json) || !rules_json.is_array()) continue;
                    for (const auto& item : rules_json.as_array()) {
                        DnrRule rule;
                        rule.extension_id = ext.id;
                        rule.id = static_cast<int>(item.get_number("id", 0));
                        rule.priority = static_cast<int>(item.get_number("priority", 1));
                        if (const JsonValue* act = item.get("action"); act && act->is_object()) {
                            rule.action_type = act->get_string("type", "block");
                            if (const JsonValue* redir = act->get("redirect"); redir && redir->is_object()) {
                                rule.redirect_url = redir->get_string("url");
                            }
                            if (const JsonValue* hdrs = act->get("requestHeaders"); hdrs && hdrs->is_array()) {
                                for (const auto& h : hdrs->as_array()) {
                                    std::string hn = h.get_string("header");
                                    std::string hv = h.get_string("value");
                                    if (!hn.empty()) rule.request_headers.emplace_back(hn, hv);
                                }
                            }
                        }
                        if (const JsonValue* cond = item.get("condition"); cond && cond->is_object()) {
                            rule.url_filter = cond->get_string("urlFilter");
                        }
                        if (!rule.url_filter.empty()) {
                            ext.dnr_rules.push_back(std::move(rule));
                        }
                    }
                }
            }
        }
    }

    // Load content_scripts
    if (const JsonValue* cs = root.get("content_scripts"); cs && cs->is_array()) {
        for (const auto& entry : cs->as_array()) {
            ContentScriptSpec spec;
            spec.run_at = entry.get_string("run_at", "document_idle");
            if (const JsonValue* m = entry.get("matches"); m && m->is_array()) {
                for (const auto& s : m->as_array()) if (s.is_string()) spec.matches.push_back(s.as_string());
            }
            if (const JsonValue* js = entry.get("js"); js && js->is_array()) {
                for (const auto& s : js->as_array()) if (s.is_string()) spec.js_files.push_back(s.as_string());
            }
            if (const JsonValue* css = entry.get("css"); css && css->is_array()) {
                for (const auto& s : css->as_array()) if (s.is_string()) spec.css_files.push_back(s.as_string());
            }
            ext.content_scripts.push_back(std::move(spec));
        }
    }

    extensions_.push_back(std::move(ext));
    return true;
}

DnrHookResult ExtensionRuntime::evaluate_dnr(const std::string& url, const std::string& /*top_site*/) const {
    DnrHookResult result;
    int best_priority = -1;

    for (const auto& ext : extensions_) {
        if (!ext.enabled) continue;
        for (const auto& rule : ext.dnr_rules) {
            if (url.find(rule.url_filter) == std::string::npos) continue;
            if (rule.action_type == "modifyHeaders") {
                for (const auto& kv : rule.request_headers) {
                    result.add_headers.push_back(kv);
                }
            } else if (rule.priority >= best_priority) {
                best_priority = rule.priority;
                if (rule.action_type == "block") {
                    result.block = true;
                    result.rule_source = ext.name + " (rule #" + std::to_string(rule.id) + ")";
                } else if (rule.action_type == "redirect" && !rule.redirect_url.empty()) {
                    result.redirect_url = rule.redirect_url;
                    result.rule_source = ext.name + " (rule #" + std::to_string(rule.id) + ")";
                }
            }
        }
    }
    if (result.block) ++dnr_blocks_;
    return result;
}

ExtensionRunSummary ExtensionRuntime::run_content_scripts(const std::string& url,
                                                          HtmlDocument& doc,
                                                          std::string& extra_css_out) {
    ExtensionRunSummary sum;
    for (const auto& ext : extensions_) {
        if (!ext.enabled) continue;
        for (const auto& spec : ext.content_scripts) {
            bool matched = false;
            for (const auto& pat : spec.matches) {
                if (match_pattern(pat, url)) { matched = true; break; }
            }
            if (!matched) continue;

            for (const auto& css_rel : spec.css_files) {
                std::string css_text;
                if (read_file_text(ext.root_dir + "/" + css_rel, css_text)) {
                    extra_css_out += "\n" + css_text;
                    ++sum.styles_injected;
                }
            }

            if (!spec.js_files.empty()) {
                AstraJsEngine vm;
                bool has_storage = ext.permissions.count("storage") > 0;
                if (has_storage) {
                    std::string ext_id = ext.id;
                    vm.bind_storage(
                        [this, ext_id](const std::string& k) {
                            return ext_storage_[ext_id][k];
                        },
                        [this, ext_id](const std::string& k, const std::string& v) {
                            ext_storage_[ext_id][k] = v;
                        });
                }
                for (const auto& js_rel : spec.js_files) {
                    std::string js_text;
                    if (read_file_text(ext.root_dir + "/" + js_rel, js_text)) {
                        auto rep = vm.execute(js_text, doc);
                        ++sum.scripts_injected;
                        for (const auto& l : rep.logs) {
                            sum.logs.push_back("[" + ext.name + "] " + l);
                        }
                    }
                }
            }
        }
    }
    return sum;
}

} // namespace astra
