#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esb {
namespace json {

namespace {

const Value kNull;

class Parser
{
public:
    Parser(const std::string& text) : m_text(text) {}

    bool ParseValue(Value& out)
    {
        SkipSpace();
        if (m_pos >= m_text.size())
            return Fail("unexpected end of input");

        switch (m_text[m_pos])
        {
        case '{': return ParseObject(out);
        case '[': return ParseArray(out);
        case '"': { std::string s; if (!ParseString(s)) return false; out = Value(s); return true; }
        case 't': return Literal("true",  Value(true),  out);
        case 'f': return Literal("false", Value(false), out);
        case 'n': return Literal("null",  Value(),      out);
        default:  return ParseNumber(out);
        }
    }

    bool AtEnd()
    {
        SkipSpace();
        return m_pos >= m_text.size();
    }

    const std::string& Error() const { return m_error; }

private:
    bool Fail(const char* why)
    {
        if (m_error.empty())
        {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s at offset %u", why,
                          static_cast<unsigned>(m_pos));
            m_error = buf;
        }
        return false;
    }

    void SkipSpace()
    {
        while (m_pos < m_text.size())
        {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                ++m_pos;
                continue;
            }
            // Not JSON, but a config file people edit by hand needs comments.
            if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/')
            {
                while (m_pos < m_text.size() && m_text[m_pos] != '\n')
                    ++m_pos;
                continue;
            }
            break;
        }
    }

    bool Literal(const char* word, const Value& v, Value& out)
    {
        const size_t n = std::strlen(word);
        if (m_text.compare(m_pos, n, word) != 0)
            return Fail("bad literal");
        m_pos += n;
        out = v;
        return true;
    }

    bool ParseString(std::string& out)
    {
        if (m_text[m_pos] != '"')
            return Fail("expected a string");
        ++m_pos;

        out.clear();
        while (m_pos < m_text.size())
        {
            const char c = m_text[m_pos++];
            if (c == '"')
                return true;

            if (c != '\\')
            {
                out += c;
                continue;
            }

            if (m_pos >= m_text.size())
                return Fail("unterminated escape");

            const char e = m_text[m_pos++];
            switch (e)
            {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u':
            {
                if (m_pos + 4 > m_text.size())
                    return Fail("truncated \\u escape");
                const std::string hex = m_text.substr(m_pos, 4);
                m_pos += 4;
                const unsigned cp = std::strtoul(hex.c_str(), nullptr, 16);
                // UTF-8 encode. Surrogate pairs are not reconstructed: the
                // bridge only ever writes ASCII keys and UTF-8 payloads.
                if (cp < 0x80)
                {
                    out += static_cast<char>(cp);
                }
                else if (cp < 0x800)
                {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default:
                return Fail("unknown escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseNumber(Value& out)
    {
        const size_t start = m_pos;
        if (m_pos < m_text.size() && (m_text[m_pos] == '-' || m_text[m_pos] == '+'))
            ++m_pos;
        while (m_pos < m_text.size())
        {
            const char c = m_text[m_pos];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '+' || c == '-')
                ++m_pos;
            else
                break;
        }
        if (m_pos == start)
            return Fail("expected a value");

        out = Value(std::strtod(m_text.substr(start, m_pos - start).c_str(), nullptr));
        return true;
    }

    bool ParseArray(Value& out)
    {
        ++m_pos;                       // '['
        out = Value::Array();

        SkipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']')
        {
            ++m_pos;
            return true;
        }

        for (;;)
        {
            Value item;
            if (!ParseValue(item))
                return false;
            out.Push(item);

            SkipSpace();
            if (m_pos >= m_text.size())
                return Fail("unterminated array");
            if (m_text[m_pos] == ',') { ++m_pos; continue; }
            if (m_text[m_pos] == ']') { ++m_pos; return true; }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseObject(Value& out)
    {
        ++m_pos;                       // '{'
        out = Value::Object();

        SkipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}')
        {
            ++m_pos;
            return true;
        }

        for (;;)
        {
            SkipSpace();
            std::string key;
            if (m_pos >= m_text.size() || !ParseString(key))
                return Fail("expected a key");

            SkipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                return Fail("expected ':'");
            ++m_pos;

            Value item;
            if (!ParseValue(item))
                return false;
            out.Set(key, item);

            SkipSpace();
            if (m_pos >= m_text.size())
                return Fail("unterminated object");
            if (m_text[m_pos] == ',') { ++m_pos; continue; }
            if (m_text[m_pos] == '}') { ++m_pos; return true; }
            return Fail("expected ',' or '}'");
        }
    }

    const std::string& m_text;
    size_t             m_pos = 0;
    std::string        m_error;
};

void WriteString(const std::string& s, std::string& out)
{
    out += '"';
    for (char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char esc[8];
                std::snprintf(esc, sizeof esc, "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += esc;
            }
            else
            {
                out += c;
            }
            break;
        }
    }
    out += '"';
}

void WriteValue(const Value& v, std::string& out, bool pretty, int indent)
{
    const std::string pad(pretty ? static_cast<size_t>(indent) * 2 : 0, ' ');
    const std::string padIn(pretty ? static_cast<size_t>(indent + 1) * 2 : 0, ' ');
    const char* nl = pretty ? "\n" : "";

    switch (v.type())
    {
    case Value::Type::Null:
        out += "null";
        break;
    case Value::Type::Bool:
        out += v.AsBool() ? "true" : "false";
        break;
    case Value::Type::Number:
    {
        const double n = v.AsNumber();
        char buf[40];
        // Integers round-trip as integers; a TOBT of 742 should not be saved
        // as 742.0 and read back as a double.
        if (n == std::floor(n) && std::fabs(n) < 9.0e15)
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
        else
            std::snprintf(buf, sizeof buf, "%.17g", n);
        out += buf;
        break;
    }
    case Value::Type::String:
        WriteString(v.AsString(), out);
        break;
    case Value::Type::Array:
    {
        if (v.Items().empty()) { out += "[]"; break; }
        out += "[";
        out += nl;
        bool first = true;
        for (const Value& item : v.Items())
        {
            if (!first) { out += ","; out += nl; }
            first = false;
            out += padIn;
            WriteValue(item, out, pretty, indent + 1);
        }
        out += nl;
        out += pad;
        out += "]";
        break;
    }
    case Value::Type::Object:
    {
        if (v.Members().empty()) { out += "{}"; break; }
        out += "{";
        out += nl;
        bool first = true;
        for (const auto& kv : v.Members())
        {
            if (!first) { out += ","; out += nl; }
            first = false;
            out += padIn;
            WriteString(kv.first, out);
            out += pretty ? ": " : ":";
            WriteValue(kv.second, out, pretty, indent + 1);
        }
        out += nl;
        out += pad;
        out += "}";
        break;
    }
    }
}

} // namespace

bool Value::AsBool(bool fallback) const
{
    return m_type == Type::Bool ? m_bool : fallback;
}

double Value::AsNumber(double fallback) const
{
    return m_type == Type::Number ? m_number : fallback;
}

int64_t Value::AsInt(int64_t fallback) const
{
    return m_type == Type::Number ? static_cast<int64_t>(m_number) : fallback;
}

const std::string& Value::AsString() const
{
    static const std::string empty;
    return m_type == Type::String ? m_string : empty;
}

const Value& Value::operator[](const std::string& key) const
{
    auto it = m_object.find(key);
    return it == m_object.end() ? kNull : it->second;
}

bool Value::Has(const std::string& key) const
{
    return m_object.find(key) != m_object.end();
}

void Value::Set(const std::string& key, const Value& v)
{
    m_type = Type::Object;
    m_object[key] = v;
}

Value Parse(const std::string& text, std::string* error)
{
    Parser parser(text);
    Value out;

    if (!parser.ParseValue(out))
    {
        if (error)
            *error = parser.Error();
        return Value();
    }

    if (!parser.AtEnd())
    {
        if (error)
            *error = "trailing content after the top-level value";
        return Value();
    }

    if (error)
        error->clear();
    return out;
}

std::string Write(const Value& v, bool pretty)
{
    std::string out;
    WriteValue(v, out, pretty, 0);
    if (pretty)
        out += "\n";
    return out;
}

} // namespace json
} // namespace esb
