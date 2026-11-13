// Tests for the Linux installer's pieces (src/installer/linux_steam.h and
// the update info code it shares with the mod): built and run by
// scripts/build-linux.ps1. update.json + update.json.sig here are
// a real release's, signed with the release key (kept byte for byte:
// .gitattributes).

#include "core/update_info.h"
#include "installer/linux_steam.h"
#include "mod/update_key.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace polarity::installer;

static int g_failed = 0, g_checked = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checked++;                                                                                                   \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                             \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        g_checked++;                                                                                                   \
        const auto va = (a);                                                                                           \
        const auto vb = (b);                                                                                           \
        if (!(va == vb)) {                                                                                             \
            g_failed++;                                                                                                \
            std::printf("FAILED %s:%d: %s\n  got:  [%s]\n  want: [%s]\n", __FILE__, __LINE__, #a,                     \
                        std::string(va).c_str(), std::string(vb).c_str());                                             \
        }                                                                                                              \
    } while (0)

static std::string Read(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static void Signature(const std::string& dir)
{
    const std::string body = Read(dir + "/update.json"), sigText = Read(dir + "/update.json.sig");
    CHECK(!body.empty() && !sigText.empty());
    const auto sig = sfv::Base64Decode(sigText);
    const auto* b = reinterpret_cast<const uint8_t*>(body.data());
    CHECK(sfv::VerifySignature(b, body.size(), sig.data(), sig.size(), sfv::mod::kUpdatePublicKey));
    const std::string more = body + " ";
    CHECK(!sfv::VerifySignature(reinterpret_cast<const uint8_t*>(more.data()), more.size(), sig.data(), sig.size(),
                                sfv::mod::kUpdatePublicKey));
    auto flipped = sig;
    flipped[63] ^= 1;
    CHECK(!sfv::VerifySignature(b, body.size(), flipped.data(), flipped.size(), sfv::mod::kUpdatePublicKey));
    const uint8_t zero[64] = {};
    CHECK(!sfv::VerifySignature(b, body.size(), zero, 64, sfv::mod::kUpdatePublicKey));

    sfv::UpdateInfo info;
    std::string error;
    CHECK(sfv::ParseUpdateInfo(body, &info, &error));
    bool dll = false, font = false;
    for (const auto& f : info.files) {
        dll |= f.path == "polarity.dll";
        font |= f.more && sfv::UpdateFileOnDisk(f.path) == "Polarity/fonts/NotoSansCJK-Medium.ttc";
    }
    CHECK(dll && font);
    CHECK_EQ(sfv::Sha256Hex(reinterpret_cast<const uint8_t*>("abc"), 3),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

static void Paths()
{
    for (const char* ok : {"polarity.dll", "xinput1_3.dll", "polarity_fonts_license.txt", "Polarity/fonts/a.ttc", "SFVR/fonts/b.ttc"}) {
        CHECK(sfv::SafeUpdatePath(ok));
    }
    for (const char* bad : {"../x.dll", "polarity.ini", "a/b.dll", "Polarity/fonts/../x.ttc", "Polarity/fonts/x.dll", ".dll",
                            "x.exe", "Polarity/fonts/sub/x.ttc", "/etc/x.txt", "\xC3\xA9.dll"}) {
        CHECK(!sfv::SafeUpdatePath(bad));
    }
    CHECK_EQ(sfv::UpdateFileOnDisk("SFVR/fonts/x.ttc"), std::string("Polarity/fonts/x.ttc"));
    CHECK_EQ(sfv::ResolveUrl("https://polaritymod.link/update.json", "releases/0.4.2/polarity.dll"),
             std::string("https://polaritymod.link/releases/0.4.2/polarity.dll"));
}

static void Override()
{
    const std::map<std::string, std::string> cases{
        {"", "WINEDLLOVERRIDES=\"xinput1_3=n,b\" %command%"},
        {"%command%", "WINEDLLOVERRIDES=\"xinput1_3=n,b\" %command%"},
        {"PROTON_LOG=1 %command%", "WINEDLLOVERRIDES=\"xinput1_3=n,b\" PROTON_LOG=1 %command%"},
        {"gamemoderun %command% -dx11", "WINEDLLOVERRIDES=\"xinput1_3=n,b\" gamemoderun %command% -dx11"},
        {"-dx11", "WINEDLLOVERRIDES=\"xinput1_3=n,b\" %command% -dx11"},
        {"WINEDLLOVERRIDES=\"xinput1_3=n,b\" %command%", "WINEDLLOVERRIDES=\"xinput1_3=n,b\" %command%"},
        {"WINEDLLOVERRIDES='xinput1_3=n,b' %command%", "WINEDLLOVERRIDES='xinput1_3=n,b' %command%"},
        {"WINEDLLOVERRIDES=\"dinput8=n,b\" %command%", "WINEDLLOVERRIDES=\"xinput1_3=n,b;dinput8=n,b\" %command%"},
        {"WINEDLLOVERRIDES=dinput8=n %command%", "WINEDLLOVERRIDES=\"xinput1_3=n,b;dinput8=n\" %command%"},
        {"WINEDLLOVERRIDES=\"xinput1_3=b\" %command%", "WINEDLLOVERRIDES=\"xinput1_3=n,b\" %command%"},
        {"WINEDLLOVERRIDES=\"xinput1_3,d3d9=b\" %command%", "WINEDLLOVERRIDES=\"xinput1_3=n,b;d3d9=b\" %command%"},
        {"PROTON_LOG=1 WINEDLLOVERRIDES=\"xinput1_3.dll=native,builtin\" %command%",
         "PROTON_LOG=1 WINEDLLOVERRIDES=\"xinput1_3.dll=native,builtin\" %command%"},
        {"  MY_WINEDLLOVERRIDES=x %command%  ", "WINEDLLOVERRIDES=\"xinput1_3=n,b\" MY_WINEDLLOVERRIDES=x %command%"},
    };
    for (const auto& [before, after] : cases) {
        CHECK_EQ(WithOverride(before), after);
        CHECK_EQ(WithOverride(after), after);
    }
}

static const std::string kLocalConfig =
    "\"UserLocalConfigStore\"\n{\n"
    "\t\"Software\"\n\t{\n"
    "\t\t\"Valve\"\n\t\t{\n"
    "\t\t\t\"Steam\"\n\t\t\t{\n"
    "\t\t\t\t\"SteamDefaultDialog\"\t\t\"#app_games\"\n"
    "\t\t\t\t\"apps\"\n\t\t\t\t{\n"
    "\t\t\t\t\t\"1091500\"\n\t\t\t\t\t{\n"
    "\t\t\t\t\t\t\"LaunchOptions\"\t\t\"gamemoderun %command%\"\n"
    "\t\t\t\t\t}\n"
    "\t\t\t\t\t\"310950\"\n\t\t\t\t\t{\n"
    "\t\t\t\t\t\t\"LastPlayed\"\t\t\"1762900000\"\n"
    "\t\t\t\t\t\t\"cloud\"\n\t\t\t\t\t\t{\n"
    "\t\t\t\t\t\t\t\"last_sync_state\"\t\t\"synchronized\"\n"
    "\t\t\t\t\t\t}\n"
    "%OPTIONS%"
    "\t\t\t\t\t}\n"
    "\t\t\t\t}\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t}\n"
    "\t\"apptickets\"\n\t{\n"
    "\t\t\"310950\"\t\t\"AAAA\\\\BB\\\"CC\"\n"
    "\t}\n"
    "}\n";

static std::string Replace(std::string s, const std::string& from, const std::string& to)
{
    const size_t at = s.find(from);
    if (at != std::string::npos) s.replace(at, from.size(), to);
    return s;
}

static std::string Set(const std::string& before, const std::string& options)
{
    std::string after, error;
    CHECK(SetLaunchOptions(before, options, &after, &error));
    CHECK(GetLaunchOptions(after) == std::optional<std::string>(options));
    std::vector<VdfEntry> tree;
    CHECK(VdfParse(after, &tree, &error));
    return after;
}

static void Vdf()
{
    const std::string optLine = "\t\t\t\t\t\t\"LaunchOptions\"\t\t\"-dx11\"\n";
    {
        const std::string before = Replace(kLocalConfig, "%OPTIONS%", optLine);
        CHECK(GetLaunchOptions(before) == std::optional<std::string>("-dx11"));
        const std::string after = Set(before, "A=\"x y\" %command% -dx11");
        CHECK_EQ(after, Replace(before, "\"-dx11\"", "\"A=\\\"x y\\\" %command% -dx11\""));
        CHECK(after.find("\"LaunchOptions\"\t\t\"gamemoderun %command%\"") != std::string::npos);
        CHECK(after.find("\"AAAA\\\\BB\\\"CC\"") != std::string::npos);
    }
    {
        const std::string before = Replace(kLocalConfig, "%OPTIONS%", "");
        CHECK(GetLaunchOptions(before) == std::optional<std::string>(""));
        const std::string after = Set(before, "X %command%");
        CHECK_EQ(Replace(after, "\t\t\t\t\t\t\"LaunchOptions\"\t\t\"X %command%\"\n", ""), before);
    }
    {
        const std::string before = Replace(Replace(kLocalConfig, "%OPTIONS%", ""), "\"310950\"\n\t\t\t\t\t{", "\"999\"\n\t\t\t\t\t{");
        const std::string after = Set(before, "X %command%");
        CHECK(after.find("\t\t\t\t\t\"310950\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"LaunchOptions\"\t\t\"X %command%\"\n\t\t\t\t\t}\n\t\t\t\t}") !=
              std::string::npos);
    }
    {
        Set("\"UserLocalConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n\t\t\t}\n\t\t}\n\t}\n}\n",
            "X %command%");
    }
    {
        std::string after, error;
        CHECK(!SetLaunchOptions("\"UserLocalConfigStore\"\n{\n}\n", "X", &after, &error));
        CHECK(!GetLaunchOptions("\"a\"\n{\n").has_value());
    }
    {
        const std::string text = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"/home/deck/.local/share/Steam\"\n"
                                 "\t\t\"apps\"\n\t\t{\n\t\t\t\"310950\"\t\t\"1\"\n\t\t}\n\t}\n"
                                 "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"/run/media/deck/SD card\"\n\t}\n}\n";
        const auto paths = LibraryPaths(text);
        CHECK(paths.size() == 2 && paths[0] == "/home/deck/.local/share/Steam" && paths[1] == "/run/media/deck/SD card");
        CHECK_EQ(InstallDir("\"AppState\"\n{\n\t\"appid\"\t\t\"310950\"\n\t\"installdir\"\t\t\"StreetFighterV\"\n}\n"),
                 std::string("StreetFighterV"));
    }
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "tests";
    Signature(dir);
    Paths();
    Override();
    Vdf();
    std::printf("%d of %d checks passed\n", g_checked - g_failed, g_checked);
    return g_failed ? 1 : 0;
}
