#include "core/update_info.h"

#include "core/json.h"

#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#else
// The Linux installer (src/installer/installer_linux.cpp) builds this with
// mbedTLS for SHA-256 and the signature check.
#include <mbedtls/ecdsa.h>
#include <mbedtls/sha256.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace sfv {

namespace {

// "0.1.0-prototype" -> {0, 1, 0}, "prototype".
void SplitVersion(const std::string& v, std::vector<long>* numbers, std::string* suffix)
{
    const size_t dash = v.find('-');
    const std::string core = v.substr(0, dash);
    *suffix = dash == std::string::npos ? "" : v.substr(dash + 1);
    size_t at = 0;
    while (at <= core.size()) {
        const size_t dot = core.find('.', at);
        numbers->push_back(std::strtol(core.substr(at, dot - at).c_str(), nullptr, 10));
        if (dot == std::string::npos) break;
        at = dot + 1;
    }
}

// A file name without folders or "..", short, of one of these types.
bool PlainFileName(const std::string& name, std::initializer_list<const char*> types)
{
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    if (name.find("..") != std::string::npos || name[0] == '.') return false;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::any_of(types.begin(), types.end(), [&](const char* ext) {
        const size_t n = std::strlen(ext);
        return lower.size() > n && lower.compare(lower.size() - n, n, ext) == 0;
    });
}

} // namespace

bool ParseUpdateInfo(std::string_view json, UpdateInfo* out, std::string* error)
{
    JsonValue doc;
    if (!ParseJson(json, &doc, error)) return false;
    auto fail = [&](const std::string& what) {
        if (error) *error = what;
        return false;
    };
    if (doc.type != JsonValue::Type::Object) return fail("not a JSON object");
    UpdateInfo info;
    info.product = doc.Str("product");
    info.version = doc.Str("version");
    info.published = doc.Str("published");
    info.notes = doc.Str("notes");
    info.download = doc.Str("download");
    // "SFVR": the old site's update info (SFVR's updaters accept only that),
    // frozen at the first Polarity release.
    if (info.product != "Polarity" && info.product != "SFVR") return fail("not a Polarity update file");
    if (info.version.empty()) return fail("no version");
    const JsonValue* files = doc.Find("files");
    if (!files || files->type != JsonValue::Type::Array || files->array.empty()) return fail("no files");
    const JsonValue* more = doc.Find("more_files");
    if (more && more->type != JsonValue::Type::Array) return fail("more_files is not a list");
    std::vector<std::pair<const JsonValue*, bool>> all;
    for (const JsonValue& f : files->array) all.push_back({&f, false});
    if (more)
        for (const JsonValue& f : more->array) all.push_back({&f, true});
    for (const auto& [one, isMore] : all) {
        const JsonValue& f = *one;
        UpdateFile file;
        file.more = isMore;
        file.path = f.Str("path");
        file.url = f.Str("url");
        file.sha256 = f.Str("sha256");
        const double size = f.Num("size", -1);
        if (!SafeUpdatePath(file.path)) return fail("a file the updater may not write: " + file.path);
        if (file.url.empty()) return fail("no url for " + file.path);
        if (file.sha256.size() != 64) return fail("no sha256 for " + file.path);
        std::transform(file.sha256.begin(), file.sha256.end(), file.sha256.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (size < 0 || size > 256.0 * 1024 * 1024) return fail("bad size for " + file.path);
        file.size = static_cast<uint64_t>(size);
        info.files.push_back(std::move(file));
    }
    *out = std::move(info);
    return true;
}

int CompareVersions(const std::string& a, const std::string& b)
{
    std::vector<long> na, nb;
    std::string sa, sb;
    SplitVersion(a, &na, &sa);
    SplitVersion(b, &nb, &sb);
    for (size_t i = 0; i < std::max(na.size(), nb.size()); i++) {
        const long x = i < na.size() ? na[i] : 0, y = i < nb.size() ? nb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (sa.empty() != sb.empty()) return sa.empty() ? 1 : -1;
    // The suffixes part by part (the semver rule): numbers as numbers, below
    // any text; text as text; a suffix that runs out first is the lower.
    std::vector<std::string> pa, pb;
    auto parts = [](const std::string& s, std::vector<std::string>* out) {
        size_t from = 0;
        while (true) {
            const size_t dot = s.find('.', from);
            out->push_back(s.substr(from, dot - from));
            if (dot == std::string::npos) break;
            from = dot + 1;
        }
    };
    parts(sa, &pa);
    parts(sb, &pb);
    auto number = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    };
    for (size_t i = 0; i < std::min(pa.size(), pb.size()); i++) {
        const bool xn = number(pa[i]), yn = number(pb[i]);
        if (xn && yn) {
            const unsigned long long x = std::strtoull(pa[i].c_str(), nullptr, 10), y = std::strtoull(pb[i].c_str(), nullptr, 10);
            if (x != y) return x < y ? -1 : 1;
        }
        else if (xn != yn) {
            return xn ? -1 : 1;
        }
        else if (const int c = pa[i].compare(pb[i]); c != 0) {
            return c < 0 ? -1 : 1;
        }
    }
    return pa.size() == pb.size() ? 0 : pa.size() < pb.size() ? -1 : 1;
}

bool SafeUpdatePath(const std::string& path)
{
    // The one folder: the bundled fonts, .ttc only (SFVR/fonts/ in the old
    // site's update info; both land in Polarity\fonts, UpdateFileOnDisk).
    for (std::string_view fonts : {std::string_view(kUpdateFontsDir), std::string_view("SFVR/fonts/")}) {
        if (path.compare(0, fonts.size(), fonts) == 0) return PlainFileName(path.substr(fonts.size()), {".ttc"});
    }
    return PlainFileName(path, {".dll", ".pdb", ".txt"});
}

std::string UpdateFileOnDisk(const std::string& path)
{
    std::string out = path;
    constexpr std::string_view kOld = "SFVR/fonts/";
    if (out.compare(0, kOld.size(), kOld) == 0) out = kUpdateFontsDir + out.substr(kOld.size());
#ifdef _WIN32
    for (char& c : out) {
        if (c == '/') c = '\\';
    }
#endif
    return out;
}

std::string ResolveUrl(const std::string& base, const std::string& url)
{
    if (url.find("://") != std::string::npos) return url;
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) return url;
    if (!url.empty() && url[0] == '/') {
        const size_t host = base.find('/', scheme + 3);
        return base.substr(0, host) + url;
    }
    const size_t slash = base.find_last_of('/');
    return (slash == std::string::npos || slash < scheme + 3 ? base + "/" : base.substr(0, slash + 1)) + url;
}

std::string Sha256Hex(const uint8_t* data, size_t size)
{
    uint8_t digest[32] = {};
#ifdef _WIN32
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(data), static_cast<ULONG>(size), digest,
               sizeof(digest));
#else
    mbedtls_sha256(data, size, digest, 0);
#endif
    std::string hex;
    char two[3];
    for (uint8_t b : digest) {
        std::snprintf(two, sizeof(two), "%02x", b);
        hex += two;
    }
    return hex;
}

std::vector<uint8_t> Base64Decode(std::string_view text)
{
    std::vector<uint8_t> out;
    uint32_t bits = 0;
    int count = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue; // padding, line breaks
        bits = bits << 6 | uint32_t(v);
        if ((count += 6) >= 8) {
            count -= 8;
            out.push_back(static_cast<uint8_t>(bits >> count));
        }
    }
    return out;
}

bool VerifySignature(const uint8_t* data, size_t size, const uint8_t* signature, size_t signatureSize,
                     const uint8_t (&publicKey)[64])
{
    if (signatureSize != 64) return false;
    bool anyKey = false;
    for (uint8_t b : publicKey) anyKey |= b != 0;
    if (!anyKey) return false;
#ifndef _WIN32
    uint8_t digest[32] = {};
    mbedtls_sha256(data, size, digest, 0);
    uint8_t point[65] = {0x04}; // uncompressed: 04, x, y
    std::memcpy(point + 1, publicKey, 64);
    mbedtls_ecp_group group;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    const bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
                    mbedtls_ecp_point_read_binary(&group, &q, point, sizeof(point)) == 0 &&
                    mbedtls_ecp_check_pubkey(&group, &q) == 0 && mbedtls_mpi_read_binary(&r, signature, 32) == 0 &&
                    mbedtls_mpi_read_binary(&s, signature + 32, 32) == 0 &&
                    mbedtls_ecdsa_verify(&group, digest, sizeof(digest), &q, &r, &s) == 0;
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&group);
    return ok;
#else
    // BCRYPT_ECCKEY_BLOB, then x and y.
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), publicKey, 64);
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob,
                                            sizeof(blob), 0))) {
        return false;
    }
    uint8_t digest[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(data), static_cast<ULONG>(size), digest,
               sizeof(digest));
    const NTSTATUS status =
        BCryptVerifySignature(key, nullptr, digest, sizeof(digest), const_cast<uint8_t*>(signature), 64, 0);
    BCryptDestroyKey(key);
    return BCRYPT_SUCCESS(status);
#endif
}

} // namespace sfv
