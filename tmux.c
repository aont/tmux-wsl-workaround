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
#include <unistd.h>

#define SEP_CHAR '\037'
#define LAUNCH_TIMEOUT_MS 30000

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

static char *xstrdup(const char *s) {
    char *copy = strdup(s ? s : "");
    if (copy == NULL) die("strdup");
    return copy;
}

static char *xasprintf2(const char *a, const char *b) {
    size_t len = strlen(a) + strlen(b) + 1;
    char *out = malloc(len);
    if (out == NULL) die("malloc");
    memcpy(out, a, strlen(a));
    memcpy(out + strlen(a), b, strlen(b) + 1);
    return out;
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

static void passthrough(const char *tmux_bin, int argc, char **argv) {
    char **exec_argv = calloc((size_t)argc + 2, sizeof(*exec_argv));
    if (exec_argv == NULL) die("calloc");
    exec_argv[0] = (char *)tmux_bin;
    for (int i = 1; i <= argc; i++) exec_argv[i] = argv[i];
    exec_argv[argc + 1] = NULL;
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

static int drain_result_fd(int fd, char **session_id, int print_visible) {
    size_t cap = 128, len = 0;
    char *sid = malloc(cap);
    if (sid == NULL) die("malloc");
    int saw_sep = 0;
    for (;;) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n == 1) {
            if (!saw_sep) {
                if (ch == SEP_CHAR) {
                    saw_sep = 1;
                    sid[len] = '\0';
                } else {
                    if (len + 1 >= cap) {
                        cap *= 2;
                        char *new_sid = realloc(sid, cap);
                        if (new_sid == NULL) die("realloc");
                        sid = new_sid;
                    }
                    sid[len++] = ch;
                }
            } else if (print_visible) {
                if (write(STDOUT_FILENO, &ch, 1) < 0) die("write");
            }
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
    if (!saw_sep) {
        free(sid);
        return 0;
    }
    *session_id = sid;
    return 1;
}

int main(int argc, char **argv) {
    const char *tmux_bin = getenv("TMUX_BIN");
    const char *cmd_exe = getenv("CMD_EXE");
    const char *distro = getenv("WSL_DISTRO_NAME");
    if (!tmux_bin || !*tmux_bin) tmux_bin = "/usr/bin/tmux";
    if (!cmd_exe || !*cmd_exe) cmd_exe = "/mnt/c/Windows/System32/cmd.exe";
    if (!distro) distro = "";

    if (has_command_separator(argc - 1, argv)) passthrough(tmux_bin, argc - 1, argv);

    char start_dir[4096];
    if (getcwd(start_dir, sizeof(start_dir)) == NULL) die("getcwd");

    struct vec global = {0}, global_attach = {0}, new_opts = {0}, tail = {0};
    int i = 1;
    while (i < argc) {
        char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if ((strcmp(a, "-L") == 0 || strcmp(a, "-S") == 0)) {
            if (i + 1 >= argc) passthrough(tmux_bin, argc - 1, argv);
            vec_push(&global, a); vec_push(&global, argv[i + 1]);
            vec_push(&global_attach, a); vec_push(&global_attach, argv[i + 1]);
            i += 2; continue;
        }
        if ((strncmp(a, "-L", 2) == 0 || strncmp(a, "-S", 2) == 0) && a[2] != '\0') {
            char opt[3] = { a[0], a[1], 0 };
            vec_push(&global, opt); vec_push(&global, a + 2);
            vec_push(&global_attach, opt); vec_push(&global_attach, a + 2);
            i++; continue;
        }
        if (strcmp(a, "-C") == 0 || strcmp(a, "-CC") == 0 || strcmp(a, "-D") == 0 ||
            strcmp(a, "-h") == 0 || strcmp(a, "-V") == 0 || strcmp(a, "-c") == 0 ||
            strncmp(a, "-c", 2) == 0 || a[0] == '-') passthrough(tmux_bin, argc - 1, argv);
        break;
    }

    const char *subcmd = "new-session";
    if (i < argc) {
        subcmd = argv[i++];
        if (strcmp(subcmd, "new-session") != 0 && strcmp(subcmd, "new") != 0)
            passthrough(tmux_bin, argc - 1, argv);
    }

    int user_detached = 0, user_print = 0, user_format_specified = 0, no_update_env = 0;
    const char *user_format = "", *client_flags = NULL;
    int parsing = 1;
    while (i < argc) {
        char *a = argv[i++];
        if (!parsing) { vec_push(&tail, a); continue; }
        if (strcmp(a, "--") == 0) { parsing = 0; vec_push(&tail, "--"); continue; }
        if (strcmp(a, "-") == 0 || a[0] != '-') { parsing = 0; vec_push(&tail, a); continue; }
        char *cluster = a + 1;
        while (*cluster) {
            char ch = *cluster++;
            char opt[3] = { '-', ch, 0 };
            if (ch == 'A') passthrough(tmux_bin, argc - 1, argv);
            if (ch == 'd') { user_detached = 1; vec_push(&new_opts, opt); continue; }
            if (ch == 'D' || ch == 'E' || ch == 'P' || ch == 'X') {
                if (ch == 'E') no_update_env = 1;
                if (ch == 'P') user_print = 1;
                vec_push(&new_opts, opt); continue;
            }
            if (strchr("ceFfnstxy", ch) != NULL) {
                const char *val;
                vec_push(&new_opts, opt);
                if (*cluster) { val = cluster; cluster += strlen(cluster); }
                else { if (i >= argc) passthrough(tmux_bin, argc - 1, argv); val = argv[i++]; }
                vec_push(&new_opts, val);
                if (ch == 'F') { user_format_specified = 1; user_format = val; }
                if (ch == 'f') client_flags = val;
                continue;
            }
            passthrough(tmux_bin, argc - 1, argv);
        }
    }

    if (!*distro) {
        fprintf(stderr, "tmux wrapper: WSL_DISTRO_NAME must be set for FIFO transport\n");
        return 1;
    }

    const char *visible = user_print ? (user_format_specified ? user_format : "#{session_name}:") : "";
    char sep_s[2] = { SEP_CHAR, 0 };
    char *fmt_prefix = xasprintf2("#{session_id}", sep_s);
    char *internal_format = xasprintf2(fmt_prefix, visible);

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
    vec_push(&cmd, "-d"); vec_push(&cmd, "-P"); vec_push(&cmd, "-F"); vec_push(&cmd, internal_format); vec_extend(&cmd, &tail);

    if (chdir("/mnt/c") < 0) die("chdir /mnt/c");
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) { execv(cmd_exe, cmd.v); _exit(127); }

    int rfd = open(result_fifo, O_RDONLY | O_NONBLOCK);
    int sfd = open(status_fifo, O_RDONLY | O_NONBLOCK);
    if (rfd < 0 || sfd < 0) die("open fifo");

    int status = -1;
    int waited = 0;
    while (waited < LAUNCH_TIMEOUT_MS && status < 0) {
        if (read_status_fd(sfd, &status)) break;
        struct pollfd pfds[2] = {{ .fd = rfd, .events = POLLIN | POLLHUP }, { .fd = sfd, .events = POLLIN | POLLHUP }};
        poll(pfds, 2, 100);
        waited += 100;
    }
    if (status < 0) {
        fprintf(stderr, "tmux wrapper: timed out waiting for WSL launch result\n");
        return 1;
    }

    char *session_id = NULL;
    int got_session = drain_result_fd(rfd, &session_id, user_print);
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
    if (user_detached) return 0;

    struct vec attach = {0};
    vec_push(&attach, tmux_bin); vec_extend(&attach, &global_attach); vec_push(&attach, "attach-session");
    if (no_update_env) vec_push(&attach, "-E");
    if (client_flags) { vec_push(&attach, "-f"); vec_push(&attach, client_flags); }
    vec_push(&attach, "-t"); vec_push(&attach, session_id);
    execv(tmux_bin, attach.v);
    die(tmux_bin);
}
