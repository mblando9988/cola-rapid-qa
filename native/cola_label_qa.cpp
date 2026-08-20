// cola_label_qa.cpp — C++17 port of cola_label_qa.py
//
// Given a TTB F 5100.31 COLA PDF:
//   stage 1 (--task form)  : extract raw text with MuPDF, parse the page-1
//                            form into (question, answer) pairs, and extract
//                            every embedded label image
//   stage 2 (--task label) : OCR the images with tesseract (in-process),
//                            then match each label-relevant answer against
//                            the OCR'd text (MATCH / PARTIAL / NOT FOUND)
//   --task both (default)  : both stages in one run
//
// Dependencies: MuPDF C API, tesseract C++ API, vendored stb headers.
// No platform-specific code — builds on Linux (apt), Windows (vcpkg) and
// macOS (homebrew).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <tesseract/baseapi.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static std::string to_lower_ascii(const std::string& s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static std::string basename_of(const std::string& path) {
    fs::path p(path);
    return p.filename().string();
}

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// --- utf-8 ------------------------------------------------------------------

static int utf8_cp_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static std::string utf8_encode(int cp) {
    std::string out;
    if (cp < 0x80) { out += char(cp); }
    else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
    return out;
}

// decode the next code point at s[i]; returns false on malformed input
static bool utf8_decode(const std::string& s, size_t i, int& cp, int& len) {
    unsigned char c = (unsigned char)s[i];
    len = utf8_cp_len(c);
    if (len == 1) { cp = c; return true; }
    if (i + len > s.size()) return false;
    int v = 0;
    if (len == 2) {
        v = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
    } else if (len == 3) {
        v = ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6)
            | ((unsigned char)s[i + 2] & 0x3F);
    } else {
        v = ((c & 0x07) << 18) | (((unsigned char)s[i + 1] & 0x3F) << 12)
            | (((unsigned char)s[i + 2] & 0x3F) << 6)
            | ((unsigned char)s[i + 3] & 0x3F);
    }
    cp = v;
    return true;
}

// Latin-1-supplement diacritic map: U+00C0..U+00FF -> base ASCII ("" = drop)
static const char* diacritic_map(int cp) {
    if (cp < 0xC0 || cp > 0xFF) return nullptr;
    static const char* M[64] = {
        "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
        "D","N","O","O","O","O","O","","O","U","U","U","U","Y","TH","ss",
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        "d","n","o","o","o","o","o","","o","u","u","u","u","y","th","y"};
    return M[cp - 0xC0];
}

// port of Python norm(): casefold, strip diacritics, collapse punctuation
// to single spaces, trim. UTF-8 aware for the Latin-1 range + combining marks.
static std::string norm(const std::string& s) {
    std::string out;
    bool last_space = true;  // collapse leading spaces
    size_t i = 0;
    while (i < s.size()) {
        int cp = 0, len = 0;
        if (!utf8_decode(s, i, cp, len)) { out += s[i]; i++; continue; }
        i += (size_t)len;
        if (cp < 0x80) {
            char c = char(cp);
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (alnum) { out += c; last_space = false; }
            else if (!last_space) { out += ' '; last_space = true; }
            continue;
        }
        if (cp >= 0x0300 && cp <= 0x036F) continue;      // combining marks
        if (cp >= 0x00C0 && cp <= 0x00FF) {               // accented latin
            const char* base = diacritic_map(cp);
            if (base && base[0]) {
                for (const char* p = base; *p; p++) out += char(*p);
                last_space = false;
            }
            continue;
        }
        // anything else (other scripts): treat as a letter placeholder so
        // tokenization stays sane; out-of-scope for matching
        if (!last_space) out += ' ';
        out += 'a';
        last_space = true;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

static std::string compact_norm(const std::string& s) {
    std::string out = norm(s);
    out.erase(std::remove(out.begin(), out.end(), ' '), out.end());
    return out;
}

// case-insensitive rfind
static size_t irfind(const std::string& hay, const std::string& needle) {
    std::string h = to_lower_ascii(hay), n = to_lower_ascii(needle);
    return h.rfind(n);
}

// ---------------------------------------------------------------------------
// minimal JSON (parse + dump) — no external dependency
// ---------------------------------------------------------------------------

namespace json {

struct Value {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double num = 0;
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    static Value str(const std::string& v) { Value x; x.t = T::Str; x.s = v; return x; }
    static Value num_(double v) { Value x; x.t = T::Num; x.num = v; return x; }
    static Value boolean(bool v) { Value x; x.t = T::Bool; x.b = v; return x; }
    static Value array() { Value x; x.t = T::Arr; return x; }
    static Value object() { Value x; x.t = T::Obj; return x; }

    bool is_null() const { return t == T::Null; }
    bool is_str() const { return t == T::Str; }
    bool is_arr() const { return t == T::Arr; }
    bool is_obj() const { return t == T::Obj; }
    const std::string& as_str() const { return s; }

    void set(const std::string& k, Value v) { obj.emplace_back(k, std::move(v)); }
    void push(Value v) { arr.push_back(std::move(v)); }

    const Value* find(const std::string& k) const {
        if (t != T::Obj) return nullptr;
        for (auto& kv : obj)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    const Value* at(size_t i) const {
        if (t != T::Arr || i >= arr.size()) return nullptr;
        return &arr[i];
    }
};

class Parser {
    const std::string& s;
    size_t i = 0;
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    [[noreturn]] void fail(const std::string& m) { throw std::runtime_error("json: " + m + " at " + std::to_string(i)); }
    Value parse_value() {
        ws();
        if (i >= s.size()) fail("unexpected eof");
        char c = s[i];
        if (c == '{') return parse_obj();
        if (c == '[') return parse_arr();
        if (c == '"') { Value v; v.t = Value::T::Str; v.s = parse_str(); return v; }
        if (c == 't') { expect("true"); return Value::boolean(true); }
        if (c == 'f') { expect("false"); return Value::boolean(false); }
        if (c == 'n') { expect("null"); return Value(); }
        return parse_num();
    }
    void expect(const char* lit) {
        size_t n = strlen(lit);
        if (s.compare(i, n, lit) != 0) fail("bad literal");
        i += n;
    }
    std::string parse_str() {
        if (s[i] != '"') fail("expected string");
        i++;
        std::string out;
        while (i < s.size()) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"') { i++; return out; }
            if (c == '\\') {
                i++;
                if (i >= s.size()) fail("bad escape");
                char e = s[i];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i + 4 >= s.size()) fail("bad \\u");
                        int cp = 0;
                        for (int k = 1; k <= 4; k++) {
                            char h = s[i + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            else fail("bad \\u hex");
                        }
                        i += 4;
                        out += utf8_encode(cp);
                        break;
                    }
                    default: fail("bad escape char");
                }
                i++;
            } else if (c < 0x80) {
                out += char(c);
                i++;
            } else {
                // copy raw utf-8 byte(s)
                int len = utf8_cp_len(c);
                out += s.substr(i, (size_t)len);
                i += (size_t)len;
            }
        }
        fail("unterminated string");
    }
    Value parse_num() {
        size_t j = i;
        if (j < s.size() && (s[j] == '-' || s[j] == '+')) j++;
        while (j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == '.' || s[j] == 'e' || s[j] == 'E' || s[j] == '+' || s[j] == '-')) j++;
        Value v; v.t = Value::T::Num;
        v.num = strtod(s.substr(i, j - i).c_str(), nullptr);
        i = j;
        return v;
    }
    Value parse_arr() {
        Value v = Value::array();
        i++; ws();
        if (i < s.size() && s[i] == ']') { i++; return v; }
        while (true) {
            v.push(parse_value());
            ws();
            if (i >= s.size()) fail("unterminated array");
            if (s[i] == ',') { i++; continue; }
            if (s[i] == ']') { i++; return v; }
            fail("expected , or ]");
        }
    }
    Value parse_obj() {
        Value v = Value::object();
        i++; ws();
        if (i < s.size() && s[i] == '}') { i++; return v; }
        while (true) {
            ws();
            if (i >= s.size() || s[i] != '"') fail("expected key");
            std::string k = parse_str();
            ws();
            if (i >= s.size() || s[i] != ':') fail("expected :");
            i++;
            v.obj.emplace_back(k, parse_value());
            ws();
            if (i >= s.size()) fail("unterminated object");
            if (s[i] == ',') { i++; continue; }
            if (s[i] == '}') { i++; return v; }
            fail("expected , or }");
        }
    }
public:
    explicit Parser(const std::string& text) : s(text) {}
    Value run() {
        Value v = parse_value();
        ws();
        if (i != s.size()) fail("trailing data");
        return v;
    }
};

static Value parse(const std::string& text) { return Parser(text).run(); }

static void dump_str(std::ostringstream& o, const std::string& s) {
    o << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    o << buf;
                } else o << char(c);
        }
    }
    o << '"';
}

static std::string dump(const Value& v, int indent = 0) {
    std::ostringstream o;
    const auto pad = [&](int n) { for (int k = 0; k < n; k++) o << ' '; };
    switch (v.t) {
        case Value::T::Null: o << "null"; break;
        case Value::T::Bool: o << (v.b ? "true" : "false"); break;
        case Value::T::Num: {
            char buf[32];
            snprintf(buf, sizeof buf, "%g", v.num);
            o << buf;
            break;
        }
        case Value::T::Str: dump_str(o, v.s); break;
        case Value::T::Arr: {
            if (v.arr.empty()) { o << "[]"; break; }
            o << "[\n";
            for (size_t k = 0; k < v.arr.size(); k++) {
                pad(indent + 2);
                o << dump(v.arr[k], indent + 2);
                if (k + 1 < v.arr.size()) o << ',';
                o << '\n';
            }
            pad(indent);
            o << ']';
            break;
        }
        case Value::T::Obj: {
            if (v.obj.empty()) { o << "{}"; break; }
            o << "{\n";
            for (size_t k = 0; k < v.obj.size(); k++) {
                pad(indent + 2);
                dump_str(o, v.obj[k].first);
                o << ": " << dump(v.obj[k].second, indent + 2);
                if (k + 1 < v.obj.size()) o << ',';
                o << '\n';
            }
            pad(indent);
            o << '}';
            break;
        }
    }
    return o.str();
}

}  // namespace json

// ---------------------------------------------------------------------------
// form Q&A parsing (port of parse_form_qa)
// ---------------------------------------------------------------------------

static const std::regex Q_PAT(R"(^(\d{1,2}[a-z]?)\.\s)");
static const std::regex STOP_PAT(R"(^(PART I|PART II|PART III)\b)");
static const std::regex FURNITURE_PAT(
    R"(OMB No\.|https?://|FOR TTB USE ONLY|^TTB ID$|^TTB F \d)"
    R"(|^DEPARTMENT OF THE TREASURY$)"
    R"(|^ALCOHOL AND TOBACCO TAX AND TRADE BUREAU$)"
    R"(|^APPLICATION FOR AND$|^CERTIFICATION/EXEMPTION OF LABEL/BOTTLE$)"
    R"(|^APPROVAL$|^\(See Instructions and Paperwork Reduction Act)"
    R"( Notice on Back\)$|^\d{1,2}/\d{1,2}/\d{2,4}, \d{1,2}:\d{2}( AM| PM)?$)"
    R"(|^\d+/\d+$)", std::regex::icase);
// NOTE: the python original uses CONT_PAT.match() (anchored at start);
// every alternative here therefore starts with ^ — a line merely *ending*
// in "(Required)" must NOT be dropped
static const std::regex CONT_PAT(
    R"(^(?:NO\.\s*)?\(?(?:Required|If any)\)?$|^IF ON LABEL$|^APPLICATION$)",
    std::regex::icase);
static const std::regex TTBID_PAT(R"(TTB ID\s*\n?\s*(\d{6,20}))",
                                  std::regex::icase);

struct QAEntry {
    std::string num, question, answer;
    bool has_bbox = false;
    float bbox_x1 = 0, bbox_y1 = 0, bbox_x2 = 0, bbox_y2 = 0;
};

static std::vector<QAEntry> parse_form_qa(const std::string& page1) {
    std::vector<QAEntry> qa;
    bool in_entry = false;
    for (const std::string& raw : split_lines(page1)) {
        std::string s = trim(raw);
        if (s.empty()) continue;
        if (std::regex_search(s, FURNITURE_PAT) || std::regex_search(s, CONT_PAT))
            continue;
        if (std::regex_search(s, STOP_PAT)) { in_entry = false; continue; }
        std::smatch m;
        if (std::regex_search(s, m, Q_PAT)) {
            QAEntry e;
            e.num = m[1].str();
            e.question = s;
            qa.push_back(std::move(e));
            in_entry = true;
            continue;
        }
        if (in_entry && !qa.empty()) {
            // The applicant field's printed prompt wraps across multiple text
            // lines. Keep it out of the answer until Required is consumed.
            if (to_lower_ascii(qa.back().question).find("name and address") != std::string::npos
                && qa.back().answer.empty()
                && qa.back().question.find("(Required)") == std::string::npos) {
                qa.back().question += ' ';
                qa.back().question += s;
                continue;
            }
            if (!qa.back().answer.empty()) qa.back().answer += ' ';
            qa.back().answer += s;
        }
    }
    return qa;
}

// ---------------------------------------------------------------------------
// image pipeline (hand-rolled; no OpenCV)
// ---------------------------------------------------------------------------

struct Gray {
    int w = 0, h = 0;
    std::vector<uint8_t> p;  // row-major, w*h
};

static Gray load_gray(const std::string& path) {
    Gray g;
    int n = 0;
    unsigned char* data = stbi_load(path.c_str(), &g.w, &g.h, &n, 1);
    if (!data) return g;
    g.p.assign(data, data + (size_t)g.w * g.h);
    stbi_image_free(data);
    return g;
}

static Gray resize_bilinear(const Gray& src, int nw, int nh) {
    Gray d;
    d.w = nw; d.h = nh;
    d.p.resize((size_t)nw * nh);
    if (src.w <= 0 || src.h <= 0) return d;
    double fx = double(src.w) / nw, fy = double(src.h) / nh;
    for (int y = 0; y < nh; y++) {
        double sy = y * fy;
        int y0 = std::min((int)sy, src.h - 1);
        int y1 = std::min(y0 + 1, src.h - 1);
        double wy = sy - y0;
        for (int x = 0; x < nw; x++) {
            double sx = x * fx;
            int x0 = std::min((int)sx, src.w - 1);
            int x1 = std::min(x0 + 1, src.w - 1);
            double wx = sx - x0;
            double v = (1 - wy) * ((1 - wx) * src.p[(size_t)y0 * src.w + x0]
                                   + wx * src.p[(size_t)y0 * src.w + x1])
                     + wy * ((1 - wx) * src.p[(size_t)y1 * src.w + x0]
                             + wx * src.p[(size_t)y1 * src.w + x1]);
            d.p[(size_t)y * nw + x] = (uint8_t)std::lround(v);
        }
    }
    return d;
}

// separable box blur (moving average)
static Gray box_blur(const Gray& src, int radius) {
    Gray d = src;
    if (radius <= 0 || src.w <= 0) return d;
    int r = std::min(radius, std::min(src.w, src.h) / 2);
    int win = 2 * r + 1;
    // horizontal
    std::vector<uint8_t> tmp(src.p.size());
    for (int y = 0; y < src.h; y++) {
        long sum = 0;
        for (int x = -r; x <= r; x++) sum += src.p[(size_t)y * src.w + std::max(0, std::min(x, src.w - 1))];
        for (int x = 0; x < src.w; x++) {
            tmp[(size_t)y * src.w + x] = (uint8_t)(sum / win);
            int xin = std::max(0, std::min(x + r + 1, src.w - 1));
            int xout = std::max(0, std::min(x - r, src.w - 1));
            sum += src.p[(size_t)y * src.w + xin] - src.p[(size_t)y * src.w + xout];
        }
    }
    // vertical
    for (int x = 0; x < src.w; x++) {
        long sum = 0;
        for (int y = -r; y <= r; y++) sum += tmp[(size_t)std::max(0, std::min(y, src.h - 1)) * src.w + x];
        for (int y = 0; y < src.h; y++) {
            d.p[(size_t)y * src.w + x] = (uint8_t)(sum / win);
            int yin = std::max(0, std::min(y + r + 1, src.h - 1));
            int yout = std::max(0, std::min(y - r, src.h - 1));
            sum += tmp[(size_t)yin * src.w + x] - tmp[(size_t)yout * src.w + x];
        }
    }
    return d;
}

// illumination/tint flattening: divide by a large-radius background estimate
static Gray flatten_background(const Gray& src) {
    Gray flat = src;
    if (src.w <= 0) return flat;
    int radius = std::max(9, (std::min(src.w, src.h) / 25) | 1);
    Gray bg = box_blur(src, radius);
    for (size_t i = 0; i < src.p.size(); i++) {
        double v = double(src.p[i]) / (double(bg.p[i]) + 1e-6) * 255.0;
        flat.p[i] = (uint8_t)std::clamp(v, 0.0, 255.0);
    }
    return flat;
}

// Sauvola local threshold -> dark text on white
static Gray sauvola_bin(const Gray& src, int window = 25, double k = 0.2) {
    Gray out = src;
    if (src.w <= 0) return out;
    int win = std::min(window | 1, (std::min(src.w, src.h) - 1) | 1);
    if (win < 3) {  // fall back to global Otsu-ish mean
        long sum = 0;
        for (uint8_t v : src.p) sum += v;
        uint8_t th = (uint8_t)(sum / src.p.size());
        for (size_t i = 0; i < src.p.size(); i++) out.p[i] = src.p[i] > th ? 255 : 0;
        return out;
    }
    int r = win / 2;
    // integral images of gray and gray^2
    std::vector<long long> I((size_t)(src.w + 1) * (src.h + 1), 0), I2(I.size(), 0);
    for (int y = 0; y < src.h; y++) {
        for (int x = 0; x < src.w; x++) {
            size_t idx = (size_t)(y + 1) * (src.w + 1) + (x + 1);
            long long v = src.p[(size_t)y * src.w + x];
            I[idx] = v + I[idx - 1] + I[idx - (src.w + 1)] - I[idx - (src.w + 1) - 1];
            I2[idx] = v * v + I2[idx - 1] + I2[idx - (src.w + 1)] - I2[idx - (src.w + 1) - 1];
        }
    }
    auto rect_sum = [&](const std::vector<long long>& T, int x0, int y0, int x1, int y1) {
        x0 = std::max(0, x0); y0 = std::max(0, y0);
        x1 = std::min(src.w, x1); y1 = std::min(src.h, y1);
        if (x1 <= x0 || y1 <= y0) return 0LL;
        return T[(size_t)y1 * (src.w + 1) + x1] - T[(size_t)y1 * (src.w + 1) + x0]
             - T[(size_t)y0 * (src.w + 1) + x1] + T[(size_t)y0 * (src.w + 1) + x0];
    };
    for (int y = 0; y < src.h; y++) {
        for (int x = 0; x < src.w; x++) {
            int x0 = x - r, y0 = y - r, x1 = x + r + 1, y1 = y + r + 1;
            long long n = (long long)(x1 - std::max(0, x0)) * (y1 - std::max(0, y0));
            long long s = rect_sum(I, x0, y0, x1, y1);
            long long s2 = rect_sum(I2, x0, y0, x1, y1);
            double mean = double(s) / n;
            double var = std::max(0.0, double(s2) / n - mean * mean);
            double sd = std::sqrt(var);
            double t = mean * (1 + k * (sd / 128.0 - 1.0));
            out.p[(size_t)y * src.w + x] = src.p[(size_t)y * src.w + x] > t ? 255 : 0;
        }
    }
    return out;
}

static bool is_dark(const Gray& g) {
    // median-based polarity probe (port of normalize_polarity)
    if (g.p.empty()) return false;
    std::vector<uint8_t> v(g.p);
    size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid] < 110;
}

static Gray invert(const Gray& g) {
    Gray d = g;
    for (uint8_t& v : d.p) v = (uint8_t)(255 - v);
    return d;
}

// ---------------------------------------------------------------------------
// OCR with tesseract C++ API
// ---------------------------------------------------------------------------

struct OcrWord {
    std::string text;
    double conf = 0;
    int x = 0, y = 0, w = 0, h = 0;
};

struct Line {
    std::string text;
    double conf = 0;
    int y = 0;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0; // bounding box in image coords
};

struct ImgInfo {
    int page = 0;
    std::string name;      // e.g. p2_1.png
    std::string path;
    int width = 0, height = 0;
    std::string bbox;      // "x0 y0 x1 y1"
    std::string ocr_error;
    std::vector<OcrWord> words;
    std::vector<Line> lines;
    std::string text;
};

static std::string find_tessdata(const std::string& flag) {
    if (!flag.empty() && fs::exists(fs::path(flag) / "eng.traineddata")) return flag;
    const char* env = std::getenv("TESSDATA_PREFIX");
    if (env && *env && fs::exists(fs::path(env) / "eng.traineddata")) return env;
    static const char* cands[] = {
        "/usr/share/tesseract-ocr/5/tessdata",
        "/usr/share/tesseract-ocr/4.00/tessdata",
        "/usr/share/tesseract-ocr/tessdata",
        "/opt/homebrew/share/tessdata",
        "/usr/local/share/tessdata",
        "/usr/share/tessdata",
        "./tessdata",
        "C:/Program Files/Tesseract-OCR/tessdata",
        "C:/Program Files (x86)/Tesseract-OCR/tessdata",
    };
    for (const char* c : cands)
        if (fs::exists(fs::path(c) / "eng.traineddata")) return c;
    return "";
}

// one tesseract pass over a binarized image; coords divided by `scale`
static std::vector<OcrWord> tess_pass(tesseract::TessBaseAPI& api,
                                      const Gray& bin, int psm, double scale) {
    std::vector<OcrWord> out;
    if (bin.w <= 0 || bin.h <= 0) return out;
    api.SetPageSegMode((tesseract::PageSegMode)psm);
    api.SetImage(bin.p.data(), bin.w, bin.h, 1, bin.w);
    if (api.Recognize(nullptr) != 0) return out;
    tesseract::ResultIterator* it = api.GetIterator();
    if (!it) return out;
    do {
        if (it->Empty(tesseract::RIL_WORD)) continue;
        char* txt = it->GetUTF8Text(tesseract::RIL_WORD);
        if (!txt) continue;
        std::string word = trim(txt);
        delete[] txt;
        if (word.empty()) continue;
        float conf = it->Confidence(tesseract::RIL_WORD);
        if (conf < 0) continue;
        int x1, y1, x2, y2;
        it->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2);
        OcrWord w;
        w.text = word;
        w.conf = conf;
        w.x = (int)std::lround(x1 / scale);
        w.y = (int)std::lround(y1 / scale);
        w.w = std::max(1, (int)std::lround((x2 - x1) / scale));
        w.h = std::max(1, (int)std::lround((y2 - y1) / scale));
        out.push_back(std::move(w));
    } while (it->Next(tesseract::RIL_WORD));
    delete it;
    return out;
}

static double iou(const OcrWord& a, const OcrWord& b) {
    int iw = std::max(0, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
    int ih = std::max(0, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
    double inter = double(iw) * ih;
    if (inter <= 0) return 0;
    return inter / std::min(double(a.w) * a.h, double(b.w) * b.h);
}

// confidence-greedy NMS across psm/scale passes (port of merge_boxes)
static std::vector<OcrWord> merge_words(std::vector<OcrWord> words) {
    std::sort(words.begin(), words.end(), [](const OcrWord& a, const OcrWord& b) {
        if (a.conf != b.conf) return a.conf > b.conf;
        return double(a.w) * a.h > double(b.w) * b.h;
    });
    std::vector<OcrWord> kept;
    for (const OcrWord& w : words) {
        bool dup = false;
        for (const OcrWord& k : kept) {
            double ov = iou(w, k);
            if (ov >= 0.5 || (ov > 0.2 && to_lower_ascii(w.text) == to_lower_ascii(k.text))) {
                dup = true;
                break;
            }
        }
        if (!dup) kept.push_back(w);
    }
    return kept;
}

// group OCR words into reading-order lines (port of group_lines)
static std::vector<Line> group_lines(const std::vector<OcrWord>& words) {
    std::vector<const OcrWord*> ws;
    for (const OcrWord& w : words) ws.push_back(&w);
    std::sort(ws.begin(), ws.end(), [](const OcrWord* a, const OcrWord* b) {
        return a->y < b->y;
    });
    std::vector<std::vector<const OcrWord*>> lines;
    for (const OcrWord* w : ws) {
        double mid = w->y + w->h / 2.0;
        bool placed = false;
        for (auto& ln : lines) {
            const OcrWord* first = ln.front();
            const OcrWord* last = ln.back();
            double lmid = (first->y + first->h / 2.0 + last->y + last->h / 2.0) / 2.0;
            int lh = 0;
            for (const OcrWord* x : ln) lh = std::max(lh, x->h);
            if (std::abs(mid - lmid) < 0.6 * std::max(w->h, lh)) {
                ln.push_back(w);
                placed = true;
                break;
            }
        }
        if (!placed) lines.push_back({w});
    }
    std::vector<Line> out;
    for (auto& ln : lines) {
        std::sort(ln.begin(), ln.end(), [](const OcrWord* a, const OcrWord* b) {
            return a->x < b->x;
        });
        Line l;
        double conf_sum = 0;
        int bx1 = INT_MAX, by1 = INT_MAX, bx2 = INT_MIN, by2 = INT_MIN;
        for (const OcrWord* w : ln) {
            if (!l.text.empty()) l.text += ' ';
            l.text += w->text;
            conf_sum += w->conf;
            bx1 = std::min(bx1, w->x);
            by1 = std::min(by1, w->y);
            bx2 = std::max(bx2, w->x + w->w);
            by2 = std::max(by2, w->y + w->h);
        }
        l.conf = conf_sum / ln.size();
        l.y = ln.front()->y;
        l.x1 = bx1; l.y1 = by1; l.x2 = bx2; l.y2 = by2;
        out.push_back(std::move(l));
    }
    std::sort(out.begin(), out.end(), [](const Line& a, const Line& b) {
        return a.y < b.y;
    });
    return out;
}

// probe several preprocessing variants with a quick OCR pass and keep the
// one that reads best (port of label_ocr's _pick_document_plate): fancy
// label typography often reads better on the raw grayscale, while fine
// print benefits from flattening/binarization
static std::string pick_plate(std::vector<tesseract::TessBaseAPI*>& apis,
                              const Gray& g, Gray& out) {
    Gray flat = flatten_background(g);
    Gray fb = sauvola_bin(flat);
    if (is_dark(fb)) fb = invert(fb);
    struct Cand { const char* name; const Gray* img; } cands[3] = {
        {"raw", &g}, {"flat", &flat}, {"bin", &fb}};
    int maxdim = std::max(g.w, g.h);
    double sf = std::min(2.0, 1400.0 / maxdim);
    Gray big[3];
    double score[3];
    for (int i = 0; i < 3; i++)
        big[i] = resize_bilinear(*cands[i].img, (int)std::lround(g.w * sf),
                                 (int)std::lround(g.h * sf));
    std::vector<std::thread> ts;
    for (int i = 0; i < 3; i++) {
        ts.emplace_back([&, i]() {
            auto words = tess_pass(*apis[i], big[i], 3, sf);
            double sum = 0;
            int n = 0;
            for (auto& w : words) { sum += w.conf; n++; }
            double mean = n ? sum / n : 0;
            score[i] = mean * std::min(1.0, n / 20.0);
        });
    }
    for (auto& t : ts) t.join();
    double best = -1;
    std::string best_name = "raw";
    for (int i = 0; i < 3; i++)
        if (score[i] > best) {
            best = score[i];
            best_name = cands[i].name;
            out = *cands[i].img;
        }
    return best_name;
}

static void ocr_image(ImgInfo& im, std::vector<tesseract::TessBaseAPI*>& apis) {
    Gray g = load_gray(im.path);
    if (g.w <= 0 || g.h <= 0) { im.ocr_error = "cannot read image"; return; }
    im.width = g.w;
    im.height = g.h;
    Gray plate;
    std::string plate_name = pick_plate(apis, g, plate);
    std::vector<OcrWord> all;
    int maxdim = std::max(g.w, g.h);
    // Resize each scale up-front, then run the (scale, psm) passes in parallel.
    // Results land in slots and are appended in the original order, so the
    // merge is bit-identical to the sequential version.
    struct Pass { const Gray* img; double sf; int psm; };
    std::vector<Gray> bigs;
    bigs.reserve(2);
    std::vector<Pass> passes;
    for (double scale : {2.0, 4.0}) {
        double maxdim_cap = (scale == 2.0 ? 1400.0 : 2800.0);
        double sf = std::min(scale, maxdim_cap / maxdim);
        if (sf <= 1.0) continue;
        bigs.push_back(resize_bilinear(plate, (int)std::lround(g.w * sf),
                                       (int)std::lround(g.h * sf)));
        const Gray* big = &bigs.back();
        for (int psm : {3, 11}) passes.push_back({big, sf, psm});
    }
    std::vector<std::vector<OcrWord>> results(passes.size());
    std::vector<std::thread> ts;
    for (size_t i = 0; i < passes.size(); i++) {
        ts.emplace_back([&, i]() {
            results[i] = tess_pass(*apis[i], *passes[i].img, passes[i].psm,
                                   passes[i].sf);
        });
    }
    for (auto& t : ts) t.join();
    for (auto& r : results)
        for (auto& w : r) all.push_back(std::move(w));
    im.words = merge_words(std::move(all));
    im.lines = group_lines(im.words);
    for (const Line& l : im.lines) {
        if (!im.text.empty()) im.text += ' ';
        im.text += l.text;
    }
    (void)plate_name;
}

// ---------------------------------------------------------------------------
// matching (port of match_answer + match_class_type)
// ---------------------------------------------------------------------------

enum class FieldKind { Brand, Fanciful, NetContents, Alcohol, Appellation,
                       Vintage, ProductType, Wording, NameAddress, Admin };

static FieldKind field_kind(const std::string& question) {
    std::string q = to_lower_ascii(question);
    struct { const char* key; FieldKind kind; } tab[] = {
        {"show any wording", FieldKind::Wording},
        {"show any information", FieldKind::Wording},
        {"brand name", FieldKind::Brand},
        {"fanciful name", FieldKind::Fanciful},
        {"net contents", FieldKind::NetContents},
        {"alcohol content", FieldKind::Alcohol},
        {"appellation", FieldKind::Appellation},
        {"vintage", FieldKind::Vintage},
        {"type of product", FieldKind::ProductType},
        {"name and address", FieldKind::NameAddress},
    };
    for (auto& t : tab)
        if (q.find(t.key) != std::string::npos) return t.kind;
    return FieldKind::Admin;
}

struct Evidence {
    bool found = false;
    std::string image, line;
    double conf = -1;
    int found_n = 0, of_n = 0;
    bool has_counts = false;
    std::string matched_as;
    int line_x1 = 0, line_y1 = 0, line_x2 = 0, line_y2 = 0; // bbox of matched line in image
};

struct MatchResult {
    std::string status;  // MATCH | PARTIAL | NOT FOUND | SKIP | EMPTY
    Evidence ev;
    std::string num, question, answer, note;
    bool mandatory = false;
    std::string mandatory_message;
};

// all OCR lines across every image, tagged with the source image name
struct TaggedLine {
    std::string image;
    Line line;
};

static std::vector<TaggedLine> tagged_corpus(const std::vector<ImgInfo>& images) {
    std::vector<TaggedLine> out;
    for (const ImgInfo& im : images)
        for (const Line& l : im.lines) out.push_back({im.name, l});
    return out;
}

// first OCR line matching pred -> evidence
template <typename Pred>
static Evidence find_in_corpus(const std::vector<TaggedLine>& tl, Pred pred) {
    Evidence e;
    for (const TaggedLine& t : tl) {
        if (pred(norm(t.line.text))) {
            e.found = true;
            e.image = t.image;
            e.line = t.line.text;
            e.conf = t.line.conf;
            e.line_x1 = t.line.x1;
            e.line_y1 = t.line.y1;
            e.line_x2 = t.line.x2;
            e.line_y2 = t.line.y2;
            return e;
        }
    }
    return e;
}

static Evidence full_match(const std::string& answer,
                           const std::vector<TaggedLine>& tl) {
    std::string a = norm(answer);
    if (a.empty()) return Evidence{};
    return find_in_corpus(tl, [&](const std::string& nl) {
        return nl.find(a) != std::string::npos;
    });
}

static Evidence full_match_compact(const std::string& answer,
                                   const std::vector<TaggedLine>& tl) {
    const std::string compact = compact_norm(answer);
    if (compact.empty()) return Evidence{};
    return find_in_corpus(tl, [&](const std::string& line) {
        std::string normalized = line;
        normalized.erase(std::remove(normalized.begin(), normalized.end(), ' '), normalized.end());
        return normalized.find(compact) != std::string::npos;
    });
}

static Evidence token_match(const std::string& answer,
                            const std::vector<TaggedLine>& tl,
                            int min_len, double need) {
    std::vector<std::string> toks;
    {
        std::istringstream is(norm(answer));
        std::string t;
        while (is >> t)
            if ((int)t.size() >= min_len) toks.push_back(t);
    }
    if (toks.empty()) return Evidence{};
    Evidence same_line = find_in_corpus(tl, [&](const std::string& line) {
        for (const std::string& token : toks)
            if (line.find(token) == std::string::npos) return false;
        return true;
    });
    if (same_line.found) {
        same_line.has_counts = true;
        same_line.found_n = (int)toks.size();
        same_line.of_n = (int)toks.size();
        return same_line;
    }
    std::vector<std::string> found;
    for (const std::string& t : toks) {
        Evidence e = find_in_corpus(tl, [&](const std::string& nl) {
            return nl.find(t) != std::string::npos;
        });
        if (e.found) found.push_back(t);
    }
    Evidence e;
    e.has_counts = true;
    e.found_n = (int)found.size();
    e.of_n = (int)toks.size();
    if (found.size() / double(toks.size()) >= need) {
        e.found = true;
        e.image = "multiple";
        for (size_t i = 0; i < found.size(); i++) {
            if (i) e.line += ' ';
            e.line += found[i];
        }
    }
    return e;
}

static MatchResult match_government_warning(const std::vector<TaggedLine>& tl) {
    static const std::vector<std::string> anchors = {
        "governmentwarning",
        "surgeongeneral",
        "womenshouldnotdrink",
        "pregnancy",
        "birthdefects",
        "consumptionofalcoholicbeverages",
        "abilitytodrive",
        "healthproblems",
    };
    std::string corpus;
    for (const TaggedLine& tagged : tl) corpus += compact_norm(tagged.line.text);
    int found_n = 0;
    for (const std::string& anchor : anchors)
        if (corpus.find(anchor) != std::string::npos) found_n++;

    MatchResult result;
    result.num = "GW";
    result.question = "GOVERNMENT WARNING (100% REQUIRED)";
    result.answer = "100% REQUIRED";
    result.note = "mandatory government warning check";
    result.mandatory = true;
    result.ev = find_in_corpus(tl, [](const std::string& line) {
        return line.find("government") != std::string::npos
            && line.find("warning") != std::string::npos;
    });
    if (!result.ev.found && found_n) {
        result.ev = find_in_corpus(tl, [&](const std::string& line) {
            std::string compact = line;
            compact.erase(std::remove(compact.begin(), compact.end(), ' '), compact.end());
            for (const std::string& anchor : anchors)
                if (compact.find(anchor) != std::string::npos) return true;
            return false;
        });
    }
    result.ev.has_counts = true;
    result.ev.found_n = found_n;
    result.ev.of_n = (int)anchors.size();
    if (found_n == (int)anchors.size()) {
        result.status = "MATCH";
        result.mandatory_message = "SEEN - government warning complete ("
            + std::to_string(found_n) + "/" + std::to_string(anchors.size()) + " checks)";
    } else if (found_n) {
        result.status = "PARTIAL";
        result.mandatory_message = "INCOMPLETE - government warning ("
            + std::to_string(found_n) + "/" + std::to_string(anchors.size()) + " checks)";
    } else {
        result.status = "NOT FOUND";
        result.mandatory_message = "NOT SEEN - government warning is mandatory";
    }
    return result;
}

static MatchResult match_answer(FieldKind kind, const std::string& question,
                                const std::string& answer,
                                const std::vector<TaggedLine>& tl) {
    MatchResult r;
    r.question = question;
    r.answer = answer;
    std::string a = norm(answer);

    if (kind == FieldKind::NetContents) {
        Evidence ev = full_match(answer, tl);
        if (!ev.found) {
            std::smatch m;
            if (std::regex_search(answer, m, std::regex(R"((\d+(?:\.\d+)?))"))) {
                std::string vol = m[1].str();
                // 'l' misread as i/!/1/| is common at low res (750ml -> "750 mi")
                std::regex pat(R"(\b)" + vol + R"((?:\.0+)?\s*m[l|i|!|1]\b)"
                               R"(|\b)" + vol + R"((?:\.0+)?\s*millilit(?:er|re)s?\b)",
                               std::regex::icase);
                ev = find_in_corpus(tl, [&](const std::string& raw) {
                    return std::regex_search(raw, pat);
                });
            }
        }
        r.status = ev.found ? "MATCH" : "NOT FOUND";
        r.ev = ev;
        return r;
    }

    if (kind == FieldKind::Alcohol) {
        std::smatch m;
        Evidence ev;
        if (std::regex_search(answer, m, std::regex(R"((\d+(?:\.\d+)?))"))) {
            std::string n = m[1].str();
            // tolerate low-res misreads: "14%" can come out as "14 0",
            // "14 %", "14%0", and "by volume" is the alcohol context
            std::regex pat(
                R"(\b)" + n +
                R"((?:\.0+)?\s*0?\s*(?:%|percent|per cent|alc\.?|alcohol|by vol(?:ume)?))"
                R"(|(?:alc\.?|alcohol)[.:]?\s*\b)" + n + R"((?:\.0+)?\b)",
                std::regex::icase);
            ev = find_in_corpus(tl, [&](const std::string& raw) {
                return std::regex_search(raw, pat);
            });
        } else {
            ev = full_match(answer, tl);
        }
        r.status = ev.found ? "MATCH" : "NOT FOUND";
        r.ev = ev;
        return r;
    }

    if (kind == FieldKind::ProductType) {
        // the form lists all three options as checkboxes; match whichever
        // actually appears on the label — prefer the Italian rendering
        std::string u = to_lower_ascii(answer);
        std::vector<std::pair<std::string, std::string>> cands;
        if (u.find("wine") != std::string::npos) {
            cands.push_back({"vino", "wine"});
            cands.push_back({"wine", "wine"});
        }
        if (u.find("distilled") != std::string::npos)
            cands.push_back({"distilled spirits", "distilled spirits"});
        if (u.find("malt") != std::string::npos)
            cands.push_back({"malt beverage", "malt beverage"});
        for (auto& cand : cands) {
            Evidence ev = full_match(cand.first, tl);
            if (ev.found) {
                r.status = "MATCH";
                ev.matched_as = cand.second;
                r.ev = ev;
                return r;
            }
        }
        r.status = "NOT FOUND";
        return r;
    }

    if (kind == FieldKind::Wording) {
        std::vector<std::string> toks;
        {
            std::istringstream is(a);
            std::string t;
            while (is >> t)
                if ((int)t.size() >= 4) toks.push_back(t);
        }
        if (toks.empty()) { r.status = "EMPTY"; return r; }
        std::vector<std::string> found;
        for (const std::string& t : toks) {
            Evidence e = find_in_corpus(tl, [&](const std::string& nl) {
                return nl.find(t) != std::string::npos;
            });
            if (e.found) found.push_back(t);
        }
        double cov = found.size() / double(toks.size());
        r.status = cov >= 0.9 ? "MATCH" : (cov >= 0.5 ? "PARTIAL" : "NOT FOUND");
        r.ev.found = true;
        r.ev.image = "multiple";
        r.ev.has_counts = true;
        r.ev.found_n = (int)found.size();
        r.ev.of_n = (int)toks.size();
        for (size_t i = 0; i < found.size(); i++) {
            if (i) r.ev.line += ' ';
            r.ev.line += found[i];
        }
        return r;
    }

    if (kind == FieldKind::NameAddress) {
        size_t idx = irfind(answer, "(Used on label)");
        if (idx == std::string::npos) {
            r.status = "SKIP";
            r.note = "no tradename marked 'Used on label'";
            return r;
        }
        std::istringstream is(answer.substr(0, idx));
        std::vector<std::string> toks;
        std::string t;
        while (is >> t) toks.push_back(t);
        for (int n = 4; n >= 1; n--) {
            if ((int)toks.size() < n) continue;
            std::string dba;
            for (int k = (int)toks.size() - n; k < (int)toks.size(); k++) {
                if (!dba.empty()) dba += ' ';
                dba += toks[k];
            }
            dba = trim(dba);
            while (!dba.empty() && (dba.back() == ',' || dba.back() == '.'))
                dba.pop_back();
            Evidence ev = full_match(dba, tl);
            if (!ev.found) ev = full_match_compact(dba, tl);
            if (ev.found) {
                r.status = "MATCH";
                ev.matched_as = dba;
                r.ev = ev;
                return r;
            }
        }
        r.status = "NOT FOUND";
        return r;
    }

    if (kind == FieldKind::Admin) {
        r.status = "SKIP";
        r.note = "administrative field — not printed on label";
        return r;
    }

    // brand / fanciful / appellation / vintage
    Evidence ev = full_match(answer, tl);
    if (ev.found) { r.status = "MATCH"; r.ev = ev; return r; }
    ev = token_match(answer, tl, 3, 1.0);
    if (ev.found) {
        r.status = (ev.found_n == ev.of_n) ? "MATCH" : "PARTIAL";
        r.ev = ev;
        return r;
    }
    r.status = "NOT FOUND";
    return r;
}

struct ClassTypeResult {
    std::string status, class_type, matched_as;
    Evidence ev;
};

static ClassTypeResult match_class_type(const std::vector<TaggedLine>& tl,
                                        const std::string& page2) {
    ClassTypeResult r;
    std::smatch m;
    if (!std::regex_search(page2, m, std::regex(R"(CLASS/TYPE DESCRIPTION\s*\n([^\n]+))",
                                                std::regex::icase))) {
        r.status = "SKIP";
        return r;
    }
    r.class_type = trim(m[1].str());
    std::string nct = norm(r.class_type);
    std::vector<std::string> variants = {nct};
    {
        std::istringstream is(nct);
        std::vector<std::string> words;
        std::string t;
        while (is >> t) words.push_back(t);
        if (words.size() == 3 && words[0] == "table" && words[1] == "red"
            && words[2] == "wine") {
            variants.push_back("red wine");
            variants.push_back("red table wine");
            variants.push_back("vino rosso da tavola");
            variants.push_back("vino rosso");
        }
    }
    for (const std::string& v : variants) {
        Evidence ev = full_match(v, tl);
        if (!ev.found) ev = full_match_compact(v, tl);
        if (ev.found) {
            r.status = "MATCH";
            r.matched_as = v;
            r.ev = ev;
            return r;
        }
    }
    r.status = "NOT FOUND";
    return r;
}

// ---------------------------------------------------------------------------
// PDF extraction with MuPDF (stage 1)
// ---------------------------------------------------------------------------

static fz_stext_options stext_options(fz_context* ctx) {
    fz_stext_options opts;
    memset(&opts, 0, sizeof(opts));
    // without PRESERVE_IMAGES this MuPDF build drops image blocks from the
    // structured-text page entirely
    opts.flags |= FZ_STEXT_PRESERVE_IMAGES;
    return opts;
}

static constexpr float DOCUMENT_RENDER_SCALE = 1.5f;

static std::string extract_page_text(fz_context* ctx, fz_page* page) {
    std::string out;
    fz_stext_options opts = stext_options(ctx);
    fz_stext_page* st = fz_new_stext_page_from_page(ctx, page, &opts);
    if (!st) return out;
    for (fz_stext_block* b = st->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line* l = b->u.t.first_line; l; l = l->next) {
            std::string line;
            bool have_prev = false;
            double prev_ur_x = 0;
            for (fz_stext_char* c = l->first_char; c; c = c->next) {
                if (have_prev) {
                    double gap = c->quad.ul.x - prev_ur_x;
                    double ch = c->quad.ll.y - c->quad.ul.y;
                    if (gap > 0.15 * ch && !line.empty() && line.back() != ' ')
                        line += ' ';
                }
                if (c->c < 0x80) line += char(c->c);
                else line += utf8_encode(c->c);
                prev_ur_x = c->quad.ur.x;
                have_prev = true;
            }
            out += line;
            out += '\n';
        }
    }
    fz_drop_stext_page(ctx, st);
    return out;
}

static void attach_form_field_boxes(fz_context* ctx, fz_document* doc,
                                    std::vector<QAEntry>& qa) {
    if (fz_count_pages(ctx, doc) < 1 || qa.empty()) return;
    fz_page* page = fz_load_page(ctx, doc, 0);
    if (!page) return;
    fz_stext_options opts = stext_options(ctx);
    fz_stext_page* st = fz_new_stext_page_from_page(ctx, page, &opts);
    if (st) {
        for (fz_stext_block* block = st->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                std::string text;
                for (fz_stext_char* c = line->first_char; c; c = c->next) {
                    if (c->c < 0x80) text += char(c->c);
                    else text += utf8_encode(c->c);
                }
                text = trim(text);
                std::smatch match;
                if (!std::regex_search(text, match, Q_PAT)) continue;
                const std::string num = match[1].str();
                for (QAEntry& entry : qa) {
                    if (entry.has_bbox || entry.num != num) continue;
                    entry.has_bbox = true;
                    entry.bbox_x1 = line->bbox.x0 * DOCUMENT_RENDER_SCALE;
                    entry.bbox_y1 = line->bbox.y0 * DOCUMENT_RENDER_SCALE;
                    entry.bbox_x2 = line->bbox.x1 * DOCUMENT_RENDER_SCALE;
                    entry.bbox_y2 = line->bbox.y1 * DOCUMENT_RENDER_SCALE;
                    break;
                }
            }
        }
        fz_drop_stext_page(ctx, st);
    }
    fz_drop_page(ctx, page);
}

static bool apply_pdf_widget_values(fz_context* ctx, fz_document* doc,
                                    std::vector<QAEntry>& qa) {
    pdf_document* pdf = pdf_specifics(ctx, doc);
    if (!pdf || fz_count_pages(ctx, doc) < 1) return false;
    fz_page* generic_page = fz_load_page(ctx, doc, 0);
    if (!generic_page) return false;
    pdf_page* page = pdf_page_from_fz_page(ctx, generic_page);
    if (!page) {
        fz_drop_page(ctx, generic_page);
        return false;
    }

    bool applied = false;
    bool product_widget_applied = false;
    for (pdf_annot* widget = pdf_first_widget(ctx, page);
         widget; widget = pdf_next_widget(ctx, widget)) {
        pdf_obj* object = pdf_annot_obj(ctx, widget);
        char* loaded_name = pdf_load_field_name(ctx, object);
        const char* raw_value = pdf_annot_field_value(ctx, widget);
        const std::string name = loaded_name ? loaded_name : "";
        const std::string value = raw_value ? trim(raw_value) : "";
        if (loaded_name) fz_free(ctx, loaded_name);
        if (name.empty() || value.empty() || value == "Off" || value == "/Off")
            continue;

        if (!product_widget_applied && name == "Check Box22") {
            std::string selected = to_lower_ascii(value);
            if (selected.find("wine") != std::string::npos
                || selected.find("spirit") != std::string::npos
                || selected.find("malt") != std::string::npos) {
                const std::string product = selected.find("wine") != std::string::npos
                    ? "WINE" : selected.find("spirit") != std::string::npos
                    ? "DISTILLED SPIRITS" : "MALT BEVERAGE";
                for (QAEntry& entry : qa) {
                    if (field_kind(entry.question) != FieldKind::ProductType) continue;
                    entry.answer = product;
                    const fz_rect bbox = pdf_bound_widget(ctx, widget);
                    entry.has_bbox = true;
                    entry.bbox_x1 = bbox.x0 * DOCUMENT_RENDER_SCALE;
                    entry.bbox_y1 = bbox.y0 * DOCUMENT_RENDER_SCALE;
                    entry.bbox_x2 = bbox.x1 * DOCUMENT_RENDER_SCALE;
                    entry.bbox_y2 = bbox.y1 * DOCUMENT_RENDER_SCALE;
                    product_widget_applied = true;
                    applied = true;
                    break;
                }
                continue;
            }
        }

        const FieldKind kind = field_kind(name);
        if (kind == FieldKind::Admin || kind == FieldKind::ProductType)
            continue;

        QAEntry* target = nullptr;
        for (QAEntry& entry : qa) {
            if (field_kind(entry.question) == kind) {
                target = &entry;
                break;
            }
        }
        if (!target) {
            QAEntry entry;
            std::smatch match;
            if (std::regex_search(name, match, Q_PAT)) entry.num = match[1].str();
            entry.question = name;
            qa.push_back(std::move(entry));
            target = &qa.back();
        }

        target->answer = value;
        const fz_rect bbox = pdf_bound_widget(ctx, widget);
        target->has_bbox = true;
        target->bbox_x1 = bbox.x0 * DOCUMENT_RENDER_SCALE;
        target->bbox_y1 = bbox.y0 * DOCUMENT_RENDER_SCALE;
        target->bbox_x2 = bbox.x1 * DOCUMENT_RENDER_SCALE;
        target->bbox_y2 = bbox.y1 * DOCUMENT_RENDER_SCALE;
        applied = true;
    }
    fz_drop_page(ctx, generic_page);
    return applied;
}

static std::string selected_product_type(fz_context* ctx, fz_document* doc) {
    if (fz_count_pages(ctx, doc) < 1) return "";
    fz_page* page = fz_load_page(ctx, doc, 0);
    if (!page) return "";

    struct OptionLine {
        const char* label;
        fz_rect bbox;
        bool found = false;
        double dark_ratio = 0;
    } options[] = {
        {"WINE", fz_empty_rect, false, 0},
        {"DISTILLED SPIRITS", fz_empty_rect, false, 0},
        {"MALT BEVERAGE", fz_empty_rect, false, 0},
    };

    fz_stext_options opts = stext_options(ctx);
    fz_stext_page* st = fz_new_stext_page_from_page(ctx, page, &opts);
    if (st) {
        for (fz_stext_block* b = st->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* line = b->u.t.first_line; line; line = line->next) {
                std::string text;
                for (fz_stext_char* c = line->first_char; c; c = c->next) {
                    if (c->c < 0x80) text += char(c->c);
                    else text += utf8_encode(c->c);
                }
                text = trim(text);
                for (auto& option : options) {
                    if (text == option.label
                        || (std::string(option.label) == "MALT BEVERAGE"
                            && text == "MALT BEVERAGES")) {
                        option.bbox = line->bbox;
                        option.found = true;
                    }
                }
            }
        }
    }

    const float scale = DOCUMENT_RENDER_SCALE;
    fz_pixmap* pm = fz_new_pixmap_from_page(
        ctx, page, fz_scale(scale, scale), fz_device_rgb(ctx), 0);
    if (pm) {
        const int width = fz_pixmap_width(ctx, pm);
        const int height = fz_pixmap_height(ctx, pm);
        const int components = fz_pixmap_components(ctx, pm);
        const int stride = fz_pixmap_stride(ctx, pm);
        const int origin_x = fz_pixmap_x(ctx, pm);
        const int origin_y = fz_pixmap_y(ctx, pm);
        unsigned char* samples = fz_pixmap_samples(ctx, pm);

        for (auto& option : options) {
            if (!option.found) continue;
            const int x0 = std::max(0, (int)std::lround((option.bbox.x0 - 14) * scale) - origin_x);
            const int x1 = std::min(width, (int)std::lround((option.bbox.x0 - 6) * scale) - origin_x);
            const int y0 = std::max(0, (int)std::lround((option.bbox.y0 - 3) * scale) - origin_y);
            const int y1 = std::min(height, (int)std::lround((option.bbox.y0 + 5) * scale) - origin_y);
            int dark = 0;
            int total = 0;
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    const size_t offset = (size_t)y * stride + (size_t)x * components;
                    const int gray = components >= 3
                        ? (samples[offset] + samples[offset + 1] + samples[offset + 2]) / 3
                        : samples[offset];
                    if (gray < 220) dark++;
                    total++;
                }
            }
            option.dark_ratio = total ? dark / double(total) : 0;
        }
        fz_drop_pixmap(ctx, pm);
    }

    if (st) fz_drop_stext_page(ctx, st);
    fz_drop_page(ctx, page);

    OptionLine* selected = nullptr;
    for (auto& option : options) {
        if (!selected || option.dark_ratio > selected->dark_ratio) {
            selected = &option;
        }
    }
    double second_best = 0;
    for (auto& option : options) {
        if (&option != selected) second_best = std::max(second_best, option.dark_ratio);
    }
    return selected && selected->dark_ratio >= 0.30
        && selected->dark_ratio - second_best >= 0.10 ? selected->label : "";
}

static std::vector<ImgInfo> extract_images(fz_context* ctx, fz_document* doc,
                                           const std::string& out_dir) {
    std::vector<ImgInfo> imgs;
    int npages = fz_count_pages(ctx, doc);
    for (int p = 0; p < npages; p++) {
        fz_page* page = fz_load_page(ctx, doc, p);
        if (!page) continue;
        fz_stext_options opts = stext_options(ctx);
        fz_stext_page* st = fz_new_stext_page_from_page(ctx, page, &opts);
        int seq = 0;
        if (st) {
            for (fz_stext_block* b = st->first_block; b; b = b->next) {
                if (b->type != FZ_STEXT_BLOCK_IMAGE) continue;
                fz_image* img = b->u.i.image;
                if (!img) continue;
                fz_pixmap* pm = fz_get_unscaled_pixmap_from_image(ctx, img);
                if (!pm) continue;
                int w = fz_pixmap_width(ctx, pm);
                int h = fz_pixmap_height(ctx, pm);
                int n = fz_pixmap_components(ctx, pm);
                // Skip tiny images (icons, checkboxes) — not labels
                if (w < 50 || h < 50) {
                    fz_drop_pixmap(ctx, pm);
                    continue;
                }
                // Skip images on pages beyond the label pages (signatures are on pg 4+)
                if (p + 1 > 3) {
                    fz_drop_pixmap(ctx, pm);
                    continue;
                }
                unsigned char* sp = fz_pixmap_samples(ctx, pm);
                std::vector<uint8_t> rgb((size_t)w * h * 3);
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        size_t si = (size_t)y * fz_pixmap_stride(ctx, pm) + (size_t)x * n;
                        size_t di = ((size_t)y * w + x) * 3;
                        if (n >= 3) {
                            rgb[di] = sp[si];
                            rgb[di + 1] = sp[si + 1];
                            rgb[di + 2] = sp[si + 2];
                        } else {
                            rgb[di] = rgb[di + 1] = rgb[di + 2] = sp[si];
                        }
                    }
                }
                seq++;
                ImgInfo im;
                im.page = p + 1;
                im.name = "p" + std::to_string(p + 1) + "_" + std::to_string(seq) + ".png";
                im.path = (fs::path(out_dir) / im.name).string();
                im.width = w;
                im.height = h;
                char bboxbuf[128];
                snprintf(bboxbuf, sizeof bboxbuf, "%g %g %g %g",
                         b->bbox.x0, b->bbox.y0, b->bbox.x1, b->bbox.y1);
                im.bbox = bboxbuf;
                stbi_write_png(im.path.c_str(), w, h, 3, rgb.data(), w * 3);
                imgs.push_back(std::move(im));
                fz_drop_pixmap(ctx, pm);
            }
            fz_drop_stext_page(ctx, st);
        }
        fz_drop_page(ctx, page);
    }
    return imgs;
}

static std::vector<ImgInfo> render_document_pages(fz_context* ctx,
                                                  fz_document* doc,
                                                  const std::string& out_dir) {
    std::vector<ImgInfo> pages;
    const int npages = fz_count_pages(ctx, doc);
    const fz_matrix transform = fz_scale(DOCUMENT_RENDER_SCALE, DOCUMENT_RENDER_SCALE);
    // The review workspace shows the application form only. Label evidence is
    // presented separately from the embedded images extracted on later pages.
    for (int p = 0; p < std::min(npages, 1); p++) {
        fz_page* page = fz_load_page(ctx, doc, p);
        if (!page) continue;
        fz_pixmap* pm = fz_new_pixmap_from_page(
            ctx, page, transform, fz_device_rgb(ctx), 0);
        if (!pm) {
            fz_drop_page(ctx, page);
            continue;
        }

        const int w = fz_pixmap_width(ctx, pm);
        const int h = fz_pixmap_height(ctx, pm);
        const int n = fz_pixmap_components(ctx, pm);
        const int stride = fz_pixmap_stride(ctx, pm);
        unsigned char* samples = fz_pixmap_samples(ctx, pm);
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const size_t si = (size_t)y * stride + (size_t)x * n;
                const size_t di = ((size_t)y * w + x) * 3;
                if (n >= 3) {
                    rgb[di] = samples[si];
                    rgb[di + 1] = samples[si + 1];
                    rgb[di + 2] = samples[si + 2];
                } else {
                    rgb[di] = rgb[di + 1] = rgb[di + 2] = samples[si];
                }
            }
        }

        ImgInfo rendered;
        rendered.page = p + 1;
        rendered.name = "document_p" + std::to_string(p + 1) + ".png";
        rendered.path = (fs::path(out_dir) / rendered.name).string();
        rendered.width = w;
        rendered.height = h;
        if (stbi_write_png(rendered.path.c_str(), w, h, 3, rgb.data(), w * 3))
            pages.push_back(std::move(rendered));

        fz_drop_pixmap(ctx, pm);
        fz_drop_page(ctx, page);
    }
    return pages;
}

// ---------------------------------------------------------------------------
// result model + report
// ---------------------------------------------------------------------------

struct AnalysisResult {
    std::string task, pdf, ttb_id, ocr_backend;
    std::vector<std::string> raw_text;
    std::vector<QAEntry> form_qa;
    std::vector<ImgInfo> document_pages;
    std::vector<ImgInfo> images;
    std::vector<MatchResult> matches;
    ClassTypeResult class_type;
};

static const char* BAR = "==============================================================================";

static void print_report(const AnalysisResult& r) {
    std::cout << BAR << "\n";
    std::cout << " COLA PDF ANALYSIS — " << r.pdf << "\n";
    std::cout << BAR << "\n";
    std::cout << " TTB ID: " << (r.ttb_id.empty() ? "(none found)" : r.ttb_id) << "\n";
    std::cout << " Pages : [";
    for (size_t i = 0; i < r.raw_text.size(); i++) {
        if (i) std::cout << ", ";
        std::cout << (i + 1);
    }
    std::cout << "] (chars: ";
    for (size_t i = 0; i < r.raw_text.size(); i++) {
        if (i) std::cout << ", ";
        std::cout << r.raw_text[i].size();
    }
    std::cout << ")\n";

    std::cout << "\n[1] FORM Q&A — parsed from page 1 ("
              << r.form_qa.size() << " fields)\n";
    for (const QAEntry& e : r.form_qa) {
        std::cout << "    " << e.num << ". " << e.question << "\n";
        std::cout << "          -> " << (e.answer.empty() ? "(blank)" : e.answer) << "\n";
    }

    if (r.matches.empty() && r.task == "form") {
        std::cout << "\n (label task not run — use --task label or --task both)\n";
        std::cout << BAR << "\n";
        return;
    }

    std::cout << "\n[2] EMBEDDED GRAPHICS — " << r.images.size() << " image(s)\n";
    for (const ImgInfo& im : r.images) {
        std::cout << "    p" << im.page << " " << im.name << ": " << im.path
                  << " (" << im.width << "x" << im.height << ", png)\n";
    }

    std::cout << "\n[3] OCR — backend: " << r.ocr_backend << "\n";
    for (const ImgInfo& im : r.images) {
        std::cout << "    --- " << im.name << " ---\n";
        if (!im.ocr_error.empty()) {
            std::cout << "    OCR error: " << im.ocr_error << "\n";
            continue;
        }
        for (const Line& l : im.lines)
            std::cout << "      " << l.text << "   (conf " << (int)std::lround(l.conf) << ")\n";
    }

    std::cout << "\n[4] MATCH — label text vs form answers ("
              << r.matches.size() << " fields)\n";
    for (const MatchResult& m : r.matches) {
        std::string icon = "[?]   ";
        if (m.status == "MATCH") icon = "[OK]  ";
        else if (m.status == "PARTIAL") icon = "[~]   ";
        else if (m.status == "NOT FOUND") icon = "[MISS]";
        else if (m.status == "SKIP") icon = "[n/a] ";
        std::cout << "  " << icon << " " << m.num << ". " << m.question << "\n";
        std::cout << "        answer : " << m.answer << "\n";
        if (m.ev.found && !m.ev.line.empty()) {
            std::cout << "        label  : \"" << m.ev.line << "\" on " << m.ev.image;
            if (m.ev.conf >= 0) std::cout << ", conf " << m.ev.conf;
            if (m.ev.has_counts)
                std::cout << " (" << m.ev.found_n << "/" << m.ev.of_n << " tokens)";
            if (!m.ev.matched_as.empty())
                std::cout << " (matched as '" << m.ev.matched_as << "')";
            std::cout << "\n";
        } else if (m.status == "SKIP") {
            std::cout << "        note   : " << m.note << "\n";
        } else {
            std::cout << "        label  : (not found)\n";
        }
    }

    const ClassTypeResult& ct = r.class_type;
    if (ct.status != "SKIP") {
        std::string icon = ct.status == "MATCH" ? "[OK]  " : "[MISS]";
        std::cout << "  " << icon << " page-2 class/type: " << ct.class_type << "\n";
        if (ct.ev.found && !ct.ev.line.empty()) {
            std::cout << "        label  : \"" << ct.ev.line << "\" on " << ct.ev.image;
            if (!ct.matched_as.empty())
                std::cout << " (matched as '" << ct.matched_as << "')";
            std::cout << "\n";
        } else if (ct.status == "NOT FOUND") {
            std::cout << "        label  : (not found)\n";
        }
    }
    std::cout << BAR << "\n";
}

// ---------------------------------------------------------------------------
// JSON output (schema-compatible with the python version)
// ---------------------------------------------------------------------------

static json::Value qa_to_json(const std::vector<QAEntry>& qa) {
    json::Value v = json::Value::array();
    for (const QAEntry& e : qa) {
        json::Value o = json::Value::object();
        o.set("num", json::Value::str(e.num));
        o.set("question", json::Value::str(e.question));
        o.set("answer", json::Value::str(e.answer));
        if (e.has_bbox) {
            json::Value bbox = json::Value::object();
            bbox.set("x1", json::Value::num_(e.bbox_x1));
            bbox.set("y1", json::Value::num_(e.bbox_y1));
            bbox.set("x2", json::Value::num_(e.bbox_x2));
            bbox.set("y2", json::Value::num_(e.bbox_y2));
            o.set("bbox", std::move(bbox));
        }
        v.push(std::move(o));
    }
    return v;
}

static json::Value ocr_to_json(const ImgInfo& im) {
    json::Value o = json::Value::object();
    o.set("width", json::Value::num_(im.width));
    o.set("height", json::Value::num_(im.height));
    json::Value lines = json::Value::array();
    for (const Line& l : im.lines) {
        json::Value lo = json::Value::object();
        lo.set("text", json::Value::str(l.text));
        lo.set("conf", json::Value::num_(l.conf));
        lines.push(std::move(lo));
    }
    o.set("lines", std::move(lines));
    o.set("text", json::Value::str(im.text));
    return o;
}

static json::Value result_to_json(const AnalysisResult& r) {
    json::Value o = json::Value::object();
    o.set("pdf", json::Value::str(r.pdf));
    o.set("task", json::Value::str(r.task));
    o.set("ttb_id", json::Value::str(r.ttb_id));
    json::Value pages = json::Value::array();
    for (size_t i = 0; i < r.raw_text.size(); i++) {
        json::Value p = json::Value::object();
        p.set("page", json::Value::num_(i + 1));
        p.set("chars", json::Value::num_(r.raw_text[i].size()));
        pages.push(std::move(p));
    }
    o.set("pages", std::move(pages));
    json::Value rt = json::Value::array();
    for (const std::string& t : r.raw_text) rt.push(json::Value::str(t));
    o.set("raw_text", std::move(rt));
    o.set("form_qa", qa_to_json(r.form_qa));

    json::Value document_pages = json::Value::array();
    for (const ImgInfo& page : r.document_pages) {
        json::Value po = json::Value::object();
        po.set("page", json::Value::num_(page.page));
        po.set("name", json::Value::str(page.name));
        po.set("width", json::Value::num_(page.width));
        po.set("height", json::Value::num_(page.height));
        po.set("path", json::Value::str(page.path));
        document_pages.push(std::move(po));
    }
    o.set("document_pages", std::move(document_pages));

    json::Value imgs = json::Value::array();
    for (const ImgInfo& im : r.images) {
        json::Value io = json::Value::object();
        io.set("page", json::Value::num_(im.page));
        io.set("name", json::Value::str(im.name));
        io.set("width", json::Value::num_(im.width));
        io.set("height", json::Value::num_(im.height));
        io.set("path", json::Value::str(im.path));
        io.set("bbox", json::Value::str(im.bbox));
        imgs.push(std::move(io));
    }
    o.set("images", std::move(imgs));

    json::Value ocr = json::Value::object();
    for (const ImgInfo& im : r.images) ocr.set(im.name, ocr_to_json(im));
    o.set("ocr", std::move(ocr));

    json::Value matches = json::Value::array();
    for (const MatchResult& m : r.matches) {
        json::Value mo = json::Value::object();
        mo.set("num", json::Value::str(m.num));
        mo.set("question", json::Value::str(m.question));
        mo.set("answer", json::Value::str(m.answer));
        mo.set("status", json::Value::str(m.status));
        mo.set("mandatory", json::Value::boolean(m.mandatory));
        mo.set("mandatory_message", json::Value::str(m.mandatory_message));
        json::Value ev = json::Value::object();
        ev.set("found", json::Value::boolean(m.ev.found));
        ev.set("image", json::Value::str(m.ev.image));
        ev.set("line", json::Value::str(m.ev.line));
        ev.set("conf", json::Value::num_(m.ev.conf));
        ev.set("found_n", json::Value::num_(m.ev.found_n));
        ev.set("of_n", json::Value::num_(m.ev.of_n));
        ev.set("line_x1", json::Value::num_(m.ev.line_x1));
        ev.set("line_y1", json::Value::num_(m.ev.line_y1));
        ev.set("line_x2", json::Value::num_(m.ev.line_x2));
        ev.set("line_y2", json::Value::num_(m.ev.line_y2));
        mo.set("evidence", std::move(ev));
        mo.set("note", json::Value::str(m.note));
        matches.push(std::move(mo));
    }
    o.set("matches", std::move(matches));

    json::Value ct = json::Value::object();
    ct.set("status", json::Value::str(r.class_type.status));
    ct.set("class_type", json::Value::str(r.class_type.class_type));
    ct.set("matched_as", json::Value::str(r.class_type.matched_as));
    o.set("class_type", std::move(ct));
    return o;
}

// ---------------------------------------------------------------------------
// stages
// ---------------------------------------------------------------------------

static std::string find_ttb_id(const std::string& page1) {
    std::smatch m;
    if (std::regex_search(page1, m, TTBID_PAT)) return m[1].str();
    if (std::regex_search(page1, m, std::regex(R"(\b(\d{14})\b)")))
        return m[1].str();
    return "";
}

struct Stage1Result {
    std::vector<std::string> raw_text;
    std::vector<QAEntry> form_qa;
    std::vector<ImgInfo> document_pages;
    std::vector<ImgInfo> images;
    std::string ttb_id;
};

static Stage1Result stage1(const std::string& pdf_path,
                           const std::string& out_dir) {
    Stage1Result s1;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) { std::cerr << "ERROR: cannot create MuPDF context\n"; exit(1); }
    fz_register_document_handlers(ctx);
    fz_document* doc = fz_open_document(ctx, pdf_path.c_str());
    if (!doc) {
        std::cerr << "ERROR: cannot open " << pdf_path << "\n";
        fz_drop_context(ctx);
        exit(1);
    }
    int npages = fz_count_pages(ctx, doc);
    for (int i = 0; i < npages; i++) {
        fz_page* page = fz_load_page(ctx, doc, i);
        if (page) {
            s1.raw_text.push_back(extract_page_text(ctx, page));
            fz_drop_page(ctx, page);
        } else {
            s1.raw_text.emplace_back();
        }
    }
    const std::string product_type = selected_product_type(ctx, doc);
    s1.document_pages = render_document_pages(ctx, doc, out_dir);
    s1.images = extract_images(ctx, doc, out_dir);

    if (!s1.raw_text.empty()) {
        s1.form_qa = parse_form_qa(s1.raw_text[0]);
        if (!product_type.empty()) {
            for (QAEntry& entry : s1.form_qa) {
                if (to_lower_ascii(entry.question).find("type of product") != std::string::npos) {
                    entry.answer = product_type;
                    break;
                }
            }
        }
        attach_form_field_boxes(ctx, doc, s1.form_qa);
        if (apply_pdf_widget_values(ctx, doc, s1.form_qa)) {
            s1.form_qa.erase(
                std::remove_if(s1.form_qa.begin(), s1.form_qa.end(), [](const QAEntry& entry) {
                    return field_kind(entry.question) == FieldKind::Admin;
                }),
                s1.form_qa.end());
        }
        s1.ttb_id = find_ttb_id(s1.raw_text[0]);
    }
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return s1;
}

static void stage2(Stage1Result& s1, AnalysisResult& out,
                   const std::string& tessdata_flag) {
    if (out.images.empty()) {
        out.ocr_backend = "no images";
        return;
    }
    std::string tessdir = find_tessdata(tessdata_flag);
    // prefer eng+ita when ita.traineddata is present, else eng
    std::string lang = "eng";
    if (!tessdir.empty() && fs::exists(fs::path(tessdir) / "ita.traineddata"))
        lang = "eng+ita";

    // Pool of independent Tesseract engines (one per concurrent OCR pass).
    auto init_api = [&](tesseract::TessBaseAPI& a, const std::string& l) {
        return a.Init(tessdir.empty() ? nullptr : tessdir.c_str(), l.c_str(),
                      tesseract::OEM_LSTM_ONLY);
    };
    auto configure_api = [&](tesseract::TessBaseAPI& a) {
        a.SetVariable("preserve_interword_spaces", "1");
        a.SetSourceResolution(300);
#ifdef _WIN32
        a.SetVariable("debug_file", "NUL");
#else
        a.SetVariable("debug_file", "/dev/null");
#endif
    };
    const int kEngines = 4;  // widest wave = 4 (scale, psm) passes
    std::vector<std::unique_ptr<tesseract::TessBaseAPI>> engines;
    engines.reserve(kEngines);
    std::vector<tesseract::TessBaseAPI*> api_ptrs;
    auto first = std::make_unique<tesseract::TessBaseAPI>();
    if (init_api(*first, lang) != 0) {
        if (init_api(*first, "eng") != 0) {
            std::cerr << "ERROR: tesseract Init failed (tessdata: '"
                      << tessdir << "', lang '" << lang
                      << "'). Set TESSDATA_PREFIX or pass --tessdata.\n";
            exit(1);
        }
        lang = "eng";
    }
    configure_api(*first);
    engines.push_back(std::move(first));
    api_ptrs.push_back(engines.back().get());
    for (int i = 1; i < kEngines; i++) {
        auto a = std::make_unique<tesseract::TessBaseAPI>();
        if (init_api(*a, lang) != 0) {
            std::cerr << "ERROR: tesseract Init failed (tessdata: '"
                      << tessdir << "', lang '" << lang << "')\n";
            exit(1);
        }
        configure_api(*a);
        engines.push_back(std::move(a));
        api_ptrs.push_back(engines.back().get());
    }

    for (ImgInfo& im : out.images) ocr_image(im, api_ptrs);
    out.ocr_backend = "tesseract C++ API (flatten+sauvola, psm 3+11, 2x/4x merged"
                      + std::string(lang == "eng" ? "" : ", " + lang) + ")";

    std::vector<TaggedLine> tl = tagged_corpus(out.images);
    for (const QAEntry& e : s1.form_qa) {
        if (trim(e.answer).empty()) continue;
        FieldKind k = field_kind(e.question);
        MatchResult m = match_answer(k, e.question, e.answer, tl);
        m.num = e.num;
        m.question = e.question;
        m.answer = e.answer;
        out.matches.push_back(std::move(m));
    }
    out.matches.push_back(match_government_warning(tl));
    out.class_type = match_class_type(
        tl, s1.raw_text.size() > 1 ? s1.raw_text[1] : "");
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void usage(const char* prog) {
    std::cout <<
        "cola_label_qa — TTB COLA label-analysis tool (C++ port)\n"
        "\n"
        "usage: " << prog << " <pdf> [--out DIR] [--task both|form|label]\n"
        "                        [--qa-file FILE] [--tessdata DIR] [--json]\n"
        "\n"
        "  --task both   one run: extract text+images, OCR, match (default)\n"
        "  --task form   page-1 raw text -> generic Q&A + extract images\n"
        "                (needs only MuPDF; no OCR)\n"
        "  --task label  OCR images in --out, match vs --qa-file Q&A\n"
        "                (needs only tesseract; no MuPDF)\n"
        "  --out DIR     output dir for extracted images\n"
        "                (default: <pdf>_analysis next to the pdf)\n"
        "  --qa-file     Q&A JSON from a prior --task form --json run\n"
        "  --tessdata    directory containing eng/ita.traineddata\n"
        "                (default: $TESSDATA_PREFIX or well-known paths)\n"
        "  --json        also write cola_analysis.json next to the pdf\n";
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string pdf, out_dir, qa_file, tessdata;
    std::string task = "both";
    bool want_json = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: " << name << " needs a value\n";
                exit(1);
            }
            return argv[++i];
        };
        if (a == "--task") task = next("--task");
        else if (a == "--out") out_dir = next("--out");
        else if (a == "--qa-file") qa_file = next("--qa-file");
        else if (a == "--tessdata") tessdata = next("--tessdata");
        else if (a == "--json") want_json = true;
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "ERROR: unknown option " << a << "\n";
            usage(argv[0]);
            return 1;
        }
        else if (pdf.empty()) pdf = a;
        else {
            std::cerr << "ERROR: unexpected argument " << a << "\n";
            return 1;
        }
    }
    if (pdf.empty()) { usage(argv[0]); return 1; }
    if (task != "both" && task != "form" && task != "label") {
        std::cerr << "ERROR: --task must be both|form|label\n";
        return 1;
    }

    fs::path pdfp(pdf);
    if (!fs::exists(pdfp)) {
        std::cerr << "ERROR: no such file: " << pdf << "\n";
        return 1;
    }
    if (out_dir.empty())
        out_dir = (pdfp.parent_path() /
                   (pdfp.stem().string() + "_analysis")).string();
    fs::create_directories(out_dir);

    AnalysisResult result;
    result.task = task;
    result.pdf = fs::absolute(pdfp).string();

    Stage1Result s1;
    if (task == "both" || task == "form") {
        s1 = stage1(pdf, out_dir);
        result.raw_text = s1.raw_text;
        result.form_qa = s1.form_qa;
        result.ttb_id = s1.ttb_id;
        result.document_pages = s1.document_pages;
        result.images = s1.images;
        result.class_type.status = "SKIP";
    }

    result.class_type.status = "SKIP";

    if (task == "label") {
        // labels-only: reuse previously extracted images + Q&A JSON
        json::Value data;
        if (qa_file.empty()) {
            qa_file = (pdfp.parent_path() / "cola_analysis.json").string();
        }
        {
            std::ifstream f(qa_file);
            if (!f) {
                std::cerr << "ERROR: cannot read --qa-file " << qa_file
                          << " (run --task form --json first)\n";
                return 1;
            }
            std::stringstream ss;
            ss << f.rdbuf();
            try {
                data = json::parse(ss.str());
            } catch (const std::exception& e) {
                std::cerr << "ERROR: bad qa file: " << e.what() << "\n";
                return 1;
            }
        }
        const json::Value* fqa = data.is_obj() ? data.find("form_qa") : &data;
        if (!fqa || !fqa->is_arr()) {
            std::cerr << "ERROR: qa file has no form_qa array\n";
            return 1;
        }
        for (size_t i = 0; i < fqa->arr.size(); i++) {
            const json::Value* e = fqa->at(i);
            if (!e || !e->is_obj()) continue;
            QAEntry q;
            const json::Value* num = e->find("num");
            const json::Value* qq = e->find("question");
            const json::Value* aa = e->find("answer");
            if (num && num->is_str()) q.num = num->as_str();
            if (qq && qq->is_str()) q.question = qq->as_str();
            if (aa && aa->is_str()) q.answer = aa->as_str();
            s1.form_qa.push_back(std::move(q));
        }
        if (data.is_obj()) {
            const json::Value* rt = data.find("raw_text");
            if (rt && rt->is_arr())
                for (size_t i = 0; i < rt->arr.size(); i++)
                    if (rt->at(i) && rt->at(i)->is_str())
                        s1.raw_text.push_back(rt->at(i)->as_str());
            const json::Value* id = data.find("ttb_id");
            if (id && id->is_str()) s1.ttb_id = id->as_str();
        }
        result.raw_text = s1.raw_text;
        result.form_qa = s1.form_qa;
        result.ttb_id = s1.ttb_id;
        // scan out_dir for previously extracted images
        for (const auto& de : fs::directory_iterator(out_dir)) {
            std::string fn = de.path().filename().string();
            std::smatch m;
            if (std::regex_match(fn, m, std::regex(R"(p(\d+)_(\d+)\.png)"))) {
                ImgInfo im;
                im.page = std::stoi(m[1].str());
                im.name = fn;
                im.path = de.path().string();
                result.images.push_back(std::move(im));
            }
        }
        std::sort(result.images.begin(), result.images.end(),
                  [](const ImgInfo& a, const ImgInfo& b) {
                      return a.name < b.name;
                  });
    }

    if (task == "both" || task == "label") {
        stage2(s1, result, tessdata);
    }

    print_report(result);

    if (want_json) {
        std::string jpath = (pdfp.parent_path() / "cola_analysis.json").string();
        std::ofstream f(jpath);
        f << json::dump(result_to_json(result), 0) << "\n";
        std::cout << "\nFull JSON written to " << jpath << "\n";
    }
    return 0;
}
