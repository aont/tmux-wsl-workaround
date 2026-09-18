#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CMD_EXE_PATH
#define CMD_EXE_PATH "/mnt/c/Windows/System32/cmd.exe"
#endif
#ifndef CMD_WORKDIR
#define CMD_WORKDIR "/mnt/c"
#endif
#ifndef TMUX_PATH
#define TMUX_PATH "/usr/bin/tmux"
#endif

struct strings {
    char **v;
    size_t n, cap;
};

static void die(const char *where)
{
    perror(where);
    exit(EXIT_FAILURE);
}

static void push(struct strings *s, char *value)
{
    if (s->n + 1 >= s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        char **v = realloc(s->v, cap * sizeof(*v));
        if (!v)
            die("realloc argv");
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = value;
    s->v[s->n] = NULL;
}

static int run_at(char *const argv[], const char *stage, const char *directory,
                  int quiet_stderr)
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
        die(stage);
    if (pid == 0) {
        if (quiet_stderr) {
            int nullfd = open("/dev/null", O_WRONLY);
            if (nullfd >= 0) {
                (void)dup2(nullfd, STDERR_FILENO);
                close(nullfd);
            }
        }
        if (directory && chdir(directory) < 0) {
            perror("bootstrap chdir /mnt/c");
            _exit(127);
        }
        execvp(argv[0], argv);
        perror(stage);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            die(stage);
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    fprintf(stderr, "%s: child terminated by signal %d\n", stage,
            WTERMSIG(status));
    return 128 + WTERMSIG(status);
}

static int run_and_wait(char *const argv[], const char *stage, int quiet_stderr)
{
    return run_at(argv, stage, NULL, quiet_stderr);
}

/* Start a disposable server to ask tmux for the compiled-in server default.
 * The final command is intentionally part of the same invocation: when the
 * default is on, it also makes the verification server remove its socket. */
static int default_exit_empty(void)
{
    char *const check[] = {TMUX_PATH, "-L", "check", "start", ";", "show",
                           "-s", "-v", "exit-empty", ";", "set-option", "-g",
                           "exit-empty", "on", NULL};
    int output[2], status;
    pid_t pid;
    char value[16];
    ssize_t n, used = 0;

    if (pipe(output) < 0)
        die("checking exit-empty default");
    pid = fork();
    if (pid < 0)
        die("checking exit-empty default");
    if (pid == 0) {
        close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(output[1]);
        execv(TMUX_PATH, check);
        perror("checking exit-empty default");
        _exit(127);
    }
    close(output[1]);
    while (used < (ssize_t)sizeof(value) - 1 &&
           (n = read(output[0], value + used, sizeof(value) - 1 - used)) > 0)
        used += n;
    close(output[0]);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            die("checking exit-empty default");
    }
    value[used] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "checking exit-empty default: tmux command failed\n");
        exit(EXIT_FAILURE);
    }
    if (strcmp(value, "on\n") == 0 || strcmp(value, "on") == 0)
        return 1;
    if (strcmp(value, "off\n") == 0 || strcmp(value, "off") == 0)
        return 0;
    fprintf(stderr, "checking exit-empty default: unexpected value: %s", value);
    if (!strchr(value, '\n'))
        fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void ignore_signal(int signal_number)
{
    (void)signal_number;
}

static void restore_child_signals(void)
{
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
}

static int run_final_with_exit_empty(char **argv,
                                     const struct strings *socket_options)
{
    struct strings enable = {0};
    pid_t client, setter;
    int client_status, setter_status;
    size_t i;

    push(&enable, TMUX_PATH);
    for (i = 0; i < socket_options->n; i++)
        push(&enable, socket_options->v[i]);
    push(&enable, "set-option"); push(&enable, "-g");
    push(&enable, "exit-empty"); push(&enable, "on");

    signal(SIGHUP, ignore_signal);
    signal(SIGINT, ignore_signal);
    signal(SIGQUIT, ignore_signal);
    signal(SIGTERM, ignore_signal);

    client = fork();
    if (client < 0)
        die("fork final tmux");
    if (client == 0) {
        restore_child_signals();
        execv(TMUX_PATH, argv);
        perror("final exec tmux");
        _exit(127);
    }
    setter = fork();
    if (setter < 0)
        die("fork exit-empty setter");
    if (setter == 0) {
        restore_child_signals();
        execv(TMUX_PATH, enable.v);
        perror("setting exit-empty");
        _exit(127);
    }

    while (waitpid(client, &client_status, 0) < 0)
        if (errno != EINTR)
            die("waiting for final tmux");
    while (waitpid(setter, &setter_status, 0) < 0)
        if (errno != EINTR)
            die("waiting for exit-empty setter");
    free(enable.v);
    if (!WIFEXITED(setter_status) || WEXITSTATUS(setter_status) != 0)
        fprintf(stderr, "setting exit-empty: tmux command failed\n");
    if (WIFEXITED(client_status))
        return WEXITSTATUS(client_status);
    return 128 + WTERMSIG(client_status);
}

/* show-options is a server command, but unlike has-session it also succeeds
 * when the server deliberately has no sessions. */
static int server_exists(const struct strings *socket_options)
{
    struct strings a = {0};
    size_t i;
    int status;

    push(&a, TMUX_PATH);
    for (i = 0; i < socket_options->n; i++)
        push(&a, socket_options->v[i]);
    push(&a, "show-options");
    push(&a, "-g");
    push(&a, "exit-empty");
    status = run_and_wait(a.v, "checking tmux server", 1);
    free(a.v);
    if (status == 127) {
        fprintf(stderr, "checking tmux server: could not execute tmux\n");
        exit(EXIT_FAILURE);
    }
    return status == 0;
}

static int bootstrap(const struct strings *carry, const char *tmpdir)
{
    const char *distro = getenv("WSL_DISTRO_NAME");
    const char *pwd = getenv("PWD");
    char cwd[4096];
    struct strings wsl = {0};
    struct strings cmd = {0};
    char *assignment = NULL;
    size_t i;
    int status;

    if (!distro || !*distro) {
        fprintf(stderr, "bootstrap: WSL_DISTRO_NAME is not set\n");
        return 1;
    }
    if (!pwd) {
        pwd = getcwd(cwd, sizeof(cwd));
        if (!pwd) {
            perror("bootstrap getcwd");
            return 1;
        }
    }
    push(&wsl, "wsl.exe"); push(&wsl, "-d"); push(&wsl, (char *)distro);
    push(&wsl, "--cd"); push(&wsl, (char *)pwd); push(&wsl, "--exec");
    if (tmpdir) {
        size_t len = strlen(tmpdir) + sizeof("TMUX_TMPDIR=");
        assignment = malloc(len);
        if (!assignment)
            die("malloc TMUX_TMPDIR");
        snprintf(assignment, len, "TMUX_TMPDIR=%s", tmpdir);
        push(&wsl, "env");
        push(&wsl, assignment);
    }
    push(&wsl, TMUX_PATH);
    for (i = 0; i < carry->n; i++)
        push(&wsl, carry->v[i]);
    push(&wsl, "start-server"); push(&wsl, ";");
    push(&wsl, "set-option"); push(&wsl, "-g");
    push(&wsl, "exit-empty"); push(&wsl, "off");

    /* Keep each word as an argv element.  WSL interop can then serialize the
     * vector once; in particular, do not put a second, CRT-escaped command
     * line inside the /c argument (cmd.exe does not parse CRT quoting). */
    push(&cmd, CMD_EXE_PATH);
    push(&cmd, "/d"); push(&cmd, "/v:off"); push(&cmd, "/c");
    push(&cmd, "start"); push(&cmd, "");
    push(&cmd, "/wait"); push(&cmd, "/min");
    for (i = 0; i < wsl.n; i++)
        push(&cmd, wsl.v[i]);

    status = run_at(cmd.v, "bootstrap cmd.exe", CMD_WORKDIR, 0);
    if (status)
        fprintf(stderr, "bootstrap: cmd.exe exited with status %d\n", status);
    free(assignment); free(wsl.v); free(cmd.v);
    return status;
}

int main(int argc, char **argv)
{
    struct strings carry = {0}, socket_options = {0};
    int c, direct = 0, server_running, exit_empty_default = 0;
    const char *tmpdir = getenv("TMUX_TMPDIR");

    opterr = 0;
    while ((c = getopt(argc, argv, "+2CDhlNuVvc:f:L:S:T:")) != -1) {
        if (c == 'h' || c == 'V')
            direct = 1;
        if (c == 'f' || c == 'L' || c == 'S') {
            char option[3] = {'-', (char)c, '\0'};
            char *saved = strdup(option);
            if (!saved)
                die("strdup option");
            push(&carry, saved);
            push(&carry, optarg);
            if (c == 'L' || c == 'S') {
                push(&socket_options, saved);
                push(&socket_options, optarg);
            }
        } else if (c == '?') {
            /* tmux owns validation and diagnostics for unsupported syntax. */
            direct = 1;
        }
    }

    server_running = direct || server_exists(&socket_options);
    if (!server_running) {
        exit_empty_default = default_exit_empty();
        if (bootstrap(&carry, tmpdir))
            return EXIT_FAILURE;
    }
    if (!server_running && exit_empty_default)
        return run_final_with_exit_empty(argv, &socket_options);
    execv(TMUX_PATH, argv);
    perror("final exec tmux");
    return EXIT_FAILURE;
}
