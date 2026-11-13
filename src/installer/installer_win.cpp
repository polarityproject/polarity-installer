// polarity-installer.exe: installs the latest Polarity release into a
// Street Fighter V folder on Windows.
//
// 1. Reads the site's signed update info (the mod updater's own check:
//    update.json + update.json.sig, the key in mod/update_key.h).
// 2. Finds the game through Steam (its libraries, app 310950), or asks for
//    the folder.
// 3. Downloads the release's files ("files" and "more_files"), checks each
//    one's size and SHA-256, and puts them next to StreetFighterV.exe
//    (StreetFighterV\Binaries\Win64), all or nothing.
// 4. Says Polarity runs at the game's next start.
//
// It makes no shortcuts, no Start menu entries and no registry entries, and
// never writes polarity.ini (the mod's defaults are the public setup).
//
// Options (for tests and the elevated retry):
//   --site <url>     update info other than https://polaritymod.link/update.json
//   --game <folder>  the game folder (any of its levels down to Binaries\Win64)
//   --yes            no questions: the folder found (or --game), messages
//                    on stdout, exit code 0 when installed
//   --find           prints the game folder Steam has (or nothing) and exits

#include "core/update_info.h"
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
#include <functional>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr const char* kDefaultSite = "https://polaritymod.link/update.json";
constexpr const wchar_t* kTitle = L"Polarity Installer";
constexpr const char* kAppId = "310950";
constexpr DWORD kTimeoutMs = 15000;
constexpr size_t kMaxInfoBytes = 64 * 1024;

enum ButtonId { kInstall = 100, kChoose, kRetry, kAdmin };

struct Options {
    std::string site = kDefaultSite;
    std::wstring game;
    bool yes = false;
    bool elevated = false;
    bool find = false;
};
Options g_opt;

// ----- text ------------------------------------------------------------------

std::wstring Wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

std::wstring Megabytes(uint64_t bytes)
{
    wchar_t text[32];
    swprintf(text, 32, L"%.1f MB", bytes / (1024.0 * 1024.0));
    return text;
}

// --yes: messages to the console the installer was started from.
void Print(const std::wstring& text)
{
    const std::string s = Utf8(text) + "\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
}

// ----- files -----------------------------------------------------------------

bool IsFile(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsDir(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::optional<std::string> ReadFileText(const std::wstring& path, size_t maxBytes = 4 * 1024 * 1024)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return std::nullopt;
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && data.size() < maxBytes) data.append(buf, n);
    std::fclose(f);
    return data;
}

void MakeDirs(const std::wstring& dir)
{
    if (dir.empty() || IsDir(dir)) return;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) MakeDirs(dir.substr(0, slash));
    CreateDirectoryW(dir.c_str(), nullptr);
}

std::wstring Trimmed(std::wstring p)
{
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

// The game's Binaries\Win64 folder for a folder the player picked: the
// library's StreetFighterV folder, the StreetFighterV inside it, Binaries,
// or Win64 itself.
std::optional<std::wstring> GameWin64(const std::wstring& picked)
{
    const std::wstring p = Trimmed(picked);
    for (const std::wstring& c : {p + L"\\StreetFighterV\\Binaries\\Win64", p + L"\\Binaries\\Win64", p + L"\\Win64", p}) {
        const std::wstring lower = Lower(c);
        const bool win64 = lower.size() >= 15 && lower.compare(lower.size() - 15, 15, L"\\binaries\\win64") == 0;
        if (win64 && IsFile(c + L"\\StreetFighterV.exe")) return c;
    }
    return std::nullopt;
}

// ----- Steam -----------------------------------------------------------------

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* name)
{
    wchar_t value[MAX_PATH * 2] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
    return value;
}

// A VDF string's text: \\ -> \, \" -> ".
std::string VdfUnescape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) out += s[++i];
        else out += s[i];
    }
    return out;
}

std::vector<std::wstring> SteamLibraries()
{
    std::vector<std::wstring> roots;
    for (const std::wstring& r : {RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
                                  RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
                                  RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath")}) {
        if (!r.empty()) roots.push_back(Trimmed(r));
    }
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

std::optional<std::wstring> FindGame()
{
    for (const std::wstring& lib : SteamLibraries()) {
        std::wstring folder = L"StreetFighterV";
        if (const auto acf = ReadFileText(lib + L"\\steamapps\\appmanifest_" + Wide(kAppId) + L".acf")) {
            static const std::regex dir(R"re("installdir"\s+"((?:[^"\\]|\\.)*)")re", std::regex::icase);
            std::smatch m;
            if (std::regex_search(*acf, m, dir)) folder = Wide(VdfUnescape(m[1].str()));
        }
        if (auto win64 = GameWin64(lib + L"\\steamapps\\common\\" + folder)) return win64;
    }
    return std::nullopt;
}

// The library's StreetFighterV folder (two levels above Binaries\Win64,
// then one more), for showing the player.
std::wstring GameRoot(const std::wstring& win64)
{
    std::wstring p = win64;
    for (int i = 0; i < 3; i++) {
        const size_t slash = p.find_last_of(L'\\');
        if (slash == std::wstring::npos) break;
        p = p.substr(0, slash);
    }
    return p;
}

// Whether a StreetFighterV.exe is running (the launcher or the game).
bool GameRunning()
{
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

// ----- download --------------------------------------------------------------

std::atomic<bool> g_cancel{false};

bool HttpGet(const std::string& url, size_t maxBytes, std::vector<uint8_t>* out, std::wstring* error,
             const std::function<void(uint64_t)>& progress = nullptr)
{
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
            if (status != 200) {
                *error = L"The Polarity site answered " + std::to_wstring(status) + L" for " + wurl + L".";
            }
            else {
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
        }
        else {
            *error = L"Cannot reach the Polarity site (error " + std::to_wstring(GetLastError()) +
                     L"). Check your internet connection and try again.";
        }
    }
    else {
        *error = L"Cannot open " + wurl + L".";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

// The site's update info, checked against the release key before anything
// in it counts.
bool FetchInfo(sfv::UpdateInfo* info, std::wstring* error)
{
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
    return true;
}

bool SameFile(const std::wstring& path, const sfv::UpdateFile& f)
{
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

// Downloads what differs from the game folder, then swaps it in: old files
// aside first, so a failure puts everything back.
InstallResult Install(const std::wstring& win64, const sfv::UpdateInfo& info, Progress* progress)
{
    InstallResult r;
    struct Item {
        const sfv::UpdateFile* file;
        std::wstring target, fresh, old;
        bool hadOld = false;
    };
    std::vector<Item> items;
    for (const sfv::UpdateFile& f : info.files) {
        const std::wstring target = win64 + L"\\" + Wide(sfv::UpdateFileOnDisk(f.path));
        if (SameFile(target, f)) continue;
        items.push_back({&f, target, target + L".polarity-new", target + L".polarity-old"});
        progress->total += f.size;
    }
    if (items.empty()) {
        r.ok = r.upToDate = true;
        return r;
    }
    auto cleanup = [&] {
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
            for (auto p = swapped.rbegin(); p != swapped.rend(); ++p) {
                DeleteFileW((*p)->target.c_str());
                if ((*p)->hadOld) MoveFileExW((*p)->old.c_str(), (*p)->target.c_str(), 0);
            }
            cleanup();
            return r;
        }
        swapped.push_back(&it);
    }
    // The files it replaced; one still in use stays until the next run.
    for (const Item& it : items) {
        if (it.hadOld) DeleteFileW(it.old.c_str());
    }
    r.ok = true;
    return r;
}

// ----- dialogs ---------------------------------------------------------------

struct Button {
    int id;
    const wchar_t* text;
};

int Ask(const std::wstring& heading, const std::wstring& text, const std::vector<Button>& buttons,
        PCWSTR icon = nullptr, TASKDIALOG_COMMON_BUTTON_FLAGS common = TDCBF_CANCEL_BUTTON)
{
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
    int pressed = IDCANCEL;
    TaskDialogIndirect(&c, &pressed, nullptr, nullptr);
    return pressed;
}

void Tell(const std::wstring& heading, const std::wstring& text, PCWSTR icon = nullptr)
{
    if (g_opt.yes) {
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

HRESULT CALLBACK WorkCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM, LONG_PTR ref)
{
    auto* w = reinterpret_cast<WorkDialog*>(ref);
    switch (msg) {
    case TDN_CREATED:
        SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
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
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, static_cast<WPARAM>(done * 1000 / total), 0);
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

// Runs `work` on a thread behind a progress dialog (none with --yes).
void RunWithProgress(const std::wstring& heading, const std::function<void(Progress*)>& work)
{
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
        c.cxWidth = 260;
        TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
    }
    t.join();
}

std::optional<std::wstring> PickFolder(const std::wstring& start)
{
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
    return out;
}

// The game folder to install into: the one Steam has (confirmed), or one
// the player picks.
std::optional<std::wstring> ChooseGame(const sfv::UpdateInfo& info)
{
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
        }
        else {
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

// The same installer again as administrator, for this folder.
bool RelaunchElevated(const std::wstring& win64)
{
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
    return ShellExecuteExW(&e) != FALSE;
}

void ParseArgs()
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        const std::wstring a = argv[i];
        if (a == L"--site" && i + 1 < argc) g_opt.site = Utf8(argv[++i]);
        else if (a == L"--game" && i + 1 < argc) g_opt.game = argv[++i];
        else if (a == L"--yes") g_opt.yes = true;
        else if (a == L"--elevated") g_opt.elevated = true;
        else if (a == L"--find") g_opt.find = true;
    }
    LocalFree(argv);
}

int Run()
{
    ParseArgs();
    if (g_opt.yes || g_opt.find) AttachConsole(ATTACH_PARENT_PROCESS);
    if (g_opt.find) {
        const auto win64 = FindGame();
        if (win64) Print(*win64);
        return win64 ? 0 : 1;
    }

    sfv::UpdateInfo info;
    std::wstring error;
    for (;;) {
        bool ok = false;
        RunWithProgress(L"Looking for the latest Polarity", [&](Progress*) { ok = FetchInfo(&info, &error); });
        if (ok) break;
        if (g_cancel) return 1;
        if (g_opt.yes) {
            Tell(L"Polarity couldn't be installed", error, TD_ERROR_ICON);
            return 1;
        }
        if (Ask(L"Polarity couldn't be downloaded", error, {{kRetry, L"Try again"}}, TD_ERROR_ICON) != kRetry) return 1;
    }

    const auto win64 = ChooseGame(info);
    if (!win64) return 1;

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
        if (g_cancel) return 1;
        if (g_opt.yes) {
            Tell(L"Polarity couldn't be installed", r.error, TD_ERROR_ICON);
            return 1;
        }
        std::vector<Button> buttons{{kRetry, L"Try again"}};
        if (r.accessDenied && !g_opt.elevated) buttons.insert(buttons.begin(), {kAdmin, L"Install as administrator"});
        const int pressed = Ask(L"Polarity couldn't be installed", r.error, buttons, TD_ERROR_ICON);
        if (pressed == kAdmin) return RelaunchElevated(*win64) ? 0 : 1;
        if (pressed != kRetry) return 1;
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const int code = Run();
    CoUninitialize();
    return code;
}
