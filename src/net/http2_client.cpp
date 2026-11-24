#include "net/http2_client.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>

namespace astra {

namespace {

// RFC 7541 Appendix A: Static Table (1-indexed; entry 0 unused)
struct StaticEntry {
    const char* name;
    const char* value;
};

constexpr StaticEntry kStaticTable[] = {
    {"", ""},
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""}
};

constexpr size_t kStaticTableSize = sizeof(kStaticTable) / sizeof(kStaticTable[0]) - 1;

// RFC 7541 Appendix B: Canonical Huffman Code Table (code, bit_length) for symbols 0..255
struct HuffSym {
    uint32_t code;
    uint8_t bits;
};

constexpr HuffSym kHuffTable[256] = {
    {0x1ff8,13},{0x7fffd8,23},{0xfffffe2,28},{0xfffffe3,28},{0xfffffe4,28},{0xfffffe5,28},{0xfffffe6,28},{0xfffffe7,28},
    {0xfffffe8,28},{0xffffea,24},{0x3ffffffc,30},{0xfffffe9,28},{0xfffffea,28},{0x3ffffffd,30},{0xfffffeb,28},{0xfffffec,28},
    {0xfffffed,28},{0xfffffee,28},{0xfffffef,28},{0xffffff0,28},{0xffffff1,28},{0xffffff2,28},{0x3ffffffe,30},{0xffffff3,28},
    {0xffffff4,28},{0xffffff5,28},{0xffffff6,28},{0xffffff7,28},{0xffffff8,28},{0xffffff9,28},{0xffffffa,28},{0xffffffb,28},
    {0x14,6},{0x3f8,10},{0x3f9,10},{0xffa,12},{0x1ff9,13},{0x15,6},{0xf8,8},{0x7fa,11},
    {0x3fa,10},{0x3fb,10},{0xf9,8},{0x7fb,11},{0xfa,8},{0x16,6},{0x17,6},{0x18,6},
    {0x0,5},{0x1,5},{0x2,5},{0x19,6},{0x1a,6},{0x1b,6},{0x1c,6},{0x1d,6},
    {0x1e,6},{0x1f,6},{0x5c,7},{0xfb,8},{0x7ffc,15},{0x20,6},{0xffb,12},{0x3fc,10},
    {0x1ffa,13},{0x21,6},{0x5d,7},{0x5e,7},{0x5f,7},{0x60,7},{0x61,7},{0x62,7},
    {0x63,7},{0x64,7},{0x65,7},{0x66,7},{0x67,7},{0x68,7},{0x69,7},{0x6a,7},
    {0x6b,7},{0x6c,7},{0x6d,7},{0x6e,7},{0x6f,7},{0x70,7},{0x71,7},{0x72,7},
    {0xfc,8},{0x73,7},{0xfd,8},{0x1ffb,13},{0x7fff0,19},{0x1ffc,13},{0x3ffc,14},{0x22,6},
    {0x7ffd,15},{0x3,5},{0x23,6},{0x4,5},{0x24,6},{0x5,5},{0x25,6},{0x26,6},
    {0x27,6},{0x6,5},{0x74,7},{0x75,7},{0x28,6},{0x29,6},{0x2a,6},{0x7,5},
    {0x2b,6},{0x76,7},{0x2c,6},{0x8,5},{0x9,5},{0x2d,6},{0x77,7},{0x78,7},
    {0x79,7},{0x7a,7},{0x7b,7},{0x7ffe,15},{0x7fc,11},{0x3ffd,14},{0x1ffd,13},{0xffffffc,28},
    {0xfffe6,20},{0x3fffd2,22},{0xfffe7,20},{0xfffe8,20},{0x3fffd3,22},{0x3fffd4,22},{0x3fffd5,22},{0x7fffd9,23},
    {0x3fffd6,22},{0x7fffda,23},{0x7fffdb,23},{0x7fffdc,23},{0x7fffdd,23},{0x7fffde,23},{0xffffeb,24},{0x7fffdf,23},
    {0xffffec,24},{0xffffed,24},{0x3fffd7,22},{0x7fffe0,23},{0xffffee,24},{0x7fffe1,23},{0x7fffe2,23},{0x7fffe3,23},
    {0x7fffe4,23},{0x1fffdc,21},{0x3fffd8,22},{0x7fffe5,23},{0x3fffd9,22},{0x7fffe6,23},{0x7fffe7,23},{0xffffef,24},
    {0x3fffda,22},{0x1fffdd,21},{0xfffe9,20},{0x3fffdb,22},{0x3fffdc,22},{0x7fffe8,23},{0x7fffe9,23},{0x1fffde,21},
    {0x7fffea,23},{0x3fffdd,22},{0x3fffde,22},{0xfffff0,24},{0x1fffdf,21},{0x3fffdf,22},{0x7fffeb,23},{0x7fffec,23},
    {0x1fffe0,21},{0x1fffe1,21},{0x3fffe0,22},{0x1fffe2,21},{0x7fffed,23},{0x3fffe1,22},{0x7fffee,23},{0x7fffef,23},
    {0xfffea,20},{0x3fffe2,22},{0x3fffe3,22},{0x3fffe4,22},{0x7ffff0,23},{0x3fffe5,22},{0x3fffe6,22},{0x7ffff1,23},
    {0x3ffffe0,26},{0x3ffffe1,26},{0xfffeb,20},{0x7fff1,19},{0x3fffe7,22},{0x7ffff2,23},{0x3fffe8,22},{0x1ffffec,25},
    {0x3ffffe2,26},{0x3ffffe3,26},{0x3ffffe4,26},{0x7ffffde,27},{0x7ffffdf,27},{0x3ffffe5,26},{0xfffff1,24},{0x1ffffed,25},
    {0x7fff2,19},{0x1fffe3,21},{0x3ffffe6,26},{0x7ffffe0,27},{0x7ffffe1,27},{0x3ffffe7,26},{0x7ffffe2,27},{0xfffff2,24},
    {0x1fffe4,21},{0x1fffe5,21},{0x3ffffe8,26},{0x3ffffe9,26},{0xffffffd,28},{0x7ffffe3,27},{0x7ffffe4,27},{0x7ffffe5,27},
    {0xfffec,20},{0xfffff3,24},{0xfffed,20},{0x1fffe6,21},{0x3fffe9,22},{0x1fffe7,21},{0x1fffe8,21},{0x7ffff3,23},
    {0x3fffea,22},{0x3fffeb,22},{0x1ffffee,25},{0x1ffffef,25},{0xfffff4,24},{0xfffff5,24},{0x3ffffea,26},{0x7ffff4,23},
    {0x3ffffeb,26},{0x7ffffe6,27},{0x3ffffec,26},{0x3ffffed,26},{0x7ffffe7,27},{0x7ffffe8,27},{0x7ffffe9,27},{0x7ffffea,27},
    {0x7ffffeb,27},{0xffffffe,28},{0x7ffffec,27},{0x7ffffed,27},{0x7ffffee,27},{0x7ffffef,27},{0x7fffff0,27},{0x3ffffee,26}
};

struct HuffNode {
    int16_t left = -1;
    int16_t right = -1;
    int16_t sym = -1;
};

const std::vector<HuffNode>& huffman_trie() {
    static const std::vector<HuffNode> trie = []() {
        std::vector<HuffNode> nodes;
        nodes.reserve(600);
        nodes.emplace_back();
        for (int sym = 0; sym < 256; ++sym) {
            uint32_t code = kHuffTable[sym].code;
            uint8_t bits = kHuffTable[sym].bits;
            int16_t cur = 0;
            for (int b = bits - 1; b >= 0; --b) {
                int bit = static_cast<int>((code >> b) & 1u);
                int16_t next_idx = bit ? nodes[static_cast<size_t>(cur)].right
                                       : nodes[static_cast<size_t>(cur)].left;
                if (next_idx < 0) {
                    next_idx = static_cast<int16_t>(nodes.size());
                    nodes.emplace_back();
                    if (bit) nodes[static_cast<size_t>(cur)].right = next_idx;
                    else nodes[static_cast<size_t>(cur)].left = next_idx;
                }
                cur = next_idx;
            }
            nodes[static_cast<size_t>(cur)].sym = static_cast<int16_t>(sym);
        }
        return nodes;
    }();
    return trie;
}

void encode_int(std::vector<uint8_t>& out, uint8_t prefix_bits, uint8_t first_mask, size_t value) {
    size_t max_prefix = (1u << prefix_bits) - 1u;
    if (value < max_prefix) {
        out.push_back(static_cast<uint8_t>(first_mask | value));
        return;
    }
    out.push_back(static_cast<uint8_t>(first_mask | max_prefix));
    value -= max_prefix;
    while (value >= 128) {
        out.push_back(static_cast<uint8_t>((value & 0x7Fu) | 0x80u));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

bool decode_int(const uint8_t* data, size_t len, size_t& pos, uint8_t prefix_bits, size_t& out_val) {
    if (pos >= len) return false;
    size_t max_prefix = (1u << prefix_bits) - 1u;
    size_t val = data[pos++] & max_prefix;
    if (val < max_prefix) {
        out_val = val;
        return true;
    }
    size_t m = 0;
    while (pos < len) {
        uint8_t b = data[pos++];
        val += static_cast<size_t>(b & 0x7Fu) << m;
        m += 7;
        if ((b & 0x80u) == 0) {
            out_val = val;
            return true;
        }
        if (m > 28) return false;
    }
    return false;
}

void encode_str(std::vector<uint8_t>& out, const std::string& s) {
    encode_int(out, 7, 0x00, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

bool decode_str(const uint8_t* data, size_t len, size_t& pos, std::string& out_s) {
    if (pos >= len) return false;
    bool huff = (data[pos] & 0x80u) != 0;
    size_t slen = 0;
    if (!decode_int(data, len, pos, 7, slen)) return false;
    if (pos + slen > len) return false;
    if (huff) {
        if (!HpackCodec::huffman_decode(data + pos, slen, out_s)) return false;
    } else {
        out_s.assign(reinterpret_cast<const char*>(data + pos), slen);
    }
    pos += slen;
    return true;
}

std::string to_lower_ascii(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

bool write_all(const Http2Client::WriteFn& write_fn, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = write_fn(data + sent, len - sent);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool read_exact(const Http2Client::ReadFn& read_fn, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = read_fn(buf + got, len - got);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

bool HpackCodec::huffman_decode(const uint8_t* data, size_t len, std::string& out) {
    out.clear();
    const auto& trie = huffman_trie();
    int16_t cur = 0;
    int pending_bits = 0;
    uint32_t pending_val = 0;

    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        for (int b = 7; b >= 0; --b) {
            int bit = (byte >> b) & 1;
            pending_val = (pending_val << 1) | static_cast<uint32_t>(bit);
            ++pending_bits;
            cur = bit ? trie[static_cast<size_t>(cur)].right : trie[static_cast<size_t>(cur)].left;
            if (cur < 0) return false;
            if (trie[static_cast<size_t>(cur)].sym >= 0) {
                out.push_back(static_cast<char>(trie[static_cast<size_t>(cur)].sym));
                cur = 0;
                pending_bits = 0;
                pending_val = 0;
            }
        }
    }
    // Trailing padding must be <= 7 bits and all 1s (EOS prefix)
    if (pending_bits > 7) return false;
    if (pending_bits > 0 && pending_val != ((1u << pending_bits) - 1u)) return false;
    return true;
}

void HpackCodec::add_dynamic(const std::string& name, const std::string& value) {
    DynEntry e{name, value};
    size_t esz = e.size();
    while (!dynamic_.empty() && dynamic_bytes_ + esz > max_dynamic_bytes_) {
        dynamic_bytes_ -= dynamic_.back().size();
        dynamic_.pop_back();
    }
    if (esz <= max_dynamic_bytes_) {
        dynamic_.insert(dynamic_.begin(), std::move(e));
        dynamic_bytes_ += esz;
    }
}

bool HpackCodec::lookup_index(size_t idx, std::string& name, std::string& value) const {
    if (idx == 0) return false;
    if (idx <= kStaticTableSize) {
        name = kStaticTable[idx].name;
        value = kStaticTable[idx].value;
        return true;
    }
    size_t di = idx - kStaticTableSize - 1;
    if (di >= dynamic_.size()) return false;
    name = dynamic_[di].name;
    value = dynamic_[di].value;
    return true;
}

std::vector<uint8_t> HpackCodec::encode_headers(const std::vector<std::pair<std::string, std::string>>& headers) {
    std::vector<uint8_t> out;
    for (const auto& [raw_k, v] : headers) {
        std::string k = to_lower_ascii(raw_k);
        // Check exact match in static table
        size_t exact = 0;
        size_t name_match = 0;
        for (size_t i = 1; i <= kStaticTableSize; ++i) {
            if (k == kStaticTable[i].name) {
                if (name_match == 0) name_match = i;
                if (v == kStaticTable[i].value) { exact = i; break; }
            }
        }
        if (exact > 0) {
            // Indexed Header Field (1xxxxxxx)
            encode_int(out, 7, 0x80, exact);
        } else if (name_match > 0) {
            // Literal Header Field without Indexing — Indexed Name (0000xxxx)
            encode_int(out, 4, 0x00, name_match);
            encode_str(out, v);
        } else {
            // Literal Header Field without Indexing — New Name
            out.push_back(0x00);
            encode_str(out, k);
            encode_str(out, v);
        }
    }
    return out;
}

bool HpackCodec::decode_headers(const uint8_t* data, size_t len,
                                std::vector<std::pair<std::string, std::string>>& out_headers) {
    size_t pos = 0;
    while (pos < len) {
        uint8_t b = data[pos];
        if (b & 0x80u) {
            // Indexed Header Field
            size_t idx = 0;
            if (!decode_int(data, len, pos, 7, idx)) return false;
            std::string n, v;
            if (!lookup_index(idx, n, v)) return false;
            out_headers.emplace_back(std::move(n), std::move(v));
        } else if ((b & 0xC0u) == 0x40u) {
            // Literal Header Field with Incremental Indexing
            size_t idx = 0;
            if (!decode_int(data, len, pos, 6, idx)) return false;
            std::string n, v, unused;
            if (idx > 0) {
                if (!lookup_index(idx, n, unused)) return false;
            } else {
                if (!decode_str(data, len, pos, n)) return false;
            }
            if (!decode_str(data, len, pos, v)) return false;
            add_dynamic(n, v);
            out_headers.emplace_back(std::move(n), std::move(v));
        } else if ((b & 0xE0u) == 0x20u) {
            // Dynamic Table Size Update
            size_t new_sz = 0;
            if (!decode_int(data, len, pos, 5, new_sz)) return false;
            max_dynamic_bytes_ = std::min<size_t>(new_sz, 65536);
            while (!dynamic_.empty() && dynamic_bytes_ > max_dynamic_bytes_) {
                dynamic_bytes_ -= dynamic_.back().size();
                dynamic_.pop_back();
            }
        } else {
            // Literal Header Field without Indexing (0000xxxx) or Never Indexed (0001xxxx)
            size_t idx = 0;
            if (!decode_int(data, len, pos, 4, idx)) return false;
            std::string n, v, unused;
            if (idx > 0) {
                if (!lookup_index(idx, n, unused)) return false;
            } else {
                if (!decode_str(data, len, pos, n)) return false;
            }
            if (!decode_str(data, len, pos, v)) return false;
            out_headers.emplace_back(std::move(n), std::move(v));
        }
    }
    return true;
}

std::vector<uint8_t> Http2Client::pack_frame(const Http2Frame& frame) {
    uint32_t plen = static_cast<uint32_t>(frame.payload.size());
    std::vector<uint8_t> out;
    out.reserve(9 + plen);
    out.push_back(static_cast<uint8_t>((plen >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((plen >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(plen & 0xFFu));
    out.push_back(static_cast<uint8_t>(frame.type));
    out.push_back(frame.flags);
    uint32_t sid = frame.stream_id & 0x7FFFFFFFu;
    out.push_back(static_cast<uint8_t>((sid >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((sid >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((sid >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(sid & 0xFFu));
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return out;
}

bool Http2Client::unpack_frame(const uint8_t* buf, size_t len, Http2Frame& out, size_t& consumed) {
    if (!buf || len < 9) return false;
    uint32_t plen = (static_cast<uint32_t>(buf[0]) << 16) |
                    (static_cast<uint32_t>(buf[1]) << 8) |
                    static_cast<uint32_t>(buf[2]);
    if (len < 9 + plen) return false;
    out.type = static_cast<Http2FrameType>(buf[3]);
    out.flags = buf[4];
    out.stream_id = ((static_cast<uint32_t>(buf[5]) & 0x7Fu) << 24) |
                    (static_cast<uint32_t>(buf[6]) << 16) |
                    (static_cast<uint32_t>(buf[7]) << 8) |
                    static_cast<uint32_t>(buf[8]);
    out.payload.assign(buf + 9, buf + 9 + plen);
    consumed = 9 + plen;
    return true;
}

Http2Response Http2Client::execute_request(
    const ReadFn& read_fn,
    const WriteFn& write_fn,
    const std::string& method,
    const std::string& scheme,
    const std::string& authority,
    const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& extra_headers,
    const std::string& req_body) {
    Http2Response resp;

    Http2RequestSpec spec;
    spec.method = method;
    spec.scheme = scheme;
    spec.authority = authority;
    spec.path = path;
    spec.extra_headers = extra_headers;
    spec.body = req_body;
    auto batch = execute_multiplexed_batch(read_fn, write_fn, {spec});
    return batch.empty() ? Http2Response{} : std::move(batch[0]);
}

std::vector<Http2Response> Http2Client::execute_multiplexed_batch(
    const ReadFn& read_fn,
    const WriteFn& write_fn,
    const std::vector<Http2RequestSpec>& requests) {
    std::vector<Http2Response> responses(requests.size());
    if (requests.empty()) return responses;

    // 1. Send RFC 7540 client connection preface ONCE for all multiplexed streams
    static constexpr char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    if (!write_all(write_fn, reinterpret_cast<const uint8_t*>(kPreface), 24)) {
        for (auto& r : responses) r.error = "HTTP/2 failed to send preface";
        return responses;
    }

    // 2. Send client SETTINGS frame (ENABLE_PUSH=0, MAX_CONCURRENT_STREAMS=32, INITIAL_WINDOW_SIZE=1MiB)
    Http2Frame settings;
    settings.type = Http2FrameType::Settings;
    settings.flags = 0;
    settings.stream_id = 0;
    settings.payload = {
        0x00, 0x02, 0x00, 0x00, 0x00, 0x00, // SETTINGS_ENABLE_PUSH = 0
        0x00, 0x03, 0x00, 0x00, 0x00, 0x20, // SETTINGS_MAX_CONCURRENT_STREAMS = 32
        0x00, 0x04, 0x00, 0x10, 0x00, 0x00  // SETTINGS_INITIAL_WINDOW_SIZE = 1048576
    };
    auto sf_bytes = pack_frame(settings);
    if (!write_all(write_fn, sf_bytes.data(), sf_bytes.size())) {
        for (auto& r : responses) r.error = "HTTP/2 failed to send SETTINGS";
        return responses;
    }

    // 3. Dispatch all N requests on odd client-initiated streams (1, 3, 5, ...) over the same connection
    struct StreamState {
        size_t req_index = 0;
        std::vector<uint8_t> header_block;
        bool collecting_headers = false;
        bool done = false;
    };
    std::map<uint32_t, StreamState> streams;
    HpackCodec encoder;
    uint32_t last_sid = 1;

    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& req = requests[i];
        uint32_t sid = static_cast<uint32_t>(i * 2 + 1);
        last_sid = sid;
        streams[sid].req_index = i;

        std::vector<std::pair<std::string, std::string>> hlist;
        hlist.emplace_back(":method", req.method.empty() ? "GET" : req.method);
        hlist.emplace_back(":scheme", req.scheme.empty() ? "https" : req.scheme);
        hlist.emplace_back(":authority", req.authority);
        hlist.emplace_back(":path", req.path.empty() ? "/" : req.path);
        hlist.emplace_back("accept", "text/html,text/css,image/jpeg,image/png,*/*;q=0.8");
        hlist.emplace_back("accept-encoding", "gzip, deflate");
        for (const auto& [k, v] : req.extra_headers) {
            std::string lk = to_lower_ascii(k);
            if (lk == "host" || lk == "connection" || lk == "transfer-encoding" || lk == "keep-alive") continue;
            hlist.emplace_back(lk, v);
        }

        Http2Frame hframe;
        hframe.type = Http2FrameType::Headers;
        hframe.flags = 0x04u | (req.body.empty() ? 0x01u : 0x00u); // END_HEADERS | (END_STREAM if no body)
        hframe.stream_id = sid;
        hframe.payload = encoder.encode_headers(hlist);
        auto hf_bytes = pack_frame(hframe);
        if (!write_all(write_fn, hf_bytes.data(), hf_bytes.size())) {
            responses[i].error = "HTTP/2 failed to send HEADERS on stream " + std::to_string(sid);
            streams[sid].done = true;
            continue;
        }

        if (!req.body.empty()) {
            Http2Frame dframe;
            dframe.type = Http2FrameType::Data;
            dframe.flags = 0x01u; // END_STREAM
            dframe.stream_id = sid;
            dframe.payload.assign(req.body.begin(), req.body.end());
            auto df_bytes = pack_frame(dframe);
            if (!write_all(write_fn, df_bytes.data(), df_bytes.size())) {
                responses[i].error = "HTTP/2 failed to send DATA on stream " + std::to_string(sid);
                streams[sid].done = true;
            }
        }
    }

    // 4. Demultiplex interleaved binary frames until all active streams complete
    size_t remaining = 0;
    for (const auto& [sid, st] : streams) {
        if (!st.done) ++remaining;
    }

    HpackCodec decoder;
    size_t total_frames = 0;
    uint8_t hdr9[9];
    const size_t max_frames = 4096 * std::max<size_t>(1, requests.size());

    auto send_wu = [&](uint32_t sid, uint32_t inc) {
        Http2Frame wu;
        wu.type = Http2FrameType::WindowUpdate;
        wu.stream_id = sid;
        wu.payload = {
            static_cast<uint8_t>((inc >> 24) & 0x7Fu),
            static_cast<uint8_t>((inc >> 16) & 0xFFu),
            static_cast<uint8_t>((inc >> 8) & 0xFFu),
            static_cast<uint8_t>(inc & 0xFFu)
        };
        auto wb = pack_frame(wu);
        write_all(write_fn, wb.data(), wb.size());
    };

    while (remaining > 0 && total_frames < max_frames) {
        if (!read_exact(read_fn, hdr9, 9)) break;
        uint32_t plen = (static_cast<uint32_t>(hdr9[0]) << 16) |
                        (static_cast<uint32_t>(hdr9[1]) << 8) |
                        static_cast<uint32_t>(hdr9[2]);
        if (plen > 4 * 1024 * 1024) {
            for (auto& r : responses) {
                if (!r.ok && r.error.empty()) r.error = "HTTP/2 frame exceeds 4 MiB limit";
            }
            break;
        }
        Http2Frame f;
        f.type = static_cast<Http2FrameType>(hdr9[3]);
        f.flags = hdr9[4];
        f.stream_id = ((static_cast<uint32_t>(hdr9[5]) & 0x7Fu) << 24) |
                      (static_cast<uint32_t>(hdr9[6]) << 16) |
                      (static_cast<uint32_t>(hdr9[7]) << 8) |
                      static_cast<uint32_t>(hdr9[8]);
        f.payload.resize(plen);
        if (plen > 0 && !read_exact(read_fn, f.payload.data(), plen)) break;
        ++total_frames;

        if (f.type == Http2FrameType::Settings) {
            if ((f.flags & 0x01u) == 0) {
                Http2Frame ack;
                ack.type = Http2FrameType::Settings;
                ack.flags = 0x01u;
                ack.stream_id = 0;
                auto ab = pack_frame(ack);
                write_all(write_fn, ab.data(), ab.size());
            }
            continue;
        }
        if (f.type == Http2FrameType::Ping) {
            if ((f.flags & 0x01u) == 0 && f.payload.size() == 8) {
                Http2Frame pong;
                pong.type = Http2FrameType::Ping;
                pong.flags = 0x01u;
                pong.stream_id = 0;
                pong.payload = f.payload;
                auto pb = pack_frame(pong);
                write_all(write_fn, pb.data(), pb.size());
            }
            continue;
        }
        if (f.type == Http2FrameType::GoAway) {
            break;
        }

        auto sit = streams.find(f.stream_id);
        if (sit == streams.end()) continue;
        StreamState& st = sit->second;
        Http2Response& resp = responses[st.req_index];
        ++resp.frames_received;

        if (f.type == Http2FrameType::RstStream) {
            resp.error = "HTTP/2 stream " + std::to_string(f.stream_id) + " reset by peer";
            if (!st.done) { st.done = true; --remaining; }
        } else if (f.type == Http2FrameType::Headers || f.type == Http2FrameType::Continuation) {
            size_t start = 0;
            size_t end = f.payload.size();
            if (f.type == Http2FrameType::Headers) {
                st.header_block.clear();
                st.collecting_headers = true;
                if ((f.flags & 0x08u) && start < end) { // PADDED
                    uint8_t pad = f.payload[start++];
                    if (end >= pad) end -= pad;
                }
                if ((f.flags & 0x20u) && start + 5 <= end) { // PRIORITY
                    start += 5;
                }
            }
            if (st.collecting_headers && start <= end) {
                st.header_block.insert(st.header_block.end(),
                                       f.payload.begin() + static_cast<ptrdiff_t>(start),
                                       f.payload.begin() + static_cast<ptrdiff_t>(end));
            }
            if (f.flags & 0x04u) { // END_HEADERS
                st.collecting_headers = false;
                std::vector<std::pair<std::string, std::string>> decoded;
                if (decoder.decode_headers(st.header_block.data(), st.header_block.size(), decoded)) {
                    for (const auto& [k, v] : decoded) {
                        if (k == ":status") {
                            resp.status_code = std::atoi(v.c_str());
                        } else if (k == "set-cookie") {
                            resp.set_cookies.push_back(v);
                        } else if (!k.empty() && k[0] != ':') {
                            resp.headers[k] = v;
                        }
                    }
                }
            }
            if (f.type == Http2FrameType::Headers && (f.flags & 0x01u)) {
                if (!st.done) { st.done = true; --remaining; }
            }
        } else if (f.type == Http2FrameType::Data) {
            size_t start = 0;
            size_t end = f.payload.size();
            if ((f.flags & 0x08u) && start < end) { // PADDED
                uint8_t pad = f.payload[start++];
                if (end >= pad) end -= pad;
            }
            if (start < end && resp.body.size() < 16 * 1024 * 1024) {
                resp.body.append(reinterpret_cast<const char*>(f.payload.data() + start), end - start);
            }
            if (plen > 0 && !(f.flags & 0x01u)) {
                send_wu(0, plen);
                send_wu(f.stream_id, plen);
            }
            if (f.flags & 0x01u) {
                if (!st.done) { st.done = true; --remaining; }
            }
        }
    }

    // Send polite GOAWAY before closing socket
    Http2Frame goaway;
    goaway.type = Http2FrameType::GoAway;
    goaway.stream_id = 0;
    goaway.payload = {
        static_cast<uint8_t>((last_sid >> 24) & 0x7Fu),
        static_cast<uint8_t>((last_sid >> 16) & 0xFFu),
        static_cast<uint8_t>((last_sid >> 8) & 0xFFu),
        static_cast<uint8_t>(last_sid & 0xFFu),
        0, 0, 0, 0
    };
    auto gb = pack_frame(goaway);
    write_all(write_fn, gb.data(), gb.size());

    for (auto& r : responses) {
        r.ok = (r.status_code > 0);
        if (!r.ok && r.error.empty()) r.error = "HTTP/2 incomplete response";
    }
    return responses;
}

} // namespace astra
