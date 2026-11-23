#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace astra {

struct ParsedDocument {
    std::string format;        // "PDF", "Markdown", "Text"
    std::string title;
    std::string author;
    std::string pdf_version;
    size_t page_count = 0;
    std::vector<std::string> pages;
    bool ok = false;
    std::string error;
};

// AstraDocs: Zero-dependency native PDF, Markdown, and plaintext/code viewer.
// Parses real PDF objects, FlateDecode streams, and BT/ET/Tj/TJ text operators
// directly in C++20 without pdf.js or third-party PDF libraries.
class DocViewer {
public:
    static ParsedDocument parse_pdf_file(const std::string& path);
    static ParsedDocument parse_pdf_bytes(std::string_view bytes, const std::string& fallback_title = "Document.pdf");
    static ParsedDocument parse_markdown_file(const std::string& path);
    static ParsedDocument parse_markdown(std::string_view md, const std::string& fallback_title = "Document.md");

    // Compiles any ParsedDocument into clean Astra HTML for native layout & rendering.
    static std::string to_astra_html(const ParsedDocument& doc);
};

} // namespace astra
