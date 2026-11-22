// Steam: its folders and libraries, closing and opening it, the launch options.

#include "installer/linux_internal.h"

namespace polarity::installer {

using namespace detail;

namespace detail {

std::vector<std::string> SteamRoots(const std::vector<std::string>& given) {
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

std::vector<std::string> SteamLibraries(const std::vector<std::string>& roots) {
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

std::optional<std::string> FindGame(const std::vector<std::string>& libs) {
    for (const std::string& lib : libs) {
        std::string folder = "StreetFighterV";
        const auto acf = ReadFile(lib + "/steamapps/appmanifest_" + kAppId + ".acf", 1024 * 1024);
        if (acf) {
            const std::string dir = InstallDir(*acf);
            if (!dir.empty()) folder = dir;
        }
        auto win64 = GameWin64(lib + "/steamapps/common/" + folder);
        Log("library " + lib + ": " + (acf ? "lists the game" : "doesn't list the game") + ", " +
            (win64 ? "StreetFighterV.exe found" : "no StreetFighterV.exe in steamapps/common/" + folder));
        if (win64) return win64;
    }
    return std::nullopt;
}

std::vector<std::string> LocalConfigs(const std::vector<std::string>& roots) {
    std::vector<std::string> out;
    for (const std::string& root : roots) {
        for (const std::string& id : ListDir(root + "/userdata")) {
            const std::string p = root + "/userdata/" + id + "/config/localconfig.vdf";
            if (id != "0" && id.find_first_not_of("0123456789") == std::string::npos && IsFile(p)) out.push_back(p);
        }
    }
    return out;
}

std::vector<std::string> AccountsNeedingOptions(const std::vector<std::string>& paths) {
    std::vector<std::string> out;
    for (const std::string& p : paths) {
        const auto text = ReadFile(p);
        const auto opts = text ? GetLaunchOptions(*text) : std::nullopt;
        if (!opts) {
            Log(p + (text ? ": launch options unreadable" : ": cannot read the file"));
            out.push_back(p);
            continue;
        }
        std::string trimmed = *opts;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
        const bool loads = WithOverride(*opts) == trimmed;
        Log(p + ": launch options " + Quoted(*opts) + (loads ? ", load Polarity" : ", need the override"));
        if (!loads) out.push_back(p);
    }
    return out;
}

} // namespace detail

namespace {

std::vector<std::string> SteamCommand(const std::vector<std::string>& roots) {
    if (!Which("steam").empty()) return {"steam"};
    for (const std::string& r : roots) {
        if (r.find("/com.valvesoftware.Steam/") != std::string::npos && !Which("flatpak").empty()) {
            return {"flatpak", "run", "com.valvesoftware.Steam"};
        }
    }
    return {};
}

} // namespace

namespace detail {

void CloseSteam(const std::vector<std::string>& roots) {
    std::vector<std::string> cmd = SteamCommand(roots);
    if (cmd.empty()) throw Failure{"The steam command was not found. Close Steam yourself and run the installer again."};
    cmd.push_back("-shutdown");
    std::string line;
    for (const std::string& a : cmd) line += (line.empty() ? "" : " ") + a;
    Log("closing Steam: " + line);
    RunDetached(cmd);
    for (int i = 0; i < 90; i++) {
        sleep(1);
        if (!SteamRunning()) {
            Log("Steam closed after " + std::to_string(i + 1) + " s");
            sleep(2);
            return;
        }
    }
    throw Failure{"Steam did not close. Close it yourself and run the installer again."};
}

void OpenSteam(const std::vector<std::string>& roots) {
    const std::vector<std::string> cmd = SteamCommand(roots);
    Log(cmd.empty() ? "cannot reopen Steam: no steam command" : "reopening Steam: " + cmd.front());
    if (!cmd.empty()) RunDetached(cmd);
}

std::vector<std::string> WriteLaunchOptions(const std::vector<std::string>& paths) {
    std::vector<std::string> failed;
    for (const std::string& p : paths) {
        const auto text = ReadFile(p);
        const auto opts = text ? GetLaunchOptions(*text) : std::nullopt;
        std::string updated, why;
        if (!opts || !SetLaunchOptions(*text, WithOverride(*opts), &updated, &why)) {
            Log(p + ": cannot set the launch options (" + (!text ? "cannot read the file" : !opts ? "unreadable"
                                                                                                  : why) +
                ")");
            failed.push_back(p);
            continue;
        }
        if (updated == *text) continue;
        struct stat st{};
        const mode_t mode = stat(p.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0644;
        const std::string tmp = Dirname(p) + "/.localconfig.polarity-new";
        const char* step = nullptr;
        if (!WriteFile(p + ".before-polarity", text->data(), text->size(), mode)) step = "writing the backup";
        else if (!WriteFile(tmp, updated.data(), updated.size(), mode)) step = "writing the new file";
        else if (rename(tmp.c_str(), p.c_str()) != 0) step = "replacing the file";
        if (step) {
            Log(p + ": failed " + step + " (" + std::strerror(errno) + ")");
            unlink(tmp.c_str());
            failed.push_back(p);
            continue;
        }
        Log(p + ": launch options set to " + Quoted(WithOverride(*opts)) + ", backup in localconfig.vdf.before-polarity");
    }
    return failed;
}

} // namespace detail

} // namespace polarity::installer
