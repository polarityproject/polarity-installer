// The log, and the crash report: signal handlers that write the stack and show a window.

#include "installer/linux_internal.h"

#include <sys/utsname.h>
#include <ucontext.h>

namespace polarity::installer {

using namespace detail;

namespace {

struct CrashWindow {
    std::vector<std::string> args;
    std::vector<char*> argv;
    std::string terminal;
} g_crashWindow;

} // namespace

namespace detail {

void PrepareCrashWindow(const std::string& uiKind) {
    const std::string& path = log::Path();
    const std::string heading = "Polarity Installer stopped unexpectedly";
    const std::string text = path.empty() ? "It couldn't write a log."
                                          : "Its log is at\n" + path + "\n\nSend that file to the Polarity team.";
    g_crashWindow.terminal = "\n" + heading + "\n" + text + "\n";
    const std::string program = uiKind == "terminal" ? "" : Which(uiKind);
    if (program.empty()) return;
    if (uiKind == "zenity") {
        g_crashWindow.args = {program, std::string("--title=") + kTitle, "--width=460", "--error",
                              "--text=<b>" + Markup(heading) + "</b>\n\n" + Markup(text)};
    } else {
        g_crashWindow.args = {program, std::string("--title=") + kTitle, "--error", heading + "\n\n" + text};
    }
    g_crashWindow.argv = Argv(g_crashWindow.args);
}

} // namespace detail

namespace {

void RawText(const char* s) {
    log::WriteRaw(s, std::strlen(s));
}

void RawNumber(uintptr_t v, int base) {
    char text[24];
    char* p = text + sizeof(text);
    *--p = '\0';
    do {
        *--p = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    if (base == 16) {
        *--p = 'x';
        *--p = '0';
    }
    RawText(p);
}

const char* SignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        case SIGABRT: return "SIGABRT";
        case SIGINT: return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGHUP: return "SIGHUP";
        default: return "?";
    }
}

extern "C" const char __ehdr_start[];
extern "C" const char __executable_start[];
extern "C" const char etext[];
uintptr_t g_slide = 0;

} // namespace

namespace detail {

void FindSlide() {
    uint16_t type = 0;
    std::memcpy(&type, __ehdr_start + 16, sizeof(type));
    g_slide = type == 3 ? reinterpret_cast<uintptr_t>(__ehdr_start) : 0;
}

} // namespace detail

namespace {

bool InCode(uintptr_t address) {
    return address >= reinterpret_cast<uintptr_t>(__executable_start) && address < reinterpret_cast<uintptr_t>(etext);
}

void RawStack(uintptr_t bp, uintptr_t sp) {
    for (int i = 0; i < 32 && bp >= sp && bp - sp < (8u << 20) && (bp & 7) == 0; i++) {
        const auto* frame = reinterpret_cast<const uintptr_t*>(bp);
        if (!InCode(frame[1])) break;
        RawText(" ");
        RawNumber(frame[1] - 1 - g_slide, 16);
        if (frame[0] <= bp) break;
        bp = frame[0];
    }
}

volatile sig_atomic_t g_stackWritten = 0;

void OnCrash(int sig, siginfo_t* info, void* context) {
    RawText("crash: signal ");
    RawNumber(static_cast<uintptr_t>(sig), 10);
    RawText(" (");
    RawText(SignalName(sig));
    RawText(")");
    if (sig != SIGABRT) {
        RawText(", address ");
        RawNumber(reinterpret_cast<uintptr_t>(info->si_addr), 16);
    }
    RawText("\n");
#if defined(__x86_64__)
    if (!g_stackWritten) {
        const auto* uc = static_cast<const ucontext_t*>(context);
        RawText("  stack: ");
        RawNumber(static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]) - g_slide, 16);
        RawStack(static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]), static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]));
        RawText("\n");
    }
#else
    (void)context;
#endif
    if (!g_crashWindow.argv.empty()) {
        const pid_t pid = fork();
        if (pid == 0) {
            execve(g_crashWindow.argv[0], g_crashWindow.argv.data(), environ);
            _exit(127);
        }
        int status;
        if (pid > 0) waitpid(pid, &status, 0);
    } else {
        (void)!write(2, g_crashWindow.terminal.data(), g_crashWindow.terminal.size());
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void OnStop(int sig) {
    RawText("stopped by signal ");
    RawNumber(static_cast<uintptr_t>(sig), 10);
    RawText(" (");
    RawText(SignalName(sig));
    RawText(")\n");
    signal(sig, SIG_DFL);
    raise(sig);
}

void OnTerminate() {
    std::string what = "std::terminate without an exception";
    if (const std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            what = std::string("uncaught exception: ") + x.what();
        } catch (...) {
            what = "uncaught exception of an unknown type";
        }
    }
    Log(what);
    const auto here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    RawText("  stack:");
    RawStack(here, here);
    RawText("\n");
    g_stackWritten = 1;
    std::abort();
}

} // namespace

namespace detail {

void InstallCrashHandlers() {
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(sig, &sa, nullptr);
    struct sigaction stop{};
    stop.sa_handler = OnStop;
    stop.sa_flags = SA_RESETHAND;
    sigemptyset(&stop.sa_mask);
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) sigaction(sig, &stop, nullptr);
    std::set_terminate(OnTerminate);
}

void OpenLog() {
    const char* home = std::getenv("HOME");
    if (home && *home) log::SetHome(home);
    if (log::Open({Dirname(RealPath("/proc/self/exe"))})) return;
    std::string state;
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg == '/') state = xdg;
    else if (home && *home) state = std::string(home) + "/.local/state";
    if (!state.empty()) {
        state += "/polarity-installer";
        MakeDirs(state);
    }
    log::Open({state, "/tmp"});
}

} // namespace detail

namespace {

std::string OsName() {
    for (const char* f : {"/etc/os-release", "/usr/lib/os-release"}) {
        const auto text = ReadFile(f, 64 * 1024);
        if (!text) continue;
        const size_t at = text->find("PRETTY_NAME=");
        if (at == std::string::npos) continue;
        std::string v = text->substr(at + 12, text->find('\n', at) - at - 12);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
        return v;
    }
    return "unknown";
}

} // namespace

namespace detail {

void LogStart(int argc, char** argv, const std::string& uiKind) {
    char when[64] = "?";
    const std::time_t now = std::time(nullptr);
    tm local{};
    if (localtime_r(&now, &local)) std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S %z", &local);
    log::WriteRaw("\n", 1);
    Log("=== Polarity Installer " POLARITY_INSTALLER_VERSION " (commit " POLARITY_INSTALLER_COMMIT "), Linux, " +
        std::string(when));
    std::string args;
    for (int i = 1; i < argc; i++) args += std::string(" ") + argv[i];
    Log("command:" + (args.empty() ? std::string(" (none)") : args));
    utsname u{};
    uname(&u);
    auto env = [](const char* name) {
        const char* v = std::getenv(name);
        return std::string(v && *v ? v : "-");
    };
    Log("system: " + OsName() + ", kernel " + u.release + " " + u.machine + ", desktop " + env("XDG_CURRENT_DESKTOP") +
        ", session " + env("XDG_SESSION_TYPE") + ", windows: " + uiKind);
}

[[gnu::noinline]] void TestCrash(const std::string& kind) {
    Log("crashing on purpose: " + kind);
    if (kind == "segv") {
        volatile uintptr_t address = 8;
        *reinterpret_cast<volatile int*>(address) = 1;
    } else if (kind == "throw") {
        throw std::runtime_error("a test exception");
    } else if (kind == "terminate") {
        std::terminate();
    }
}

} // namespace detail

} // namespace polarity::installer
