#pragma once
// What the Linux installer's files share: failures, the log, processes, the update steps and the windows.

#include "installer/linux_steam.h"

#include "core/update_info.h"
#include "installer/install_log.h"
#include "installer/version.h"
#include "mod/update_key.h"

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
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace polarity::installer::detail {

constexpr const char* kSite = "https://polaritymod.link/update.json";
constexpr const char* kTitle = "Polarity Installer";
constexpr size_t kMaxInfoBytes = 64 * 1024;
constexpr int kTimeoutMs = 20000;
constexpr int kExecFailed = 127; // a child's exit code when its program did not start

// Something the player is told about.
struct Failure {
    std::string message;
};

// The player stopped it (message: why, or empty to say nothing).
struct Cancelled {
    std::string message;
};

void Log(const std::string& line);
std::string Quoted(const std::string& s);
bool IsFile(const std::string& p);
bool IsDir(const std::string& p);
bool Exists(const std::string& p);
std::optional<std::string> ReadFile(const std::string& path, size_t maxBytes = 64 * 1024 * 1024);
bool WriteFile(const std::string& path, const void* data, size_t size, mode_t mode = 0644);
void MakeDirs(const std::string& dir);
std::string Dirname(const std::string& p);
std::string RealPath(const std::string& p);
std::vector<std::string> ListDir(const std::string& dir);
std::string Which(const std::string& name);
std::vector<char*> Argv(const std::vector<std::string>& args);

// Runs a program and waits: its exit code (-1 when it could not run) and stdout.
int RunCapture(const std::vector<std::string>& args, std::string* out);

// Starts a program on its own (its own session, not waited for).
void RunDetached(const std::vector<std::string>& args);
bool SteamRunning();
bool GameRunning();
sfv::UpdateInfo FetchInfo(const std::string& site);

// Downloads what differs from the game folder and swaps it in, old files
// aside first so a failure puts everything back. How many files changed.
int InstallFiles(const std::string& win64, const sfv::UpdateInfo& info, const std::string& site, const std::function<bool(uint64_t, uint64_t)>& progress);
std::vector<std::string> SteamRoots(const std::vector<std::string>& given);
std::vector<std::string> SteamLibraries(const std::vector<std::string>& roots);
std::optional<std::string> FindGame(const std::vector<std::string>& libs);
std::vector<std::string> LocalConfigs(const std::vector<std::string>& roots);

// The accounts whose launch options do not load the mod yet.
std::vector<std::string> AccountsNeedingOptions(const std::vector<std::string>& paths);
void CloseSteam(const std::vector<std::string>& roots);
void OpenSteam(const std::vector<std::string>& roots);

// Sets the launch options in each account's localconfig.vdf (a copy of the
// file as it was next to it); the files it could not change.
std::vector<std::string> WriteLaunchOptions(const std::vector<std::string>& paths);

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

std::string Markup(const std::string& s);

struct Options {
    std::string site = kSite;
    std::string game;
    std::vector<std::string> steamRoots;
    bool yes = false;
    bool terminal = false;
    std::string testCrash;
};

// Which windows to use: "zenity", "kdialog" or "terminal".
std::string UiKind(const Options& o);
std::unique_ptr<Ui> MakeUi(const Options& o, const std::string& kind);
void PrepareCrashWindow(const std::string& uiKind);
void FindSlide();
void InstallCrashHandlers();

// polarity-installer.log next to the installer, else in the player's state
// folder, else /tmp.
void OpenLog();
void LogStart(int argc, char** argv, const std::string& uiKind);

// A deliberate crash, to test the report (--test-crash).
[[gnu::noinline]] void TestCrash(const std::string& kind);
int Run(const Options& o, Ui& ui);

} // namespace polarity::installer::detail
