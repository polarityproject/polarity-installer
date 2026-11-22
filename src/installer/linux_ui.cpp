// The windows and questions: zenity, kdialog or the terminal, and the log of what they asked.

#include "installer/linux_internal.h"

namespace polarity::installer {

using namespace detail;

namespace {

class TerminalUi : public Ui {
public:
    explicit TerminalUi(bool answerYes) { yes = answerYes; }

    void Tell(const std::string& heading, const std::string& text, const char* kind) override {
        (std::strcmp(kind, "error") == 0 ? std::cerr : std::cout) << "\n"
                                                                  << heading << "\n"
                                                                  << text << std::endl;
    }

    int Choose(const std::string& heading, const std::string& text, const std::vector<std::string>& buttons) override {
        std::cout << "\n"
                  << heading << "\n"
                  << text << std::endl;
        if (yes) return 0;
        for (size_t i = 0; i < buttons.size(); i++) std::cout << "  " << i + 1 << ". " << buttons[i] << "\n";
        for (;;) {
            std::cout << "Choose 1-" << buttons.size() << " (Enter: 1, q: cancel): " << std::flush;
            std::string answer;
            if (!std::getline(std::cin, answer)) return -1;
            if (answer == "q" || answer == "Q") return -1;
            if (answer.empty()) return 0;
            const int n = std::atoi(answer.c_str());
            if (n >= 1 && n <= static_cast<int>(buttons.size())) return n - 1;
        }
    }

    std::optional<std::string> PickFolder(const std::string& title, const std::string&) override {
        if (yes) return std::nullopt;
        std::cout << title << " (the folder path, Enter to cancel): " << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer) || answer.empty()) return std::nullopt;
        return answer;
    }

    void ProgressStart(const std::string& heading) override {
        std::cout << heading << std::endl;
        percent_ = -1;
    }

    bool Progress(double fraction, const std::string& text) override {
        const int percent = static_cast<int>(fraction * 100);
        if (percent != percent_) {
            percent_ = percent;
            std::printf("\r  %3d%%  %s   ", percent, text.c_str());
            std::fflush(stdout);
        }
        return true;
    }

    void ProgressEnd() override {
        if (percent_ >= 0) std::cout << std::endl;
        percent_ = -1;
    }

private:
    int percent_ = -1;
};

} // namespace

namespace detail {

std::string Markup(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out += c;
    }
    return out;
}

} // namespace detail

namespace {

class ZenityUi : public Ui {
public:
    void Tell(const std::string& heading, const std::string& text, const char* kind) override {
        const std::string k = std::strcmp(kind, "error") == 0 ? "--error" : std::strcmp(kind, "warning") == 0 ? "--warning"
                                                                                                              : "--info";
        Run({k, "--text=<b>" + Markup(heading) + "</b>\n\n" + Markup(text)}, nullptr);
    }

    int Choose(const std::string& heading, const std::string& text, const std::vector<std::string>& buttons) override {
        std::vector<std::string> args{"--question", "--text=<b>" + Markup(heading) + "</b>\n\n" + Markup(text),
                                      "--ok-label=" + buttons[0], "--cancel-label=Cancel"};
        for (size_t i = 1; i < buttons.size(); i++) args.push_back("--extra-button=" + buttons[i]);
        std::string out;
        const int code = Run(args, &out);
        if (code == 0) return 0;
        for (size_t i = 1; i < buttons.size(); i++) {
            if (out == buttons[i]) return static_cast<int>(i);
        }
        return -1;
    }

    std::optional<std::string> PickFolder(const std::string& title, const std::string& start) override {
        std::vector<std::string> args{"--file-selection", "--directory", "--title=" + title};
        if (!start.empty()) args.push_back("--filename=" + start + "/");
        std::string out;
        if (Run(args, &out) != 0 || out.empty()) return std::nullopt;
        return out;
    }

    void ProgressStart(const std::string& heading) override {
        heading_ = heading;
        int fds[2];
        if (pipe(fds) != 0) return;
        pid_ = fork();
        if (pid_ == 0) {
            const int nul = open("/dev/null", O_RDWR);
            dup2(fds[0], 0);
            dup2(nul, 1);
            dup2(nul, 2);
            close(fds[0]);
            close(fds[1]);
            auto argv = Argv({"zenity", "--progress", std::string("--title=") + kTitle, "--width=460", "--auto-close",
                              "--text=" + Markup(heading), "--percentage=0"});
            execvp(argv[0], argv.data());
            _exit(127);
        }
        close(fds[0]);
        fd_ = pid_ > 0 ? fds[1] : -1;
        if (pid_ <= 0) close(fds[1]);
    }

    bool Progress(double fraction, const std::string& text) override {
        if (pid_ <= 0) return true;
        int status;
        if (waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = 0;
            return false;
        }
        const std::string line = std::to_string(std::min(99, static_cast<int>(fraction * 100))) + "\n# " +
                                 Markup(heading_) + " (" + text + ")\n";
        return write(fd_, line.data(), line.size()) == static_cast<ssize_t>(line.size());
    }

    void ProgressEnd() override {
        if (fd_ >= 0) {
            const char done[] = "100\n";
            (void)!write(fd_, done, sizeof(done) - 1);
            close(fd_);
            fd_ = -1;
        }
        if (pid_ > 0) {
            int status;
            waitpid(pid_, &status, 0);
        }
        pid_ = 0;
    }

private:
    int Run(std::vector<std::string> args, std::string* out) {
        args.insert(args.begin(), {"zenity", std::string("--title=") + kTitle, "--width=460"});
        std::string ignored;
        return RunCapture(args, out ? out : &ignored);
    }

    std::string heading_;
    pid_t pid_ = 0;
    int fd_ = -1;
};

class KdialogUi : public TerminalUi {
public:
    KdialogUi() : TerminalUi(false) {}

    void Tell(const std::string& heading, const std::string& text, const char* kind) override {
        Run({std::strcmp(kind, "error") == 0 ? "--error" : "--msgbox", heading + "\n\n" + text}, nullptr);
    }

    int Choose(const std::string& heading, const std::string& text, const std::vector<std::string>& buttons) override {
        const std::string body = heading + "\n\n" + text;
        if (buttons.size() == 1) {
            return Run({"--yesno", body, "--yes-label=" + buttons[0], "--no-label=Cancel"}, nullptr) == 0 ? 0 : -1;
        }
        const int code = Run({"--yesnocancel", body, "--yes-label=" + buttons[0], "--no-label=" + buttons[1],
                              "--cancel-label=Cancel"},
                             nullptr);
        return code == 0 ? 0 : code == 1 ? 1
                                         : -1;
    }

    std::optional<std::string> PickFolder(const std::string&, const std::string& start) override {
        const char* home = std::getenv("HOME");
        std::string out;
        if (Run({"--getexistingdirectory", start.empty() ? std::string(home ? home : "/") : start}, &out) != 0 || out.empty()) {
            return std::nullopt;
        }
        return out;
    }

private:
    int Run(std::vector<std::string> args, std::string* out) {
        args.insert(args.begin(), {"kdialog", std::string("--title=") + kTitle});
        std::string ignored;
        return RunCapture(args, out ? out : &ignored);
    }
};

class LoggedUi : public Ui {
public:
    explicit LoggedUi(std::unique_ptr<Ui> inner) : inner_(std::move(inner)) { yes = inner_->yes; }

    void Tell(const std::string& heading, const std::string& text, const char* kind) override {
        Log(std::string("shown (") + kind + "): " + heading + " / " + log::OneLine(text));
        inner_->Tell(heading, text, kind);
    }

    int Choose(const std::string& heading, const std::string& text, const std::vector<std::string>& buttons) override {
        Log("asked: " + heading + " / " + log::OneLine(text));
        const int pick = inner_->Choose(heading, text, buttons);
        Log("  answer: " + (pick >= 0 && pick < static_cast<int>(buttons.size()) ? buttons[pick] : std::string("Cancel")));
        return pick;
    }

    std::optional<std::string> PickFolder(const std::string& title, const std::string& start) override {
        auto picked = inner_->PickFolder(title, start);
        Log(picked ? "folder picked: " + *picked : std::string("folder picker cancelled"));
        return picked;
    }

    void ProgressStart(const std::string& heading) override { inner_->ProgressStart(heading); }

    bool Progress(double fraction, const std::string& text) override {
        const bool go = inner_->Progress(fraction, text);
        if (!go) Log("progress window closed: cancelled");
        return go;
    }

    void ProgressEnd() override { inner_->ProgressEnd(); }

private:
    std::unique_ptr<Ui> inner_;
};

} // namespace

namespace detail {

std::string UiKind(const Options& o) {
    if (!o.yes && !o.terminal && (std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY"))) {
        if (!Which("zenity").empty()) return "zenity";
        if (!Which("kdialog").empty()) return "kdialog";
    }
    return "terminal";
}

std::unique_ptr<Ui> MakeUi(const Options& o, const std::string& kind) {
    std::unique_ptr<Ui> ui;
    if (kind == "zenity") ui = std::make_unique<ZenityUi>();
    else if (kind == "kdialog") ui = std::make_unique<KdialogUi>();
    else ui = std::make_unique<TerminalUi>(o.yes);
    return std::make_unique<LoggedUi>(std::move(ui));
}

} // namespace detail

} // namespace polarity::installer
