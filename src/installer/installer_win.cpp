// polarity-installer.exe: installs the latest Polarity release on Windows (README.md).

#include "core/update_info.h"
#include "installer/install_log.h"
#include "installer/version.h"
#include "mod/update_key.h"

#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace instlog = polarity::installer::log;

namespace {

constexpr const char* kDefaultSite = "https://polaritymod.link/update.json";
constexpr const wchar_t* kTitle = L"Polarity Installer";
constexpr const char* kAppId = "310950";
constexpr DWORD kTimeoutMs = 15000;
constexpr size_t kMaxInfoBytes = 64 * 1024;
constexpr DWORD kHttpOk = 200;
constexpr int kProgressSteps = 1000;
constexpr int kDialogWidth = 260;

enum ButtonId { kInstall = 100,
                kChoose,
                kRetry,
                kAdmin };

struct Options {
    std::string site = kDefaultSite;
    std::wstring game;
    bool yes = false;
    bool elevated = false;
    bool find = false;
    std::string testCrash;
};

Options g_opt;

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

std::wstring Megabytes(uint64_t bytes) {
    wchar_t text[32];
    swprintf(text, 32, L"%.1f MB", bytes / (1024.0 * 1024.0));
    return text;
}

void Print(const std::wstring& text) {
    const std::string s = Utf8(text) + "\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
}

void Log(const std::string& line) {
    instlog::Write(line);
}

void Log(const std::wstring& line) {
    instlog::Write(Utf8(line));
}

std::wstring LogNote() {
    return !g_opt.yes || instlog::Path().empty() ? L"" : L"\n\nDetails are in the installer's log:\n" + Wide(instlog::Path());
}

void ShowLogFile() {
    const std::wstring args = L"/select,\"" + Wide(instlog::Path()) + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

bool IsFile(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsDir(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::optional<std::string> ReadFileText(const std::wstring& path, size_t maxBytes = 4 * 1024 * 1024) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return std::nullopt;
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && data.size() < maxBytes) data.append(buf, n);
    std::fclose(f);
    return data;
}

void MakeDirs(const std::wstring& dir) {
    if (dir.empty() || IsDir(dir)) return;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) MakeDirs(dir.substr(0, slash));
    CreateDirectoryW(dir.c_str(), nullptr);
}

std::wstring Trimmed(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

std::optional<std::wstring> GameWin64(const std::wstring& picked) {
    const std::wstring p = Trimmed(picked);
    for (const std::wstring& c : {p + L"\\StreetFighterV\\Binaries\\Win64", p + L"\\Binaries\\Win64", p + L"\\Win64", p}) {
        const std::wstring lower = Lower(c);
        const bool win64 = lower.size() >= 15 && lower.compare(lower.size() - 15, 15, L"\\binaries\\win64") == 0;
        if (win64 && IsFile(c + L"\\StreetFighterV.exe")) return c;
    }
    return std::nullopt;
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t value[MAX_PATH * 2] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
    return value;
}

std::string VdfUnescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) out += s[++i];
        else out += s[i];
    }
    return out;
}

std::vector<std::wstring> SteamLibraries() {
    std::vector<std::wstring> roots;
    for (const std::wstring& r : {RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
                                  RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
                                  RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath")}) {
        if (!r.empty()) roots.push_back(Trimmed(r));
    }
    std::wstring list;
    for (const std::wstring& r : roots) list += L" " + r;
    Log(L"Steam folders (registry):" + (list.empty() ? std::wstring(L" none") : list));
    std::vector<std::wstring> libs;
    auto add = [&](const std::wstring& lib) {
        const std::wstring t = Trimmed(lib);
        for (const std::wstring& seen : libs) {
            if (Lower(seen) == Lower(t)) return;
        }
        libs.push_back(t);
    };
    for (const std::wstring& root : roots) {
        add(root);
        const auto text = ReadFileText(root + L"\\steamapps\\libraryfolders.vdf");
        if (!text) continue;
        static const std::regex path(R"re("path"\s+"((?:[^"\\]|\\.)*)")re", std::regex::icase);
        for (auto it = std::sregex_iterator(text->begin(), text->end(), path); it != std::sregex_iterator(); ++it) {
            add(Wide(VdfUnescape((*it)[1].str())));
        }
    }
    return libs;
}

std::optional<std::wstring> FindGame() {
    for (const std::wstring& lib : SteamLibraries()) {
        std::wstring folder = L"StreetFighterV";
        const auto acf = ReadFileText(lib + L"\\steamapps\\appmanifest_" + Wide(kAppId) + L".acf");
        if (acf) {
            static const std::regex dir(R"re("installdir"\s+"((?:[^"\\]|\\.)*)")re", std::regex::icase);
            std::smatch m;
            if (std::regex_search(*acf, m, dir)) folder = Wide(VdfUnescape(m[1].str()));
        }
        auto win64 = GameWin64(lib + L"\\steamapps\\common\\" + folder);
        Log(L"library " + lib + L": " + (acf ? L"lists the game" : L"doesn't list the game") + L", " +
            (win64 ? std::wstring(L"StreetFighterV.exe found") : L"no StreetFighterV.exe in steamapps\\common\\" + folder));
        if (win64) return win64;
    }
    return std::nullopt;
}

std::wstring GameRoot(const std::wstring& win64) {
    std::wstring p = win64;
    for (int i = 0; i < 3; i++) {
        const size_t slash = p.find_last_of(L'\\');
        if (slash == std::wstring::npos) break;
        p = p.substr(0, slash);
    }
    return p;
}

bool GameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
        found = _wcsicmp(e.szExeFile, L"StreetFighterV.exe") == 0;
    }
    CloseHandle(snap);
    return found;
}

std::atomic<bool> g_cancel{false};

bool HttpGetOnce(const std::string& url, size_t maxBytes, std::vector<uint8_t>* out, std::wstring* error,
                 const std::function<void(uint64_t)>& progress);

bool HttpGet(const std::string& url, size_t maxBytes, std::vector<uint8_t>* out, std::wstring* error,
             const std::function<void(uint64_t)>& progress = nullptr) {
    Log("GET " + url);
    const bool ok = HttpGetOnce(url, maxBytes, out, error, progress);
    Log(ok ? "  " + std::to_string(out->size()) + " bytes" : "  failed: " + Utf8(*error));
    return ok;
}

bool HttpGetOnce(const std::string& url, size_t maxBytes, std::vector<uint8_t>* out, std::wstring* error,
                 const std::function<void(uint64_t)>& progress) {
    const std::wstring wurl = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        *error = L"Bad address: " + wurl;
        return false;
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET session = WinHttpOpen(L"Polarity installer", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    bool ok = false;
    if (request) {
        WinHttpSetTimeouts(request, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            if (status != kHttpOk) {
                *error = L"The Polarity site answered " + std::to_wstring(status) + L" for " + wurl + L".";
            } else {
                out->clear();
                ok = true;
                for (;;) {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(request, &available)) {
                        ok = false;
                        *error = L"The download of " + wurl + L" broke off.";
                        break;
                    }
                    if (available == 0) break;
                    if (out->size() + available > maxBytes) {
                        ok = false;
                        *error = wurl + L" is larger than the update info says.";
                        break;
                    }
                    const size_t at = out->size();
                    out->resize(at + available);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, out->data() + at, available, &read)) {
                        ok = false;
                        *error = L"The download of " + wurl + L" broke off.";
                        break;
                    }
                    out->resize(at + read);
                    if (progress) progress(out->size());
                    if (g_cancel) {
                        ok = false;
                        *error = L"Cancelled.";
                        break;
                    }
                }
            }
        } else {
            *error = L"Cannot reach the Polarity site (error " + std::to_wstring(GetLastError()) +
                     L"). Check your internet connection and try again.";
        }
    } else {
        *error = L"Cannot open " + wurl + L".";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

bool FetchInfo(sfv::UpdateInfo* info, std::wstring* error) {
    std::vector<uint8_t> body, sig;
    if (!HttpGet(g_opt.site, kMaxInfoBytes, &body, error) || !HttpGet(g_opt.site + ".sig", 4096, &sig, error)) return false;
    const std::vector<uint8_t> signature =
        sfv::Base64Decode(std::string_view(reinterpret_cast<const char*>(sig.data()), sig.size()));
    if (!sfv::VerifySignature(body.data(), body.size(), signature.data(), signature.size(), sfv::mod::kUpdatePublicKey)) {
        *error = L"The update info on the Polarity site is not signed with the release key, so nothing was installed.";
        return false;
    }
    std::string why;
    if (!sfv::ParseUpdateInfo(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), info, &why)) {
        *error = L"Cannot read the update info: " + Wide(why);
        return false;
    }
    for (const sfv::UpdateFile& f : info->files) {
        if (!sfv::SafeUpdatePath(f.path)) {
            *error = L"The update info lists a file the installer may not write: " + Wide(f.path);
            return false;
        }
    }
    Log("update info: Polarity " + info->version + " (" + info->published + "), " + std::to_string(info->files.size()) +
        " files, signature good");
    return true;
}

bool SameFile(const std::wstring& path, const sfv::UpdateFile& f) {
    FILE* in = _wfopen(path.c_str(), L"rb");
    if (!in) return false;
    std::vector<uint8_t> data(static_cast<size_t>(f.size) + 1);
    const size_t got = std::fread(data.data(), 1, data.size(), in);
    std::fclose(in);
    return got == f.size && sfv::Sha256Hex(data.data(), got) == f.sha256;
}

struct Progress {
    std::atomic<uint64_t> done{0}, total{0};
    std::atomic<bool> finished{false};
};

struct InstallResult {
    bool ok = false;
    bool upToDate = false;
    bool accessDenied = false;
    std::wstring error;
};

InstallResult Install(const std::wstring& win64, const sfv::UpdateInfo& info, Progress* progress) {
    InstallResult r;

    struct Item {
        const sfv::UpdateFile* file;
        std::wstring target, fresh, old;
        bool hadOld = false;
    };

    std::vector<Item> items;
    for (const sfv::UpdateFile& f : info.files) {
        const std::wstring target = win64 + L"\\" + Wide(sfv::UpdateFileOnDisk(f.path));
        if (SameFile(target, f)) {
            Log(f.path + ": up to date");
            continue;
        }
        Log(f.path + (IsFile(target) ? ": differs, to replace" : ": missing, to add"));
        items.push_back({&f, target, target + L".polarity-new", target + L".polarity-old"});
        progress->total += f.size;
    }
    if (items.empty()) {
        r.ok = r.upToDate = true;
        return r;
    }
    auto cleanup = [&] {
        if (!r.ok) Log(L"failed: " + r.error + L" (the game folder is unchanged)");
        for (const Item& it : items) DeleteFileW(it.fresh.c_str());
    };
    uint64_t before = 0;
    for (Item& it : items) {
        const sfv::UpdateFile& f = *it.file;
        std::vector<uint8_t> data;
        const std::string url = sfv::ResolveUrl(g_opt.site, f.url);
        if (!HttpGet(url, static_cast<size_t>(f.size) + 1, &data, &r.error, [&](uint64_t n) { progress->done = before + n; })) {
            cleanup();
            return r;
        }
        if (data.size() != f.size || sfv::Sha256Hex(data.data(), data.size()) != f.sha256) {
            Log(f.path + ": got " + std::to_string(data.size()) + " bytes, sha256 " + sfv::Sha256Hex(data.data(), data.size()) +
                "; expected " + std::to_string(f.size) + ", " + f.sha256);
            r.error = Wide(f.path) + L" from the Polarity site does not match the update info. Nothing was changed.";
            cleanup();
            return r;
        }
        MakeDirs(it.target.substr(0, it.target.find_last_of(L'\\')));
        HANDLE h = CreateFileW(it.fresh.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                        written == data.size();
        const DWORD err = GetLastError();
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!ok) {
            r.accessDenied = err == ERROR_ACCESS_DENIED;
            r.error = L"Cannot write into " + win64 + L" (error " + std::to_wstring(err) + L").";
            cleanup();
            return r;
        }
        before += f.size;
        progress->done = before;
    }
    std::vector<Item*> swapped;
    for (Item& it : items) {
        it.hadOld = IsFile(it.target);
        DeleteFileW(it.old.c_str());
        bool ok = !it.hadOld || MoveFileExW(it.target.c_str(), it.old.c_str(), 0);
        if (ok) {
            ok = MoveFileExW(it.fresh.c_str(), it.target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!ok && it.hadOld) MoveFileExW(it.old.c_str(), it.target.c_str(), 0);
        }
        if (!ok) {
            const DWORD err = GetLastError();
            r.accessDenied = err == ERROR_ACCESS_DENIED;
            r.error = L"Cannot replace " + it.target + L" (error " + std::to_wstring(err) +
                      L"). If Street Fighter V is open, close it and try again.";
            Log("putting back the " + std::to_string(swapped.size()) + " files already replaced");
            for (auto p = swapped.rbegin(); p != swapped.rend(); ++p) {
                DeleteFileW((*p)->target.c_str());
                if ((*p)->hadOld) MoveFileExW((*p)->old.c_str(), (*p)->target.c_str(), 0);
            }
            cleanup();
            return r;
        }
        swapped.push_back(&it);
    }
    for (const Item& it : items) {
        if (it.hadOld) DeleteFileW(it.old.c_str());
    }
    Log(L"installed " + std::to_wstring(items.size()) + L" files into " + win64);
    r.ok = true;
    return r;
}

struct Button {
    int id;
    const wchar_t* text;
};

HRESULT CALLBACK AskCallback(HWND, UINT msg, WPARAM, LPARAM, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED) {
        Log("log link clicked");
        ShowLogFile();
    }
    return S_OK;
}

int Ask(const std::wstring& heading, const std::wstring& text, const std::vector<Button>& buttons,
        PCWSTR icon = nullptr, TASKDIALOG_COMMON_BUTTON_FLAGS common = TDCBF_CANCEL_BUTTON) {
    std::vector<TASKDIALOG_BUTTON> b;
    for (const Button& x : buttons) b.push_back({x.id, x.text});
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof(c);
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    c.pszWindowTitle = kTitle;
    c.pszMainIcon = icon;
    c.pszMainInstruction = heading.c_str();
    c.pszContent = text.c_str();
    c.cButtons = static_cast<UINT>(b.size());
    c.pButtons = b.empty() ? nullptr : b.data();
    c.nDefaultButton = buttons.empty() ? 0 : buttons.front().id;
    c.dwCommonButtons = common;
    if (icon == TD_ERROR_ICON && !instlog::Path().empty()) {
        c.dwFlags |= TDF_ENABLE_HYPERLINKS;
        c.pszFooter = L"<a href=\"log\">Show the installer's log</a>";
        c.pfCallback = AskCallback;
    }
    int pressed = IDCANCEL;
    Log(std::string(buttons.empty() ? "shown: " : "asked: ") + Utf8(heading) + " / " + instlog::OneLine(Utf8(text)));
    TaskDialogIndirect(&c, &pressed, nullptr, nullptr);
    if (!buttons.empty()) {
        std::wstring answer = pressed == IDCANCEL ? L"Cancel" : L"?";
        for (const Button& x : buttons) {
            if (x.id == pressed) answer = x.text;
        }
        Log(L"  answer: " + answer);
    }
    return pressed;
}

void Tell(const std::wstring& heading, const std::wstring& text, PCWSTR icon = nullptr) {
    if (g_opt.yes) {
        Log("shown: " + Utf8(heading) + " / " + instlog::OneLine(Utf8(text)));
        Print(heading + L"\n" + text);
        return;
    }
    Ask(heading, text, {}, icon, TDCBF_OK_BUTTON);
}

struct WorkDialog {
    const std::wstring* heading;
    Progress* progress;
    std::wstring shown;
};

HRESULT CALLBACK WorkCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM, LONG_PTR ref) {
    auto* w = reinterpret_cast<WorkDialog*>(ref);
    switch (msg) {
        case TDN_CREATED:
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, kProgressSteps));
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);
            break;
        case TDN_TIMER: {
            if (w->progress->finished) {
                SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
                break;
            }
            const uint64_t total = w->progress->total, done = w->progress->done;
            if (total > 0) {
                SendMessageW(hwnd, TDM_SET_MARQUEE_PROGRESS_BAR, FALSE, 0);
                SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, static_cast<WPARAM>(done * kProgressSteps / total), 0);
                const std::wstring text = Megabytes(done) + L" of " + Megabytes(total);
                if (text != w->shown) {
                    w->shown = text;
                    SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(w->shown.c_str()));
                }
            }
            break;
        }
        case TDN_BUTTON_CLICKED:
            if (wParam == IDCANCEL && !w->progress->finished) g_cancel = true;
            break;
    }
    return S_OK;
}

void RunWithProgress(const std::wstring& heading, const std::function<void(Progress*)>& work) {
    Progress progress;
    std::thread t([&] {
        work(&progress);
        progress.finished = true;
    });
    if (!g_opt.yes) {
        WorkDialog w{&heading, &progress, L" "};
        TASKDIALOGCONFIG c{};
        c.cbSize = sizeof(c);
        c.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.pszWindowTitle = kTitle;
        c.pszMainInstruction = heading.c_str();
        c.pszContent = L" ";
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pfCallback = WorkCallback;
        c.lpCallbackData = reinterpret_cast<LONG_PTR>(&w);
        c.cxWidth = kDialogWidth;
        TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
    }
    t.join();
}

std::optional<std::wstring> PickFolder(const std::wstring& start) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return std::nullopt;
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose your Street Fighter V folder");
    if (!start.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dialog->SetFolder(item);
            item->Release();
        }
    }
    std::optional<std::wstring> out;
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    Log(out ? L"folder picked: " + *out : std::wstring(L"folder picker cancelled"));
    return out;
}

std::optional<std::wstring> ChooseGame(const sfv::UpdateInfo& info) {
    std::optional<std::wstring> win64;
    if (!g_opt.game.empty()) {
        win64 = GameWin64(g_opt.game);
        if (!win64) {
            Tell(L"That isn't a Street Fighter V folder", L"StreetFighterV.exe isn't in " + g_opt.game + L".", TD_ERROR_ICON);
            if (g_opt.yes) return std::nullopt;
        }
    }
    if (!win64) win64 = FindGame();
    if (g_opt.yes || g_opt.elevated) {
        if (!win64) Tell(L"Street Fighter V wasn't found", L"Pass its folder with --game.", TD_ERROR_ICON);
        return win64;
    }
    const std::wstring version = Wide(info.version);
    for (;;) {
        int pressed;
        if (win64) {
            pressed = Ask(L"Install Polarity " + version + L" for Street Fighter V",
                          L"Polarity goes into your Street Fighter V folder:\n" + GameRoot(*win64),
                          {{kInstall, L"Install"}, {kChoose, L"Choose another folder"}});
            if (pressed == kInstall) return win64;
        } else {
            pressed = Ask(L"Where is Street Fighter V?",
                          L"Steam doesn't list it on this PC. Choose the folder it's installed in. In Steam, right-click "
                          L"the game, then Manage, Browse local files shows it.",
                          {{kChoose, L"Choose folder"}});
        }
        if (pressed != kChoose) return std::nullopt;
        const auto picked = PickFolder(win64 ? GameRoot(*win64) : L"");
        if (!picked) continue;
        if (auto found = GameWin64(*picked)) {
            win64 = found;
            continue;
        }
        Ask(L"That isn't a Street Fighter V folder",
            L"StreetFighterV.exe isn't in " + *picked + L". Choose the folder named StreetFighterV in your Steam library.",
            {}, TD_WARNING_ICON, TDCBF_OK_BUTTON);
    }
}

bool RelaunchElevated(const std::wstring& win64) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring args = L"--elevated --game \"" + win64 + L"\"";
    if (g_opt.site != kDefaultSite) args += L" --site \"" + Wide(g_opt.site) + L"\"";
    SHELLEXECUTEINFOW e{};
    e.cbSize = sizeof(e);
    e.lpVerb = L"runas";
    e.lpFile = exe;
    e.lpParameters = args.c_str();
    e.nShow = SW_SHOWNORMAL;
    const bool ok = ShellExecuteExW(&e) != FALSE;
    Log(ok ? std::string("started the installer again as administrator")
           : "cannot start the installer as administrator (error " + std::to_string(GetLastError()) + ")");
    return ok;
}

void ParseArgs() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        const std::wstring a = argv[i];
        if (a == L"--site" && i + 1 < argc) g_opt.site = Utf8(argv[++i]);
        else if (a == L"--game" && i + 1 < argc) g_opt.game = argv[++i];
        else if (a == L"--yes") g_opt.yes = true;
        else if (a == L"--elevated") g_opt.elevated = true;
        else if (a == L"--find") g_opt.find = true;
        else if (a == L"--test-crash" && i + 1 < argc) g_opt.testCrash = Utf8(argv[++i]);
    }
    LocalFree(argv);
}

std::wstring g_crashText;

void PrepareCrashText() {
    g_crashText = instlog::Path().empty() ? L"It couldn't write a log."
                                          : L"Its log is at\n" + Wide(instlog::Path()) + L"\n\nSend that file to the Polarity team.";
}

void ShowCrashWindow() {
    if (g_opt.yes) return;
    const std::wstring text = L"Polarity Installer stopped unexpectedly\n\n" + g_crashText;
    MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}

void RawText(const char* s) {
    instlog::WriteRaw(s, std::strlen(s));
}

void RawWhere(DWORD64 address) {
    char line[MAX_PATH + 48];
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) &&
        GetModuleFileNameA(module, name, MAX_PATH)) {
        const char* slash = std::strrchr(name, '\\');
        std::snprintf(line, sizeof(line), "%s+0x%llx", slash ? slash + 1 : name,
                      static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    } else {
        std::snprintf(line, sizeof(line), "0x%llx", static_cast<unsigned long long>(address));
    }
    RawText(line);
}

void RawStack(CONTEXT context) {
    RawText("  stack:");
    for (int i = 0; i < 32 && context.Rip != 0; i++) {
        RawText(" ");
        RawWhere(i == 0 ? context.Rip : context.Rip - 1);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (fn) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, fn, &context, &handlerData, &establisher, nullptr);
        } else {
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
        }
    }
    RawText("\r\n");
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    char line[160];
    std::snprintf(line, sizeof(line), "crash: exception 0x%08lX at ", static_cast<unsigned long>(r->ExceptionCode));
    RawText(line);
    RawWhere(reinterpret_cast<DWORD64>(r->ExceptionAddress));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        std::snprintf(line, sizeof(line), ", %s 0x%llx",
                      r->ExceptionInformation[0] == 0 ? "reading" : r->ExceptionInformation[0] == 1 ? "writing"
                                                                                                    : "running",
                      static_cast<unsigned long long>(r->ExceptionInformation[1]));
        RawText(line);
    }
    RawText("\r\n");
    RawStack(*ep->ContextRecord);
    ShowCrashWindow();
    return EXCEPTION_EXECUTE_HANDLER;
}

void OnTerminate() {
    std::string what = "std::terminate without an exception";
    if (const std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            what = std::string("uncaught exception: ") + x.what();
        } catch (...) {
            what = "uncaught exception of an unknown type";
        }
    }
    Log(what);
    CONTEXT context{};
    RtlCaptureContext(&context);
    RawStack(context);
    ShowCrashWindow();
    _exit(3);
}

void InstallCrashHandlers() {
    SetUnhandledExceptionFilter(OnCrash);
    std::set_terminate(OnTerminate);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

void OpenLog() {
    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) instlog::SetHome(Utf8(profile));
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    instlog::Open({Utf8(dir), Utf8(temp)});
}

void LogStart() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char when[32];
    std::snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    instlog::WriteRaw("\r\n", 2);
    Log("=== Polarity Installer " POLARITY_INSTALLER_VERSION " (commit " POLARITY_INSTALLER_COMMIT "), Windows, " +
        std::string(when));
    const std::wstring command = GetCommandLineW();
    const size_t args = command[0] == L'"' ? command.find(L'"', 1) + 1 : command.find(L' ');
    std::wstring rest = args == std::wstring::npos || args >= command.size() ? L"" : command.substr(args);
    rest.erase(0, rest.find_first_not_of(L' '));
    Log(L"command: " + (rest.empty() ? std::wstring(L"(none)") : rest));
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) get(&v);
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &admin);
        FreeSid(group);
    }
    Log("system: Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + " build " +
        std::to_string(v.dwBuildNumber) + (admin ? ", as administrator" : ""));
}

__declspec(noinline) void TestCrash(const std::string& kind) {
    Log("crashing on purpose: " + kind);
    if (kind == "segv") {
        volatile uintptr_t address = 8;
        *reinterpret_cast<volatile int*>(address) = 1;
    } else if (kind == "throw") {
        throw std::runtime_error("a test exception");
    } else if (kind == "terminate") {
        std::terminate();
    }
}

int Run() {
    if (g_opt.find) {
        const auto win64 = FindGame();
        if (win64) Print(*win64);
        return win64 ? 0 : 1;
    }
    if (!g_opt.testCrash.empty()) TestCrash(g_opt.testCrash);

    sfv::UpdateInfo info;
    std::wstring error;
    for (;;) {
        bool ok = false;
        RunWithProgress(L"Looking for the latest Polarity", [&](Progress*) { ok = FetchInfo(&info, &error); });
        if (ok) break;
        if (g_cancel) return 1;
        if (g_opt.yes) {
            Tell(L"Polarity couldn't be installed", error + LogNote(), TD_ERROR_ICON);
            return 1;
        }
        if (Ask(L"Polarity couldn't be downloaded", error + LogNote(), {{kRetry, L"Try again"}}, TD_ERROR_ICON) != kRetry) {
            return 1;
        }
    }

    const auto win64 = ChooseGame(info);
    if (!win64) return 1;
    Log(L"game folder: " + *win64);

    while (GameRunning()) {
        if (g_opt.yes) {
            Tell(L"Street Fighter V is running", L"Close it and run the installer again.", TD_WARNING_ICON);
            return 1;
        }
        if (Ask(L"Close Street Fighter V first", L"The game is running. Close it, then press Try again.",
                {{kRetry, L"Try again"}}, TD_WARNING_ICON) != kRetry) {
            return 1;
        }
    }

    for (;;) {
        InstallResult r;
        RunWithProgress(L"Installing Polarity " + Wide(info.version), [&](Progress* p) { r = Install(*win64, info, p); });
        if (r.ok) {
            const std::wstring heading = r.upToDate ? L"Polarity " + Wide(info.version) + L" is already installed"
                                                    : L"Polarity is installed";
            const std::wstring text = r.upToDate ? L"It runs whenever you start Street Fighter V."
                                                 : L"Polarity " + Wide(info.version) +
                                                       L" will run the next time you start Street Fighter V.";
            Tell(heading, text, TD_INFORMATION_ICON);
            return 0;
        }
        if (g_cancel) {
            Log("cancelled");
            return 1;
        }
        if (g_opt.yes) {
            Tell(L"Polarity couldn't be installed", r.error + LogNote(), TD_ERROR_ICON);
            return 1;
        }
        std::vector<Button> buttons{{kRetry, L"Try again"}};
        if (r.accessDenied && !g_opt.elevated) buttons.insert(buttons.begin(), {kAdmin, L"Install as administrator"});
        const int pressed = Ask(L"Polarity couldn't be installed", r.error + LogNote(), buttons, TD_ERROR_ICON);
        if (pressed == kAdmin) return RelaunchElevated(*win64) ? 0 : 1;
        if (pressed != kRetry) return 1;
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    ParseArgs();
    if (g_opt.yes || g_opt.find) AttachConsole(ATTACH_PARENT_PROCESS);
    if (!g_opt.find) {
        OpenLog();
        LogStart();
    }
    PrepareCrashText();
    InstallCrashHandlers();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int code = 1;
    try {
        code = Run();
    } catch (const std::exception& e) {
        Log(std::string("unexpected error: ") + e.what());
        Tell(L"Polarity couldn't be installed",
             L"Something unexpected went wrong (" + Wide(e.what()) + L")." + LogNote(), TD_ERROR_ICON);
    }
    Log("exit " + std::to_string(code));
    CoUninitialize();
    return code;
}
