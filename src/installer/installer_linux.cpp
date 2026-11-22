// polarity-installer (Linux): the steps, the options and main (README.md).

#include "installer/linux_internal.h"

#include <psa/crypto.h>

namespace polarity::installer {

using namespace detail;

namespace {

std::string Megabytes(uint64_t n) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", n / (1024.0 * 1024.0));
    return text;
}

} // namespace

namespace detail {

void Log(const std::string& line) {
    log::Write(line);
}

std::string Quoted(const std::string& s) {
    return "\"" + s + "\"";
}

} // namespace detail

namespace {

std::string LogNote() {
    return log::Path().empty() ? "" : "\n\nDetails are in the installer's log:\n" + log::Path();
}

sfv::UpdateInfo GetInfo(Ui& ui, const std::string& site) {
    for (;;) {
        try {
            return FetchInfo(site);
        } catch (const Failure& f) {
            if (ui.yes || ui.Choose("Polarity couldn't be downloaded", f.message, {"Try again"}) != 0) throw Cancelled{f.message};
        }
    }
}

std::string ChooseGame(Ui& ui, const Options& o, const sfv::UpdateInfo& info, const std::vector<std::string>& libs) {
    std::optional<std::string> win64;
    if (!o.game.empty()) {
        win64 = GameWin64(o.game);
        if (!win64) throw Failure{"StreetFighterV.exe isn't in " + o.game + "."};
    } else {
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
        } else {
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
        } else {
            ui.Tell("That isn't a Street Fighter V folder",
                    "StreetFighterV.exe isn't in " + *picked + ". Choose the folder named StreetFighterV in your Steam library.",
                    "warning");
        }
    }
}

void WaitForGameClosed(Ui& ui) {
    while (GameRunning()) {
        if (ui.yes || ui.Choose("Close Street Fighter V first", "The game is running. Close it, then choose Try again.",
                                {"Try again"}) != 0) {
            throw Cancelled{"Street Fighter V is running."};
        }
    }
}

int Install(Ui& ui, const std::string& site, const std::string& win64, const sfv::UpdateInfo& info) {
    for (;;) {
        ui.ProgressStart("Installing Polarity " + info.version);
        try {
            const int changed = InstallFiles(win64, info, site, [&](uint64_t done, uint64_t total) {
                return ui.Progress(total ? static_cast<double>(done) / total : 1.0, Megabytes(done) + " of " + Megabytes(total));
            });
            ui.ProgressEnd();
            return changed;
        } catch (const Failure& f) {
            ui.ProgressEnd();
            if (ui.yes || ui.Choose("Polarity couldn't be installed", f.message, {"Try again"}) != 0) throw Cancelled{f.message};
        } catch (const Cancelled&) {
            ui.ProgressEnd();
            throw;
        }
    }
}

enum class LaunchOptions { Set,
                           Had,
                           Left };

LaunchOptions SetOptions(Ui& ui, const std::vector<std::string>& roots) {
    const std::vector<std::string> paths = LocalConfigs(roots);
    Log("Steam accounts with a localconfig.vdf: " + std::to_string(paths.size()));
    if (paths.empty()) return LaunchOptions::Left;
    const bool wasRunning = SteamRunning();
    Log(std::string("Steam is ") + (wasRunning ? "running" : "closed"));
    if (wasRunning) {
        if (ui.Choose("Steam needs to close for a moment",
                      "Polarity loads through Street Fighter V's launch options, which can only be checked and changed "
                      "while Steam is closed. The installer closes Steam, sets them and opens it again.",
                      {"Close Steam", "Skip"}) != 0) {
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

void Usage() {
    std::cout << "polarity-installer: installs Polarity into Street Fighter V (Steam on Linux).\n"
                 "  --site URL         update info (default "
              << kSite << ")\n"
                          "  --game DIR         the game folder\n"
                          "  --steam-root DIR   a Steam folder to use (repeatable)\n"
                          "  --yes              no questions\n"
                          "  --terminal         questions in the terminal\n";
}

} // namespace

namespace detail {

int Run(const Options& o, Ui& ui) {
    sfv::UpdateInfo info;
    int changed = 0;
    LaunchOptions options = LaunchOptions::Left;
    try {
        if (!o.testCrash.empty()) TestCrash(o.testCrash);
        info = GetInfo(ui, o.site);
        const std::vector<std::string> roots = SteamRoots(o.steamRoots);
        std::string list;
        for (const std::string& r : roots) list += " " + r;
        Log("Steam folders:" + (list.empty() ? std::string(" none found") : list));
        const std::string win64 = ChooseGame(ui, o, info, SteamLibraries(roots));
        Log("game folder: " + win64);
        WaitForGameClosed(ui);
        changed = Install(ui, o.site, win64, info);
        options = SetOptions(ui, roots);
    } catch (const Cancelled& c) {
        Log(c.message.empty() ? "cancelled" : "stopped: " + c.message);
        if (!c.message.empty()) ui.Tell("Polarity couldn't be installed", c.message + LogNote(), "error");
        return 1;
    } catch (const Failure& f) {
        Log("failed: " + f.message);
        ui.Tell("Polarity couldn't be installed", f.message + LogNote(), "error");
        return 1;
    } catch (const std::exception& e) {
        Log(std::string("unexpected error: ") + e.what());
        ui.Tell("Polarity couldn't be installed", std::string("Something unexpected went wrong (") + e.what() + ")." + LogNote(),
                "error");
        return 1;
    }
    Log("result: " + std::to_string(changed) + " files changed, launch options " +
        (options == LaunchOptions::Set ? "set" : options == LaunchOptions::Had ? "already set"
                                                                               : "left to the player"));
    const bool fresh = changed > 0 || options == LaunchOptions::Set;
    const std::string heading = fresh ? "Polarity is installed" : "Polarity " + info.version + " is already installed";
    std::string text = fresh ? "Polarity " + info.version + " will run the next time you start Street Fighter V."
                             : "It runs whenever you start Street Fighter V.";
    if (options == LaunchOptions::Left) {
        text += std::string("\n\nOne step is left: in Steam, open Street Fighter V's Properties and set its launch "
                            "options to\n") +
                kLaunchHint;
    }
    ui.Tell(heading, text, "info");
    return 0;
}

} // namespace detail

namespace {

int Main(int argc, char** argv) {
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
        else if (a == "--test-crash") o.testCrash = next();
        else if (a == "--help" || a == "-h") {
            Usage();
            return 0;
        } else {
            Usage();
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    OpenLog();
    const std::string uiKind = UiKind(o);
    LogStart(argc, argv, uiKind);
    PrepareCrashWindow(uiKind);
    FindSlide();
    InstallCrashHandlers();
    std::unique_ptr<Ui> ui = MakeUi(o, uiKind);
    if (psa_crypto_init() != PSA_SUCCESS) {
        Log("failed: psa_crypto_init");
        ui->Tell("Polarity couldn't be installed", "Cannot start the crypto library." + LogNote(), "error");
        return 1;
    }
    const int code = Run(o, *ui);
    Log("exit " + std::to_string(code));
    return code;
}

} // namespace

} // namespace polarity::installer

int main(int argc, char** argv) {
    return polarity::installer::Main(argc, argv);
}
