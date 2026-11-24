#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <json-c/json.h>
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
    if (!args || !*args) {
        *out_result = strdup("Error: expected path or JSON with path");
        return -1;
    }

    char target_path[4096] = {0};
    int offset = 0;
    int limit = 0;

    struct json_object *obj = json_tokener_parse(args);
    if (obj && json_object_is_type(obj, json_type_object)) {
        struct json_object *p = json_object_object_get(obj, "path");
        if (!p) p = json_object_object_get(obj, "file");
        if (p && json_object_is_type(p, json_type_string)) {
            snprintf(target_path, sizeof(target_path), "%s", json_object_get_string(p));
        }
        struct json_object *o = json_object_object_get(obj, "offset");
        if (o && json_object_is_type(o, json_type_int)) {
            offset = json_object_get_int(o);
        }
        struct json_object *l = json_object_object_get(obj, "limit");
        if (l && json_object_is_type(l, json_type_int)) {
            limit = json_object_get_int(l);
        }
        json_object_put(obj);
    } else {
        if (obj) json_object_put(obj);
        snprintf(target_path, sizeof(target_path), "%s", args);
    }

    if (!target_path[0]) {
        *out_result = strdup("Error: missing path");
        return -1;
    }

    char pathbuf[4096];
    const char *path = expand_tilde(target_path, pathbuf, sizeof(pathbuf));
    FILE *f = fopen(path, "r");
    if (!f) {
        *out_result = strdup("Error: could not open file");
        return -1;
    }

    /* If offset/limit requested, read line-by-line */
    if (offset > 0 || limit > 0) {
        size_t cap = 8192, len = 0;
        char *buf = malloc(cap);
        if (!buf) {
            fclose(f);
            *out_result = strdup("Error: out of memory");
            return -1;
        }
        buf[0] = '\0';

        char *line = NULL;
        size_t linecap = 0;
        ssize_t linelen;
        int cur_line = 1;
        int lines_read = 0;

        while ((linelen = getline(&line, &linecap, f)) >= 0) {
            if (offset > 0 && cur_line < offset) {
                cur_line++;
                continue;
            }
            if (limit > 0 && lines_read >= limit) {
                break;
            }
            if (len + (size_t)linelen + 1 > cap) {
                while (len + (size_t)linelen + 1 > cap) cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) break;
                buf = nb;
            }
            memcpy(buf + len, line, (size_t)linelen);
            len += (size_t)linelen;
            buf[len] = '\0';
            lines_read++;
            cur_line++;
        }
        free(line);
        fclose(f);
        *out_result = buf;
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out_result = malloc(size + 1);
    if (!*out_result) {
        fclose(f);
        *out_result = strdup("Error: out of memory");
        return -1;
    }
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
    if (!args || !*args) {
        *out_result = strdup("Error: expected path::content or JSON with path, content");
        return -1;
    }

    char target_path[4096] = {0};
    char *target_content = NULL;

    struct json_object *obj = json_tokener_parse(args);
    if (obj && json_object_is_type(obj, json_type_object)) {
        struct json_object *p = json_object_object_get(obj, "path");
        if (!p) p = json_object_object_get(obj, "file");
        if (p && json_object_is_type(p, json_type_string)) {
            snprintf(target_path, sizeof(target_path), "%s", json_object_get_string(p));
        }
        struct json_object *c = json_object_object_get(obj, "content");
        if (c && json_object_is_type(c, json_type_string)) {
            target_content = strdup(json_object_get_string(c));
        }
        json_object_put(obj);
    } else {
        if (obj) json_object_put(obj);
        char *copy = strdup(args);
        char *sep = strstr(copy, "::");
        if (sep) {
            *sep = '\0';
            snprintf(target_path, sizeof(target_path), "%s", copy);
            target_content = strdup(sep + 2);
        }
        free(copy);
    }

    if (!target_path[0] || !target_content) {
        free(target_content);
        *out_result = strdup("Error: expected path::content or JSON with path, content");
        return -1;
    }

    char pathbuf[4096];
    const char *path = expand_tilde(target_path, pathbuf, sizeof(pathbuf));
    FILE *f = fopen(path, "w");
    if (!f) {
        free(target_content);
        *out_result = strdup("Error: could not open file for writing");
        return -1;
    }
    fputs(target_content, f);
    fclose(f);
    free(target_content);
    *out_result = strdup("OK");
    return 0;
}

static int apply_single_edit(const char *path, const char *old_str, const char *new_str, char **out_result) {
    char *buf = NULL;
    if (read_file(path, &buf) != 0) {
        *out_result = buf ? buf : strdup("Error: could not read file");
        return -1;
    }

    char *match = strstr(buf, old_str);
    if (!match) {
        free(buf);
        *out_result = strdup("Error: old text not found");
        return -1;
    }

    size_t prefix_len = (size_t)(match - buf);
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t suffix_len = strlen(match + old_len);

    char pathbuf[4096];
    const char *exp_path = expand_tilde(path, pathbuf, sizeof(pathbuf));
    FILE *f = fopen(exp_path, "w");
    if (!f) {
        free(buf);
        *out_result = strdup("Error: could not open file for writing");
        return -1;
    }
    fwrite(buf, 1, prefix_len, f);
    fwrite(new_str, 1, new_len, f);
    fwrite(match + old_len, 1, suffix_len, f);
    fclose(f);

    free(buf);
    *out_result = strdup("OK");
    return 0;
}

static int edit_file(const char *args, char **out_result) {
    if (!args || !*args) {
        *out_result = strdup("Error: expected path::old::new or JSON with path, oldText, newText");
        return -1;
    }

    char target_path[4096] = {0};
    char *old_text = NULL;
    char *new_text = NULL;

    struct json_object *obj = json_tokener_parse(args);
    if (obj && json_object_is_type(obj, json_type_object)) {
        struct json_object *p = json_object_object_get(obj, "path");
        if (!p) p = json_object_object_get(obj, "file");
        if (p && json_object_is_type(p, json_type_string)) {
            snprintf(target_path, sizeof(target_path), "%s", json_object_get_string(p));
        }

        /* Check for batch edits: "edits": [{"oldText": "...", "newText": "..."}] */
        struct json_object *edits_arr = json_object_object_get(obj, "edits");
        if (edits_arr && json_object_is_type(edits_arr, json_type_array)) {
            int n_edits = json_object_array_length(edits_arr);
            for (int i = 0; i < n_edits; i++) {
                struct json_object *ed = json_object_array_get_idx(edits_arr, i);
                if (!ed || !json_object_is_type(ed, json_type_object)) continue;
                struct json_object *o = json_object_object_get(ed, "oldText");
                if (!o) o = json_object_object_get(ed, "old");
                struct json_object *n = json_object_object_get(ed, "newText");
                if (!n) n = json_object_object_get(ed, "new");
                if (o && n && json_object_is_type(o, json_type_string) && json_object_is_type(n, json_type_string)) {
                    char *sub_res = NULL;
                    int rc = apply_single_edit(target_path, json_object_get_string(o), json_object_get_string(n), &sub_res);
                    if (rc != 0) {
                        json_object_put(obj);
                        *out_result = sub_res;
                        return -1;
                    }
                    free(sub_res);
                }
            }
            json_object_put(obj);
            *out_result = strdup("OK");
            return 0;
        }

        struct json_object *o = json_object_object_get(obj, "oldText");
        if (!o) o = json_object_object_get(obj, "old");
        if (o && json_object_is_type(o, json_type_string)) {
            old_text = strdup(json_object_get_string(o));
        }

        struct json_object *n = json_object_object_get(obj, "newText");
        if (!n) n = json_object_object_get(obj, "new");
        if (n && json_object_is_type(n, json_type_string)) {
            new_text = strdup(json_object_get_string(n));
        }
        json_object_put(obj);
    } else {
        if (obj) json_object_put(obj);
        char *copy = strdup(args);
        char *sep1 = strstr(copy, "::");
        if (sep1) {
            *sep1 = '\0';
            char *sep2 = strstr(sep1 + 2, "::");
            if (sep2) {
                *sep2 = '\0';
                snprintf(target_path, sizeof(target_path), "%s", copy);
                old_text = strdup(sep1 + 2);
                new_text = strdup(sep2 + 2);
            }
        }
        free(copy);
    }

    if (!target_path[0] || !old_text || !new_text) {
        free(old_text);
        free(new_text);
        *out_result = strdup("Error: expected path::old::new or JSON with path, oldText, newText");
        return -1;
    }

    int rc = apply_single_edit(target_path, old_text, new_text, out_result);
    free(old_text);
    free(new_text);
    return rc;
}

static const char READ_SCHEMA[] =
    "{\n"
    "  \"type\": \"object\",\n"
    "  \"properties\": {\n"
    "    \"path\": {\"type\": \"string\", \"description\": \"File path to read\"},\n"
    "    \"offset\": {\"type\": \"integer\", \"description\": \"Optional line number to start reading from (1-indexed)\"},\n"
    "    \"limit\": {\"type\": \"integer\", \"description\": \"Optional maximum number of lines to read\"}\n"
    "  },\n"
    "  \"required\": [\"path\"]\n"
    "}";

static const char WRITE_SCHEMA[] =
    "{\n"
    "  \"type\": \"object\",\n"
    "  \"properties\": {\n"
    "    \"path\": {\"type\": \"string\", \"description\": \"File path to write\"},\n"
    "    \"content\": {\"type\": \"string\", \"description\": \"Content to write to the file\"}\n"
    "  },\n"
    "  \"required\": [\"path\", \"content\"]\n"
    "}";

static const char EDIT_SCHEMA[] =
    "{\n"
    "  \"type\": \"object\",\n"
    "  \"properties\": {\n"
    "    \"path\": {\"type\": \"string\", \"description\": \"File path to edit\"},\n"
    "    \"oldText\": {\"type\": \"string\", \"description\": \"Exact text to replace\"},\n"
    "    \"newText\": {\"type\": \"string\", \"description\": \"Replacement text\"}\n"
    "  },\n"
    "  \"required\": [\"path\", \"oldText\", \"newText\"]\n"
    "}";

XY_MODULE_API void xy_install(void) {
    ren_register_tool_with_schema("read", "read <path> (or JSON {path, offset, limit})", READ_SCHEMA, read_file);
    ren_register_tool_with_schema("write", "write <path>::<content> (or JSON {path, content})", WRITE_SCHEMA, write_file);
    ren_register_tool_with_schema("edit", "edit <path>::<old>::<new> (or JSON {path, oldText, newText})", EDIT_SCHEMA, edit_file);
}
