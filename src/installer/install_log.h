#pragma once
// The installer's log: polarity-installer.log, where a player can find it
// and send it when something goes wrong. Each line is written straight to
// the file, so a crash loses nothing before it. Runs append to the same file;
// one past 1 MB is started over.

#include <cstddef>
#include <string>
#include <vector>

namespace polarity::installer::log {

// Opens the log in the first of `dirs` (UTF-8) it can write to. False when
// none works: the installer runs on without a log.
bool Open(const std::vector<std::string>& dirs);
// The log's full path (UTF-8), empty without one.
const std::string& Path();

// The player's home folder: written as ~ in the log, which the player may
// pass on.
void SetHome(const std::string& home);

// A line, with the time in front.
void Write(const std::string& line);

// For crash handlers: writes the bytes as they are. No lock, no allocation,
// safe in a signal handler.
void WriteRaw(const char* text, size_t size);

// "a\nb" -> "a / b": a dialog's text on one log line.
std::string OneLine(const std::string& text);

} // namespace polarity::installer::log
