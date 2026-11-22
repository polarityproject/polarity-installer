// Files and processes.

#include "installer/linux_internal.h"

namespace polarity::installer {

using namespace detail;

namespace detail {

bool IsFile(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsDir(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool Exists(const std::string& p) {
    struct stat st{};
    return lstat(p.c_str(), &st) == 0;
}

std::optional<std::string> ReadFile(const std::string& path, size_t maxBytes) {
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

bool WriteFile(const std::string& path, const void* data, size_t size, mode_t mode) {
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

void MakeDirs(const std::string& dir) {
    if (dir.empty() || IsDir(dir)) return;
    const size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && slash > 0) MakeDirs(dir.substr(0, slash));
    mkdir(dir.c_str(), 0755);
}

std::string Dirname(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : slash == 0 ? "/"
                                                         : p.substr(0, slash);
}

std::string RealPath(const std::string& p) {
    char real[PATH_MAX];
    return realpath(p.c_str(), real) ? std::string(real) : p;
}

std::vector<std::string> ListDir(const std::string& dir) {
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

std::string Which(const std::string& name) {
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

std::vector<char*> Argv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    return argv;
}

int RunCapture(const std::vector<std::string>& args, std::string* out) {
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
        _exit(kExecFailed);
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

void RunDetached(const std::vector<std::string>& args) {
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

} // namespace detail

namespace {

std::vector<int> Processes(const std::function<bool(const std::string&)>& match) {
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
        if (paren == std::string::npos || stat_->compare(paren + 2, 1, "Z") == 0) continue;
        if (match(*comm)) found.push_back(std::atoi(name.c_str()));
    }
    return found;
}

} // namespace

namespace detail {

bool SteamRunning() {
    return !Processes([](const std::string& c) { return c == "steam"; }).empty();
}

bool GameRunning() {
    return !Processes([](const std::string& c) { return c.rfind("StreetFighterV", 0) == 0; }).empty();
}

} // namespace detail

} // namespace polarity::installer
