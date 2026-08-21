#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s --output FILE --status FILE command [args ...]\n",
            program);
}

static int child_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return EXIT_FAILURE;
}

int main(int argc, char **argv) {
    if (argc < 7 || strcmp(argv[1], "--output") != 0 ||
        strcmp(argv[3], "--status") != 0 || strcmp(argv[5], "--") != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *output_path = argv[2];
    const char *status_path = argv[4];
    char **command = &argv[6];

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }
    if (pid == 0) {
        int fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror(output_path);
            _exit(EXIT_FAILURE);
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("dup2");
            close(fd);
            _exit(EXIT_FAILURE);
        }
        close(fd);
        execvp(command[0], command);
        perror(command[0]);
        _exit(127);
    }

    int wait_status;
    while (waitpid(pid, &wait_status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return EXIT_FAILURE;
        }
    }

    int status = child_status(wait_status);
    int fd = open(status_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror(status_path);
        return EXIT_FAILURE;
    }
    if (dprintf(fd, "%d\n", status) < 0) {
        perror("write status");
        close(fd);
        return EXIT_FAILURE;
    }
    if (close(fd) < 0) {
        perror("close status");
        return EXIT_FAILURE;
    }
    return status;
}
