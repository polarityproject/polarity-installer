// The update info and the release's files, over HTTP(S) with mbedTLS.

#include "installer/linux_internal.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

namespace polarity::installer {

using namespace detail;

namespace {

struct Url {
    bool https = false;
    std::string host, port, path;
};

bool ParseUrl(const std::string& url, Url* out) {
    size_t rest;
    if (url.rfind("https://", 0) == 0) {
        out->https = true;
        rest = 8;
    } else if (url.rfind("http://", 0) == 0) {
        out->https = false;
        rest = 7;
    } else {
        return false;
    }
    const size_t slash = url.find('/', rest);
    std::string hostport = url.substr(rest, slash == std::string::npos ? std::string::npos : slash - rest);
    out->path = slash == std::string::npos ? "/" : url.substr(slash);
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
        out->host = hostport.substr(0, colon);
        out->port = hostport.substr(colon + 1);
    } else {
        out->host = hostport;
        out->port = out->https ? "443" : "80";
    }
    if (out->host.size() > 2 && out->host.front() == '[') out->host = out->host.substr(1, out->host.size() - 2);
    return !out->host.empty();
}

std::string TlsError(int code) {
    char text[200];
    mbedtls_strerror(code, text, sizeof(text));
    return text;
}

class Roots {
public:
    Roots() {
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
                Log("certificates: " + f);
                break;
            }
        }
        if (!loaded_) Log("certificates: none of the usual bundles; downloads are checked by signature and hashes only");
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
    Connection() {
        mbedtls_net_init(&net_);
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
    }

    ~Connection() {
        if (tls_) mbedtls_ssl_close_notify(&ssl_);
        mbedtls_net_free(&net_);
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }

    void Open(const Url& url) {
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
        } else {
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

    void Send(const std::string& data) {
        size_t done = 0;
        while (done < data.size()) {
            const auto* p = reinterpret_cast<const unsigned char*>(data.data()) + done;
            const int r = tls_ ? mbedtls_ssl_write(&ssl_, p, data.size() - done) : mbedtls_net_send(&net_, p, data.size() - done);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r <= 0) throw Failure{"The connection to the Polarity site broke off (" + TlsError(r) + ")."};
            done += static_cast<size_t>(r);
        }
    }

    size_t Read(unsigned char* buf, size_t size) {
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

std::vector<uint8_t> HttpGetOnce(std::string url, size_t maxBytes, const std::function<bool(uint64_t)>& progress);

std::vector<uint8_t> HttpGet(const std::string& url, size_t maxBytes, const std::function<bool(uint64_t)>& progress = nullptr) {
    Log("GET " + url);
    try {
        std::vector<uint8_t> body = HttpGetOnce(url, maxBytes, progress);
        Log("  " + std::to_string(body.size()) + " bytes");
        return body;
    } catch (const Failure& f) {
        Log("  failed: " + f.message);
        throw;
    } catch (const Cancelled&) {
        Log("  cancelled");
        throw;
    }
}

std::vector<uint8_t> HttpGetOnce(std::string url, size_t maxBytes, const std::function<bool(uint64_t)>& progress) {
    for (int redirects = 0; redirects < 6; redirects++) {
        Url u;
        if (!ParseUrl(url, &u)) throw Failure{"Not a web address: " + url};
        Connection c;
        c.Open(u);
        const bool defaultPort = u.port == (u.https ? "443" : "80");
        c.Send("GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + (defaultPort ? "" : ":" + u.port) +
               "\r\nUser-Agent: Polarity installer\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
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
            Log("  " + std::to_string(status) + " to " + url);
            continue;
        }
        if (status != 200) throw Failure{"The Polarity site answered " + std::to_string(status) + " for " + url + "."};

        std::string te = headers["transfer-encoding"];
        std::transform(te.begin(), te.end(), te.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const bool chunked = te.find("chunked") != std::string::npos;
        const bool hasLength = !chunked && headers.count("content-length");
        const uint64_t length = hasLength ? std::strtoull(headers["content-length"].c_str(), nullptr, 10) : 0;
        if (hasLength && length > maxBytes) throw Failure{url + " is larger than the update info says."};
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

} // namespace

namespace detail {

sfv::UpdateInfo FetchInfo(const std::string& site) {
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
    Log("update info: Polarity " + info.version + " (" + info.published + "), " + std::to_string(info.files.size()) +
        " files, signature good");
    return info;
}

} // namespace detail

namespace {

bool SameFile(const std::string& path, const sfv::UpdateFile& f) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || static_cast<uint64_t>(st.st_size) != f.size) return false;
    const auto data = ReadFile(path, f.size + 1);
    return data && data->size() == f.size &&
           sfv::Sha256Hex(reinterpret_cast<const uint8_t*>(data->data()), data->size()) == f.sha256;
}

} // namespace

namespace detail {

int InstallFiles(const std::string& win64, const sfv::UpdateInfo& info, const std::string& site, const std::function<bool(uint64_t, uint64_t)>& progress) {
    struct Item {
        const sfv::UpdateFile* f;
        std::string target, fresh, old;
        bool hadOld = false;
    };

    std::vector<Item> items;
    uint64_t total = 0;
    for (const sfv::UpdateFile& f : info.files) {
        const std::string target = win64 + "/" + sfv::UpdateFileOnDisk(f.path);
        if (SameFile(target, f)) {
            Log(f.path + ": up to date");
            continue;
        }
        Log(f.path + (Exists(target) ? ": differs, to replace" : ": missing, to add"));
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
                Log(f.path + ": got " + std::to_string(data.size()) + " bytes, sha256 " +
                    sfv::Sha256Hex(data.data(), data.size()) + "; expected " + std::to_string(f.size) + ", " + f.sha256);
                throw Failure{f.path + " from the Polarity site does not match the update info. Nothing was changed."};
            }
            MakeDirs(Dirname(it.target));
            if (!WriteFile(it.fresh, data.data(), data.size())) {
                throw Failure{"Cannot write into " + win64 + " (" + std::strerror(errno) + ")."};
            }
            done += f.size;
            progress(done, total);
        }
    } catch (...) {
        Log("removing the downloaded files; the game folder is unchanged");
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
            Log("cannot replace " + it.target + " (" + why + "); putting back the " + std::to_string(swapped.size()) +
                " files already replaced");
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
    Log("installed " + std::to_string(items.size()) + " files into " + win64);
    return static_cast<int>(items.size());
}

} // namespace detail

} // namespace polarity::installer
