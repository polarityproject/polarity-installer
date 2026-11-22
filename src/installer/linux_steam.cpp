#include "installer/linux_steam.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>

namespace polarity::installer {

namespace {

std::string Lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return std::string(s.substr(a, b - a));
}

bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {
        if (s_.substr(0, 3) == "\xEF\xBB\xBF") i_ = 3;
    }

    bool Document(std::vector<VdfEntry>* out, std::string* error) {
        size_t end = 0;
        if (Block(true, out, &end)) return true;
        if (error) *error = error_;
        return false;
    }

private:
    enum class Kind { End,
                      Open,
                      Close,
                      String };

    struct Token {
        Kind kind;
        std::string text;
        size_t start, end;
    };

    void Skip() {
        while (i_ < s_.size()) {
            if (Space(s_[i_])) i_++;
            else if (s_.compare(i_, 2, "//") == 0) {
                while (i_ < s_.size() && s_[i_] != '\n') i_++;
            } else return;
        }
    }

    bool Next(Token* t) {
        Skip();
        t->start = i_;
        if (i_ >= s_.size()) {
            t->kind = Kind::End;
            t->end = i_;
            return true;
        }
        if (s_[i_] == '{' || s_[i_] == '}') {
            t->kind = s_[i_] == '{' ? Kind::Open : Kind::Close;
            t->end = ++i_;
            return true;
        }
        t->kind = Kind::String;
        t->text.clear();
        if (s_[i_] == '"') {
            i_++;
            while (i_ < s_.size() && s_[i_] != '"') {
                if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                    const char c = s_[i_ + 1];
                    t->text += c == 'n' ? '\n' : c == 't' ? '\t'
                                                          : c;
                    i_ += 2;
                } else {
                    t->text += s_[i_++];
                }
            }
            if (i_ >= s_.size()) return Fail("a string runs to the end of the file");
            t->end = ++i_;
            return true;
        }
        while (i_ < s_.size() && !Space(s_[i_]) && s_[i_] != '{' && s_[i_] != '}' && s_[i_] != '"') t->text += s_[i_++];
        t->end = i_;
        return true;
    }

    bool Fail(const std::string& why) {
        if (error_.empty()) error_ = why + " (offset " + std::to_string(i_) + ")";
        return false;
    }

    bool Block(bool top, std::vector<VdfEntry>* out, size_t* end) {
        if (++depth_ > 64) return Fail("blocks nest too deep");
        for (;;) {
            Token key;
            if (!Next(&key)) return false;
            if (key.kind == Kind::End) {
                if (!top) return Fail("a block is not closed");
                *end = key.end;
                depth_--;
                return true;
            }
            if (key.kind == Kind::Close) {
                if (top) return Fail("a stray }");
                *end = key.end;
                depth_--;
                return true;
            }
            if (key.kind == Kind::Open) return Fail("a block without a key");
            Token value;
            if (!Next(&value)) return false;
            VdfEntry e;
            e.key = key.text;
            e.kstart = key.start;
            e.kend = key.end;
            e.vstart = value.start;
            if (value.kind == Kind::Open) {
                e.block = true;
                if (!Block(false, &e.children, &e.vend)) return false;
            } else if (value.kind == Kind::String) {
                e.value = value.text;
                e.vend = value.end;
            } else {
                return Fail("no value for \"" + key.text + "\"");
            }
            out->push_back(std::move(e));
            const size_t save = i_;
            Token cond;
            if (!Next(&cond) || cond.kind != Kind::String || cond.text.rfind("[$", 0) != 0) i_ = save;
        }
    }

    std::string_view s_;
    size_t i_ = 0;
    int depth_ = 0;
    std::string error_;
};

const VdfEntry* Child(const std::vector<VdfEntry>& entries, std::string_view key, bool block) {
    const std::string want = Lower(key);
    for (const VdfEntry& e : entries) {
        if (e.block == block && Lower(e.key) == want) return &e;
    }
    return nullptr;
}

std::string IndentOf(std::string_view text, size_t pos) {
    const size_t line = text.rfind('\n', pos == 0 ? 0 : pos - 1);
    const size_t from = line == std::string_view::npos ? 0 : line + 1;
    size_t i = from;
    while (i < pos && (text[i] == ' ' || text[i] == '\t')) i++;
    return std::string(text.substr(from, i - from));
}

std::string InsertInBlock(std::string_view text, const VdfEntry& block, const std::vector<std::string>& lines) {
    const size_t close = block.vend - 1;
    const std::string outer = IndentOf(text, block.kstart), inner = outer + "\t";
    std::string body;
    for (const std::string& l : lines) body += inner + l + "\n";
    const size_t nl = text.rfind('\n', close == 0 ? 0 : close - 1);
    const size_t lineStart = nl == std::string_view::npos ? 0 : nl + 1;
    std::string out;
    if (!Trim(text.substr(lineStart, close - lineStart)).empty()) {
        out.append(text.substr(0, close)).append("\n").append(body).append(outer).append(text.substr(close));
    } else {
        out.append(text.substr(0, lineStart)).append(body).append(text.substr(lineStart));
    }
    return out;
}

const VdfEntry* LaunchOptionsEntry(const VdfEntry& app) {
    return Child(app.children, "LaunchOptions", false);
}

bool IsDir(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace

bool VdfParse(std::string_view text, std::vector<VdfEntry>* out, std::string* error) {
    out->clear();
    return Parser(text).Document(out, error);
}

const VdfEntry* VdfFind(const std::vector<VdfEntry>& entries, const std::vector<std::string_view>& path) {
    const std::vector<VdfEntry>* at = &entries;
    const VdfEntry* found = nullptr;
    for (std::string_view key : path) {
        found = Child(*at, key, true);
        if (!found) return nullptr;
        at = &found->children;
    }
    return found;
}

std::string VdfQuote(std::string_view value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::optional<std::string> GetLaunchOptions(std::string_view text) {
    std::vector<VdfEntry> tree;
    if (!VdfParse(text, &tree, nullptr)) return std::nullopt;
    const VdfEntry* app = VdfFind(tree, {"UserLocalConfigStore", "Software", "Valve", "Steam", "apps", kAppId});
    if (!app) return std::string();
    const VdfEntry* opts = LaunchOptionsEntry(*app);
    return opts ? opts->value : std::string();
}

bool SetLaunchOptions(std::string_view text, const std::string& options, std::string* out, std::string* error) {
    std::vector<VdfEntry> tree;
    if (!VdfParse(text, &tree, error)) return false;
    const VdfEntry* steam = VdfFind(tree, {"UserLocalConfigStore", "Software", "Valve", "Steam"});
    if (!steam) {
        if (error) *error = "no Software/Valve/Steam block";
        return false;
    }
    const std::string quoted = VdfQuote(options);
    const VdfEntry* apps = VdfFind(steam->children, {"apps"});
    if (!apps) {
        *out = InsertInBlock(text, *steam,
                             {"\"apps\"", "{", "\t" + VdfQuote(kAppId), "\t{", "\t\t\"LaunchOptions\"\t\t" + quoted, "\t}", "}"});
        return true;
    }
    const VdfEntry* app = VdfFind(apps->children, {kAppId});
    if (!app) {
        *out = InsertInBlock(text, *apps, {VdfQuote(kAppId), "{", "\t\"LaunchOptions\"\t\t" + quoted, "}"});
        return true;
    }
    if (const VdfEntry* opts = LaunchOptionsEntry(*app)) {
        *out = std::string(text.substr(0, opts->vstart)) + quoted + std::string(text.substr(opts->vend));
        return true;
    }
    *out = InsertInBlock(text, *app, {"\"LaunchOptions\"\t\t" + quoted});
    return true;
}

std::string WithOverride(std::string_view options) {
    const std::string o = Trim(options);
    const std::string key = "WINEDLLOVERRIDES=";
    const std::string ours = std::string("WINEDLLOVERRIDES=\"") + kOverride + "\"";
    size_t at = std::string::npos;
    for (size_t p = o.find(key); p != std::string::npos; p = o.find(key, p + 1)) {
        const char before = p == 0 ? ' ' : o[p - 1];
        if (!(std::isalnum(static_cast<unsigned char>(before)) || before == '_' || before == '$')) {
            at = p;
            break;
        }
    }
    if (at == std::string::npos) {
        if (o.find("%command%") != std::string::npos) return ours + " " + o;
        return ours + " %command%" + (o.empty() ? "" : " " + o);
    }
    const size_t vs = at + key.size();
    size_t ve = vs;
    std::string value;
    if (vs < o.size() && (o[vs] == '"' || o[vs] == '\'')) {
        const size_t close = o.find(o[vs], vs + 1);
        if (close != std::string::npos) {
            value = o.substr(vs + 1, close - vs - 1);
            ve = close + 1;
        }
    } else {
        while (ve < o.size() && !std::isspace(static_cast<unsigned char>(o[ve])) && o[ve] != '"' && o[ve] != '\'') ve++;
        value = o.substr(vs, ve - vs);
    }
    std::string merged = kOverride;
    size_t from = 0;
    while (from <= value.size()) {
        size_t semi = value.find(';', from);
        if (semi == std::string::npos) semi = value.size();
        const std::string entry = value.substr(from, semi - from);
        from = semi + 1;
        if (Trim(entry).empty()) continue;
        const size_t eq = entry.find('=');
        const std::string names = entry.substr(0, eq);
        const std::string tail = eq == std::string::npos ? "" : entry.substr(eq);
        const std::string mode = Lower(Trim(eq == std::string::npos ? "" : entry.substr(eq + 1)));
        std::vector<std::string> rest;
        bool mine = false;
        size_t nf = 0;
        while (nf <= names.size()) {
            size_t comma = names.find(',', nf);
            if (comma == std::string::npos) comma = names.size();
            const std::string dll = names.substr(nf, comma - nf);
            nf = comma + 1;
            if (Trim(dll).empty()) continue;
            const std::string l = Lower(Trim(dll));
            if (l == "xinput1_3" || l == "xinput1_3.dll") mine = true;
            else rest.push_back(dll);
        }
        if (mine && !mode.empty() && mode[0] == 'n') return o;
        if (rest.empty()) continue;
        std::string joined;
        for (size_t i = 0; i < rest.size(); i++) joined += (i ? "," : "") + rest[i];
        merged += ";" + joined + tail;
    }
    return o.substr(0, at) + "WINEDLLOVERRIDES=\"" + merged + "\"" + o.substr(ve);
}

std::vector<std::string> LibraryPaths(std::string_view text) {
    std::vector<std::string> out;
    std::vector<VdfEntry> tree;
    if (!VdfParse(text, &tree, nullptr)) return out;
    std::vector<const std::vector<VdfEntry>*> todo{&tree};
    while (!todo.empty()) {
        const auto* entries = todo.back();
        todo.pop_back();
        for (auto it = entries->rbegin(); it != entries->rend(); ++it) {
            if (it->block) todo.push_back(&it->children);
        }
        for (const VdfEntry& e : *entries) {
            if (!e.block && Lower(e.key) == "path") out.push_back(e.value);
        }
    }
    return out;
}

std::string InstallDir(std::string_view text) {
    std::vector<VdfEntry> tree;
    if (!VdfParse(text, &tree, nullptr)) return {};
    const VdfEntry* state = VdfFind(tree, {"AppState"});
    const VdfEntry* dir = state ? Child(state->children, "installdir", false) : nullptr;
    return dir ? dir->value : std::string();
}

std::optional<std::string> GameWin64(const std::string& picked) {
    std::string p = picked;
    if (p.rfind("~/", 0) == 0 || p == "~") {
        const char* home = std::getenv("HOME");
        p = std::string(home ? home : "") + p.substr(1);
    }
    char real[PATH_MAX];
    if (realpath(p.c_str(), real)) p = real;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    for (const std::string& c : {p + "/StreetFighterV/Binaries/Win64", p + "/Binaries/Win64", p + "/Win64", p}) {
        const std::string l = Lower(c);
        const bool win64 = l.size() >= 15 && l.compare(l.size() - 15, 15, "/binaries/win64") == 0;
        if (win64 && IsDir(c) && IsFile(c + "/StreetFighterV.exe")) return c;
    }
    return std::nullopt;
}

std::string GameRoot(const std::string& win64) {
    std::string p = win64;
    for (int i = 0; i < 3; i++) {
        const size_t slash = p.find_last_of('/');
        if (slash == std::string::npos || slash == 0) break;
        p = p.substr(0, slash);
    }
    return p;
}

} // namespace polarity::installer
