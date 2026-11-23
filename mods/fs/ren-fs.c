#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ttypt/stele.h>
#include "ttypt/libren.h"
#include <ttypt/xy.h>

static const char *expand_tilde(const char *path, char *buf, size_t sz) {
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\\' || path[1] == '\0')) {
        char home[512];
        stl_home_dir(home, sizeof(home));
        snprintf(buf, sz, "%s%s", home, path + 1);
        return buf;
    }
    return path;
}

static int read_file(const char *args, char **out_result) {
    char pathbuf[4096];
    const char *path = expand_tilde(args, pathbuf, sizeof(pathbuf));
    FILE *f = fopen(path, "r");
    if (!f) {
        *out_result = strdup("Error: could not open file");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out_result = malloc(size + 1);
    if (size > 0) {
        size_t n = fread(*out_result, 1, size, f);
        (*out_result)[n] = '\0';
    } else {
        (*out_result)[0] = '\0';
    }
    fclose(f);
    return 0;
}

static int write_file(const char *args, char **out_result) {
    char *copy = strdup(args);
    char *sep = strstr(copy, "::");
    if (!sep) {
        free(copy);
        *out_result = strdup("Error: expected path::content");
        return -1;
    }
    *sep = '\0';
    char pathbuf[4096];
    const char *path = expand_tilde(copy, pathbuf, sizeof(pathbuf));
    const char *content = sep + 2;
    FILE *f = fopen(path, "w");
    if (!f) {
        free(copy);
        *out_result = strdup("Error: could not open file for writing");
        return -1;
    }
    fputs(content, f);
    fclose(f);
    free(copy);
    *out_result = strdup("OK");
    return 0;
}

static int edit_file(const char *args, char **out_result) {
    char *copy = strdup(args);
    char *sep1 = strstr(copy, "::");
    if (!sep1) {
        free(copy);
        *out_result = strdup("Error: expected path::old::new");
        return -1;
    }
    *sep1 = '\0';
    char *sep2 = strstr(sep1 + 2, "::");
    if (!sep2) {
        free(copy);
        *out_result = strdup("Error: expected path::old::new");
        return -1;
    }
    *sep2 = '\0';

    const char *path = copy;
    const char *old_str = sep1 + 2;
    const char *new_str = sep2 + 2;

    char *buf = NULL;
    if (read_file(path, &buf) != 0) {
        free(copy);
        *out_result = buf;
        return -1;
    }

    char *match = strstr(buf, old_str);
    if (!match) {
        free(buf);
        free(copy);
        *out_result = strdup("Error: old text not found");
        return -1;
    }

    size_t prefix_len = (size_t)(match - buf);
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t suffix_len = strlen(match + old_len);

    FILE *f = fopen(path, "w");
    if (!f) {
        free(buf);
        free(copy);
        *out_result = strdup("Error: could not open file for writing");
        return -1;
    }
    fwrite(buf, 1, prefix_len, f);
    fwrite(new_str, 1, new_len, f);
    fwrite(match + old_len, 1, suffix_len, f);
    fclose(f);

    free(buf);
    free(copy);
    *out_result = strdup("OK");
    return 0;
}

XY_MODULE_API void xy_install(void) {
    ren_register_tool("read", "read <path>", read_file);
    ren_register_tool("write", "write <path>::<content>", write_file);
    ren_register_tool("edit", "edit <path>::<old>::<new>", edit_file);
}
