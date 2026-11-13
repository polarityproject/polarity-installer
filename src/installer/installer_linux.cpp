// polarity-installer (Linux): installs the latest Polarity release into
// Street Fighter V under Steam with Proton, the Steam Deck included.
//
// One static binary (musl, mbedTLS: scripts/build-linux.ps1):
// needs nothing on the player's system but Steam. zenity or kdialog show its
// windows when there are any; the terminal asks otherwise.
//
// 1. Reads the Polarity site's signed update info (what the mod's own
//    updater reads: update.json + update.json.sig, checked against the
//    release key in mod/update_key.h).
// 2. Finds Street Fighter V in Steam's libraries, or asks for its folder.
// 3. Downloads the release's files, checks each one's size and SHA-256, and
//    puts them next to StreetFighterV.exe (StreetFighterV/Binaries/Win64),
//    all or nothing.
// 4. Adds WINEDLLOVERRIDES="xinput1_3=n,b" to the game's Steam launch
//    options (Proton loads the mod's xinput1_3.dll only with it). Steam keeps
//    them in userdata/<account>/config/localconfig.vdf and writes that file
//    when it closes, so Steam has to be closed while they are saved: the
//    installer asks, closes Steam and opens it again.
// 5. Says Polarity runs at the game's next start.
//
// It makes no desktop files, menu entries or Steam shortcuts.
//
// Options (mostly for tests):
//   --site URL         update info other than https://polaritymod.link/update.json
//   --game DIR         the game folder (any of its levels down to Binaries/Win64)
//   --steam-root DIR   a Steam folder to use instead of looking for one (repeatable)
//   --yes              no questions: the folder found (or --game); closes and
//                      reopens Steam when the launch options need it
//   --terminal         questions in the terminal even when windows are possible

#include "core/update_info.h"
#include "installer/linux_steam.h"
#include "mod/update_key.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace polarity::installer {
namespace {

constexpr const char* kSite = "https://polaritymod.link/update.json";
constexpr const char* kTitle = "Polarity Installer";
constexpr size_t kMaxInfoBytes = 64 * 1024;
constexpr int kTimeoutMs = 20000;

// Something the player is told about.
struct Failure {
    std::string message;
};
// The player stopped it (message: why, or empty to say nothing).
struct Cancelled {
    std::string message;
};

std::string Megabytes(uint64_t n)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", n / (1024.0 * 1024.0));
    return text;
}

// ----- files and processes ---------------------------------------------------------

bool IsFile(const std::string& p)
{
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsDir(const std::string& p)
{
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool Exists(const std::string& p)
{
    struct stat st{};
    return lstat(p.c_str(), &st) == 0;
}

std::optional<std::string> ReadFile(const std::string& path, size_t maxBytes = 64 * 1024 * 1024)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::nullopt;
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        data.append(buf, n);
        if (data.size() > maxBytes) break;
    }
    std::fclose(f);
    return data;
}

bool WriteFile(const std::string& path, const void* data, size_t size, mode_t mode = 0644)
{
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        const ssize_t w = write(fd, p + done, size - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            close(fd);
            return false;
        }
        done += static_cast<size_t>(w);
    }
    const bool ok = fsync(fd) == 0;
    return close(fd) == 0 && ok;
}

void MakeDirs(const std::string& dir)
{
    if (dir.empty() || IsDir(dir)) return;
    const size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && slash > 0) MakeDirs(dir.substr(0, slash));
    mkdir(dir.c_str(), 0755);
}

std::string Dirname(const std::string& p)
{
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : slash == 0 ? "/" : p.substr(0, slash);
}

std::string RealPath(const std::string& p)
{
    char real[PATH_MAX];
    return realpath(p.c_str(), real) ? std::string(real) : p;
}

std::vector<std::string> ListDir(const std::string& dir)
{
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) out.push_back(e->d_name);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string Which(const std::string& name)
{
    const char* path = std::getenv("PATH");
    std::string all = path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t from = 0;
    while (from <= all.size()) {
        size_t colon = all.find(':', from);
        if (colon == std::string::npos) colon = all.size();
        const std::string dir = all.substr(from, colon - from);
        from = colon + 1;
        const std::string full = (dir.empty() ? "." : dir) + "/" + name;
        if (IsFile(full) && access(full.c_str(), X_OK) == 0) return full;
    }
    return {};
}

std::vector<char*> Argv(const std::vector<std::string>& args)
{
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    return argv;
}

// Runs a program and waits: its exit code (-1 when it could not run) and stdout.
int RunCapture(const std::vector<std::string>& args, std::string* out)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        const int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(fds[1], 1);
        dup2(nul, 2);
        close(fds[0]);
        close(fds[1]);
        auto argv = Argv(args);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR)) {
        if (n > 0 && out) out->append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (out) {
        while (!out->empty() && (out->back() == '\n' || out->back() == '\r')) out->pop_back();
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Starts a program on its own (its own session, not waited for).
void RunDetached(const std::vector<std::string>& args)
{
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            const int nul = open("/dev/null", O_RDWR);
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
            auto argv = Argv(args);
            execvp(argv[0], argv.data());
        }
        _exit(0);
    }
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

// This user's live processes whose name (/proc/<pid>/comm) passes `match`.
std::vector<int> Processes(const std::function<bool(const std::string&)>& match)
{
    std::vector<int> found;
    const uid_t me = getuid();
    for (const std::string& name : ListDir("/proc")) {
        if (name.find_first_not_of("0123456789") != std::string::npos) continue;
        const std::string dir = "/proc/" + name;
        struct stat st{};
        if (stat(dir.c_str(), &st) != 0 || st.st_uid != me) continue;
        auto comm = ReadFile(dir + "/comm", 4096);
        auto stat_ = ReadFile(dir + "/stat", 4096);
        if (!comm || !stat_) continue;
        while (!comm->empty() && comm->back() == '\n') comm->pop_back();
        const size_t paren = stat_->rfind(") ");
        if (paren == std::string::npos || stat_->compare(paren + 2, 1, "Z") == 0) continue; // a zombie: gone
        if (match(*comm)) found.push_back(std::atoi(name.c_str()));
    }
    return found;
}

bool SteamRunning()
{
    return !Processes([](const std::string& c) { return c == "steam"; }).empty();
}

bool GameRunning()
{
    return !Processes([](const std::string& c) { return c.rfind("StreetFighterV", 0) == 0; }).empty();
}

// ----- HTTP(S) -----------------------------------------------------------------

struct Url {
    bool https = false;
    std::string host, port, path;
};

bool ParseUrl(const std::string& url, Url* out)
{
    size_t rest;
    if (url.rfind("https://", 0) == 0) {
        out->https = true;
        rest = 8;
    }
    else if (url.rfind("http://", 0) == 0) {
        out->https = false;
        rest = 7;
    }
    else {
        return false;
    }
    const size_t slash = url.find('/', rest);
    std::string hostport = url.substr(rest, slash == std::string::npos ? std::string::npos : slash - rest);
    out->path = slash == std::string::npos ? "/" : url.substr(slash);
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
        out->host = hostport.substr(0, colon);
        out->port = hostport.substr(colon + 1);
    }
    else {
        out->host = hostport;
        out->port = out->https ? "443" : "80";
    }
    if (out->host.size() > 2 && out->host.front() == '[') out->host = out->host.substr(1, out->host.size() - 2);
    return !out->host.empty();
}

std::string TlsError(int code)
{
    char text[200];
    mbedtls_strerror(code, text, sizeof(text));
    return text;
}

// The system's trusted roots, when it has a bundle where distros keep one.
// Without one the download still goes ahead: what it fetches only counts
// once the signature and the hashes check out.
class Roots {
public:
    Roots()
    {
        mbedtls_x509_crt_init(&crt_);
        std::vector<std::string> files;
        if (const char* env = std::getenv("SSL_CERT_FILE")) files.push_back(env);
        for (const char* f : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                              "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/ca-bundle.pem",
                              "/etc/ssl/cert.pem", "/etc/pki/tls/cacert.pem"}) {
            files.push_back(f);
        }
        for (const std::string& f : files) {
            if (IsFile(f) && mbedtls_x509_crt_parse_file(&crt_, f.c_str()) >= 0 && crt_.version != 0) {
                loaded_ = true;
                break;
            }
        }
    }
    ~Roots() { mbedtls_x509_crt_free(&crt_); }
    bool Loaded() const { return loaded_; }
    mbedtls_x509_crt* Get() { return &crt_; }

private:
    mbedtls_x509_crt crt_;
    bool loaded_ = false;
};

class Connection {
public:
    Connection()
    {
        mbedtls_net_init(&net_);
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
    }
    ~Connection()
    {
        if (tls_) mbedtls_ssl_close_notify(&ssl_);
        mbedtls_net_free(&net_);
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }

    void Open(const Url& url)
    {
        static Roots roots;
        int r = mbedtls_net_connect(&net_, url.host.c_str(), url.port.c_str(), MBEDTLS_NET_PROTO_TCP);
        if (r != 0) {
            throw Failure{"Cannot reach " + url.host + " (" + TlsError(r) + "). Check your internet connection and try again."};
        }
        if (!url.https) return;
        tls_ = true;
        if ((r = mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_, nullptr, 0)) != 0 ||
            (r = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                             MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
            throw Failure{"Cannot set up a secure connection (" + TlsError(r) + ")."};
        }
        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
        mbedtls_ssl_conf_read_timeout(&conf_, kTimeoutMs);
        if (roots.Loaded()) {
            mbedtls_ssl_conf_ca_chain(&conf_, roots.Get(), nullptr);
            mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
        }
        else {
            mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_NONE);
        }
        if ((r = mbedtls_ssl_setup(&ssl_, &conf_)) != 0 || (r = mbedtls_ssl_set_hostname(&ssl_, url.host.c_str())) != 0) {
            throw Failure{"Cannot set up a secure connection (" + TlsError(r) + ")."};
        }
        mbedtls_ssl_set_bio(&ssl_, &net_, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
        while ((r = mbedtls_ssl_handshake(&ssl_)) != 0) {
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
                throw Failure{"Cannot make a secure connection to " + url.host + " (" + TlsError(r) + ")."};
            }
        }
    }

    void Send(const std::string& data)
    {
        size_t done = 0;
        while (done < data.size()) {
            const auto* p = reinterpret_cast<const unsigned char*>(data.data()) + done;
            const int r = tls_ ? mbedtls_ssl_write(&ssl_, p, data.size() - done) : mbedtls_net_send(&net_, p, data.size() - done);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r <= 0) throw Failure{"The connection to the Polarity site broke off (" + TlsError(r) + ")."};
            done += static_cast<size_t>(r);
        }
    }

    // Bytes read; 0 at the end of the stream.
    size_t Read(unsigned char* buf, size_t size)
    {
        for (;;) {
            const int r = tls_ ? mbedtls_ssl_read(&ssl_, buf, size) : mbedtls_net_recv_timeout(&net_, buf, size, kTimeoutMs);
            if (r > 0) return static_cast<size_t>(r);
            if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE ||
                r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                continue;
            }
            if (r == MBEDTLS_ERR_SSL_TIMEOUT) throw Failure{"The Polarity site stopped answering. Try again in a moment."};
            throw Failure{"The connection to the Polarity site broke off (" + TlsError(r) + ")."};
        }
    }

private:
    mbedtls_net_context net_;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
    bool tls_ = false;
};

// GET `url` (http or https, redirects followed) into `out`, at most `maxBytes`
// of body. `progress` gets the bytes so far and returns false to stop.
std::vector<uint8_t> HttpGet(std::string url, size_t maxBytes, const std::function<bool(uint64_t)>& progress = nullptr)
{
    for (int redirects = 0; redirects < 6; redirects++) {
        Url u;
        if (!ParseUrl(url, &u)) throw Failure{"Not a web address: " + url};
        Connection c;
        c.Open(u);
        const bool defaultPort = u.port == (u.https ? "443" : "80");
        c.Send("GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + (defaultPort ? "" : ":" + u.port) +
               "\r\nUser-Agent: Polarity installer\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
        // The head.
        std::string raw;
        unsigned char buf[65536];
        size_t headEnd;
        while ((headEnd = raw.find("\r\n\r\n")) == std::string::npos) {
            if (raw.size() > 64 * 1024) throw Failure{"The Polarity site sent an answer that cannot be read."};
            const size_t n = c.Read(buf, sizeof(buf));
            if (n == 0) throw Failure{"The Polarity site closed the connection early."};
            raw.append(reinterpret_cast<char*>(buf), n);
        }
        const std::string head = raw.substr(0, headEnd);
        std::string body = raw.substr(headEnd + 4);
        int status = 0;
        if (std::sscanf(head.c_str(), "HTTP/%*d.%*d %d", &status) != 1) {
            throw Failure{"The Polarity site sent an answer that cannot be read."};
        }
        std::map<std::string, std::string> headers;
        size_t at = head.find("\r\n");
        while (at != std::string::npos) {
            const size_t next = head.find("\r\n", at + 2);
            const std::string line = head.substr(at + 2, next == std::string::npos ? std::string::npos : next - at - 2);
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                std::string value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                headers[name] = value;
            }
            at = next;
        }
        if (status >= 300 && status < 400 && headers.count("location")) {
            url = sfv::ResolveUrl(url, headers["location"]);
            continue;
        }
        if (status != 200) throw Failure{"The Polarity site answered " + std::to_string(status) + " for " + url + "."};

        std::string te = headers["transfer-encoding"];
        std::transform(te.begin(), te.end(), te.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const bool chunked = te.find("chunked") != std::string::npos;
        const bool hasLength = !chunked && headers.count("content-length");
        const uint64_t length = hasLength ? std::strtoull(headers["content-length"].c_str(), nullptr, 10) : 0;
        if (hasLength && length > maxBytes) throw Failure{url + " is larger than the update info says."};
        // Chunk heads take a little room beyond the body itself.
        const size_t rawLimit = chunked ? maxBytes + maxBytes / 8 + 64 * 1024 : maxBytes;
        for (;;) {
            if (hasLength && body.size() >= length) break;
            if (body.size() > rawLimit) throw Failure{url + " is larger than the update info says."};
            if (progress && !progress(body.size())) throw Cancelled{};
            const size_t n = c.Read(buf, sizeof(buf));
            if (n == 0) break;
            body.append(reinterpret_cast<char*>(buf), n);
        }
        if (hasLength && body.size() < length) throw Failure{"The download of " + url + " broke off."};
        if (hasLength) body.resize(length);
        if (chunked) {
            std::string plain;
            size_t pos = 0;
            for (;;) {
                const size_t eol = body.find("\r\n", pos);
                if (eol == std::string::npos) throw Failure{"The download of " + url + " broke off."};
                const size_t size = std::strtoull(body.substr(pos, eol - pos).c_str(), nullptr, 16);
                pos = eol + 2;
                if (size == 0) break;
                if (pos + size > body.size()) throw Failure{"The download of " + url + " broke off."};
                plain.append(body, pos, size);
                pos += size + 2;
            }
            body = std::move(plain);
        }
        if (body.size() > maxBytes) throw Failure{url + " is larger than the update info says."};
        if (progress) progress(body.size());
        return std::vector<uint8_t>(body.begin(), body.end());
    }
    throw Failure{"The Polarity site redirected too many times."};
}

sfv::UpdateInfo FetchInfo(const std::string& site)
{
    const std::vector<uint8_t> body = HttpGet(site, kMaxInfoBytes);
    const std::vector<uint8_t> sig = HttpGet(site + ".sig", 4096);
    const std::vector<uint8_t> signature =
        sfv::Base64Decode(std::string_view(reinterpret_cast<const char*>(sig.data()), sig.size()));
    if (!sfv::VerifySignature(body.data(), body.size(), signature.data(), signature.size(), sfv::mod::kUpdatePublicKey)) {
        throw Failure{"The update info on the Polarity site is not signed with the release key, so nothing was installed."};
    }
    sfv::UpdateInfo info;
    std::string why;
    if (!sfv::ParseUpdateInfo(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), &info, &why)) {
        throw Failure{"Cannot read the update info: " + why};
    }
    return info;
}

bool SameFile(const std::string& path, const sfv::UpdateFile& f)
{
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || static_cast<uint64_t>(st.st_size) != f.size) return false;
    const auto data = ReadFile(path, f.size + 1);
    return data && data->size() == f.size &&
           sfv::Sha256Hex(reinterpret_cast<const uint8_t*>(data->data()), data->size()) == f.sha256;
}

// Downloads what differs from the game folder and swaps it in, old files
// aside first so a failure puts everything back. How many files changed.
int InstallFiles(const std::string& win64, const sfv::UpdateInfo& info, const std::string& site,
                 const std::function<bool(uint64_t, uint64_t)>& progress)
{
    struct Item {
        const sfv::UpdateFile* f;
        std::string target, fresh, old;
        bool hadOld = false;
    };
    std::vector<Item> items;
    uint64_t total = 0;
    for (const sfv::UpdateFile& f : info.files) {
        const std::string target = win64 + "/" + sfv::UpdateFileOnDisk(f.path);
        if (SameFile(target, f)) continue;
        items.push_back({&f, target, target + ".polarity-new", target + ".polarity-old"});
        total += f.size;
    }
    if (items.empty()) return 0;
    auto cleanup = [&] {
        for (const Item& it : items) unlink(it.fresh.c_str());
    };
    uint64_t done = 0;
    try {
        for (Item& it : items) {
            const sfv::UpdateFile& f = *it.f;
            const auto data = HttpGet(sfv::ResolveUrl(site, f.url), f.size + 1, [&](uint64_t n) { return progress(done + n, total); });
            if (data.size() != f.size || sfv::Sha256Hex(data.data(), data.size()) != f.sha256) {
                throw Failure{f.path + " from the Polarity site does not match the update info. Nothing was changed."};
            }
            MakeDirs(Dirname(it.target));
            if (!WriteFile(it.fresh, data.data(), data.size())) {
                throw Failure{"Cannot write into " + win64 + " (" + std::strerror(errno) + ")."};
            }
            done += f.size;
            progress(done, total);
        }
    }
    catch (...) {
        cleanup();
        throw;
    }
    std::vector<Item*> swapped;
    for (Item& it : items) {
        it.hadOld = Exists(it.target);
        bool ok = !it.hadOld || rename(it.target.c_str(), it.old.c_str()) == 0;
        if (ok) {
            ok = rename(it.fresh.c_str(), it.target.c_str()) == 0;
            if (!ok && it.hadOld) rename(it.old.c_str(), it.target.c_str());
        }
        if (!ok) {
            const std::string why = std::strerror(errno);
            for (auto p = swapped.rbegin(); p != swapped.rend(); ++p) {
                unlink((*p)->target.c_str());
                if ((*p)->hadOld) rename((*p)->old.c_str(), (*p)->target.c_str());
            }
            cleanup();
            throw Failure{"Cannot replace " + it.target + " (" + why + ")."};
        }
        swapped.push_back(&it);
    }
    for (const Item& it : items) {
        if (it.hadOld) unlink(it.old.c_str());
    }
    return static_cast<int>(items.size());
}

// ----- Steam ---------------------------------------------------------------------

std::vector<std::string> SteamRoots(const std::vector<std::string>& given)
{
    std::vector<std::string> out;
    if (!given.empty()) {
        for (const std::string& r : given) {
            if (IsDir(r)) out.push_back(RealPath(r));
        }
        return out;
    }
    const char* home = std::getenv("HOME");
    if (!home) return out;
    for (const char* r : {".steam/steam", ".local/share/Steam", ".steam/root", ".steam/debian-installation",
                          ".var/app/com.valvesoftware.Steam/.local/share/Steam", "snap/steam/common/.local/share/Steam"}) {
        const std::string p = RealPath(std::string(home) + "/" + r);
        if (IsDir(p + "/steamapps") && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    return out;
}

std::vector<std::string> SteamLibraries(const std::vector<std::string>& roots)
{
    std::vector<std::string> libs;
    auto add = [&](const std::string& lib) {
        const std::string p = RealPath(lib);
        if (std::find(libs.begin(), libs.end(), p) == libs.end()) libs.push_back(p);
    };
    for (const std::string& root : roots) {
        add(root);
        for (const char* f : {"/steamapps/libraryfolders.vdf", "/config/libraryfolders.vdf"}) {
            if (const auto text = ReadFile(root + f, 4 * 1024 * 1024)) {
                for (const std::string& lib : LibraryPaths(*text)) add(lib);
                break;
            }
        }
    }
    return libs;
}

std::optional<std::string> FindGame(const std::vector<std::string>& libs)
{
    for (const std::string& lib : libs) {
        std::string folder = "StreetFighterV";
        if (const auto acf = ReadFile(lib + "/steamapps/appmanifest_" + kAppId + ".acf", 1024 * 1024)) {
            const std::string dir = InstallDir(*acf);
            if (!dir.empty()) folder = dir;
        }
        if (auto win64 = GameWin64(lib + "/steamapps/common/" + folder)) return win64;
    }
    return std::nullopt;
}

std::vector<std::string> LocalConfigs(const std::vector<std::string>& roots)
{
    std::vector<std::string> out;
    for (const std::string& root : roots) {
        for (const std::string& id : ListDir(root + "/userdata")) {
            const std::string p = root + "/userdata/" + id + "/config/localconfig.vdf";
            if (id != "0" && id.find_first_not_of("0123456789") == std::string::npos && IsFile(p)) out.push_back(p);
        }
    }
    return out;
}

// The accounts whose launch options do not load the mod yet.
std::vector<std::string> AccountsNeedingOptions(const std::vector<std::string>& paths)
{
    std::vector<std::string> out;
    for (const std::string& p : paths) {
        const auto text = ReadFile(p);
        const auto opts = text ? GetLaunchOptions(*text) : std::nullopt;
        if (!opts) {
            out.push_back(p);
            continue;
        }
        // WithOverride gives the options back (trimmed) when they load it already.
        std::string trimmed = *opts;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
        if (WithOverride(*opts) != trimmed) out.push_back(p);
    }
    return out;
}

std::vector<std::string> SteamCommand(const std::vector<std::string>& roots)
{
    if (!Which("steam").empty()) return {"steam"};
    for (const std::string& r : roots) {
        if (r.find("/com.valvesoftware.Steam/") != std::string::npos && !Which("flatpak").empty()) {
            return {"flatpak", "run", "com.valvesoftware.Steam"};
        }
    }
    return {};
}

void CloseSteam(const std::vector<std::string>& roots)
{
    std::vector<std::string> cmd = SteamCommand(roots);
    if (cmd.empty()) throw Failure{"The steam command was not found. Close Steam yourself and run the installer again."};
    cmd.push_back("-shutdown");
    RunDetached(cmd);
    for (int i = 0; i < 90; i++) {
        sleep(1);
        if (!SteamRunning()) {
            sleep(2); // its last writes
            return;
        }
    }
    throw Failure{"Steam did not close. Close it yourself and run the installer again."};
}

void OpenSteam(const std::vector<std::string>& roots)
{
    const std::vector<std::string> cmd = SteamCommand(roots);
    if (!cmd.empty()) RunDetached(cmd);
}

// Sets the launch options in each account's localconfig.vdf (a copy of the
// file as it was next to it); the files it could not change.
std::vector<std::string> WriteLaunchOptions(const std::vector<std::string>& paths)
{
    std::vector<std::string> failed;
    for (const std::string& p : paths) {
        const auto text = ReadFile(p);
        const auto opts = text ? GetLaunchOptions(*text) : std::nullopt;
        std::string updated;
        if (!opts || !SetLaunchOptions(*text, WithOverride(*opts), &updated, nullptr)) {
            failed.push_back(p);
            continue;
        }
        if (updated == *text) continue;
        struct stat st{};
        const mode_t mode = stat(p.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0644;
        const std::string tmp = Dirname(p) + "/.localconfig.polarity-new";
        if (!WriteFile(p + ".before-polarity", text->data(), text->size(), mode) ||
            !WriteFile(tmp, updated.data(), updated.size(), mode) || rename(tmp.c_str(), p.c_str()) != 0) {
            unlink(tmp.c_str());
            failed.push_back(p);
        }
    }
    return failed;
}

// ----- windows and questions ---------------------------------------------------------

class Ui {
public:
    virtual ~Ui() = default;
    bool yes = false;
    virtual void Tell(const std::string& heading, const std::string& text, const char* kind = "info") = 0;
    // The index of the button picked (buttons[0] is the default), -1 for cancel.
    virtual int Choose(const std::string& heading, const std::string& text, const std::vector<std::string>& buttons) = 0;
    virtual std::optional<std::string> PickFolder(const std::string& title, const std::string& start) = 0;
    virtual void ProgressStart(const std::string& heading) = 0;
    // False: the player cancelled.
    virtual bool Progress(double fraction, const std::string& text) = 0;
    virtual void ProgressEnd() = 0;
};

class TerminalUi : public Ui {
public:
    explicit TerminalUi(bool answerYes) { yes = answerYes; }

    void Tell(const std::string& heading, const std::string& text, const char* kind) override
    {
        (std::strcmp(kind, "error") == 0 ? std::cerr : std::cout) << "\n" << heading << "\n" << text << std::endl;
    }

    int Choose(const std::string& heading, const std::string& text, const std::vector<std::string>& buttons) override
    {
        std::cout << "\n" << heading << "\n" << text << std::endl;
        if (yes) return 0;
        for (size_t i = 0; i < buttons.size(); i++) std::cout << "  " << i + 1 << ". " << buttons[i] << "\n";
        for (;;) {
            std::cout << "Choose 1-" << buttons.size() << " (Enter: 1, q: cancel): " << std::flush;
            std::string answer;
            if (!std::getline(std::cin, answer)) return -1;
            if (answer == "q" || answer == "Q") return -1;
            if (answer.empty()) return 0;
            const int n = std::atoi(answer.c_str());
            if (n >= 1 && n <= static_cast<int>(buttons.size())) return n - 1;
        }
    }

    std::optional<std::string> PickFolder(const std::string& title, const std::string&) override
    {
        if (yes) return std::nullopt;
        std::cout << title << " (the folder path, Enter to cancel): " << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer) || answer.empty()) return std::nullopt;
        return answer;
    }

    void ProgressStart(const std::string& heading) override
    {
        std::cout << heading << std::endl;
        percent_ = -1;
    }

    bool Progress(double fraction, const std::string& text) override
    {
        const int percent = static_cast<int>(fraction * 100);
        if (percent != percent_) {
            percent_ = percent;
            std::printf("\r  %3d%%  %s   ", percent, text.c_str());
            std::fflush(stdout);
        }
        return true;
    }

    void ProgressEnd() override
    {
        if (percent_ >= 0) std::cout << std::endl;
        percent_ = -1;
    }

private:
    int percent_ = -1;
};

std::string Markup(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out += c;
    }
    return out;
}

class ZenityUi : public Ui {
public:
    void Tell(const std::string& heading, const std::string& text, const char* kind) override
    {
        const std::string k = std::strcmp(kind, "error") == 0 ? "--error" : std::strcmp(kind, "warning") == 0 ? "--warning" : "--info";
        Run({k, "--text=<b>" + Markup(heading) + "</b>\n\n" + Markup(text)}, nullptr);
    }

    int Choose(const std::string& heading, const std::string& text, const std::vector<std::string>& buttons) override
    {
        std::vector<std::string> args{"--question", "--text=<b>" + Markup(heading) + "</b>\n\n" + Markup(text),
                                      "--ok-label=" + buttons[0], "--cancel-label=Cancel"};
        for (size_t i = 1; i < buttons.size(); i++) args.push_back("--extra-button=" + buttons[i]);
        std::string out;
        const int code = Run(args, &out);
        if (code == 0) return 0;
        for (size_t i = 1; i < buttons.size(); i++) {
            if (out == buttons[i]) return static_cast<int>(i);
        }
        return -1;
    }

    std::optional<std::string> PickFolder(const std::string& title, const std::string& start) override
    {
        std::vector<std::string> args{"--file-selection", "--directory", "--title=" + title};
        if (!start.empty()) args.push_back("--filename=" + start + "/");
        std::string out;
        if (Run(args, &out) != 0 || out.empty()) return std::nullopt;
        return out;
    }

    void ProgressStart(const std::string& heading) override
    {
        heading_ = heading;
        int fds[2];
        if (pipe(fds) != 0) return;
        pid_ = fork();
        if (pid_ == 0) {
            const int nul = open("/dev/null", O_RDWR);
            dup2(fds[0], 0);
            dup2(nul, 1);
            dup2(nul, 2);
            close(fds[0]);
            close(fds[1]);
            auto argv = Argv({"zenity", "--progress", std::string("--title=") + kTitle, "--width=460", "--auto-close",
                              "--text=" + Markup(heading), "--percentage=0"});
            execvp(argv[0], argv.data());
            _exit(127);
        }
        close(fds[0]);
        fd_ = pid_ > 0 ? fds[1] : -1;
        if (pid_ <= 0) close(fds[1]);
    }

    bool Progress(double fraction, const std::string& text) override
    {
        if (pid_ <= 0) return true;
        int status;
        if (waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = 0; // closed: Cancel
            return false;
        }
        const std::string line = std::to_string(std::min(99, static_cast<int>(fraction * 100))) + "\n# " +
                                 Markup(heading_) + " (" + text + ")\n";
        return write(fd_, line.data(), line.size()) == static_cast<ssize_t>(line.size());
    }

    void ProgressEnd() override
    {
        if (fd_ >= 0) {
            const char done[] = "100\n";
            (void)!write(fd_, done, sizeof(done) - 1);
            close(fd_);
            fd_ = -1;
        }
        if (pid_ > 0) {
            int status;
            waitpid(pid_, &status, 0);
        }
        pid_ = 0;
    }

private:
    int Run(std::vector<std::string> args, std::string* out)
    {
        args.insert(args.begin(), {"zenity", std::string("--title=") + kTitle, "--width=460"});
        std::string ignored;
        return RunCapture(args, out ? out : &ignored);
    }

    std::string heading_;
    pid_t pid_ = 0;
    int fd_ = -1;
};

class KdialogUi : public TerminalUi {
public:
    KdialogUi() : TerminalUi(false) {}

    void Tell(const std::string& heading, const std::string& text, const char* kind) override
    {
        Run({std::strcmp(kind, "error") == 0 ? "--error" : "--msgbox", heading + "\n\n" + text}, nullptr);
    }

    int Choose(const std::string& heading, const std::string& text, const std::vector<std::string>& buttons) override
    {
        const std::string body = heading + "\n\n" + text;
        if (buttons.size() == 1) {
            return Run({"--yesno", body, "--yes-label=" + buttons[0], "--no-label=Cancel"}, nullptr) == 0 ? 0 : -1;
        }
        const int code = Run({"--yesnocancel", body, "--yes-label=" + buttons[0], "--no-label=" + buttons[1],
                              "--cancel-label=Cancel"},
                             nullptr);
        return code == 0 ? 0 : code == 1 ? 1 : -1;
    }

    std::optional<std::string> PickFolder(const std::string&, const std::string& start) override
    {
        const char* home = std::getenv("HOME");
        std::string out;
        if (Run({"--getexistingdirectory", start.empty() ? std::string(home ? home : "/") : start}, &out) != 0 || out.empty()) {
            return std::nullopt;
        }
        return out;
    }

private:
    int Run(std::vector<std::string> args, std::string* out)
    {
        args.insert(args.begin(), {"kdialog", std::string("--title=") + kTitle});
        std::string ignored;
        return RunCapture(args, out ? out : &ignored);
    }
};

// ----- the steps ---------------------------------------------------------------------

struct Options {
    std::string site = kSite;
    std::string game;
    std::vector<std::string> steamRoots;
    bool yes = false;
    bool terminal = false;
};

std::unique_ptr<Ui> MakeUi(const Options& o)
{
    if (!o.yes && !o.terminal && (std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY"))) {
        if (!Which("zenity").empty()) return std::make_unique<ZenityUi>();
        if (!Which("kdialog").empty()) return std::make_unique<KdialogUi>();
    }
    return std::make_unique<TerminalUi>(o.yes);
}

sfv::UpdateInfo GetInfo(Ui& ui, const std::string& site)
{
    for (;;) {
        try {
            return FetchInfo(site);
        }
        catch (const Failure& f) {
            if (ui.yes || ui.Choose("Polarity couldn't be downloaded", f.message, {"Try again"}) != 0) throw Cancelled{f.message};
        }
    }
}

std::string ChooseGame(Ui& ui, const Options& o, const sfv::UpdateInfo& info, const std::vector<std::string>& libs)
{
    std::optional<std::string> win64;
    if (!o.game.empty()) {
        win64 = GameWin64(o.game);
        if (!win64) throw Failure{"StreetFighterV.exe isn't in " + o.game + "."};
    }
    else {
        win64 = FindGame(libs);
    }
    if (ui.yes) {
        if (!win64) throw Failure{"Street Fighter V wasn't found. Pass its folder with --game."};
        return *win64;
    }
    for (;;) {
        int pick;
        if (win64) {
            pick = ui.Choose("Install Polarity " + info.version + " for Street Fighter V",
                             "Polarity goes into your Street Fighter V folder:\n" + GameRoot(*win64),
                             {"Install", "Choose another folder"});
            if (pick == 0) return *win64;
        }
        else {
            pick = ui.Choose("Where is Street Fighter V?",
                             "Steam doesn't list it here. Choose the folder it's installed in. In Steam, the game's gear "
                             "button, then Manage, Browse local files shows it.",
                             {"Choose folder"}) == 0
                       ? 1
                       : -1;
        }
        if (pick != 1) throw Cancelled{};
        const auto picked = ui.PickFolder("Choose your Street Fighter V folder", win64 ? GameRoot(*win64) : "");
        if (!picked) continue;
        if (auto found = GameWin64(*picked)) {
            win64 = found;
        }
        else {
            ui.Tell("That isn't a Street Fighter V folder",
                    "StreetFighterV.exe isn't in " + *picked + ". Choose the folder named StreetFighterV in your Steam library.",
                    "warning");
        }
    }
}

void WaitForGameClosed(Ui& ui)
{
    while (GameRunning()) {
        if (ui.yes || ui.Choose("Close Street Fighter V first", "The game is running. Close it, then choose Try again.",
                                {"Try again"}) != 0) {
            throw Cancelled{"Street Fighter V is running."};
        }
    }
}

int Install(Ui& ui, const std::string& site, const std::string& win64, const sfv::UpdateInfo& info)
{
    for (;;) {
        ui.ProgressStart("Installing Polarity " + info.version);
        try {
            const int changed = InstallFiles(win64, info, site, [&](uint64_t done, uint64_t total) {
                return ui.Progress(total ? static_cast<double>(done) / total : 1.0, Megabytes(done) + " of " + Megabytes(total));
            });
            ui.ProgressEnd();
            return changed;
        }
        catch (const Failure& f) {
            ui.ProgressEnd();
            if (ui.yes || ui.Choose("Polarity couldn't be installed", f.message, {"Try again"}) != 0) throw Cancelled{f.message};
        }
        catch (const Cancelled&) {
            ui.ProgressEnd();
            throw;
        }
    }
}

enum class LaunchOptions { Set, Had, Left };

LaunchOptions SetOptions(Ui& ui, const std::vector<std::string>& roots)
{
    const std::vector<std::string> paths = LocalConfigs(roots);
    if (paths.empty()) return LaunchOptions::Left;
    // A running Steam holds the launch options in memory: what its file says
    // may be older (an edit in Properties reaches it when Steam closes), and
    // Steam writes over the file then. So they are read and set only with
    // Steam closed.
    const bool wasRunning = SteamRunning();
    if (wasRunning) {
        if (ui.Choose("Steam needs to close for a moment",
                      "Polarity loads through Street Fighter V's launch options, which can only be checked and changed "
                      "while Steam is closed. The installer closes Steam, sets them and opens it again.",
                      {"Close Steam", "Skip"}) != 0) {
            // The file's word, then (likely right when nobody edited them since).
            return AccountsNeedingOptions(paths).empty() ? LaunchOptions::Had : LaunchOptions::Left;
        }
        CloseSteam(roots);
    }
    const std::vector<std::string> need = AccountsNeedingOptions(paths);
    const std::vector<std::string> failed = WriteLaunchOptions(need);
    if (wasRunning) OpenSteam(roots);
    if (!failed.empty()) return LaunchOptions::Left;
    return need.empty() ? LaunchOptions::Had : LaunchOptions::Set;
}

void Usage()
{
    std::cout << "polarity-installer: installs Polarity into Street Fighter V (Steam on Linux).\n"
                 "  --site URL         update info (default " << kSite << ")\n"
                 "  --game DIR         the game folder\n"
                 "  --steam-root DIR   a Steam folder to use (repeatable)\n"
                 "  --yes              no questions\n"
                 "  --terminal         questions in the terminal\n";
}

int Main(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                Usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--site") o.site = next();
        else if (a == "--game") o.game = next();
        else if (a == "--steam-root") o.steamRoots.push_back(next());
        else if (a == "--yes") o.yes = true;
        else if (a == "--terminal") o.terminal = true;
        else if (a == "--help" || a == "-h") {
            Usage();
            return 0;
        }
        else {
            Usage();
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    if (psa_crypto_init() != PSA_SUCCESS) {
        std::cerr << "Cannot start the crypto library.\n";
        return 1;
    }
    std::unique_ptr<Ui> ui = MakeUi(o);
    sfv::UpdateInfo info;
    int changed = 0;
    LaunchOptions options = LaunchOptions::Left;
    try {
        info = GetInfo(*ui, o.site);
        const std::vector<std::string> roots = SteamRoots(o.steamRoots);
        const std::string win64 = ChooseGame(*ui, o, info, SteamLibraries(roots));
        WaitForGameClosed(*ui);
        changed = Install(*ui, o.site, win64, info);
        options = SetOptions(*ui, roots);
    }
    catch (const Cancelled& c) {
        if (!c.message.empty()) ui->Tell("Polarity couldn't be installed", c.message, "error");
        return 1;
    }
    catch (const Failure& f) {
        ui->Tell("Polarity couldn't be installed", f.message, "error");
        return 1;
    }
    const bool fresh = changed > 0 || options == LaunchOptions::Set;
    const std::string heading = fresh ? "Polarity is installed" : "Polarity " + info.version + " is already installed";
    std::string text = fresh ? "Polarity " + info.version + " will run the next time you start Street Fighter V."
                             : "It runs whenever you start Street Fighter V.";
    if (options == LaunchOptions::Left) {
        text += std::string("\n\nOne step is left: in Steam, open Street Fighter V's Properties and set its launch "
                            "options to\n") +
                kLaunchHint;
    }
    ui->Tell(heading, text, "info");
    return 0;
}

} // namespace
} // namespace polarity::installer

int main(int argc, char** argv)
{
    return polarity::installer::Main(argc, argv);
}
