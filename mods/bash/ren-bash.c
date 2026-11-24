#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json-c/json.h>
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

    char *cmd_buf = NULL;
    const char *cmd = args;

    struct json_object *obj = json_tokener_parse(args);
    if (obj && json_object_is_type(obj, json_type_object)) {
        struct json_object *c = json_object_object_get(obj, "command");
        if (!c) c = json_object_object_get(obj, "cmd");
        if (!c) c = json_object_object_get(obj, "args");
        if (c && json_object_is_type(c, json_type_string)) {
            cmd_buf = strdup(json_object_get_string(c));
            cmd = cmd_buf;
        }
        json_object_put(obj);
    } else if (obj) {
        json_object_put(obj);
    }

#ifdef _WIN32
    const char *argv[] = { "cmd.exe", "/c", cmd, NULL };
#else
    const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
#endif
    stl_proc_t proc;
    if (stl_spawn(argv, STL_SPAWN_STDOUT | STL_SPAWN_MERGE_STDERR | STL_SPAWN_NEW_GROUP, &proc) != 0) {
        free(cmd_buf);
        *out_result = strdup("Error: spawn failed");
        return -1;
    }
    free(cmd_buf);

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

static const char BASH_SCHEMA[] =
    "{\n"
    "  \"type\": \"object\",\n"
    "  \"properties\": {\n"
    "    \"command\": {\"type\": \"string\", \"description\": \"Shell command to execute\"}\n"
    "  },\n"
    "  \"required\": [\"command\"]\n"
    "}";

XY_MODULE_API void xy_install(void) {
    ren_register_tool_with_schema("bash", "bash <command> (or JSON {command})", BASH_SCHEMA, exec_bash);
}
