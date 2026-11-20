#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include "ttypt/libren.h"
#include <ttypt/xy.h>

static int exec_bash(const char *args, char **out_result) {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        *out_result = strdup("Error: pipe failed");
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        *out_result = strdup("Error: fork failed");
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", args, NULL);
        exit(1);
    }

    close(pipefd[1]);
    size_t cap = 4096;
    size_t len = 0;
    *out_result = malloc(cap);
    char buf[256];
    int status;
    while (1) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;
        if (len + n >= cap) {
            cap *= 2;
            *out_result = realloc(*out_result, cap);
        }
        memcpy(*out_result + len, buf, n);
        len += n;
    }
    (*out_result)[len] = '\0';
    close(pipefd[0]);
    waitpid(pid, &status, 0);
    return WEXITSTATUS(status);
}

XY_MODULE_API void xy_install(void) {
    ren_register_tool("bash", "bash <command>", exec_bash);
}
