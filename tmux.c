#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LAUNCH_TIMEOUT_MS 30000
#ifndef TMUX_BIN
#define TMUX_BIN "/usr/bin/tmux"
#endif
#ifndef CMD_EXE
#define CMD_EXE "/mnt/c/Windows/System32/cmd.exe"
#endif

struct vec {
    char **v;
    size_t n;
    size_t cap;
};

static char *tmpdir;

static void cleanup(void) {
    if (tmpdir != NULL) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/result.fifo", tmpdir);
        unlink(path);
        snprintf(path, sizeof(path), "%s/status.fifo", tmpdir);
        unlink(path);
        rmdir(tmpdir);
    }
}

static void on_signal(int sig) {
    cleanup();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static long long monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) die("clock_gettime");
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char *xstrdup(const char *s) {
    char *copy = strdup(s ? s : "");
    if (copy == NULL) die("strdup");
    return copy;
}

static void vec_push(struct vec *vec, const char *s) {
    if (vec->n + 1 >= vec->cap) {
        size_t new_cap = vec->cap ? vec->cap * 2 : 16;
        char **new_v = realloc(vec->v, new_cap * sizeof(*new_v));
        if (new_v == NULL) die("realloc");
        vec->v = new_v;
        vec->cap = new_cap;
    }
    vec->v[vec->n++] = xstrdup(s);
    vec->v[vec->n] = NULL;
}

static void vec_extend(struct vec *dst, const struct vec *src) {
    for (size_t i = 0; i < src->n; i++) vec_push(dst, src->v[i]);
}

static void save_terminal_title(void) {
    const char save[] = "\033[22;2t";
    ssize_t ignored = write(STDOUT_FILENO, save, sizeof(save) - 1);
    (void)ignored;
}

static void restore_terminal_title(void) {
    const char restore[] = "\033[23;2t";
    ssize_t ignored = write(STDOUT_FILENO, restore, sizeof(restore) - 1);
    (void)ignored;
}

static void passthrough(const char *tmux_bin, int argc, char **argv) {
    char **exec_argv = calloc((size_t)argc + 2, sizeof(*exec_argv));
    if (exec_argv == NULL) die("calloc");
    exec_argv[0] = (char *)tmux_bin;
    for (int i = 1; i <= argc; i++) exec_argv[i] = argv[i];
    exec_argv[argc + 1] = NULL;
    restore_terminal_title();
    execv(tmux_bin, exec_argv);
    die(tmux_bin);
}

static int has_command_separator(int argc, char **argv) {
    for (int i = 1; i <= argc; i++) if (strcmp(argv[i], ";") == 0) return 1;
    return 0;
}

static int plausible_session_id(const char *s) {
    if (s[0] != '$' || !isdigit((unsigned char)s[1])) return 0;
    for (size_t i = 2; s[i] != '\0'; i++) if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

static int read_status_fd(int fd, int *status) {
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    char *end = NULL;
    long val = strtol(buf, &end, 10);
    if (end == buf || val < 0 || val > 255) return 0;
    *status = (int)val;
    return 1;
}

static int read_session_id_fd(int fd, char **session_id) {
    size_t cap = 128, len = 0;
    char *sid = malloc(cap);
    if (sid == NULL) die("malloc");
    for (;;) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n == 1) {
            if (len + 1 >= cap) {
                cap *= 2;
                char *new_sid = realloc(sid, cap);
                if (new_sid == NULL) die("realloc");
                sid = new_sid;
            }
            sid[len++] = ch;
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLHUP };
            (void)poll(&pfd, 1, 100);
            continue;
        }
        die("read result fifo");
    }
    while (len > 0 && (sid[len - 1] == '\n' || sid[len - 1] == '\r')) len--;
    sid[len] = '\0';
    if (len == 0) {
        free(sid);
        return 0;
    }
    *session_id = sid;
    return 1;
}

static int run_and_wait(const struct vec *command) {
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        execv(command->v[0], command->v);
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) die("waitpid");
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int main(int argc, char **argv) {
    save_terminal_title();

    const char *tmux_bin = TMUX_BIN;
    const char *cmd_exe = CMD_EXE;
    const char *distro = getenv("WSL_DISTRO_NAME");
    if (!distro) distro = "";

    if (has_command_separator(argc - 1, argv)) passthrough(tmux_bin, argc - 1, argv);

    char start_dir[4096];
    if (getcwd(start_dir, sizeof(start_dir)) == NULL) die("getcwd");

    struct vec global = {0}, global_attach = {0}, new_opts = {0}, tail = {0};

    opterr = 0;
    optind = 1;
    int opt;
    while ((opt = getopt(argc, argv, "+L:S:CDhVc:")) != -1) {
        char opt_s[3] = { '-', (char)opt, 0 };
        if (opt == 'L' || opt == 'S') {
            vec_push(&global, opt_s); vec_push(&global, optarg);
            vec_push(&global_attach, opt_s); vec_push(&global_attach, optarg);
            continue;
        }
        passthrough(tmux_bin, argc - 1, argv);
    }

    int i = optind;
    const char *subcmd = "new-session";
    if (i < argc) {
        subcmd = argv[i++];
        if (strcmp(subcmd, "new-session") != 0 && strcmp(subcmd, "new") != 0)
            passthrough(tmux_bin, argc - 1, argv);
    }

    int user_detached = 0, user_print = 0, user_format_specified = 0, no_update_env = 0;
    const char *user_format = "", *client_flags = NULL;
    optind = i;
    while ((opt = getopt(argc, argv, "+AdEPXc:e:F:f:n:s:t:x:y:")) != -1) {
        char opt_s[3] = { '-', (char)opt, 0 };
        if (opt == 'A') passthrough(tmux_bin, argc - 1, argv);
        if (opt == 'd') user_detached = 1;
        if (opt == 'E') no_update_env = 1;
        if (opt == 'P') user_print = 1;
        if (opt == '?') passthrough(tmux_bin, argc - 1, argv);

        if (opt != 'F') vec_push(&new_opts, opt_s);
        if (strchr("ceFfnstxy", opt) != NULL) {
            if (opt != 'F') vec_push(&new_opts, optarg);
            if (opt == 'F') { user_format_specified = 1; user_format = optarg; }
            if (opt == 'f') client_flags = optarg;
        }
    }

    if (optind > i && strcmp(argv[optind - 1], "--") == 0) vec_push(&tail, "--");
    for (i = optind; i < argc; i++) vec_push(&tail, argv[i]);

    if (!*distro) {
        fprintf(stderr, "tmux wrapper: WSL_DISTRO_NAME must be set for FIFO transport\n");
        return 1;
    }

    char tmpl[4096];
    snprintf(tmpl, sizeof(tmpl), "%s/tmux-wsl-workaround.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    tmpdir = mkdtemp(tmpl);
    if (tmpdir == NULL) die("mkdtemp");
    tmpdir = xstrdup(tmpdir);
    atexit(cleanup);
    signal(SIGHUP, on_signal); signal(SIGINT, on_signal); signal(SIGTERM, on_signal);

    char result_fifo[4096], status_fifo[4096];
    snprintf(result_fifo, sizeof(result_fifo), "%s/result.fifo", tmpdir);
    snprintf(status_fifo, sizeof(status_fifo), "%s/status.fifo", tmpdir);
    if (mkfifo(result_fifo, 0600) < 0 || mkfifo(status_fifo, 0600) < 0) die("mkfifo");

    struct vec cmd = {0};
    vec_push(&cmd, cmd_exe); vec_push(&cmd, "/c"); vec_push(&cmd, "start"); vec_push(&cmd, "");
    vec_push(&cmd, "/min"); vec_push(&cmd, "wsl.exe"); vec_push(&cmd, "-d"); vec_push(&cmd, distro);
    vec_push(&cmd, "--cd"); vec_push(&cmd, start_dir); vec_push(&cmd, "--exec");
    vec_push(&cmd, "/bin/sh"); vec_push(&cmd, "-c");
    vec_push(&cmd, "fifo=$1; status_fifo=$2; shift 2; \"$@\" >\"$fifo\"; status=$?; printf '%s\\n' \"$status\" >\"$status_fifo\"; exit \"$status\"");
    vec_push(&cmd, "tmux-wsl-workaround"); vec_push(&cmd, result_fifo); vec_push(&cmd, status_fifo); vec_push(&cmd, tmux_bin);
    vec_extend(&cmd, &global); vec_push(&cmd, "new-session"); vec_extend(&cmd, &new_opts);
    vec_push(&cmd, "-d"); vec_push(&cmd, "-P"); vec_push(&cmd, "-F"); vec_push(&cmd, "#{session_id}"); vec_extend(&cmd, &tail);

    if (chdir("/mnt/c") < 0) die("chdir /mnt/c");
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) { execv(cmd_exe, cmd.v); _exit(127); }

    int rfd = open(result_fifo, O_RDONLY | O_NONBLOCK);
    int sfd = open(status_fifo, O_RDONLY | O_NONBLOCK);
    if (rfd < 0 || sfd < 0) die("open fifo");

    int status = -1;
    long long deadline = monotonic_ms() + LAUNCH_TIMEOUT_MS;
    while (status < 0) {
        if (read_status_fd(sfd, &status)) break;

        long long remaining = deadline - monotonic_ms();
        if (remaining <= 0) break;

        struct pollfd pfd = { .fd = sfd, .events = POLLIN };
        int timeout = remaining > 100 ? 100 : (int)remaining;
        int rc;
        do {
            rc = poll(&pfd, 1, timeout);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0) die("poll status fifo");
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            errno = EIO;
            die("poll status fifo");
        }
        if ((pfd.revents & POLLHUP) && !(pfd.revents & POLLIN)) {
            fprintf(stderr, "tmux wrapper: WSL launch closed status FIFO without returning a status\n");
            return 1;
        }
    }
    if (status < 0) {
        fprintf(stderr, "tmux wrapper: timed out waiting for WSL launch result\n");
        return 1;
    }

    char *session_id = NULL;
    int got_session = read_session_id_fd(rfd, &session_id);
    close(rfd); close(sfd);
    if (status != 0) return status;
    if (!got_session) {
        fprintf(stderr, "tmux wrapper: session ID was not returned\n");
        return 1;
    }
    if (!plausible_session_id(session_id)) {
        fprintf(stderr, "tmux wrapper: invalid session ID: %s\n", session_id);
        return 1;
    }

    if (user_print) {
        struct vec display = {0};
        vec_push(&display, tmux_bin); vec_extend(&display, &global);
        vec_push(&display, "display-message"); vec_push(&display, "-p");
        vec_push(&display, "-t"); vec_push(&display, session_id);
        vec_push(&display, "-F");
        vec_push(&display, user_format_specified ? user_format : "#{session_name}:");
        int display_status = run_and_wait(&display);
        if (display_status != 0) return display_status;
    }
    if (user_detached) return 0;

    struct vec attach = {0};
    vec_push(&attach, tmux_bin); vec_extend(&attach, &global_attach); vec_push(&attach, "attach-session");
    if (no_update_env) vec_push(&attach, "-E");
    if (client_flags) { vec_push(&attach, "-f"); vec_push(&attach, client_flags); }
    vec_push(&attach, "-t"); vec_push(&attach, session_id);
    restore_terminal_title();
    execv(tmux_bin, attach.v);
    die(tmux_bin);
}
