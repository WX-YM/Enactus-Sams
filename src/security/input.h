#pragma once

// Request-input helpers shared by the API handlers: regex escaping for the
// case-insensitive Mongo lookups, field validation, client IP resolution, and
// structural checks on client JSON before it is converted to BSON.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <json/json.h>
#include <drogon/HttpRequest.h>

namespace enactus::security {

// Escape every PCRE metacharacter so user input can be embedded in a
// `$regex` as a literal. Without this, `.*` as an email or team name matches
// every document.
inline std::string escapeRegex(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        switch (c) {
            case '\\': case '^': case '$': case '.': case '|': case '?':
            case '*': case '+': case '(': case ')': case '[': case ']':
            case '{': case '}': case '/': case '-': case '#': case ' ':
                out.push_back('\\');
                [[fallthrough]];
            default:
                out.push_back(c);
        }
    }
    return out;
}

// Anchored, case-insensitive exact-match pattern for `s`, tolerant of stray
// surrounding whitespace in stored values.
inline std::string exactMatchPattern(std::string_view s) {
    return "^\\s*" + escapeRegex(s) + "\\s*$";
}

inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline bool hasControlChars(std::string_view s, bool allowNewlines = false) {
    for (unsigned char c : s) {
        if (c == 0x7F) return true;
        if (c < 0x20) {
            if (allowNewlines && (c == '\n' || c == '\r' || c == '\t')) continue;
            return true;
        }
    }
    return false;
}

// Single-line text field: bounded length, no control characters.
inline bool isValidText(std::string_view s, std::size_t maxLen, bool allowEmpty = false) {
    if (s.empty()) return allowEmpty;
    return s.size() <= maxLen && !hasControlChars(s);
}

// Multi-line text field: bounded length, newlines and tabs allowed.
inline bool isValidMultiline(std::string_view s, std::size_t maxLen) {
    return s.size() <= maxLen && !hasControlChars(s, true);
}

// Pragmatic email check: one '@', non-empty local part, dotted domain, no
// whitespace or control characters, RFC 5321 length bound.
inline bool isValidEmail(std::string_view s) {
    if (s.size() < 3 || s.size() > 254) return false;
    const auto at = s.find('@');
    if (at == std::string_view::npos || at == 0 || s.find('@', at + 1) != std::string_view::npos) return false;
    const auto domain = s.substr(at + 1);
    if (domain.size() < 3 || domain.find('.') == std::string_view::npos ||
        domain.front() == '.' || domain.back() == '.') {
        return false;
    }
    for (unsigned char c : s) {
        if (c <= 0x20 || c == 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' ||
            c == '(' || c == ')' || c == ',' || c == ';' || c == ':' || c == '[' || c == ']') {
            return false;
        }
    }
    return true;
}

inline bool isHexObjectId(std::string_view s) {
    if (s.size() != 24) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// Field names that are safe to store and to use as `$set` keys: no operator
// prefix, no dotted paths, no NUL, bounded length.
inline bool isSafeFieldName(std::string_view key) {
    if (key.empty() || key.size() > 128) return false;
    if (key.front() == '$') return false;
    return key.find('.') == std::string_view::npos && key.find('\0') == std::string_view::npos;
}

// Walk client JSON and reject operator/dotted keys anywhere in the tree, plus
// pathological nesting. Applied before any client document reaches BSON.
inline bool isSafeJsonTree(const Json::Value& v, int depth = 0) {
    if (depth > 8) return false;
    if (v.isObject()) {
        for (const auto& name : v.getMemberNames()) {
            if (!isSafeFieldName(name)) return false;
            if (!isSafeJsonTree(v[name], depth + 1)) return false;
        }
    } else if (v.isArray()) {
        if (v.size() > 2000) return false;
        for (const auto& item : v) {
            if (!isSafeJsonTree(item, depth + 1)) return false;
        }
    }
    return true;
}

// Copy of `v` safe to INSERT as a stored document: keys that would be read as
// operators or paths get look-alike replacements (`.` -> U+FF0E, a leading
// `$` -> U+FF04) instead of being rejected, because form-submission keys are
// the admin-written field labels and may legitimately contain a period.
// Returns false when the tree is too deep or an array too large.
inline bool sanitizeKeysForStorage(const Json::Value& v, Json::Value& out, int depth = 0) {
    if (depth > 8) return false;
    if (v.isObject()) {
        out = Json::Value(Json::objectValue);
        for (const auto& name : v.getMemberNames()) {
            if (name.empty() || name.size() > 512 || name.find('\0') != std::string::npos) return false;
            std::string key;
            key.reserve(name.size());
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (name[i] == '.') key += "\xEF\xBC\x8E";
                else if (name[i] == '$' && i == 0) key += "\xEF\xBC\x84";
                else key.push_back(name[i]);
            }
            Json::Value child;
            if (!sanitizeKeysForStorage(v[name], child, depth + 1)) return false;
            out[key] = std::move(child);
        }
        return true;
    }
    if (v.isArray()) {
        if (v.size() > 2000) return false;
        out = Json::Value(Json::arrayValue);
        for (const auto& item : v) {
            Json::Value child;
            if (!sanitizeKeysForStorage(item, child, depth + 1)) return false;
            out.append(std::move(child));
        }
        return true;
    }
    out = v;
    return true;
}

inline std::string compactJson(const Json::Value& v) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, v);
}

// The address used to key rate limits. Behind a reverse proxy every request
// arrives from the proxy itself, which would turn each per-client limit into
// one global limit; set TRUST_PROXY=1 only when a proxy you control sets
// X-Real-IP / X-Forwarded-For and the backend is not reachable directly.
inline std::string clientIp(const drogon::HttpRequestPtr& req) {
    static const bool trustProxy = [] {
        const char* v = std::getenv("TRUST_PROXY");
        return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
    }();
    if (trustProxy) {
        const std::string& realIp = req->getHeader("x-real-ip");
        if (!realIp.empty() && realIp.size() <= 64) return realIp;
        const std::string& xff = req->getHeader("x-forwarded-for");
        if (!xff.empty()) {
            // The right-most entry is the one appended by our own proxy.
            auto comma = xff.find_last_of(',');
            std::string last = comma == std::string::npos ? xff : xff.substr(comma + 1);
            auto start = last.find_first_not_of(' ');
            if (start != std::string::npos) last = last.substr(start);
            if (!last.empty() && last.size() <= 64) return last;
        }
    }
    return req->getPeerAddr().toIp();
}

}  // namespace enactus::security
