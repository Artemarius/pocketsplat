#pragma once

// Private to splatcore: a minimal JSON reader for the camera files (cameras.json, transforms.json).

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace splatcore::detail {

// --- Minimal JSON reader: enough for cameras.json (objects, arrays, numbers, strings, literals). ---
struct JsonValue {
    enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject } kind = Kind::kNull;
    double number = 0.0;
    std::string text;
    std::vector<JsonValue> items;
    std::map<std::string, JsonValue> fields;

    const JsonValue* field(const std::string& key) const {
        const auto it = fields.find(key);
        return it == fields.end() ? nullptr : &it->second;
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : m_text(text) {}
    JsonReader(std::string&&) = delete;  // keeps a reference: the text must outlive the reader

    bool parse(JsonValue& out) {
        if (!value(out, 0)) return false;
        skip_space();
        return m_pos == m_text.size();
    }
    std::size_t position() const { return m_pos; }

private:
    static constexpr int kMaxDepth = 64;

    void skip_space() {
        while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos]))) ++m_pos;
    }
    bool literal(const char* word) {
        const std::size_t n = std::strlen(word);
        if (m_text.compare(m_pos, n, word) != 0) return false;
        m_pos += n;
        return true;
    }
    bool string(std::string& out) {
        if (m_pos >= m_text.size() || m_text[m_pos] != '"') return false;
        ++m_pos;
        while (m_pos < m_text.size() && m_text[m_pos] != '"') {
            char c = m_text[m_pos++];
            if (c == '\\') {
                if (m_pos >= m_text.size()) return false;
                const char e = m_text[m_pos++];
                if (e == 'u') {  // keep \uXXXX as '?': names are never needed verbatim here
                    if (m_pos + 4 > m_text.size()) return false;
                    m_pos += 4;
                    c = '?';
                } else {
                    c = e == 'n'   ? '\n'
                        : e == 't' ? '\t'
                        : e == 'r' ? '\r'
                        : e == 'b' ? '\b'
                        : e == 'f' ? '\f'
                                   : e;
                }
            }
            out.push_back(c);
        }
        if (m_pos >= m_text.size()) return false;
        ++m_pos;
        return true;
    }
    bool value(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        skip_space();
        if (m_pos >= m_text.size()) return false;
        const char c = m_text[m_pos];
        if (c == '{') {
            out.kind = JsonValue::Kind::kObject;
            ++m_pos;
            skip_space();
            if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            while (true) {
                skip_space();
                std::string key;
                if (!string(key)) return false;
                skip_space();
                if (m_pos >= m_text.size() || m_text[m_pos] != ':') return false;
                ++m_pos;
                if (!value(out.fields[key], depth + 1)) return false;
                skip_space();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                    ++m_pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            out.kind = JsonValue::Kind::kArray;
            ++m_pos;
            skip_space();
            if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            while (true) {
                out.items.emplace_back();
                if (!value(out.items.back(), depth + 1)) return false;
                skip_space();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                    ++m_pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out.kind = JsonValue::Kind::kString;
            return string(out.text);
        }
        if (literal("true") || literal("false")) {
            out.kind = JsonValue::Kind::kBool;
            out.number = m_text[m_pos - 1] == 'e' && m_text[m_pos - 2] == 'u' ? 1.0 : 0.0;
            return true;
        }
        if (literal("null")) {
            out.kind = JsonValue::Kind::kNull;
            return true;
        }
        // Number: strtod on a bounded copy (the text is not NUL-terminated at the number's end).
        std::size_t end = m_pos;
        while (end < m_text.size() && std::strchr("+-0123456789.eE", m_text[end]) != nullptr) ++end;
        if (end == m_pos) return false;
        const std::string token = m_text.substr(m_pos, end - m_pos);
        char* stop = nullptr;
        out.number = std::strtod(token.c_str(), &stop);
        if (stop != token.c_str() + token.size()) return false;
        out.kind = JsonValue::Kind::kNumber;
        m_pos = end;
        return true;
    }

    const std::string& m_text;
    std::size_t m_pos = 0;
};

inline bool read_number(const JsonValue& object, const char* key, double& out) {
    const JsonValue* v = object.field(key);
    if (v == nullptr || v->kind != JsonValue::Kind::kNumber) return false;
    out = v->number;
    return true;
}

}  // namespace splatcore::detail
