#pragma once

#include "core/json.hpp"
#include "dom/html_parser.hpp"
#include "js/astra_js.hpp"
#include "net/http_client.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace astra {

struct DnrRule {
    int id = 0;
    int priority = 1;
    std::string action_type;       // "block", "redirect", "modifyHeaders"
    std::string redirect_url;
    std::string url_filter;        // substring or domain pattern
    std::vector<std::pair<std::string, std::string>> request_headers;
    std::string extension_id;
};

struct ContentScriptSpec {
    std::vector<std::string> matches;
    std::vector<std::string> js_files;
    std::vector<std::string> css_files;
    std::string run_at = "document_idle";
};

struct LoadedExtension {
    std::string id;
    std::string name;
    std::string version;
    int manifest_version = 3;
    std::string root_dir;
    std::set<std::string> permissions;
    std::vector<DnrRule> dnr_rules;
    std::vector<ContentScriptSpec> content_scripts;
    bool enabled = true;
};

struct ExtensionRunSummary {
    size_t scripts_injected = 0;
    size_t styles_injected = 0;
    std::vector<std::string> logs;
};

// AstraExt: Firefox WebExtensions / Manifest V3 compatible runtime with
// permission checks, Declarative Net Request (DNR), content script injection,
// and isolated per-extension storage.
class ExtensionRuntime {
public:
    bool load_extension_dir(const std::string& dir_path, std::string* error = nullptr);

    DnrHookResult evaluate_dnr(const std::string& url, const std::string& top_site) const;

    ExtensionRunSummary run_content_scripts(const std::string& url,
                                            HtmlDocument& doc,
                                            std::string& extra_css_out);

    const std::vector<LoadedExtension>& extensions() const { return extensions_; }
    uint64_t dnr_blocks_count() const { return dnr_blocks_; }
    static bool match_pattern(const std::string& pattern, const std::string& url);

private:
    std::vector<LoadedExtension> extensions_;
    // Isolated storage per extension_id
    std::map<std::string, std::map<std::string, std::string>> ext_storage_;
    mutable uint64_t dnr_blocks_ = 0;
};

} // namespace astra
