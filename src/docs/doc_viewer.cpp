#include "docs/doc_viewer.hpp"
#include "net/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace astra {

namespace {

std::string escape_html(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string extract_pdf_literal_string(std::string_view s, size_t& pos) {
    if (pos >= s.size() || s[pos] != '(') return {};
    ++pos;
    int depth = 1;
    std::string out;
    while (pos < s.size() && depth > 0) {
        char c = s[pos++];
        if (c == '\\' && pos < s.size()) {
            char esc = s[pos++];
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '(': out.push_back('('); break;
                case ')': out.push_back(')'); break;
                case '\\': out.push_back('\\'); break;
                default: out.push_back(esc); break;
            }
        } else if (c == '(') {
            ++depth;
            out.push_back(c);
        } else if (c == ')') {
            --depth;
            if (depth > 0) out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Extracts visible text from a PDF content stream (BT...ET, Tj, TJ, Td, T*)
std::string extract_text_from_pdf_stream(std::string_view stream) {
    std::string result;
    size_t pos = 0;
    const size_t n = stream.size();
    bool in_text = false;

    while (pos < n) {
        if (!in_text) {
            size_t bt = stream.find("BT", pos);
            if (bt == std::string_view::npos) break;
            bool left_ok = (bt == 0 || std::isspace(static_cast<unsigned char>(stream[bt - 1])));
            bool right_ok = (bt + 2 >= n || std::isspace(static_cast<unsigned char>(stream[bt + 2])));
            if (left_ok && right_ok) {
                in_text = true;
                pos = bt + 2;
                continue;
            }
            pos = bt + 2;
            continue;
        }

        while (pos < n && std::isspace(static_cast<unsigned char>(stream[pos]))) ++pos;
        if (pos >= n) break;

        if (stream.substr(pos, 2) == "ET" &&
            (pos + 2 >= n || std::isspace(static_cast<unsigned char>(stream[pos + 2])))) {
            in_text = false;
            if (!result.empty() && result.back() != '\n') result.push_back('\n');
            pos += 2;
            continue;
        }

        if (stream[pos] == '(') {
            std::string lit = extract_pdf_literal_string(stream, pos);
            while (pos < n && std::isspace(static_cast<unsigned char>(stream[pos]))) ++pos;
            if (pos + 2 <= n && stream.substr(pos, 2) == "Tj") {
                result += lit;
                pos += 2;
            } else if (pos < n && (stream[pos] == '\'' || stream[pos] == '"')) {
                result.push_back('\n');
                result += lit;
                ++pos;
            }
            continue;
        }

        if (stream[pos] == '[') {
            ++pos;
            std::string arr_text;
            while (pos < n && stream[pos] != ']') {
                if (stream[pos] == '(') {
                    arr_text += extract_pdf_literal_string(stream, pos);
                } else if (stream[pos] == '-' || std::isdigit(static_cast<unsigned char>(stream[pos]))) {
                    size_t st = pos;
                    if (stream[pos] == '-') ++pos;
                    while (pos < n && (std::isdigit(static_cast<unsigned char>(stream[pos])) || stream[pos] == '.')) ++pos;
                    double kern = std::atof(std::string(stream.substr(st, pos - st)).c_str());
                    if (kern < -140.0) arr_text.push_back(' ');
                } else {
                    ++pos;
                }
            }
            if (pos < n && stream[pos] == ']') ++pos;
            while (pos < n && std::isspace(static_cast<unsigned char>(stream[pos]))) ++pos;
            if (pos + 2 <= n && stream.substr(pos, 2) == "TJ") {
                result += arr_text;
                pos += 2;
            }
            continue;
        }

        // Line break operators: Td, TD, T*
        if (pos + 2 <= n && (stream.substr(pos, 2) == "Td" || stream.substr(pos, 2) == "TD" || stream.substr(pos, 2) == "T*")) {
            if (!result.empty() && result.back() != '\n') result.push_back('\n');
            pos += 2;
            continue;
        }

        ++pos;
    }

    return result;
}

std::string find_pdf_info_field(std::string_view bytes, std::string_view key) {
    size_t pos = bytes.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.size();
    while (pos < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[pos]))) ++pos;
    if (pos < bytes.size() && bytes[pos] == '(') {
        return extract_pdf_literal_string(bytes, pos);
    }
    return {};
}

} // namespace

ParsedDocument DocViewer::parse_pdf_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ParsedDocument d;
        d.error = "Cannot open PDF file: " + path;
        return d;
    }
    std::ostringstream oss;
    oss << f.rdbuf();
    return parse_pdf_bytes(oss.str(), path);
}

ParsedDocument DocViewer::parse_pdf_bytes(std::string_view bytes, const std::string& fallback_title) {
    ParsedDocument doc;
    doc.format = "PDF";

    if (bytes.size() < 8 || bytes.substr(0, 5) != "%PDF-") {
        doc.error = "Invalid PDF signature (missing %PDF- header)";
        return doc;
    }

    size_t nl = bytes.find('\n');
    doc.pdf_version = std::string(bytes.substr(5, std::min<size_t>(4, (nl == std::string_view::npos ? 8 : nl) - 5)));
    while (!doc.pdf_version.empty() && std::isspace(static_cast<unsigned char>(doc.pdf_version.back()))) {
        doc.pdf_version.pop_back();
    }

    doc.title = find_pdf_info_field(bytes, "/Title");
    if (doc.title.empty()) doc.title = fallback_title;
    doc.author = find_pdf_info_field(bytes, "/Author");

    // Walk all "stream ... endstream" objects
    size_t pos = 0;
    while (pos < bytes.size()) {
        size_t s_kw = bytes.find("stream", pos);
        if (s_kw == std::string_view::npos) break;

        // Inspect preceding dictionary (up to 320 bytes back) for /FlateDecode
        size_t dict_start = (s_kw > 320) ? s_kw - 320 : 0;
        std::string_view dict = bytes.substr(dict_start, s_kw - dict_start);
        bool is_flate = dict.find("/FlateDecode") != std::string_view::npos;

        size_t data_start = s_kw + 6;
        if (data_start < bytes.size() && bytes[data_start] == '\r') ++data_start;
        if (data_start < bytes.size() && bytes[data_start] == '\n') ++data_start;

        size_t e_kw = bytes.find("endstream", data_start);
        if (e_kw == std::string_view::npos) break;

        size_t data_end = e_kw;
        while (data_end > data_start && (bytes[data_end - 1] == '\n' || bytes[data_end - 1] == '\r')) {
            --data_end;
        }
        std::string_view raw_stream = bytes.substr(data_start, data_end - data_start);
        pos = e_kw + 9;

        std::string content;
        if (is_flate) {
            if (!HttpClient::decompress_gzip_or_zlib(
                    reinterpret_cast<const uint8_t*>(raw_stream.data()), raw_stream.size(), content)) {
                continue;
            }
        } else {
            content.assign(raw_stream);
        }

        if (content.find("BT") != std::string::npos &&
            (content.find("Tj") != std::string::npos || content.find("TJ") != std::string::npos)) {
            std::string extracted = extract_text_from_pdf_stream(content);
            if (!extracted.empty()) {
                doc.pages.push_back(std::move(extracted));
            }
        }
    }

    doc.page_count = doc.pages.size();
    doc.ok = !doc.pages.empty();
    if (!doc.ok) {
        doc.error = "No extractable PDF text streams found";
    }
    return doc;
}

ParsedDocument DocViewer::parse_markdown_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        ParsedDocument d;
        d.error = "Cannot open Markdown file: " + path;
        return d;
    }
    std::ostringstream oss;
    oss << f.rdbuf();
    return parse_markdown(oss.str(), path);
}

ParsedDocument DocViewer::parse_markdown(std::string_view md, const std::string& fallback_title) {
    ParsedDocument doc;
    doc.format = "Markdown";
    doc.title = fallback_title;

    std::istringstream iss{std::string(md)};
    std::string line;
    std::string page_buf;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (doc.title == fallback_title && line.rfind("# ", 0) == 0) {
            doc.title = line.substr(2);
        }
        page_buf += line + "\n";
    }
    doc.pages.push_back(std::move(page_buf));
    doc.page_count = 1;
    doc.ok = true;
    return doc;
}

std::string DocViewer::to_astra_html(const ParsedDocument& doc) {
    std::ostringstream html;
    html << "<!doctype html><html><head><title>" << escape_html(doc.title) << "</title>"
         << "<style>"
         << "body { background: #0d1118; color: #e2e8f0; padding: 24px 32px; }"
         << ".doc-banner { background: #141c2b; border: 1px solid #26354d; border-radius: 10px; padding: 14px 18px; margin-bottom: 18px; }"
         << ".doc-meta { color: #38bdf8; font-size: 12px; font-weight: bold; }"
         << ".doc-page { background: #111827; border: 1px solid #1f2937; border-radius: 10px; padding: 18px 22px; margin-bottom: 16px; }"
         << ".page-num { color: #64748b; font-size: 11px; margin-bottom: 10px; }"
         << "</style></head><body>";

    html << "<div class=\"doc-banner\">"
         << "<div class=\"doc-meta\">AstraDocs Native " << escape_html(doc.format)
         << (doc.pdf_version.empty() ? "" : (" v" + escape_html(doc.pdf_version)))
         << " | Pages: " << doc.page_count
         << (doc.author.empty() ? "" : (" | Author: " + escape_html(doc.author)))
         << "</div>"
         << "<h1>" << escape_html(doc.title) << "</h1>"
         << "</div>";

    for (size_t i = 0; i < doc.pages.size(); ++i) {
        html << "<div class=\"doc-page\">";
        if (doc.format == "PDF") {
            html << "<div class=\"page-num\">PAGE " << (i + 1) << " OF " << doc.pages.size() << "</div>";
            std::istringstream pss(doc.pages[i]);
            std::string line;
            while (std::getline(pss, line)) {
                if (!line.empty()) html << "<p>" << escape_html(line) << "</p>";
            }
        } else {
            // Markdown rendering
            std::istringstream mss(doc.pages[i]);
            std::string line;
            bool in_code = false;
            std::string code_buf;
            while (std::getline(mss, line)) {
                if (line.rfind("```", 0) == 0) {
                    if (in_code) {
                        html << "<pre>" << escape_html(code_buf) << "</pre>";
                        code_buf.clear();
                        in_code = false;
                    } else {
                        in_code = true;
                    }
                    continue;
                }
                if (in_code) {
                    code_buf += line + "\n";
                    continue;
                }
                if (line.rfind("### ", 0) == 0) html << "<h3>" << escape_html(line.substr(4)) << "</h3>";
                else if (line.rfind("## ", 0) == 0) html << "<h2>" << escape_html(line.substr(3)) << "</h2>";
                else if (line.rfind("# ", 0) == 0) html << "<h1>" << escape_html(line.substr(2)) << "</h1>";
                else if (line.rfind("> ", 0) == 0) html << "<blockquote>" << escape_html(line.substr(2)) << "</blockquote>";
                else if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) html << "<p>- " << escape_html(line.substr(2)) << "</p>";
                else if (line == "---") html << "<hr>";
                else if (!line.empty()) html << "<p>" << escape_html(line) << "</p>";
            }
        }
        html << "</div>";
    }

    html << "</body></html>";
    return html.str();
}

} // namespace astra
