#include "installer/install_log.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <ctime>
#include <mutex>

namespace polarity::installer::log {
namespace {

constexpr long long kMaxBytes = 1024 * 1024;
constexpr const char* kName = "polarity-installer.log";

std::mutex g_lock;
std::string g_path;
std::string g_home;
#ifdef _WIN32
HANDLE g_file = INVALID_HANDLE_VALUE;
#else
int g_fd = -1;
#endif

#ifdef _WIN32
std::wstring Wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
#endif

std::string Join(const std::string& dir, const char* name)
{
    if (dir.empty()) return name;
    const char last = dir.back();
#ifdef _WIN32
    const char* sep = last == '\\' || last == '/' ? "" : "\\";
#else
    const char* sep = last == '/' ? "" : "/";
#endif
    return dir + sep + name;
}

std::string Now()
{
    char text[32];
#ifdef _WIN32
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::snprintf(text, sizeof(text), "%02u:%02u:%02u.%03u", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
#else
    timeval tv{};
    gettimeofday(&tv, nullptr);
    tm local{};
    localtime_r(&tv.tv_sec, &local);
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(tv.tv_usec / 1000));
#endif
    return text;
}

} // namespace

bool Open(const std::vector<std::string>& dirs)
{
    std::lock_guard<std::mutex> hold(g_lock);
    for (const std::string& dir : dirs) {
        if (dir.empty()) continue;
        const std::string path = Join(dir, kName);
#ifdef _WIN32
        // Shared: the elevated retry writes to it while this run ends.
        HANDLE h = CreateFileW(Wide(path).c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size) && size.QuadPart > kMaxBytes) {
            CloseHandle(h);
            h = CreateFileW(Wide(path).c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
        }
        g_file = h;
#else
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) continue;
        struct stat st{};
        if (fstat(fd, &st) == 0 && st.st_size > kMaxBytes) {
            if (ftruncate(fd, 0) != 0) {
                close(fd);
                continue;
            }
        }
        g_fd = fd;
#endif
        g_path = path;
        return true;
    }
    return false;
}

const std::string& Path()
{
    return g_path;
}

void SetHome(const std::string& home)
{
    std::lock_guard<std::mutex> hold(g_lock);
    g_home = home;
    while (g_home.size() > 1 && (g_home.back() == '/' || g_home.back() == '\\')) g_home.pop_back();
}

void WriteRaw(const char* text, size_t size)
{
#ifdef _WIN32
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_file, text, static_cast<DWORD>(size), &written, nullptr);
#else
    if (g_fd < 0) return;
    while (size > 0) {
        const ssize_t w = write(g_fd, text, size);
        if (w <= 0) return;
        text += w;
        size -= static_cast<size_t>(w);
    }
#endif
}

void Write(const std::string& line)
{
    std::lock_guard<std::mutex> hold(g_lock);
    std::string text = line;
    if (!g_home.empty()) {
        for (size_t at = text.find(g_home); at != std::string::npos; at = text.find(g_home, at + 1)) {
            const size_t end = at + g_home.size();
            // Only the whole folder: /home/deck, not /home/deckard.
            if (end == text.size() || text[end] == '/' || text[end] == '\\' || text[end] == '"' || text[end] == ' ' ||
                text[end] == ')' || text[end] == '\'') {
                text.replace(at, g_home.size(), "~");
            }
        }
    }
    const std::string out = Now() + "  " + text + "\n";
#ifdef _WIN32
    if (g_file == INVALID_HANDLE_VALUE) return;
    // CRLF: the log opens in Notepad.
    std::string crlf;
    for (char c : out) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    WriteRaw(crlf.data(), crlf.size());
#else
    WriteRaw(out.data(), out.size());
#endif
}

std::string OneLine(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r') continue;
        if (text[i] == '\n') {
            while (i + 1 < text.size() && text[i + 1] == '\n') i++;
            out += " / ";
        }
        else {
            out += text[i];
        }
    }
    return out;
}

} // namespace polarity::installer::log
