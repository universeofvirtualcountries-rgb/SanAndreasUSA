#include "json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace usa1to1 {
namespace core {
namespace {

void EscapeTo(std::string &out, const std::string &s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void AppendUtf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

void DumpValue(std::string &out, const Json &v, bool pretty, int depth);

void DumpIndent(std::string &out, int depth) {
    out += '\n';
    out.append(static_cast<size_t>(depth) * 2, ' ');
}

void DumpValue(std::string &out, const Json &v, bool pretty, int depth) {
    switch (v.GetType()) {
    case Json::Type::Null:
        out += "null";
        break;
    case Json::Type::Bool:
        out += v.AsBool() ? "true" : "false";
        break;
    case Json::Type::Int: {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.AsInt()));
        out += buf;
        break;
    }
    case Json::Type::Double: {
        const double d = v.AsDouble();
        char buf[40];
        if (!std::isfinite(d)) {
            out += "null"; // JSON has no NaN/Inf
            break;
        }
        // %.9g keeps float precision exactly and stays readable for doubles.
        std::snprintf(buf, sizeof(buf), "%.9g", d);
        out += buf;
        break;
    }
    case Json::Type::String:
        EscapeTo(out, v.AsString());
        break;
    case Json::Type::Array: {
        const auto &arr = v.GetArray();
        if (arr.empty()) {
            out += "[]";
            break;
        }
        out += '[';
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i)
                out += ',';
            if (pretty)
                DumpIndent(out, depth + 1);
            DumpValue(out, arr[i], pretty, depth + 1);
        }
        if (pretty)
            DumpIndent(out, depth);
        out += ']';
        break;
    }
    case Json::Type::Object: {
        const auto &mem = v.GetMembers();
        if (mem.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        for (size_t i = 0; i < mem.size(); ++i) {
            if (i)
                out += ',';
            if (pretty)
                DumpIndent(out, depth + 1);
            EscapeTo(out, mem[i].first);
            out += pretty ? ": " : ":";
            DumpValue(out, mem[i].second, pretty, depth + 1);
        }
        if (pretty)
            DumpIndent(out, depth);
        out += '}';
        break;
    }
    }
}

class Parser {
  public:
    explicit Parser(const std::string &text) : m_s(text) {}

    Json ParseDocument() {
        SkipWs();
        Json v = ParseValue(0);
        SkipWs();
        if (m_i != m_s.size())
            Fail("trailing characters");
        return v;
    }

  private:
    [[noreturn]] void Fail(const char *what) {
        throw JsonError(std::string(what) + " at offset " + std::to_string(m_i));
    }

    void SkipWs() {
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++m_i;
            else
                break;
        }
    }

    char Peek() {
        if (m_i >= m_s.size())
            Fail("unexpected end of input");
        return m_s[m_i];
    }

    bool Consume(char c) {
        if (m_i < m_s.size() && m_s[m_i] == c) {
            ++m_i;
            return true;
        }
        return false;
    }

    void Expect(char c) {
        if (!Consume(c))
            Fail("unexpected character");
    }

    Json ParseValue(int depth) {
        if (depth > 64)
            Fail("nesting too deep");
        SkipWs();
        const char c = Peek();
        switch (c) {
        case '{':
            return ParseObject(depth);
        case '[':
            return ParseArray(depth);
        case '"':
            return Json(ParseString());
        case 't':
            Literal("true");
            return Json(true);
        case 'f':
            Literal("false");
            return Json(false);
        case 'n':
            Literal("null");
            return Json(nullptr);
        default:
            return ParseNumber();
        }
    }

    void Literal(const char *lit) {
        const size_t len = std::strlen(lit);
        if (m_s.compare(m_i, len, lit) != 0)
            Fail("invalid literal");
        m_i += len;
    }

    Json ParseObject(int depth) {
        Expect('{');
        Json obj = Json::Object();
        SkipWs();
        if (Consume('}'))
            return obj;
        while (true) {
            SkipWs();
            if (Peek() != '"')
                Fail("expected object key");
            std::string key = ParseString();
            SkipWs();
            Expect(':');
            obj.Set(key, ParseValue(depth + 1));
            SkipWs();
            if (Consume(','))
                continue;
            Expect('}');
            break;
        }
        return obj;
    }

    Json ParseArray(int depth) {
        Expect('[');
        Json arr = Json::MakeArray();
        SkipWs();
        if (Consume(']'))
            return arr;
        while (true) {
            arr.Push(ParseValue(depth + 1));
            SkipWs();
            if (Consume(','))
                continue;
            Expect(']');
            break;
        }
        return arr;
    }

    uint32_t ParseHex4() {
        if (m_i + 4 > m_s.size())
            Fail("bad \\u escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = m_s[m_i++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<uint32_t>(c - 'A' + 10);
            else
                Fail("bad hex digit");
        }
        return v;
    }

    std::string ParseString() {
        Expect('"');
        std::string out;
        while (true) {
            if (m_i >= m_s.size())
                Fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(m_s[m_i++]);
            if (c == '"')
                break;
            if (c != '\\') {
                if (c < 0x20)
                    Fail("control character in string");
                out += static_cast<char>(c);
                continue;
            }
            if (m_i >= m_s.size())
                Fail("unterminated escape");
            const char e = m_s[m_i++];
            switch (e) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                uint32_t cp = ParseHex4();
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (m_i + 1 < m_s.size() && m_s[m_i] == '\\' && m_s[m_i + 1] == 'u') {
                        m_i += 2;
                        const uint32_t lo = ParseHex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            Fail("bad low surrogate");
                    } else {
                        Fail("lone high surrogate");
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    Fail("lone low surrogate");
                }
                AppendUtf8(out, cp);
                break;
            }
            default:
                Fail("bad escape");
            }
        }
        return out;
    }

    Json ParseNumber() {
        const size_t start = m_i;
        if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+'))
            ++m_i;
        bool isFloat = false;
        bool anyDigit = false;
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (c >= '0' && c <= '9') {
                anyDigit = true;
                ++m_i;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                if (c == '.')
                    isFloat = true;
                ++m_i;
            } else {
                break;
            }
        }
        if (!anyDigit)
            Fail("invalid number");
        const std::string tok = m_s.substr(start, m_i - start);
        if (!isFloat) {
            errno = 0;
            char *end = nullptr;
            const long long v = std::strtoll(tok.c_str(), &end, 10);
            if (errno == 0 && end && *end == '\0')
                return Json(static_cast<int64_t>(v));
        }
        char *end = nullptr;
        const double d = std::strtod(tok.c_str(), &end);
        if (!end || *end != '\0')
            Fail("invalid number");
        return Json(d);
    }

    const std::string &m_s;
    size_t m_i = 0;
};

} // namespace

std::string Json::Dump(bool pretty) const {
    std::string out;
    out.reserve(256);
    DumpValue(out, *this, pretty, 0);
    return out;
}

Json Json::Parse(const std::string &text) {
    Parser p(text);
    return p.ParseDocument();
}

bool Json::TryParse(const std::string &text, Json &out, std::string *error) {
    try {
        out = Parse(text);
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

} // namespace core
} // namespace usa1to1
