#define _GNU_SOURCE
#include "ren-complete.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#include <ttypt/stele.h>

static const char *const REN_SLASH_CMDS[] = {
    "/exit",
    "/new",
    "/quit",
    "/reset",
    "/status",
};
#define REN_SLASH_CMD_COUNT (sizeof(REN_SLASH_CMDS) / sizeof(REN_SLASH_CMDS[0]))

static int cmp_str_ptr(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

static size_t common_prefix_len(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return i;
}

/* Replace buf[tok_start .. *pos) with replacement string `rep` (length rep_len),
 * preserving buf[*pos .. *len]. Returns 1 if buf changed, 0 otherwise. */
static int splice_token(char *buf, size_t cap, size_t *len, size_t *pos,
                        size_t tok_start, const char *rep, size_t rep_len) {
    size_t old_tok_len = *pos - tok_start;
    size_t suffix_len = *len - *pos;
    size_t new_len = tok_start + rep_len + suffix_len;
    if (new_len + 1 > cap) return 0;

    if (rep_len == old_tok_len && memcmp(buf + tok_start, rep, rep_len) == 0) {
        return 0;
    }

    memmove(buf + tok_start + rep_len, buf + *pos, suffix_len + 1);
    memcpy(buf + tok_start, rep, rep_len);
    *len = new_len;
    *pos = tok_start + rep_len;
    return 1;
}

static void append_preview(char *out, size_t cap, size_t *used,
                           const char *name, int is_dir) {
    if (!out || cap == 0) return;
    size_t nlen = strlen(name);
    size_t need = (*used > 0 ? 1 : 0) + nlen + (is_dir ? 1 : 0);
    if (*used + need + 4 >= cap) return;
    if (*used > 0) out[(*used)++] = ' ';
    memcpy(out + *used, name, nlen);
    *used += nlen;
    if (is_dir) out[(*used)++] = '/';
    out[*used] = '\0';
}

static int complete_slash(char *buf, size_t cap, size_t *len, size_t *pos,
                          size_t tok_start, const char *tok, size_t tok_len,
                          ren_completion_result_t *res) {
    const char *matches[REN_SLASH_CMD_COUNT];
    int count = 0;
    for (size_t i = 0; i < REN_SLASH_CMD_COUNT; i++) {
        if (strncmp(REN_SLASH_CMDS[i], tok, tok_len) == 0) {
            matches[count++] = REN_SLASH_CMDS[i];
        }
    }
    if (count == 0) return 0;

    res->match_count = count;
    if (count == 1) {
        res->changed = splice_token(buf, cap, len, pos, tok_start,
                                    matches[0], strlen(matches[0]));
    } else {
        size_t lcp = strlen(matches[0]);
        for (int i = 1; i < count; i++) {
            size_t cl = common_prefix_len(matches[0], matches[i]);
            if (cl < lcp) lcp = cl;
        }
        if (lcp > tok_len) {
            res->changed = splice_token(buf, cap, len, pos, tok_start, matches[0], lcp);
        }
        size_t used = 0;
        for (int i = 0; i < count; i++) {
            append_preview(res->matches, sizeof(res->matches), &used, matches[i], 0);
        }
    }
    res->new_len = *len;
    res->new_pos = *pos;
    return count;
}

#ifndef _WIN32
static int is_dir_entry(const char *open_dir, const char *name) {
    char full[2048];
    if (strcmp(open_dir, "/") == 0) {
        snprintf(full, sizeof(full), "/%s", name);
    } else if (strcmp(open_dir, ".") == 0) {
        snprintf(full, sizeof(full), "%s", name);
    } else {
        size_t dl = strlen(open_dir);
        if (dl > 0 && open_dir[dl - 1] == '/')
            snprintf(full, sizeof(full), "%s%s", open_dir, name);
        else
            snprintf(full, sizeof(full), "%s/%s", open_dir, name);
    }
    struct stat st;
    return (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
}
#endif

int ren_complete_line(char *buf, size_t cap, size_t *len, size_t *pos,
                      ren_completion_result_t *res) {
    ren_completion_result_t local_res;
    if (!res) res = &local_res;
    memset(res, 0, sizeof(*res));

    if (!buf || !len || !pos || cap == 0 || *pos > *len || *len >= cap) {
        return 0;
    }
    res->new_len = *len;
    res->new_pos = *pos;

    /* Find start of current token preceding cursor */
    size_t tok_start = *pos;
    while (tok_start > 0 && buf[tok_start - 1] != ' ' && buf[tok_start - 1] != '\t') {
        tok_start--;
    }
    size_t tok_len = *pos - tok_start;
    if (tok_len >= 1024) return 0;

    char tok[1024];
    memcpy(tok, buf + tok_start, tok_len);
    tok[tok_len] = '\0';

    /* 1. Built-in slash commands when completing first token starting with '/'
     *    and containing no second '/'. Fall through to path completion on 0 matches. */
    if (tok_start == 0 && tok_len > 0 && tok[0] == '/' &&
        memchr(tok + 1, '/', tok_len - 1) == NULL) {
        int sc = complete_slash(buf, cap, len, pos, tok_start, tok, tok_len, res);
        if (sc > 0) return sc;
    }

#ifdef _WIN32
    return 0;
#else
    /* 2. Filesystem path completion */
    char dir_prefix[1024] = "";
    char open_dir[1536] = ".";
    const char *base_prefix = tok;
    const char *last_slash = strrchr(tok, '/');

    if (last_slash) {
        size_t dlen = (size_t)(last_slash - tok) + 1;
        if (dlen >= sizeof(dir_prefix)) return 0;
        memcpy(dir_prefix, tok, dlen);
        dir_prefix[dlen] = '\0';
        base_prefix = last_slash + 1;

        if (dir_prefix[0] == '~' && (dir_prefix[1] == '/' || dir_prefix[1] == '\0')) {
            char home[512];
            stl_home_dir(home, sizeof(home));
            snprintf(open_dir, sizeof(open_dir), "%s%s", home, dir_prefix + 1);
        } else {
            snprintf(open_dir, sizeof(open_dir), "%s", dir_prefix);
        }
    }

    size_t base_len = strlen(base_prefix);
    DIR *dp = opendir(open_dir);
    if (!dp) return 0;

    enum { MAX_COLLECT = 128 };
    char *collected[MAX_COLLECT];
    int n_collected = 0;
    int total_matches = 0;
    char lcp[512] = "";
    size_t lcp_len = 0;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (name[0] == '.' && base_prefix[0] != '.') continue;
        if (strncmp(name, base_prefix, base_len) != 0) continue;

        if (total_matches == 0) {
            snprintf(lcp, sizeof(lcp), "%s", name);
            lcp_len = strlen(lcp);
        } else {
            size_t cl = common_prefix_len(lcp, name);
            if (cl < lcp_len) {
                lcp_len = cl;
                lcp[lcp_len] = '\0';
            }
        }
        total_matches++;
        if (n_collected < MAX_COLLECT) {
            char *dup = strdup(name);
            if (dup) collected[n_collected++] = dup;
        }
    }
    closedir(dp);

    if (total_matches == 0) return 0;

    if (n_collected > 1) {
        qsort(collected, (size_t)n_collected, sizeof(collected[0]), cmp_str_ptr);
    }

    res->match_count = total_matches;
    if (total_matches == 1 && n_collected == 1) {
        int is_dir = is_dir_entry(open_dir, collected[0]);
        int add_space = (!is_dir && (*pos == *len || (buf[*pos] != ' ' && buf[*pos] != '\t')));
        char rep[2048];
        int n = snprintf(rep, sizeof(rep), "%s%s%s",
                         dir_prefix, collected[0],
                         is_dir ? "/" : (add_space ? " " : ""));
        if (n > 0 && (size_t)n < sizeof(rep)) {
            res->changed = splice_token(buf, cap, len, pos, tok_start, rep, (size_t)n);
        }
    } else {
        if (lcp_len > base_len) {
            char rep[2048];
            int n = snprintf(rep, sizeof(rep), "%s%s", dir_prefix, lcp);
            if (n > 0 && (size_t)n < sizeof(rep)) {
                res->changed = splice_token(buf, cap, len, pos, tok_start, rep, (size_t)n);
            }
        }
        size_t used = 0;
        for (int i = 0; i < n_collected; i++) {
            int is_dir = is_dir_entry(open_dir, collected[i]);
            append_preview(res->matches, sizeof(res->matches), &used, collected[i], is_dir);
        }
        if (total_matches > n_collected && used + 5 < sizeof(res->matches)) {
            snprintf(res->matches + used, sizeof(res->matches) - used, " ...");
        }
    }

    for (int i = 0; i < n_collected; i++) {
        free(collected[i]);
    }

    res->new_len = *len;
    res->new_pos = *pos;
    return total_matches;
#endif
}

int ren_format_status(char *out, size_t cap, int busy, const char *model,
                      size_t history_bytes, size_t max_tokens) {
    if (!out || cap == 0) return -1;
    const char *m = (model && *model) ? model : "fast";
    char clean_model[64];
    size_t j = 0;
    for (size_t i = 0; m[i] && j < sizeof(clean_model) - 1; i++) {
        char c = m[i];
        clean_model[j++] = (c == '\t' || c == '\r' || c == '\n') ? ' ' : c;
    }
    clean_model[j] = '\0';
    return snprintf(out, cap, "STATUS\t%s\t%s\t%zu\t%zu\t%zu\n",
                    busy ? "busy" : "idle",
                    clean_model,
                    history_bytes / 4,
                    max_tokens,
                    history_bytes);
}
