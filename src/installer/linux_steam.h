#pragma once
// What the Linux installer (installer_linux.cpp) knows about Steam: its text
// key-values files (VDF), the game's launch options in an account's
// localconfig.vdf, and where the game is. No network, no windows: tested on
// its own (tests/linux_tests.cpp).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace polarity::installer {

inline constexpr const char* kAppId = "310950";
// Proton loads the game folder's xinput1_3.dll (the mod's loader) only with
// this override.
inline constexpr const char* kOverride = "xinput1_3=n,b";
inline constexpr const char* kLaunchHint = "WINEDLLOVERRIDES=\"xinput1_3=n,b\" %command%";

// One entry of a VDF file. Offsets point into the parsed text, so an edit
// can splice the file and leave the rest of it byte for byte: for a value,
// [vstart, vend) is its quoted token; for a block, vstart is its '{' and
// vend one past its '}'.
struct VdfEntry {
    std::string key;
    size_t kstart = 0, kend = 0;
    bool block = false;
    std::string value;
    std::vector<VdfEntry> children;
    size_t vstart = 0, vend = 0;
};

bool VdfParse(std::string_view text, std::vector<VdfEntry>* out, std::string* error);
// The block at a key path (keys compared without case), or null.
const VdfEntry* VdfFind(const std::vector<VdfEntry>& entries, const std::vector<std::string_view>& path);
std::string VdfQuote(std::string_view value);

// The game's launch options in a localconfig.vdf's text ("" when none are
// set); nullopt when the text does not parse.
std::optional<std::string> GetLaunchOptions(std::string_view text);
// localconfig.vdf's text with the game's launch options set, only that
// changed. False (with `error`) when there is no Software/Valve/Steam block.
bool SetLaunchOptions(std::string_view text, const std::string& options, std::string* out, std::string* error);
// Launch options that load the mod: the override added, the player's own
// options kept. The same options (trimmed) when they load it already.
std::string WithOverride(std::string_view options);

// The library folders a libraryfolders.vdf lists.
std::vector<std::string> LibraryPaths(std::string_view libraryfoldersText);
// The "installdir" of an appmanifest_310950.acf ("" if none).
std::string InstallDir(std::string_view acfText);

// The game's Binaries/Win64 for a folder the player picked: the library's
// StreetFighterV folder, the StreetFighterV inside it, Binaries or Win64.
std::optional<std::string> GameWin64(const std::string& picked);
// The library's StreetFighterV folder for a Binaries/Win64.
std::string GameRoot(const std::string& win64);

} // namespace polarity::installer
