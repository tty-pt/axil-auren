#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <ttypt/stele.h>
#include "ttypt/libren.h"
#include <ttypt/xy.h>

static int exec_bash(const char *args, char **out_result) {
    if (!args) args = "";
#ifdef _WIN32
    const char *argv[] = { "cmd.exe", "/c", args, NULL };
#else
    const char *argv[] = { "/bin/sh", "-c", args, NULL };
#endif
    stl_proc_t proc;
    if (stl_spawn(argv, STL_SPAWN_STDOUT | STL_SPAWN_MERGE_STDERR | STL_SPAWN_NEW_GROUP, &proc) != 0) {
        *out_result = strdup("Error: spawn failed");
        return -1;
    }

    size_t cap = 4096;
    size_t len = 0;
    *out_result = malloc(cap);
    if (!*out_result) {
        stl_proc_kill(&proc, 1);
        stl_proc_wait(&proc, NULL, 1000);
        stl_proc_close(&proc);
        return -1;
    }

    char buf[256];
    for (;;) {
        ssize_t n = read(proc.stdout_fd, buf, sizeof(buf));
        if (n <= 0) break;
        if (len + (size_t)n >= cap) {
            cap *= 2;
            char *nb = realloc(*out_result, cap);
            if (!nb) break;
            *out_result = nb;
        }
        memcpy(*out_result + len, buf, (size_t)n);
        len += (size_t)n;
    }
    (*out_result)[len] = '\0';

    int status = 0;
    stl_proc_wait(&proc, &status, -1);
    stl_proc_close(&proc);
    return status;
}

XY_MODULE_API void xy_install(void) {
    ren_register_tool("bash", "bash <command>", exec_bash);
}
