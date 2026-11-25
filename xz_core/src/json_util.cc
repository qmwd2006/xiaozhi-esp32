#include "xz/json_util.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace xz {
namespace {

const char* SkipWs(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

void Utf8Append(std::string* out, unsigned cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool ParseJsonString(const char** pp, std::string* out) {
    const char* p = *pp;
    if (*p != '"') return false;
    ++p;
    out->clear();
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            switch (*p) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    char buf[5] = {0};
                    for (int i = 0; i < 4 && p[1 + i]; ++i) buf[i] = p[1 + i];
                    Utf8Append(out, static_cast<unsigned>(strtoul(buf, nullptr, 16)));
                    p += 4;
                    break;
                }
                default: out->push_back(*p); break;
            }
            if (*p) ++p;
        } else {
            out->push_back(*p++);
        }
    }
    if (*p != '"') return false;
    *pp = p + 1;
    return true;
}

}  // namespace

std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string Quote(const std::string& in) { return "\"" + JsonEscape(in) + "\""; }

bool JsonRawValue(const std::string& json, const std::string& key, std::string* out,
                  bool* was_string) {
    std::string pat = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pat, pos)) != std::string::npos) {
        const char* p = json.c_str() + pos + pat.size();
        p = SkipWs(p);
        if (*p != ':') { pos += pat.size(); continue; }
        p = SkipWs(p + 1);
        if (*p == '"') {
            std::string s;
            if (!ParseJsonString(&p, &s)) return false;
            if (out) *out = s;
            if (was_string) *was_string = true;
            return true;
        }
        int depth = 0;
        const char* q = p;
        while (*q) {
            if (*q == '{' || *q == '[') ++depth;
            if (*q == '}' || *q == ']') {
                if (depth == 0) break;
                --depth;
            }
            if (*q == ',' && depth == 0) break;
            ++q;
        }
        if (out) {
            std::string raw(p, q);
            out->assign(raw);
            *out = out->substr(0, out->find_last_not_of(" \t\r\n") + 1);
        }
        if (was_string) *was_string = false;
        return true;
    }
    return false;
}

bool Args::Parse(const std::string& json) {
    kv_.clear();
    const char* p = SkipWs(json.c_str());
    if (*p != '{') return false;
    ++p;
    while (true) {
        p = SkipWs(p);
        if (*p == '}') return true;
        if (*p == ',') { ++p; continue; }
        std::string key;
        if (!ParseJsonString(&p, &key)) return false;
        p = SkipWs(p);
        if (*p != ':') return false;
        p = SkipWs(p + 1);
        Val v;
        if (*p == '"') {
            std::string s;
            if (!ParseJsonString(&p, &s)) return false;
            v.raw = s;
            v.is_str = true;
        } else {
            const char* q = p;
            int depth = 0;
            while (*q) {
                if (*q == '{' || *q == '[') ++depth;
                if (*q == '}' || *q == ']') { if (depth == 0) break; --depth; }
                if (*q == ',' && depth == 0) break;
                ++q;
            }
            v.raw.assign(p, q);
            p = q;
        }
        kv_.emplace_back(key, v);
    }
}

bool Args::Has(const std::string& k) const { return !Get(k).empty(); }

std::string Args::Get(const std::string& k) const {
    for (const auto& kv : kv_) {
        if (kv.first == k) return kv.second.raw;
    }
    return "";
}

std::string Args::Str(const std::string& k, const std::string& def) const {
    std::string v = Get(k);
    return v.empty() ? def : v;
}

long long Args::Int(const std::string& k, long long def) const {
    std::string v = Get(k);
    if (v.empty()) return def;
    return strtoll(v.c_str(), nullptr, 10);
}

bool Args::Bool(const std::string& k, bool def) const {
    std::string v = Get(k);
    if (v.empty()) return def;
    return v == "true" || v == "1";
}

}  // namespace xz