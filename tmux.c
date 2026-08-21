#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TMUX_BIN
#define TMUX_BIN "/usr/bin/tmux"
#endif
#ifndef CONSOLE_REDIRECT_EXE
#define CONSOLE_REDIRECT_EXE "/usr/local/libexec/console_redirect.exe"
#endif

struct vec {
    char **v;
    size_t n;
    size_t cap;
};

static void die(const char *msg) {
    perror(msg);
    exit(1);
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

static char *read_result_fd(int fd) {
    size_t cap = 128, len = 0;
    char *result = malloc(cap);
    if (result == NULL) die("malloc");
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *new_result = realloc(result, cap);
            if (new_result == NULL) die("realloc");
            result = new_result;
        }
        ssize_t n = read(fd, result + len, cap - len - 1);
        if (n > 0) { len += (size_t)n; continue; }
        if (n == 0) break;
        if (errno == EINTR) continue;
        die("read console redirect output");
    }
    result[len] = '\0';
    return result;
}

static int run_and_wait(char **argv) {
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        execv(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) die("waitpid");
    if (!WIFEXITED(status)) return 1;
    return WEXITSTATUS(status);
}

int main(int argc, char **argv) {
    save_terminal_title();

    const char *tmux_bin = TMUX_BIN;
    const char *console_redirect_exe = CONSOLE_REDIRECT_EXE;
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
        fprintf(stderr, "tmux wrapper: WSL_DISTRO_NAME must be set\n");
        return 1;
    }

    struct vec cmd = {0};
    vec_push(&cmd, console_redirect_exe); vec_push(&cmd, "wsl.exe"); vec_push(&cmd, "-d"); vec_push(&cmd, distro);
    vec_push(&cmd, "--cd"); vec_push(&cmd, start_dir); vec_push(&cmd, "--exec");
    vec_push(&cmd, tmux_bin);
    vec_extend(&cmd, &global); vec_push(&cmd, "new-session"); vec_extend(&cmd, &new_opts);
    vec_push(&cmd, "-d"); vec_push(&cmd, "-P"); vec_push(&cmd, "-F"); vec_push(&cmd, "#{session_id}"); vec_extend(&cmd, &tail);

    int output_pipe[2];
    if (pipe(output_pipe) < 0) die("pipe");
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        close(output_pipe[1]);
        execv(console_redirect_exe, cmd.v);
        perror(console_redirect_exe);
        _exit(127);
    }
    close(output_pipe[1]);

    char *session_id = read_result_fd(output_pipe[0]);
    close(output_pipe[0]);
    int wait_status;
    while (waitpid(pid, &wait_status, 0) < 0) if (errno != EINTR) die("waitpid");
    if (!WIFEXITED(wait_status)) return 1;
    if (WEXITSTATUS(wait_status) != 0) {
        fputs(session_id, stderr);
        return WEXITSTATUS(wait_status);
    }
    size_t session_id_len = strlen(session_id);
    if (session_id_len > 0 && session_id[session_id_len - 1] == '\n')
        session_id[--session_id_len] = '\0';
    if (session_id_len > 0 && session_id[session_id_len - 1] == '\r')
        session_id[--session_id_len] = '\0';
    if (session_id_len == 0) {
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
        int display_status = run_and_wait(display.v);
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
