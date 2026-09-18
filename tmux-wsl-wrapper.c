#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
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

static int run_at(char *const argv[], const char *stage, const char *directory)
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
        die(stage);
    if (pid == 0) {
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

static int run_and_wait(char *const argv[], const char *stage)
{
    return run_at(argv, stage, NULL);
}

/* show-options is a server command, but unlike has-session it also succeeds
 * when the server deliberately has no sessions. */
static int server_exists(const struct strings *socket_options)
{
    struct strings a = {0};
    size_t i;
    int status;

    push(&a, "tmux");
    for (i = 0; i < socket_options->n; i++)
        push(&a, socket_options->v[i]);
    push(&a, "show-options");
    push(&a, "-g");
    push(&a, "exit-empty");
    status = run_and_wait(a.v, "checking tmux server");
    free(a.v);
    if (status == 127) {
        fprintf(stderr, "checking tmux server: could not execute tmux\n");
        exit(EXIT_FAILURE);
    }
    return status == 0;
}

struct buffer {
    char *p;
    size_t n, cap;
};

static void append_char(struct buffer *b, char c)
{
    if (b->n + 2 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        char *p = realloc(b->p, cap);
        if (!p)
            die("realloc command line");
        b->p = p;
        b->cap = cap;
    }
    b->p[b->n++] = c;
    b->p[b->n] = '\0';
}

static void append_text(struct buffer *b, const char *s)
{
    while (*s)
        append_char(b, *s++);
}

/* Quote for cmd.exe and for the Windows CommandLineToArgvW/CRT convention.
 * Every argument is quoted, so cmd metacharacters are inert.  /v:off below
 * makes ! literal. Percent signs must be doubled to survive cmd expansion. */
static void append_cmd_arg(struct buffer *b, const char *s)
{
    size_t slashes = 0;

    append_char(b, '"');
    for (;;) {
        if (*s == '\\') {
            slashes++;
            s++;
            continue;
        }
        if (*s == '"') {
            while (slashes--)
                append_text(b, "\\\\");
            append_text(b, "\\\"");
            slashes = 0;
            s++;
            continue;
        }
        if (*s == '\0') {
            while (slashes--)
                append_text(b, "\\\\");
            break;
        }
        while (slashes--)
            append_char(b, '\\');
        slashes = 0;
        if (*s == '%')
            append_text(b, "%%");
        else
            append_char(b, *s);
        s++;
    }
    append_char(b, '"');
}

static int bootstrap(const struct strings *carry, const char *tmpdir)
{
    const char *distro = getenv("WSL_DISTRO_NAME");
    const char *pwd = getenv("PWD");
    char cwd[4096];
    struct strings wsl = {0};
    struct buffer command = {0};
    char *assignment = NULL;
    char *cmd_argv[7];
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
    push(&wsl, "tmux");
    for (i = 0; i < carry->n; i++)
        push(&wsl, carry->v[i]);
    push(&wsl, "start-server"); push(&wsl, ";");
    push(&wsl, "set-option"); push(&wsl, "-g");
    push(&wsl, "exit-empty"); push(&wsl, "off");

    append_text(&command, "start \"\" /wait /min ");
    for (i = 0; i < wsl.n; i++) {
        if (i)
            append_char(&command, ' ');
        append_cmd_arg(&command, wsl.v[i]);
    }
    cmd_argv[0] = CMD_EXE_PATH;
    cmd_argv[1] = "/d"; cmd_argv[2] = "/v:off"; cmd_argv[3] = "/s";
    cmd_argv[4] = "/c"; cmd_argv[5] = command.p; cmd_argv[6] = NULL;

    status = run_at(cmd_argv, "bootstrap cmd.exe", CMD_WORKDIR);
    if (status)
        fprintf(stderr, "bootstrap: cmd.exe exited with status %d\n", status);
    free(assignment); free(wsl.v); free(command.p);
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
    execvp("tmux", argv);
    perror("final exec tmux");
    return EXIT_FAILURE;
}
