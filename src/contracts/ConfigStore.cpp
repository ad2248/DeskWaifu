#include "ConfigStore.hpp"
#include <cerrno>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
struct Member { std::string key; size_t valueStart, end; };

// 校验整个文档，拒绝重复键，包括转义后相同的键名。
class Document
{
public:
    explicit Document(const std::string& text, bool allowComments = true) : s(text), comments(allowComments) {}
    bool Parse()
    {
        Skip();
        if (!Object(true, 0)) return false;
        Skip();
        return p == s.size();
    }
    std::vector<Member> members;
    bool Strings(std::vector<std::string>& values, bool array)
    {
        values.clear();
        if (array && !Take('[')) return false;
        Skip();
        if (array && p < s.size() && s[p] == ']') ++p;
        else {
            do {
                std::string value;
                if (!String(value)) return false;
                values.push_back(std::move(value));
            } while (array && Take(','));
            if (array && !Take(']')) return false;
        }
        Skip();
        return p == s.size();
    }
private:
    const std::string& s;
    bool comments;
    size_t p = 0;
    void Skip()
    {
        for (;;) {
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
            if (!comments || p + 1 >= s.size() || s[p] != '/' || s[p + 1] != '/') return;
            while (p < s.size() && s[p] != '\n') ++p;
        }
    }
    bool Take(char c) { Skip(); if (p >= s.size() || s[p] != c) return false; ++p; return true; }
    bool Hex(unsigned& value)
    {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (p >= s.size()) return false;
            char c = s[p++];
            int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (n < 0) return false;
            value = value * 16 + n;
        }
        return true;
    }
    static void Utf8(std::string& out, unsigned n)
    {
        if (n < 0x80) out += char(n);
        else if (n < 0x800) { out += char(0xc0 | (n >> 6)); out += char(0x80 | (n & 63)); }
        else if (n < 0x10000) { out += char(0xe0 | (n >> 12)); out += char(0x80 | ((n >> 6) & 63)); out += char(0x80 | (n & 63)); }
        else { out += char(0xf0 | (n >> 18)); out += char(0x80 | ((n >> 12) & 63)); out += char(0x80 | ((n >> 6) & 63)); out += char(0x80 | (n & 63)); }
    }
    bool String(std::string& out)
    {
        if (!Take('"')) return false;
        while (p < s.size()) {
            unsigned char c = s[p++];
            if (c == '"') return true;
            if (c < 32) return false;
            if (c != '\\') {
                out += char(c);
                if (c >= 0x80) {
                    int extra; unsigned point, minimum;
                    if (c >= 0xc2 && c <= 0xdf) { extra = 1; point = c & 31; minimum = 0x80; }
                    else if (c >= 0xe0 && c <= 0xef) { extra = 2; point = c & 15; minimum = 0x800; }
                    else if (c >= 0xf0 && c <= 0xf4) { extra = 3; point = c & 7; minimum = 0x10000; }
                    else return false;
                    for (int i = 0; i < extra; ++i) {
                        if (p >= s.size()) return false;
                        unsigned char next = s[p++];
                        if ((next & 0xc0) != 0x80) return false;
                        out += char(next); point = (point << 6) | (next & 63);
                    }
                    if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
                }
                continue;
            }
            if (p >= s.size()) return false;
            c = s[p++];
            if (c == '"' || c == '\\' || c == '/') out += char(c);
            else if (c == 'b') out += '\b'; else if (c == 'f') out += '\f';
            else if (c == 'n') out += '\n'; else if (c == 'r') out += '\r'; else if (c == 't') out += '\t';
            else if (c == 'u') {
                unsigned n;
                if (!Hex(n)) return false;
                if (n >= 0xd800 && n <= 0xdbff) {
                    if (p + 2 > s.size() || s.substr(p, 2) != "\\u") return false;
                    p += 2; unsigned low;
                    if (!Hex(low) || low < 0xdc00 || low > 0xdfff) return false;
                    n = 0x10000 + ((n - 0xd800) << 10) + low - 0xdc00;
                } else if (n >= 0xdc00 && n <= 0xdfff) return false;
                Utf8(out, n);
            } else return false;
        }
        return false;
    }
    bool Object(bool root, int depth)
    {
        if (depth > 64 || !Take('{')) return false;
        Skip(); if (p < s.size() && s[p] == '}') { ++p; return true; }
        std::set<std::string> keys;
        for (;;) {
            Skip(); Member m;
            if (!String(m.key) || !keys.insert(m.key).second || !Take(':')) return false;
            Skip(); m.valueStart = p;
            if (!Value(depth + 1)) return false;
            m.end = p; Skip();
            if (p < s.size() && s[p] == ',') {
                ++p; if (root) members.push_back(m);
            } else {
                if (root) members.push_back(m);
                return Take('}');
            }
        }
    }
    bool Value(int depth)
    {
        Skip(); if (p >= s.size() || depth > 64) return false;
        if (s[p] == '{') return Object(false, depth);
        if (s[p] == '"') { std::string ignored; return String(ignored); }
        if (s[p] == '[') {
            ++p; Skip(); if (p < s.size() && s[p] == ']') { ++p; return true; }
            do { if (!Value(depth + 1)) return false; Skip(); if (p < s.size() && s[p] == ']') { ++p; return true; } } while (Take(','));
            return false;
        }
        for (const auto* literal : {"true", "false", "null"}) {
            size_t n = std::strlen(literal);
            if (s.compare(p, n, literal) == 0) { p += n; return true; }
        }
        if (s[p] == '-') ++p;
        if (p >= s.size()) return false;
        if (s[p] == '0') ++p;
        else { if (s[p] < '1' || s[p] > '9') return false; while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p; }
        if (p < s.size() && s[p] == '.') { ++p; size_t start = p; while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p; if (p == start) return false; }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) { ++p; if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p; size_t start = p; while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p; if (p == start) return false; }
        return true;
    }
};

std::string Canonical(const std::string& value)
{
    std::string out; bool quoted = false;
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (quoted) { out += c; if (c == '\\' && i + 1 < value.size()) out += value[++i]; else if (c == '"') quoted = false; }
        else if (c == '"') { quoted = true; out += c; }
        else if (c == '/' && i + 1 < value.size() && value[i + 1] == '/') { while (i < value.size() && value[i] != '\n') ++i; }
        else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') out += c;
    }
    return out;
}
bool ResolvePath(const std::string& path, std::string& resolved, std::string& error)
{
    char real[PATH_MAX];
    if (realpath(path.c_str(), real)) { resolved = real; return true; }
    struct stat st;
    if (lstat(path.c_str(), &st) == 0 || errno != ENOENT) {
        error = "Cannot resolve config path (possibly a dangling symbolic link)"; return false;
    }
    resolved = path; return true;
}
bool ReadPath(const std::string& path, std::string& text, std::string& error)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) { if (errno == ENOENT) { text = "{}\n"; return true; } error = std::strerror(errno); return false; }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) { error = "Configuration must be a regular file"; close(fd); return false; }
    text.clear(); char buf[4096]; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; error = std::strerror(errno); close(fd); return false; }
        text.append(buf, n); if (text.size() > 4 * 1024 * 1024) { error = "Config exceeds 4 MiB"; close(fd); return false; }
    }
    close(fd); return true;
}
bool EnsureDirectory(const std::string& path, std::string& error)
{
    for (size_t p = 1; p <= path.size(); ++p) if (p == path.size() || path[p] == '/') {
        const auto part = path.substr(0, p);
        if (!part.empty() && mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) { error = std::strerror(errno); return false; }
    }
    return true;
}
}

bool ConfigStore::Parse(const std::string& document, Snapshot& snapshot, std::string& error, bool allowComments)
{
    Document parser(document, allowComments);
    if (!parser.Parse()) { error = "Invalid JSON configuration or duplicate key"; return false; }
    Snapshot next; next.document = document;
    for (const auto& m : parser.members) next.values[m.key] = Canonical(document.substr(m.valueStart, m.end - m.valueStart));
    snapshot = std::move(next); error.clear(); return true;
}

bool ConfigStore::Read(Snapshot& snapshot, std::string& error) const
{
    std::string path, text;
    return ResolvePath(_path, path, error) && ReadPath(path, text, error) && Parse(text, snapshot, error);
}

bool ConfigStore::ParseString(const std::string& json, std::string& value)
{
    std::vector<std::string> strings;
    if (!Document(json).Strings(strings, false)) return false;
    value = std::move(strings.front());
    return true;
}

bool ConfigStore::ParseStringArray(const std::string& json, std::vector<std::string>& values)
{
    return Document(json).Strings(values, true);
}

bool ConfigStore::Commit(const std::string& document, const Snapshot& expected, Snapshot& saved,
    std::string& error, bool overwrite) const
{
    std::string path, text;
    if (!ResolvePath(_path, path, error) || !ReadPath(path, text, error)) return false;
    Snapshot latest;
    if (!Parse(text, latest, error)) return false;
    if (!overwrite && latest.values != expected.values) { error = "Config conflict: file changed"; return false; }
    Snapshot next; if (!Parse(document, next, error)) return false;
    size_t slash = path.find_last_of('/');
    std::string directory = slash == std::string::npos ? "." : path.substr(0, slash);
    if (directory.empty()) directory = "/";
    if (!EnsureDirectory(directory, error)) return false;
    struct stat original; bool exists = stat(path.c_str(), &original) == 0;
    std::string temporary = directory + "/.deskwaifu-config-XXXXXX";
    std::vector<char> name(temporary.begin(), temporary.end()); name.push_back(0);
    int fd = mkstemp(name.data());
    if (fd < 0) { error = std::strerror(errno); return false; }
    // 持久化的 API 密钥不能继承允许同组用户或其他用户读取的权限。
    const bool secret = next.values.count("minimax_api_key") && next.values.at("minimax_api_key") != "\"\"";
    bool ok = fchmod(fd, secret ? 0600 : exists ? original.st_mode & 0777 : 0600) == 0;
    if (ok && exists) { struct stat created; ok = fstat(fd, &created) == 0;
        if (ok && (created.st_uid != original.st_uid || created.st_gid != original.st_gid)) ok = fchown(fd, original.st_uid, original.st_gid) == 0; }
    size_t offset = 0;
    while (ok && offset < document.size()) { ssize_t n = write(fd, document.data() + offset, document.size() - offset); if (n < 0 && errno == EINTR) continue; if (n <= 0) ok = false; else offset += n; }
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    // 外部编辑器未必遵守锁约定。这里只缩小竞争窗口，不保证检查到 rename 之间
    // 面对任意外部写入者时仍具备比较并交换（CAS）语义。
    std::string current;
    if (ok && (!ReadPath(path, current, error) || current != text)) { error = "Config changed during save; retry"; ok = false; }
    if (ok) ok = rename(name.data(), path.c_str()) == 0;
    if (!ok) { if (error.empty()) error = std::strerror(errno); unlink(name.data()); return false; }
    int dir = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) { fsync(dir); close(dir); }
    saved = std::move(next); error.clear(); return true;
}
