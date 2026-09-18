#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NEWWIN_LAUNCHER_PATH
#define NEWWIN_LAUNCHER_PATH "/usr/local/libexec/newwin_launcher.exe"
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

static int run_and_wait(char *const argv[], const char *stage, int quiet_stderr)
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
    struct strings command = {0};
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
    push(&command, NEWWIN_LAUNCHER_PATH);
    push(&command, "wsl.exe"); push(&command, "-d"); push(&command, (char *)distro);
    push(&command, "--cd"); push(&command, (char *)pwd); push(&command, "--exec");
    if (tmpdir) {
        size_t len = strlen(tmpdir) + sizeof("TMUX_TMPDIR=");
        assignment = malloc(len);
        if (!assignment)
            die("malloc TMUX_TMPDIR");
        snprintf(assignment, len, "TMUX_TMPDIR=%s", tmpdir);
        push(&command, "env");
        push(&command, assignment);
    }
    push(&command, TMUX_PATH);
    for (i = 0; i < carry->n; i++)
        push(&command, carry->v[i]);
    push(&command, "start-server"); push(&command, ";");
    push(&command, "set-option"); push(&command, "-g");
    push(&command, "exit-empty"); push(&command, "off");

    status = run_and_wait(command.v, "bootstrap launcher", 0);
    if (status)
        fprintf(stderr, "bootstrap: launcher exited with status %d\n", status);
    free(assignment); free(command.v);
    return status;
}

int main(int argc, char **argv)
{
    struct strings carry = {0}, socket_options = {0};
    int c, direct = 0;
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

    if (!direct && !server_exists(&socket_options) && bootstrap(&carry, tmpdir))
        return EXIT_FAILURE;
    execv(TMUX_PATH, argv);
    perror("final exec tmux");
    return EXIT_FAILURE;
}
