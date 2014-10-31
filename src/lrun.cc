////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2012-2014 Jun Wu <quark@zju.edu.cn>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
////////////////////////////////////////////////////////////////////////////////

#include "common.h"
#include "cgroup.h"
#include "fs.h"
#include "strconv.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <stropts.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <signal.h>
#include <fcntl.h>
#include <grp.h>

using namespace lrun;

using std::string;
using std::make_pair;

static struct {
    Cgroup::spawn_arg arg;
    double cpu_time_limit;
    double real_time_limit;
    long long memory_limit;
    long long output_limit;
    bool enable_devices_whitelist;
    bool enable_network;
    bool enable_pidns;
    bool pass_exitcode;
    useconds_t interval;
    string cgname;
    Cgroup* active_cgroup;

    std::vector<gid_t> groups;
    std::map<std::pair<Cgroup::subsys_id_t, std::string>, std::string> cgroup_options;
} config;

static volatile sig_atomic_t signal_triggered = 0;

static int get_terminal_width() {
    struct winsize ts;
    ioctl(0, TIOCGWINSZ, &ts);
    return ts.ws_col;
}

static string line_wrap(const string& content, size_t width, int indent, const string& join = "") {
    if (width <= 0) return content;

    string result;
    int line_size = 0;
    for (size_t i = 0; i < content.length(); ++i) {
        char c = content[i];
        if (c == ' ') {
            // should we break here?
            bool should_break = true;
            // look ahead for the next space char
            for (size_t j = i + 1; j <= content.length(); ++j) {
                char d = (j == content.length() ? ' ' : content[j]);
                if (d == ' ' && j - i + join.length() + line_size < width) {
                    should_break = false;
                    break;
                }
            }
            if (should_break) {
                result += join + "\n";
                for (int i = 0; i < indent; ++i) result += ' ';
                line_size = indent;
            } else {
                result += c;
                ++line_size;
            }
        } else {
            // can not split from here
            result += c;
            if (c == '\n') line_size = 0; else ++line_size;
        }
    }
    return result;
}

static void print_help(const string& submodule = "") {
    int width = isatty(STDERR_FILENO) ? (get_terminal_width() - 1) : -1;
    const int MIN_WIDTH = 60;
    if (width < MIN_WIDTH && width >= 0) width = MIN_WIDTH;
    string content;

    if (submodule == "syscalls") {
        content = line_wrap(
            "--syscall FILTER_STRING\n"
            "  Default action for unlisted syscalls is to return EPERM.\n"
            "\n"
            "--syscall !FILTER_STRING\n"
            "  Default action for unlisted syscalls is to allow.\n"
            "\n"
            , width, 2);
        content += line_wrap(
            "Format:\n"
            "  FILTER_STRING  := SYSCALL_RULE | FILTER_STRING + ',' + SYSCALL_RULE\n"
            "  SYSCALL_RULE   := SYSCALL_NAME + EXTRA_ARG_RULE + EXTRA_ACTION\n"
            "  EXTRA_ARG_RULE := '' | '[' + ARG_RULES + ']'\n"
            "  ARG_RULES      := ARG_RULE | ARG_RULES + ',' + ARG_RULE\n"
            "  ARG_RULE       := ARG_NAME + ARG_OP1 + NUMBER | ARG_NAME + ARG_OP2 + '=' + NUMBER\n"
            "  ARG_NAME       := 'a' | 'b' | 'c' | 'd' | 'e' | 'f'\n"
            "  ARG_OP1        := '==' | '=' | '!=' | '!' | '>' | '<' | '>=' | '<='\n"
            "  ARG_OP2        := '&'\n"
            "  EXTRA_ACTION   := '' | ':k' | ':e' | ':a'\n"
            "\n"
            , width, 20);
        content += line_wrap(
            "Notes:\n"
            "  ARG_NAME:     `a` for the first arg, `b` for the second, ...\n"
            "  ARG_OP1:      `=` is short for `==`, `!` is short for `!=`\n"
            "  ARG_OP2:      `&`: bitwise and\n"
            "  EXTRA_ACTION: `k` is to kill, `e` is to return EPERM, `a` is to allow\n"
            "  SYSCALL_NAME: syscall name or syscall number, ex: `read`, `0`, ...\n"
            "  NUMBER:       a decimal number containing only `0` to `9`\n"
            "\n"
            , width, 16);
        content += line_wrap(
            "Examples:\n"
            "  --syscalls 'read,write,open,exit'\n"
            "    Only read, write, open, exit are allowed\n"
            "  --syscalls '!write[a=2]'\n"
            "    Disallow write to fd 2 (stderr)\n"
            "  --syscalls '!sethostname:k'\n"
            "    Whoever calls sethostname will get killed\n"
            "  --syscalls '!clone[a&268435456==268435456]'\n"
            "    Do not allow a new user namespace to be created (CLONE_NEWUSER = 0x10000000)\n"
            , width, 4);
    } else {
        content =
            "Run program with resources limited.\n"
            "\n"
            "Usage: lrun [options] [--] command-args [3>stat]\n"
            "\n";
        string options =
            "Options:\n"
            "  --max-cpu-time    seconds     Limit cpu time. `seconds` can be a floating-point number\n"
            "  --max-real-time   seconds     Limit physical time\n"
            "  --max-memory      bytes       Limit memory (+swap) usage. `bytes` supports common suffix like `k`, `m`, `g`\n"
            "  --max-output      bytes       Limit output. Note: lrun will make a \"best  effort\" to enforce the limit but it is NOT accurate\n"
            "  --max-rtprio      n           Set max realtime priority\n"
            "  --max-nfile       n           Set max number of file descriptors\n"
            "  --max-stack       bytes       Set max stack size per process\n"
            "  --max-nprocess    n           Set RLIMIT_NPROC. Note: user namespace is not separated, current processes are counted\n"
            "  --isolate-process bool        Isolate PID, IPC namespace\n"
            "  --basic-devices   bool        Enable device whitelist: null, zero, full, random, urandom\n"
            "  --remount-dev     bool        Remount /dev and create only basic device files in it (see --basic-device)\n"
            "  --reset-env       bool        Clean environment variables\n"
            "  --network         bool        Whether network access is permitted\n"
            "  --pass-exitcode   bool        Discard lrun exit code, pass child process's exit code\n"
            "  --chroot          path        Chroot to specified `path` before exec\n"
            "  --chdir           path        Chdir to specified `path` after chroot\n"
            "  --nice            value       Add nice with specified `value`. Only root can use a negative value\n"
            "  --umask           int         Set umask\n"
            "  --uid             uid         Set uid (`uid` must > 0). Only root can use this\n"
            "  --gid             gid         Set gid (`gid` must > 0). Only root can use this\n"
            "  --no-new-privs    bool        Do not allow getting higher privileges using exec. This disables things like sudo, ping, etc. Only root can set it to false. Require Linux >= 3.5\n";
        if (seccomp::supported()) options +=
            "  --syscalls        syscalls    Apply a syscall filter. "
            " `syscalls` is basically a list of syscall names separated by ',' with an optional prefix '!'. If prefix '!' exists, it's a blacklist otherwise a whitelist."
            " For full syntax of `syscalls`, see `--help-syscalls`. Conflicts with `--no-new-privs false`\n";
        options +=
            "  --cgname          string      Specify cgroup name to use. The specified cgroup will be created on demand, and will not be deleted. If this option is not set, lrun will pick"
            " an unique cgroup name and destroy it upon exit.\n"
            "  --hostname        string      Specify a new hostname\n"
            "  --interval        seconds     Set interval status update interval\n"
#ifndef NDEBUG
            "  --debug                       Print debug messages\n"
            "  --status                      Show realtime resource usage status\n"
#endif
            "  --help                        Show this help\n";
        if (seccomp::supported()) options +=
            "  --help-syscalls               Show full syntax of `syscalls`\n";
        options +=
            "  --version                     Show version information\n"
            "\n"
            "Options that could be used multiple times:\n"
            "  --bindfs          dest src    Bind `src` to `dest`. This is performed before chroot. You should have read permission on `src`\n"
            "  --bindfs-ro       dest src    Like `--bindfs` but also make `dest` read-only\n"
            "  --tmpfs           path bytes  Mount writable tmpfs to specified `path` to hide filesystem subtree. `size` is in bytes. If it is 0, mount read-only."
            " This is performed after chroot. You should have write permission on `path`\n"
            "  --env             key value   Set environment variable before exec\n"
            "  --cgroup-option   subsys k v  Apply cgroup setting before exec\n"
            "  --fd              n           Do not close fd `n`\n"
            "  --cmd             cmd         Execute system command after tmpfs mounted. Only root can use this\n"
            "  --group           gid         Set additional groups. Applied to lrun itself. Only root can use this\n"
            "\n";
        content += line_wrap(options, width, 32);
        content += line_wrap(
            "Return value:\n"
            "  - If lrun is unable to execute specified command, non-zero is returned and nothing will be written to fd 3\n"
            "  - Otherwise, lrun will return 0 and output time, memory usage, exit status of executed command to fd 3\n"
            "  - If `--pass-exitcode` is set to true, lrun will just pass exit code of the child process\n"
            "\n"
            , width, 4);
        content += line_wrap(
            "Option processing order:\n"
            "  --hostname, --fd, --bindfs, --bindfs-ro, --chroot, (mount /proc), --tmpfs,"
            " --remount-dev, --chdir, --cmd, --umask, --gid, --uid, (rlimit options), --env, --nice,"
            " (cgroup limits), --syscalls\n"
            "\n"
            , width, 2);
        content += line_wrap(
            "Default options:\n"
            "  lrun --network true --basic-devices false --isolate-process true"
            " --remount-dev false --reset-env false --interval 0.02"
            " --pass-exitcode false --no-new-privs true"
            " --max-nprocess 2048 --max-nfile 256"
            " --max-rtprio 0 --nice 0\n"
            , width, 7, " \\");
    }

    fprintf(stderr, "%s\n", content.c_str());
    exit(0);
}

static void print_version() {
    printf("lrun " VERSION "\n"
           "Copyright (C) 2012-2014 Jun Wu <quark@zju.edu.cn>\n"
           "\n"
           "libseccomp support: %s\n"
           "debug support: %s\n",
           seccomp::supported() ? "yes" : "no",
#ifdef NDEBUG
           "no"
#else
           "yes"
#endif
           );
    exit(0);
}

static void init_default_config() {
    // default settings
    config.cpu_time_limit = -1;
    config.real_time_limit = -1;
    config.memory_limit = -1;
    config.output_limit = -1;
    config.enable_devices_whitelist = false;
    config.enable_network = true;
    config.enable_pidns = true;
    config.interval = (useconds_t)(0.02 * 1000000);
    config.active_cgroup = NULL;
    config.pass_exitcode = false;

    // arg settings
    config.arg.nice = 0;
    config.arg.uid = getuid();
    config.arg.gid = getgid();
    config.arg.umask = 022;
    config.arg.chroot_path = "";
    config.arg.chdir_path = "";
    config.arg.remount_dev = 0;
    config.arg.reset_env = 0;
    config.arg.no_new_privs = true;
    config.arg.clone_flags = 0;

    // arg.rlimits settings
    config.arg.rlimits[RLIMIT_NOFILE] = 256;
    config.arg.rlimits[RLIMIT_NPROC] = 2048;
    config.arg.rlimits[RLIMIT_RTPRIO] = 0;
    config.arg.rlimits[RLIMIT_CORE] = 0;
    config.arg.reset_env = 0;
    config.arg.syscall_action = seccomp::action_t::OTHERS_EPERM;
    config.arg.syscall_list = "";
}

static void parse_cli_options(int argc, char * argv[]) {
    config.arg.args = argv + 1;
    config.arg.argc = argc - 1;

#define REQUIRE_NARGV(n) \
    if (i + n >= argc) { \
        fprintf(stderr, "Option '%s' requires %d argument%s.\n", option.c_str(), n, n > 1 ? "s" : ""); \
        exit(1); \
    } else { \
        config.arg.argc -= n; \
        config.arg.args += n; \
    }
#define NEXT_STRING_ARG string(argv[++i])
#define NEXT_LONG_LONG_ARG (strconv::to_longlong(NEXT_STRING_ARG))
#define NEXT_DOUBLE_ARG (strconv::to_double(NEXT_STRING_ARG))
#define NEXT_BOOL_ARG (strconv::to_bool(NEXT_STRING_ARG))
    for (int i = 1; i < argc; ++i) {
        // break if it is not option
        if (strncmp("--", argv[i], 2) != 0) {
            config.arg.args = argv + i;
            config.arg.argc = argc - i;
            break;
        } else {
            config.arg.args = argv + i + 1;
            config.arg.argc = argc - i - 1;
        }

        string option = argv[i] + 2;

        if (option == "max-cpu-time") {
            REQUIRE_NARGV(1);
            config.cpu_time_limit = NEXT_DOUBLE_ARG;
        } else if (option == "max-real-time") {
            REQUIRE_NARGV(1);
            config.real_time_limit = NEXT_DOUBLE_ARG;
        } else if (option == "max-memory") {
            REQUIRE_NARGV(1);
            long long max_memory = strconv::to_bytes(NEXT_STRING_ARG);
            static const long long MIN_MEMORY_LIMIT = 500000LL;
            if (max_memory > 0 && max_memory < MIN_MEMORY_LIMIT) {
                WARNING("max-memory too small, changed to %lld.", MIN_MEMORY_LIMIT);
                max_memory = MIN_MEMORY_LIMIT;
            }
            config.memory_limit = max_memory;
        } else if (option == "max-output") {
            REQUIRE_NARGV(1);
            config.output_limit = strconv::to_bytes(NEXT_STRING_ARG);
            config.arg.rlimits[RLIMIT_FSIZE] = config.output_limit;
        } else if (option == "max-nprocess") {
            REQUIRE_NARGV(1);
            config.arg.rlimits[RLIMIT_NPROC] = NEXT_LONG_LONG_ARG;
        } else if (option == "min-nice") {
            // deprecated
            REQUIRE_NARGV(1);
            config.arg.rlimits[RLIMIT_NICE] = 20 - NEXT_LONG_LONG_ARG;
        } else if (option == "max-rtprio") {
            REQUIRE_NARGV(1);
            config.arg.rlimits[RLIMIT_RTPRIO] = NEXT_LONG_LONG_ARG;
        } else if (option == "max-nfile") {
            REQUIRE_NARGV(1);
            config.arg.rlimits[RLIMIT_NOFILE] = NEXT_LONG_LONG_ARG;
        } else if (option == "max-stack") {
            REQUIRE_NARGV(1);
            config.arg.rlimits[RLIMIT_STACK] = NEXT_LONG_LONG_ARG;
        } else if (option == "isolate-process") {
            REQUIRE_NARGV(1);
            config.enable_pidns = NEXT_BOOL_ARG;
        } else if (option == "basic-devices") {
            REQUIRE_NARGV(1);
            config.enable_devices_whitelist = NEXT_BOOL_ARG;
        } else if (option == "remount-dev") {
            REQUIRE_NARGV(1);
            config.arg.remount_dev = NEXT_BOOL_ARG;
        } else if (option == "reset-env") {
            REQUIRE_NARGV(1);
            config.arg.reset_env = (int)NEXT_BOOL_ARG;
        } else if (option == "network") {
            REQUIRE_NARGV(1);
            config.enable_network = NEXT_BOOL_ARG;
        } else if (option == "pass-exitcode") {
            REQUIRE_NARGV(1);
            config.pass_exitcode = NEXT_BOOL_ARG;
        } else if (option == "chroot") {
            REQUIRE_NARGV(1);
            config.arg.chroot_path = NEXT_STRING_ARG;
        } else if (option == "chdir") {
            REQUIRE_NARGV(1);
            config.arg.chdir_path = NEXT_STRING_ARG;
        } else if (option == "nice") {
            REQUIRE_NARGV(1);
            config.arg.nice = (int)NEXT_LONG_LONG_ARG;
        } else if (option == "umask") {
            REQUIRE_NARGV(1);
            config.arg.umask = (mode_t)NEXT_LONG_LONG_ARG;
        } else if (option == "uid") {
            REQUIRE_NARGV(1);
            config.arg.uid = (uid_t)NEXT_LONG_LONG_ARG;
        } else if (option == "gid") {
            REQUIRE_NARGV(1);
            config.arg.gid = (gid_t)NEXT_LONG_LONG_ARG;
        } else if (option == "no-new-privs") {
            REQUIRE_NARGV(1);
            config.arg.no_new_privs = NEXT_BOOL_ARG;
        } else if (option == "syscalls" && seccomp::supported()) {
            REQUIRE_NARGV(1);
            string syscalls = NEXT_STRING_ARG;

            config.arg.syscall_action = seccomp::action_t::DEFAULT_EPERM;
            switch (syscalls.data()[0]) {
                case '!': case '-':
                    config.arg.syscall_action = seccomp::action_t::OTHERS_EPERM;
                    /* intended no break */
                case '=': case '+':
                    config.arg.syscall_list = string(syscalls.data() + 1);
                    break;
                default:
                    config.arg.syscall_list = syscalls;
            }
        } else if (option == "group") {
            REQUIRE_NARGV(1);
            gid_t gid = (gid_t)NEXT_LONG_LONG_ARG;
            if (gid != 0) config.groups.push_back(gid);
        } else if (option == "interval") {
            REQUIRE_NARGV(1);
            useconds_t interval = (useconds_t)(NEXT_DOUBLE_ARG * 1000000);
            if (interval > 0) config.interval = interval;
        } else if (option == "cgname") {
            REQUIRE_NARGV(1);
            config.cgname = NEXT_STRING_ARG;
        } else if (option == "hostname") {
            REQUIRE_NARGV(1);
            config.arg.uts.nodename = NEXT_STRING_ARG;
            config.arg.clone_flags |= CLONE_NEWUTS;
        } else if (option == "domainname") {
            REQUIRE_NARGV(1);
            config.arg.uts.domainname = NEXT_STRING_ARG;
            config.arg.clone_flags |= CLONE_NEWUTS;
        // these 3 ones are undocumented, only available with utsmod.ko loaded
        // see https://github.com/quark-zju/mod_utsmod
        } else if (option == "ostype") {
            REQUIRE_NARGV(1);
            config.arg.uts.sysname = NEXT_STRING_ARG;
            config.arg.clone_flags |= CLONE_NEWUTS;
        } else if (option == "osrelease") {
            REQUIRE_NARGV(1);
            config.arg.uts.release = NEXT_STRING_ARG;
            config.arg.clone_flags |= CLONE_NEWUTS;
        } else if (option == "osversion") {
            REQUIRE_NARGV(1);
            config.arg.uts.version = NEXT_STRING_ARG;
            config.arg.clone_flags |= CLONE_NEWUTS;
        } else if (option == "remount-ro") {
            REQUIRE_NARGV(1);
            string dest = NEXT_STRING_ARG;
            config.arg.remount_list[dest] |= MS_RDONLY;
        } else if (option == "bindfs") {
            REQUIRE_NARGV(2);
            string dest = NEXT_STRING_ARG;
            string src = NEXT_STRING_ARG;
            config.arg.bindfs_list.push_back(make_pair(dest, src));
            config.arg.bindfs_dest_set.insert(dest);
        } else if (option == "bindfs-ro") {
            REQUIRE_NARGV(2);
            string dest = NEXT_STRING_ARG;
            string src = NEXT_STRING_ARG;
            config.arg.bindfs_list.push_back(make_pair(dest, src));
            config.arg.bindfs_dest_set.insert(dest);
            config.arg.remount_list[dest] |= MS_RDONLY;
        } else if (option == "tmpfs") {
            REQUIRE_NARGV(2);
            string path = NEXT_STRING_ARG;
            long long bytes = strconv::to_bytes(NEXT_STRING_ARG);
            config.arg.tmpfs_list.push_back(make_pair(path, bytes));
        } else if (option == "cgroup-option") {
            REQUIRE_NARGV(3);
            string subsys_name = NEXT_STRING_ARG;
            string key = NEXT_STRING_ARG;
            string value = NEXT_STRING_ARG;
            int subsys_id = Cgroup::subsys_id_from_name(subsys_name.c_str());
            if (subsys_id >= 0) {
                config.cgroup_options[make_pair((Cgroup::subsys_id_t)subsys_id, key)] = value;
            } else {
                WARNING("cgroup option '%s' = '%s' ignored:"
                        "subsystem '%s' not found",
                        key.c_str(), value.c_str(), subsys_name.c_str());
            }
        } else if (option == "env") {
            REQUIRE_NARGV(2);
            string key = NEXT_STRING_ARG;
            string value = NEXT_STRING_ARG;
            config.arg.env_list.push_back(make_pair(key, value));
        } else if (option == "fd") {
            REQUIRE_NARGV(1);
            config.arg.keep_fds.insert((int)NEXT_LONG_LONG_ARG);
        } else if (option == "cmd") {
            REQUIRE_NARGV(1);
            string cmd = NEXT_STRING_ARG;
            config.arg.cmd_list.push_back(cmd);
        } else if (option == "help") {
            print_help();
        } else if (option == "help-syscalls" && seccomp::supported()) {
            print_help("syscalls");
        } else if (option == "version") {
            print_version();
#ifndef NDEBUG
        } else if (option == "debug") {
            DEBUG_ENABLED = 1;
            DEBUG_PID = 1;
            DEBUG_TIMESTAMP = 1;
            DEBUG_PROGRESS = 0;
            DEBUG_START_TIME = now();
        } else if (option == "status") {
            DEBUG_PROGRESS = 1;
#endif
        } else if (option == "") {
            // meet --
            break;
        } else {
            fprintf(stderr, "Unknown option: `--%s`\nUse --help for information.\n", option.c_str());
            exit(1);
        }
    }
#undef REQUIRE_NARGV
#undef REQUIRE_ROOT
#undef NEXT_STRING_ARG
#undef NEXT_LONG_LONG_ARG
#undef NEXT_DOUBLE_ARG
#undef NEXT_BOOL_ARG
}

static string access_mode_to_str(int mode) {
    string result;
    if (mode & R_OK) result += "r";
    if (mode & W_OK) result += "w";
    if (mode & X_OK) result += "x";
    return result;
}

static void check_path_permission(const string& path, std::vector<string>& error_messages, int mode = R_OK) {
    // path should be absolute and accessible
    if (!fs::is_absolute(path)) {
        error_messages.push_back(
                string("Relative paths are forbidden for non-root users.\n")
                + "Please change: " + path);
        return;
    }

    if (fs::is_dir(path)) mode |= X_OK;
    if (!fs::is_accessible(path, mode)) {
        error_messages.push_back(
                string("You do not have `") + access_mode_to_str(mode)
                + "` permission on " + path);
    }
}

static string follow_binds(const std::vector<std::pair<string, string> >& binds, const string& path) {
    // only handle absolute paths
    if (!fs::is_absolute(path)) return path;
    string result = fs::expand(path);
    for (int i = binds.size() - 1; i >= 0; --i) {
        string prefix = binds[i].first + "/";
        if (result.substr(0, prefix.length()) == prefix) {
            // once is enough, because binds[i].second already followed previous binds
            result = binds[i].second + result.substr(prefix.length() - 1);
            break;
        }
    }
    return result;
}

static void check_config() {
    int is_root = (getuid() == 0);
    std::vector<string> error_messages;

    if (config.arg.uid == 0) {
        error_messages.push_back(
                "For security reason, running commands with uid = 0 is not allowed.\n"
                "Please specify a user ID using `--uid`.");
    } else if (!is_root && config.arg.uid != getuid()) {
        error_messages.push_back(
                "For security reason, setting uid to other user requires root.");
    }

    if (config.arg.gid == 0) {
        error_messages.push_back(
                "For security reason, running commands with gid = 0 is not allowed.\n"
                "Please specify a group ID using `--gid`.");
    } else if (!is_root && config.arg.gid != getgid()) {
        error_messages.push_back(
                "For security reason, setting gid to other group requires root.");
    }

    if (config.arg.argc <= 0) {
        error_messages.push_back(
                "command_args can not be empty.\n"
                "Use `--help` to see full options.");
    }

    if (!is_root) {
        if (config.arg.cmd_list.size() > 0) {
            error_messages.push_back(
                    "For security reason, `--cmd` requires root.");
        }

        if (config.groups.size() > 0) {
            error_messages.push_back(
                    "For security reason, `--group` requires root.");
        }

        // check paths, require absolute paths and read permissions
        // check --bindfs
        std::vector<std::pair<string, string> > binds;
        FOR_EACH(p, config.arg.bindfs_list) {
            const string& dest = p.first;
            const string& src = p.second;
            check_path_permission(follow_binds(binds, src), error_messages);
            binds.push_back(make_pair(fs::expand(dest), follow_binds(binds, fs::expand(src))));
        }

        // check --chroot
        string chroot_path = config.arg.chroot_path;
        if (!chroot_path.empty()) {
            check_path_permission(follow_binds(binds, chroot_path), error_messages);
        }

        // check --chdir
        if (!config.arg.chdir_path.empty()) {
            string chdir_path = fs::join(chroot_path, config.arg.chdir_path);
            check_path_permission(follow_binds(binds, chdir_path), error_messages);
        }

        // restrict --remount-ro, only allows dest in --bindfs
        // because something like `--remount-ro /` affects outside world
        FOR_EACH(p, config.arg.remount_list) {
            string dest = p.first;
            if (!config.arg.bindfs_dest_set.count(dest)) {
                error_messages.push_back(
                        "For security reason, `--remount-ro A` is only allowed "
                        "if there is a `--bindfs A B`.");
            }
        }

        if (config.arg.no_new_privs == false) {
            error_messages.push_back(
                    "For security remount, `--no-new-privs false` is forbidden "
                    "for non-root users.");
        }

        if (config.arg.nice < 0) {
            error_messages.push_back(
                    "Non-root users cannot set a negative value of `--nice`");
        }
    }

    if (config.arg.syscall_list.empty() && config.arg.syscall_action == seccomp::action_t::DEFAULT_EPERM) {
        error_messages.push_back(
                "Syscall filter forbids all syscalls, which is not allowed.");

    }

    if (error_messages.size() > 0) {
        FOR_EACH(message, error_messages) {
            fprintf(stderr, "%s\n\n", message.c_str());
        }
        fprintf(stderr, "Please fix these errors and try again.\n");
        exit(1);
    }
}

static void check_environment() {
    // require root
    if (geteuid() != 0 || setuid(0)) {
        FATAL("lrun: root required. (current euid = %d, uid = %d)", geteuid(), getuid());
    }

    // normalize group
    int e = setgid(0);
    if (e) ERROR("setgid(0) failed");

    e = setgroups(config.groups.size(), &config.groups[0]);
    if (e) ERROR("setgroups failed");
}

#ifndef NOW
#include <sys/time.h>
static double now() {
    struct timeval t;
    gettimeofday(&t, 0);
    return t.tv_usec / 1e6 + t.tv_sec;
}
#endif

static void clean_cg_exit(Cgroup& cg, int exit_code) {
    INFO("cleaning and exiting with code = %d", exit_code);

    if (config.cgname.empty()) {
        if (cg.destroy()) WARNING("can not destroy cgroup");
    } else {
        cg.killall();
    }

    exit(exit_code);
}

static char get_process_state(pid_t pid) {
    FILE * fstatus = fopen((string(fs::PROC_PATH) + "/" + strconv::from_longlong(pid) + "/status").c_str(), "r");
    char state = 0;
    if (!fstatus) return 0;
    int ret = fscanf(fstatus, "%*[^\n] State: %c", &state);
    (void)ret;
    fclose(fstatus);
    return state;
}

static void signal_handler(int signal) {
    signal_triggered = signal;
}

#ifndef NDEBUG
// compile with -ldl
#include <dlfcn.h>
static struct LibSegFaultLoader {
    LibSegFaultLoader() {
        // try to load libSegFault.so
        // use `addr2line` if libSegFault doesn't resolve function names
        void *libSegFault = dlopen("libSegFault.so", RTLD_NOW);
        // log facility may not be initialized now, do not use INFO here
        (void)libSegFault;
    }
} _libSegFaultLoader;
#endif

static void setup_signal_handlers() {
    struct sigaction action;

    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    action.sa_handler = SIG_IGN;

    // ignore SIGPIPE so that a program reading fd 3 via a pipe may
    // close it earlier and lrun continues to do cleaning work
    sigaction(SIGPIPE, &action, NULL);
    sigaction(SIGALRM, &action, NULL);

    action.sa_handler = signal_handler;
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGTRAP, &action, NULL);
}

static void create_cgroup() {
    // pick an unique name and create a cgroup in filesystem
    string cgname = config.cgname;
    if (cgname.empty()) cgname = "lrun" + strconv::from_long((long)getpid());
    INFO("cgname = '%s'", cgname.c_str());

    // create or reuse group
    static Cgroup new_cg = Cgroup::create(cgname);

    if (!new_cg.valid()) FATAL("can not create cgroup '%s'", cgname.c_str());
    config.active_cgroup = &new_cg;
}

static void setup_cgroup() {
    Cgroup& cg = *config.active_cgroup;

    // assume cg is created just now and nobody has used it before.
    // initialize settings
    // device limits
    if (config.enable_devices_whitelist) {
        if (cg.limit_devices()) {
            ERROR("can not enable devices whitelist");
            clean_cg_exit(cg, 1);
        }
    }

    // memory limits
    if (config.memory_limit > 0) {
        if (cg.set_memory_limit(config.memory_limit)) {
            ERROR("can not set memory limit");
            clean_cg_exit(cg, 2);
        }
    }

    // some cgroup options, fail quietly
    cg.set(Cgroup::CG_MEMORY, "memory.swappiness", "0\n");

    // enable oom killer now so our buggy code won't freeze.
    // we will disable it later.
    cg.set(Cgroup::CG_MEMORY, "memory.oom_control", "0\n");

    // other cgroup options
    FOR_EACH(p, config.cgroup_options) {
        if (cg.set(p.first.first, p.first.second, p.second)) {
            ERROR("can not set cgroup option '%s' to '%s'", p.first.second.c_str(), p.second.c_str());
            clean_cg_exit(cg, 7);
        }
    }

    // reset cpu / memory usage and killall existing processes
    // not needed if cg can be guarnteed that is newly created
    cg.killall();

    if (cg.reset_usages()) {
        ERROR("can not reset cpu time / memory usage counter.");
        clean_cg_exit(cg, 4);
    }

    // rlimit time
    if (config.cpu_time_limit > 0) {
        config.arg.rlimits[RLIMIT_CPU] = (int)(ceil(config.cpu_time_limit));
    }
}

static int run_command() {
    Cgroup& cg = *config.active_cgroup;

    // fd 3 should not be inherited by child process
    if (fcntl(3, F_SETFD, FD_CLOEXEC)) {
        // ignore bad fd error
        if (errno != EBADF) {
            ERROR("can not set FD_CLOEXEC on fd 3");
            clean_cg_exit(cg, 5);
        }
    }

    // spawn child
    pid_t pid = 0;

    int& clone_flags = config.arg.clone_flags;
    if (!config.enable_network) clone_flags |= CLONE_NEWNET;
    if (config.enable_pidns) clone_flags |= CLONE_NEWPID | CLONE_NEWIPC;

    pid = cg.spawn(config.arg);

    if (pid <= 0) {
        // error messages are printed before
        clean_cg_exit(cg, 10 - pid);
    }

    // prepare signal handlers and make lrun "higher priority"
    setup_signal_handlers();
    if (nice(-5) == -1) ERROR("can not renice");

    INFO("entering main loop, watching pid %d", (int)pid);

    // monitor its cpu_usage and real time usage and memory usage
    double start_time = now();
    double deadline = config.real_time_limit > 0 ? start_time + config.real_time_limit : -1;

    // child process stat (set by waitpid)
    int stat = 0;

    // which limit exceed
    string exceeded_limit = "";

    for (bool running = true; running;) {
        // check signal
        if (signal_triggered) {
            fprintf(stderr, "Receive signal %d, exiting...\n", signal_triggered);
            fflush(stderr);
            clean_cg_exit(cg, 4);
        }

        // check stat
        int e = waitpid(pid, &stat, WNOHANG);

        if (e == pid) {
            // stat available
            if (WIFEXITED(stat) || WIFSIGNALED(stat)) {
                INFO("child exited");
                break;
            }
        } else if (e == -1) {
            // see what's wrong
            if (errno == ECHILD) {
                // strangely, this happens at the beginning (?)
                usleep(config.interval);
            }
        }

        // clean stat
        stat = 0;

        // check time limit exceed
        if (config.cpu_time_limit > 0 && cg.cpu_usage() >= config.cpu_time_limit) {
            exceeded_limit = "CPU_TIME";
            break;
        }

        // check realtime exceed
        if (deadline > 0 && now() >= deadline) {
            exceeded_limit = "REAL_TIME";
            break;
        }

        // check memory limit
        if (cg.memory_peak() >= config.memory_limit && config.memory_limit > 0) {
            exceeded_limit = "MEMORY";
            break;
        }

        // in case SIGCHILD is unreliable
        // check zombie manually here instead of waiting SIGCHILD
        if (get_process_state(pid) == 'Z') {
            INFO("child becomes zombie");
            running = false;
            // check waitpid again
            e = waitpid(pid, &stat, WNOHANG);
            if (e == -1) {
                // something goes wrong, give up
                clean_cg_exit(cg, 6);
            }
        }

        if (config.output_limit > 0) {
            cg.update_output_count();
            long long output_bytes = cg.output_usage();

            if (output_bytes > config.output_limit) {
                exceeded_limit = "OUTPUT";
                break;
            }

            PROGRESS_INFO("CPU %4.2f | REAL %4.1f | MEM %4.2f / %4.2fM | OUT %LdB",
            cg.cpu_usage(), now() - start_time, cg.memory_current() / 1.e6, cg.memory_peak() / 1.e6, output_bytes);
        } else {
            PROGRESS_INFO("CPU %4.2f | REAL %4.1f | MEM %4.2f / %4.2fM",
            cg.cpu_usage(), now() - start_time, cg.memory_current() / 1.e6, cg.memory_peak() / 1.e6);
        }

        // check empty
        if (cg.empty()) {
            INFO("no process remaining");
            running = false;
        }

        // sleep for a while
        usleep(config.interval);
    }

    PROGRESS_INFO("\nOUT OF RUNNING LOOP\n");

    // collect stats
    long long memory_usage = cg.memory_peak();
    if (config.memory_limit > 0 && memory_usage >= config.memory_limit) {
        memory_usage = config.memory_limit;
        exceeded_limit = "MEMORY";
    }

    double cpu_time_usage = cg.cpu_usage();
    if ((WIFSIGNALED(stat) && WTERMSIG(stat) == SIGXCPU) || (config.cpu_time_limit > 0 && cpu_time_usage >= config.cpu_time_limit)) {
        cpu_time_usage = config.cpu_time_limit;
        exceeded_limit = "CPU_TIME";
    }

    if (WIFSIGNALED(stat) && WTERMSIG(stat) == SIGXFSZ) {
        exceeded_limit = "OUTPUT";
    }

    double real_time_usage = now() - start_time;
    if (config.real_time_limit > 0 && real_time_usage >= config.real_time_limit) {
        real_time_usage = config.real_time_limit;
        exceeded_limit = "REAL_TIME";
    }

    char status_report[4096];

    snprintf(status_report, sizeof status_report,
            "MEMORY   %lld\n"
            "CPUTIME  %.3f\n"
            "REALTIME %.3f\n"
            "SIGNALED %d\n"
            "EXITCODE %d\n"
            "TERMSIG  %d\n"
            "EXCEED   %s\n",
            memory_usage, cpu_time_usage, real_time_usage,
            WIFSIGNALED(stat) ? 1 : 0,
            WEXITSTATUS(stat),
            WTERMSIG(stat),
            exceeded_limit.empty() ? "none" : exceeded_limit.c_str());

    int ret = write(3, status_report, strlen(status_report));
    (void)ret;

    // close output earlier so the process read the status can start to do other things.
    close(3);

    return config.pass_exitcode ? WEXITSTATUS(stat) : EXIT_SUCCESS;
}

int main(int argc, char * argv[]) {
    if (argc <= 1) print_help();

    init_default_config();
    parse_cli_options(argc, argv);

    check_config();
    check_environment();

    INFO("lrun %s pid = %d", VERSION, (int)getpid());

    create_cgroup();

    {
        Cgroup& cg = *config.active_cgroup;
        // lock the cgroup so other lrun process with same cgname will wait
        fs::ScopedFileLock cg_lock(cg.subsys_path().c_str());
        setup_cgroup();
        int ret = run_command();
        clean_cg_exit(cg, ret);
    }

    return 0;
}
