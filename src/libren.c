#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <json-c/json.h>
#include "ttypt/libren.h"

#define MAX_RESPONSE 65536
#define MAX_TOOL_CALLS_PER_TURN 16
#define MAX_TOOLS 64
#define STALL_REPEAT_THRESHOLD 3
#define MAX_OVERFLOW_RETRIES 3

typedef struct {
    char *name;
    char *usage;
    ren_tool_func fn;
} ren_tool_entry_t;

typedef struct {
    int index;
    char id[128];
    char name[128];
    char *args;
    size_t args_len;
    size_t args_cap;
} ren_tc_accum_t;

#define MAX_PROMPT_HINTS 16

static ren_tool_entry_t g_tools[MAX_TOOLS];
static int g_num_tools = 0;
static char *g_prompt_hints[MAX_PROMPT_HINTS];
static int g_num_prompt_hints = 0;
static char *g_custom_identity = NULL;
static char *g_model_name = NULL;
static char *g_system_prompt = NULL;
static int g_tools_loaded = 0;
static ren_tier0_func g_tier0_fn = NULL;
static ren_context_func g_context_fn = NULL;
static ren_compact_func g_compactor_fn = NULL;
static volatile sig_atomic_t *g_abort_flag = NULL;

static pthread_mutex_t g_ctrl_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_wake_pipe[2] = { -1, -1 };
static char *g_steer_queue = NULL;
static volatile sig_atomic_t g_local_abort = 0;

static void buf_append(char **buf, size_t *len, size_t *cap, const char *str, size_t slen) {
    if (*len + slen + 1 > *cap) {
        if (*cap == 0) *cap = 256;
        while (*len + slen + 1 > *cap) *cap *= 2;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, str, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

static void rebuild_system_prompt(void) {
    free(g_system_prompt);
    size_t cap = 1024;
    size_t len = 0;
    g_system_prompt = malloc(cap);
    g_system_prompt[0] = '\0';

    if (g_custom_identity && *g_custom_identity) {
        buf_append(&g_system_prompt, &len, &cap, g_custom_identity, strlen(g_custom_identity));
        buf_append(&g_system_prompt, &len, &cap, "\nAvailable tools:\n", 18);
    } else {
        const char *hdr = "You are ren, a pure-C coding assistant. Available tools:\n";
        buf_append(&g_system_prompt, &len, &cap, hdr, strlen(hdr));
    }

    for (int i = 0; i < g_num_tools; i++) {
        buf_append(&g_system_prompt, &len, &cap, "- ", 2);
        buf_append(&g_system_prompt, &len, &cap, g_tools[i].usage, strlen(g_tools[i].usage));
        buf_append(&g_system_prompt, &len, &cap, "\n", 1);
    }

    const char *tail =
        "Use the provided function tools, passing the tool arguments in the 'args' field according to each tool's usage above.\n"
        "If function calling is unavailable (local models), you may instead output ONLY a single line starting with: TOOL: <name> <args>\n"
        "Wait for tool results before answering.";
    buf_append(&g_system_prompt, &len, &cap, tail, strlen(tail));

    for (int i = 0; i < g_num_prompt_hints; i++) {
        buf_append(&g_system_prompt, &len, &cap, "\n", 1);
        buf_append(&g_system_prompt, &len, &cap, g_prompt_hints[i], strlen(g_prompt_hints[i]));
    }
}

void ren_register_tool(const char *name, const char *usage, ren_tool_func fn) {
    for (int i = 0; i < g_num_tools; i++) {
        if (strcmp(g_tools[i].name, name) == 0) {
            free(g_tools[i].usage);
            g_tools[i].usage = strdup(usage);
            g_tools[i].fn = fn;
            rebuild_system_prompt();
            return;
        }
    }
    if (g_num_tools >= MAX_TOOLS) return;
    g_tools[g_num_tools].name = strdup(name);
    g_tools[g_num_tools].usage = strdup(usage);
    g_tools[g_num_tools].fn = fn;
    g_num_tools++;
    rebuild_system_prompt();
}

void ren_register_prompt_hint(const char *hint) {
    if (!hint || !*hint) return;
    for (int i = 0; i < g_num_prompt_hints; i++) {
        if (strcmp(g_prompt_hints[i], hint) == 0) return;
    }
    if (g_num_prompt_hints >= MAX_PROMPT_HINTS) return;
    g_prompt_hints[g_num_prompt_hints++] = strdup(hint);
    rebuild_system_prompt();
}

void ren_set_system_prompt(const char *prompt) {
    free(g_custom_identity);
    g_custom_identity = (prompt && *prompt) ? strdup(prompt) : NULL;
    rebuild_system_prompt();
}

void ren_set_model(const char *model) {
    free(g_model_name);
    g_model_name = (model && *model) ? strdup(model) : NULL;
}

void ren_register_tier0(ren_tier0_func fn) {
    g_tier0_fn = fn;
}

void ren_register_context_provider(ren_context_func fn) {
    g_context_fn = fn;
}

void ren_register_compactor(ren_compact_func fn) {
    g_compactor_fn = fn;
}

void ren_set_abort_flag(volatile sig_atomic_t *flag) {
    g_abort_flag = flag;
}

static int ensure_wake_pipe_locked(void) {
    if (g_wake_pipe[0] >= 0 && g_wake_pipe[1] >= 0) {
        return g_wake_pipe[0];
    }
    int fds[2];
    if (pipe(fds) < 0) return -1;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(fds[i], F_GETFL, 0);
        if (fl >= 0) fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    g_wake_pipe[0] = fds[0];
    g_wake_pipe[1] = fds[1];
    return g_wake_pipe[0];
}

static void drain_wake_pipe_locked(void) {
    if (g_wake_pipe[0] < 0) return;
    char tmp[64];
    while (read(g_wake_pipe[0], tmp, sizeof(tmp)) > 0) {}
}

static void wake_poll_locked(void) {
    if (ensure_wake_pipe_locked() >= 0 && g_wake_pipe[1] >= 0) {
        char b = 1;
        ssize_t w = write(g_wake_pipe[1], &b, 1);
        (void)w;
    }
}

void ren_steer(const char *text) {
    if (!text || !*text) return;
    pthread_mutex_lock(&g_ctrl_mtx);
    size_t tlen = strlen(text);
    if (g_steer_queue) {
        size_t cur = strlen(g_steer_queue);
        char *nq = realloc(g_steer_queue, cur + 1 + tlen + 1);
        if (nq) {
            nq[cur] = '\n';
            memcpy(nq + cur + 1, text, tlen);
            nq[cur + 1 + tlen] = '\0';
            g_steer_queue = nq;
        }
    } else {
        g_steer_queue = strdup(text);
    }
    wake_poll_locked();
    pthread_mutex_unlock(&g_ctrl_mtx);
}

void ren_abort(void) {
    pthread_mutex_lock(&g_ctrl_mtx);
    g_local_abort = 1;
    wake_poll_locked();
    pthread_mutex_unlock(&g_ctrl_mtx);
}

static int has_pending_steer(void) {
    pthread_mutex_lock(&g_ctrl_mtx);
    int pending = (g_steer_queue != NULL && g_steer_queue[0] != '\0');
    pthread_mutex_unlock(&g_ctrl_mtx);
    return pending;
}

static int consume_steer(char **out_text) {
    if (out_text) *out_text = NULL;
    pthread_mutex_lock(&g_ctrl_mtx);
    char *q = g_steer_queue;
    g_steer_queue = NULL;
    drain_wake_pipe_locked();
    pthread_mutex_unlock(&g_ctrl_mtx);
    if (!q || !*q) {
        free(q);
        return 0;
    }
    if (out_text) *out_text = q;
    else free(q);
    return 1;
}

static void reset_ctrl_state(void) {
    pthread_mutex_lock(&g_ctrl_mtx);
    g_local_abort = 0;
    free(g_steer_queue);
    g_steer_queue = NULL;
    drain_wake_pipe_locked();
    pthread_mutex_unlock(&g_ctrl_mtx);
}

static int try_load_so_stem(const char *stem) {
    char probe[2100];
    snprintf(probe, sizeof(probe), "%s.so", stem);
    if (access(probe, R_OK) == 0) {
        return xy_load((char *)stem);
    }
    return -1;
}

int ren_load_module(const char *path) {
    if (!path || !*path) return -1;

    char clean[512];
    snprintf(clean, sizeof(clean), "%s", path);
    size_t len = strlen(clean);
    if (len > 3 && strcmp(clean + len - 3, ".so") == 0) {
        clean[len - 3] = '\0';
    }

    if (try_load_so_stem(clean) == 0) {
        return 0;
    }

    if (path[0] != '/') {
        const char *base = strrchr(clean, '/');
        base = base ? (base + 1) : clean;
        int has_mod_prefix = (strncmp(base, "mod_", 4) == 0);
        char candidate[2048];

        const char *mod_dir = getenv("REN_MOD_DIR");
        if (mod_dir && *mod_dir) {
            snprintf(candidate, sizeof(candidate), "%s/%s", mod_dir, base);
            if (try_load_so_stem(candidate) == 0) return 0;

            if (!has_mod_prefix) {
                snprintf(candidate, sizeof(candidate), "%s/mod_%s", mod_dir, base);
                if (try_load_so_stem(candidate) == 0) return 0;
            }
        }

        const char *home = getenv("HOME");
        if (home && *home) {
            snprintf(candidate, sizeof(candidate), "%s/.local/lib/ren/%s", home, base);
            if (try_load_so_stem(candidate) == 0) return 0;

            if (!has_mod_prefix) {
                snprintf(candidate, sizeof(candidate), "%s/.local/lib/ren/mod_%s", home, base);
                if (try_load_so_stem(candidate) == 0) return 0;
            }
        }

        static const char *const sys_dirs[] = { "/usr/local/lib/ren", "/usr/lib/ren", NULL };
        for (int i = 0; sys_dirs[i]; i++) {
            snprintf(candidate, sizeof(candidate), "%s/%s", sys_dirs[i], base);
            if (try_load_so_stem(candidate) == 0) return 0;
            if (!has_mod_prefix) {
                snprintf(candidate, sizeof(candidate), "%s/mod_%s", sys_dirs[i], base);
                if (try_load_so_stem(candidate) == 0) return 0;
            }
        }
    }
    return xy_load(clean);
}

static int dispatch_tool(const char *name, const char *args, char **out) {
    for (int i = 0; i < g_num_tools; i++) {
        if (strcmp(g_tools[i].name, name) == 0) {
            return g_tools[i].fn(args ? args : "", out);
        }
    }
    *out = strdup("Error: unknown tool");
    return -1;
}

static void load_default_tools(void) {
    if (g_tools_loaded) return;
    g_tools_loaded = 1;
    xy_init();
    ren_load_module("mods/fs/mod_fs");
    ren_load_module("mods/bash/mod_bash");
}

static const char *msg_role(struct json_object *msg) {
    if (!msg || !json_object_is_type(msg, json_type_object)) return NULL;
    struct json_object *ro = json_object_object_get(msg, "role");
    if (ro && json_object_is_type(ro, json_type_string)) {
        return json_object_get_string(ro);
    }
    return NULL;
}

static struct json_object *parse_history_array(ren_context_t *ctx) {
    if (!ctx || !ctx->history || ctx->history_len == 0) {
        return json_object_new_array();
    }
    size_t wlen = ctx->history_len + 3;
    char *wrapped = malloc(wlen);
    if (!wrapped) return json_object_new_array();
    snprintf(wrapped, wlen, "[%s]", ctx->history);
    struct json_object *arr = json_tokener_parse(wrapped);
    free(wrapped);
    if (!arr || !json_object_is_type(arr, json_type_array)) {
        if (arr) json_object_put(arr);
        fprintf(stderr, "[ren] Warning: corrupt session history, resetting\n");
        return json_object_new_array();
    }
    return arr;
}

static void write_history_array(ren_context_t *ctx, struct json_object *arr) {
    const char *ser = json_object_to_json_string_ext(arr, JSON_C_TO_STRING_PLAIN);
    if (!ser) return;
    size_t slen = strlen(ser);
    size_t inner_len = (slen >= 2 && ser[0] == '[' && ser[slen - 1] == ']') ? (slen - 2) : 0;
    if (inner_len + 1 > ctx->history_cap) {
        while (inner_len + 1 > ctx->history_cap) {
            ctx->history_cap = ctx->history_cap ? ctx->history_cap * 2 : 4096;
        }
        ctx->history = realloc(ctx->history, ctx->history_cap);
    }
    if (inner_len > 0) {
        memcpy(ctx->history, ser + 1, inner_len);
    }
    ctx->history[inner_len] = '\0';
    ctx->history_len = inner_len;
}

static void append_json_message(ren_context_t *ctx, struct json_object *msg) {
    const char *s = json_object_to_json_string_ext(msg, JSON_C_TO_STRING_PLAIN);
    if (!s) return;
    if (ctx->history_len > 0) {
        buf_append(&ctx->history, &ctx->history_len, &ctx->history_cap, ",", 1);
    }
    buf_append(&ctx->history, &ctx->history_len, &ctx->history_cap, s, strlen(s));
}

static void append_message(ren_context_t *ctx, const char *role, const char *content) {
    struct json_object *msg = json_object_new_object();
    json_object_object_add(msg, "role", json_object_new_string(role));
    json_object_object_add(msg, "content", json_object_new_string(content ? content : ""));
    append_json_message(ctx, msg);
    json_object_put(msg);
}

int ren_compact_default(ren_context_t *ctx, size_t target_bytes) {
    if (!ctx || ctx->history_len <= target_bytes) return 0;

    struct json_object *arr = parse_history_array(ctx);
    size_t arr_len = json_object_array_length(arr);
    if (arr_len <= 2) {
        json_object_put(arr);
        return 0;
    }

    size_t start_idx = 0;
    const char *first_role = msg_role(json_object_array_get_idx(arr, 0));
    if (first_role && strcmp(first_role, "system") == 0) {
        start_idx = 1;
    }

    while (1) {
        const char *ser = json_object_to_json_string_ext(arr, JSON_C_TO_STRING_PLAIN);
        size_t ser_len = ser ? strlen(ser) : 0;
        arr_len = json_object_array_length(arr);
        if (ser_len <= target_bytes + 2 || arr_len <= start_idx + 2) {
            break;
        }

        int has_later_user = 0;
        for (size_t i = start_idx + 1; i < arr_len; i++) {
            const char *r = msg_role(json_object_array_get_idx(arr, i));
            if (r && strcmp(r, "user") == 0) {
                has_later_user = 1;
                break;
            }
        }

        if (has_later_user) {
            json_object_array_del_idx(arr, start_idx, 1);
            while (json_object_array_length(arr) > start_idx + 1) {
                const char *r = msg_role(json_object_array_get_idx(arr, start_idx));
                if (r && strcmp(r, "user") == 0) break;
                json_object_array_del_idx(arr, start_idx, 1);
            }
        } else {
            size_t drop_at = start_idx;
            const char *r0 = msg_role(json_object_array_get_idx(arr, start_idx));
            if (r0 && strcmp(r0, "user") == 0 && start_idx + 1 < arr_len) {
                drop_at = start_idx + 1;
            }
            size_t group_end = drop_at + 1;
            while (group_end < arr_len) {
                const char *r = msg_role(json_object_array_get_idx(arr, group_end));
                if (!r || strcmp(r, "tool") != 0) break;
                group_end++;
            }
            if (group_end >= arr_len) break;
            json_object_array_del_idx(arr, drop_at, group_end - drop_at);
        }
    }

    size_t old_len = ctx->history_len;
    write_history_array(ctx, arr);
    json_object_put(arr);
    return ctx->history_len < old_len ? 1 : 0;
}

static int compact_history(ren_context_t *ctx, size_t target_bytes, const char *reason) {
    if (!ctx) return 0;
    size_t before = ctx->history_len;
    if (g_compactor_fn) {
        int rc = g_compactor_fn(ctx, target_bytes, reason ? reason : "limit");
        if (rc > 0 && ctx->history_len < before) {
            return 1;
        }
    }
    return ren_compact_default(ctx, target_bytes);
}

static void sync_system_message(ren_context_t *ctx) {
    if (!g_system_prompt) return;
    if (ctx->history_len == 0) {
        append_message(ctx, "system", g_system_prompt);
        return;
    }

    struct json_object *arr = parse_history_array(ctx);
    size_t n = json_object_array_length(arr);
    if (n == 0) {
        struct json_object *sys = json_object_new_object();
        json_object_object_add(sys, "role", json_object_new_string("system"));
        json_object_object_add(sys, "content", json_object_new_string(g_system_prompt));
        json_object_array_add(arr, sys);
        write_history_array(ctx, arr);
        json_object_put(arr);
        return;
    }

    struct json_object *first = json_object_array_get_idx(arr, 0);
    const char *r = msg_role(first);
    if (r && strcmp(r, "system") == 0) {
        json_object_object_add(first, "content", json_object_new_string(g_system_prompt));
        write_history_array(ctx, arr);
        json_object_put(arr);
    } else {
        struct json_object *new_arr = json_object_new_array();
        struct json_object *sys = json_object_new_object();
        json_object_object_add(sys, "role", json_object_new_string("system"));
        json_object_object_add(sys, "content", json_object_new_string(g_system_prompt));
        json_object_array_add(new_arr, sys);
        for (size_t i = 0; i < n; i++) {
            struct json_object *el = json_object_array_get_idx(arr, i);
            json_object_array_add(new_arr, json_object_get(el));
        }
        write_history_array(ctx, new_arr);
        json_object_put(new_arr);
        json_object_put(arr);
    }
}

void ren_init(ren_context_t *ctx) {
    load_default_tools();
    if (!g_system_prompt) {
        rebuild_system_prompt();
    }
    ctx->session_id = strdup("session-1");
    ctx->history_cap = 4096;
    ctx->history = malloc(ctx->history_cap);
    ctx->history[0] = '\0';
    ctx->history_len = 0;
    ctx->max_turns = 0;
    ctx->max_history_chars = 0;
    ctx->max_tool_output = REN_DEFAULT_TOOL_OUTPUT_CHARS;

    append_message(ctx, "system", g_system_prompt ? g_system_prompt : "You are ren.");
}

void ren_reset(ren_context_t *ctx) {
    reset_ctrl_state();
    if (!ctx || !ctx->history) return;
    ctx->history_len = 0;
    ctx->history[0] = '\0';
    append_message(ctx, "system", g_system_prompt ? g_system_prompt : "You are ren.");
}

void ren_free(ren_context_t *ctx) {
    free(ctx->session_id);
    free(ctx->history);
}

static struct json_object *build_chat_request_json(ren_context_t *ctx, const char *wrapup_reason) {
    struct json_object *req = json_object_new_object();
    struct json_object *msgs = parse_history_array(ctx);

    if (wrapup_reason && *wrapup_reason) {
        struct json_object *nudge = json_object_new_object();
        char nbuf[512];
        snprintf(nbuf, sizeof(nbuf),
                 "%s Do not call any more tools. "
                 "Summarize what you observed so far and answer the user's request directly.",
                 wrapup_reason);
        json_object_object_add(nudge, "role", json_object_new_string("user"));
        json_object_object_add(nudge, "content", json_object_new_string(nbuf));
        json_object_array_add(msgs, nudge);
    }

    const char *env_model = getenv("REN_MODEL");
    const char *eff_model = (g_model_name && *g_model_name) ? g_model_name :
                            ((env_model && *env_model) ? env_model : "fast");
    json_object_object_add(req, "model", json_object_new_string(eff_model));
    json_object_object_add(req, "messages", msgs);
    json_object_object_add(req, "stream", json_object_new_boolean(1));

    if (g_num_tools > 0) {
        struct json_object *tools = json_object_new_array();
        for (int i = 0; i < g_num_tools; i++) {
            struct json_object *entry = json_object_new_object();
            json_object_object_add(entry, "type", json_object_new_string("function"));

            struct json_object *fn = json_object_new_object();
            json_object_object_add(fn, "name", json_object_new_string(g_tools[i].name));
            json_object_object_add(fn, "description", json_object_new_string(g_tools[i].usage));

            struct json_object *params = json_object_new_object();
            json_object_object_add(params, "type", json_object_new_string("object"));
            struct json_object *props = json_object_new_object();
            struct json_object *args_prop = json_object_new_object();
            json_object_object_add(args_prop, "type", json_object_new_string("string"));
            json_object_object_add(args_prop, "description", json_object_new_string(g_tools[i].usage));
            json_object_object_add(props, "args", args_prop);
            json_object_object_add(params, "properties", props);

            struct json_object *required = json_object_new_array();
            json_object_array_add(required, json_object_new_string("args"));
            json_object_object_add(params, "required", required);

            json_object_object_add(fn, "parameters", params);
            json_object_object_add(entry, "function", fn);
            json_object_array_add(tools, entry);
        }
        json_object_object_add(req, "tools", tools);
        if (wrapup_reason && *wrapup_reason) {
            json_object_object_add(req, "tool_choice", json_object_new_string("none"));
        }
    }

    return req;
}

static char *unpack_tool_args(const char *raw_args) {
    if (!raw_args || !*raw_args) return strdup("");
    struct json_object *obj = json_tokener_parse(raw_args);
    if (obj && json_object_is_type(obj, json_type_object)) {
        static const char *const keys[] = {
            "args", "command", "cmd", "path", "file", "query", "input", "text", "url", NULL
        };
        for (int i = 0; keys[i]; i++) {
            struct json_object *v = json_object_object_get(obj, keys[i]);
            if (v && json_object_is_type(v, json_type_string)) {
                char *res = strdup(json_object_get_string(v));
                json_object_put(obj);
                return res ? res : strdup("");
            }
        }
        struct json_object_iterator it = json_object_iter_begin(obj);
        struct json_object_iterator end = json_object_iter_end(obj);
        while (!json_object_iter_equal(&it, &end)) {
            struct json_object *v = json_object_iter_peek_value(&it);
            if (v && json_object_is_type(v, json_type_string)) {
                char *res = strdup(json_object_get_string(v));
                json_object_put(obj);
                return res ? res : strdup("");
            }
            json_object_iter_next(&it);
        }
    } else if (obj && json_object_is_type(obj, json_type_string)) {
        char *res = strdup(json_object_get_string(obj));
        json_object_put(obj);
        return res ? res : strdup("");
    }
    if (obj) json_object_put(obj);
    return strdup(raw_args);
}

static ren_tc_accum_t *find_or_add_tc(ren_tc_accum_t *tcs, int *n_tcs, int idx, const char *id_hint) {
    for (int i = 0; i < *n_tcs; i++) {
        if (tcs[i].index == idx) {
            return &tcs[i];
        }
    }
    if (id_hint && *id_hint) {
        for (int i = 0; i < *n_tcs; i++) {
            if (strcmp(tcs[i].id, id_hint) == 0) {
                return &tcs[i];
            }
        }
    }
    if (*n_tcs >= MAX_TOOL_CALLS_PER_TURN) return NULL;
    ren_tc_accum_t *slot = &tcs[(*n_tcs)++];
    memset(slot, 0, sizeof(*slot));
    slot->index = idx;
    return slot;
}

static void free_tcs(ren_tc_accum_t *tcs, int n_tcs) {
    for (int i = 0; i < n_tcs; i++) {
        free(tcs[i].args);
        tcs[i].args = NULL;
    }
}

/* Pop a ready spoken sentence/clause from unspoken buffer if we know it's not a TOOL: call */
static int pop_spoken_clause(char *unspoken, bool is_first, char *out_clause, size_t out_cap) {
    size_t len = strlen(unspoken);
    const char *lead = unspoken;
    while (*lead && isspace((unsigned char)*lead)) lead++;
    if (strncmp(lead, "TOOL:", 5) == 0 || strncmp(lead, "TOOL", 4) == 0) {
        return 0;
    }
    if (len < (is_first ? 24 : 40)) return 0;

    size_t min_cut = is_first ? 18 : 32;
    size_t max_cut = is_first ? 90 : 160;
    size_t cut = 0;

    for (size_t i = min_cut; i < len && i <= max_cut; i++) {
        char c = unspoken[i];
        char next = unspoken[i + 1];
        if ((c == '.' || c == '!' || c == '?' || c == ';' || c == ':' || c == '\n') &&
            (next == ' ' || next == '\n' || next == '\0')) {
            cut = i + 1;
            break;
        }
        if (is_first && i >= 28 && c == ',' && next == ' ') {
            cut = i + 1;
            break;
        }
    }
    if (cut == 0 && len >= max_cut) {
        for (size_t i = max_cut; i > min_cut; i--) {
            if (unspoken[i] == ' ') {
                cut = i;
                break;
            }
        }
    }
    if (cut == 0) return 0;

    /* Do not speak if a TOOL: directive appeared before cut */
    for (size_t i = 0; i + 5 <= cut; i++) {
        if (strncmp(unspoken + i, "TOOL:", 5) == 0) return 0;
    }

    size_t clen = (cut < out_cap - 1) ? cut : (out_cap - 1);
    memcpy(out_clause, unspoken, clen);
    out_clause[clen] = '\0';

    /* Trim leading/trailing whitespace */
    while (clen > 0 && isspace((unsigned char)out_clause[clen - 1])) {
        out_clause[--clen] = '\0';
    }
    size_t rem_start = cut;
    while (unspoken[rem_start] == ' ' || unspoken[rem_start] == '\n') rem_start++;
    memmove(unspoken, unspoken + rem_start, len - rem_start + 1);
    return clen > 0 ? 1 : 0;
}

static void append_tool_result_turn(ren_context_t *ctx, const char *tname, const char *targs,
                                    const char *tool_call_id, ren_event_cb cb, void *userdata,
                                    int is_native) {
    fprintf(stderr, "[tool] %s %s\n", tname, targs);
    if (cb) {
        cb("TOOL", tname, targs, userdata);
    }

    char *tres = NULL;
    int rc = dispatch_tool(tname, targs, &tres);
    if (!tres || !*tres) {
        free(tres);
        tres = strdup(rc == 0 ? "(ok, empty output)" : "Error: tool exited non-zero with no output");
    }

    size_t tlen = strlen(tres);
    if (ctx->max_tool_output > 0 && tlen > ctx->max_tool_output) {
        const char *trunc = "\n...(truncated)";
        size_t trlen = strlen(trunc);
        if (ctx->max_tool_output > trlen) {
            memcpy(tres + ctx->max_tool_output - trlen, trunc, trlen + 1);
        } else {
            tres[ctx->max_tool_output] = '\0';
        }
    }

    printf("[tool-result] %.240s%s\n", tres, strlen(tres) > 240 ? "..." : "");
    fflush(stdout);

    if (is_native) {
        struct json_object *tmsg = json_object_new_object();
        json_object_object_add(tmsg, "role", json_object_new_string("tool"));
        json_object_object_add(tmsg, "tool_call_id", json_object_new_string(tool_call_id));
        json_object_object_add(tmsg, "name", json_object_new_string(tname));
        json_object_object_add(tmsg, "content", json_object_new_string(tres));
        append_json_message(ctx, tmsg);
        json_object_put(tmsg);
    } else {
        size_t msg_cap = strlen(tres) + 32;
        char *tmsg = malloc(msg_cap);
        snprintf(tmsg, msg_cap, "TOOL_RESULT:\n%s", tres);
        append_message(ctx, "user", tmsg);
        free(tmsg);
    }

    free(tres);
}

static void emit_error(ren_event_cb cb, void *userdata, const char *msg) {
    fprintf(stderr, "[ren] Error: %s\n", msg);
    if (cb) {
        cb("DONE", "0", msg, userdata);
    }
}

static int is_aborted(void) {
    return g_local_abort || (g_abort_flag && *g_abort_flag);
}

static int poll_sock_or_ctrl(int sock, short events) {
    pthread_mutex_lock(&g_ctrl_mtx);
    int wfd = ensure_wake_pipe_locked();
    pthread_mutex_unlock(&g_ctrl_mtx);

    while (1) {
        if (is_aborted() || has_pending_steer()) return 0;
        struct pollfd pfds[2];
        nfds_t nfds = 1;
        pfds[0].fd = sock;
        pfds[0].events = events;
        pfds[0].revents = 0;
        if (wfd >= 0) {
            pfds[1].fd = wfd;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;
            nfds = 2;
        }
        int rc = poll(pfds, nfds, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                if (is_aborted() || has_pending_steer()) return 0;
                continue;
            }
            return -1;
        }
        if (wfd >= 0 && (pfds[1].revents & POLLIN)) {
            return 0;
        }
        if (pfds[0].revents & (events | POLLHUP | POLLERR | POLLNVAL)) {
            if (is_aborted() || has_pending_steer()) return 0;
            return 1;
        }
    }
}

static int is_context_overflow_error(int http_status, const char *msg) {
    if (http_status == 413) return 1;
    if (!msg || !*msg) return 0;
    static const char *const needles[] = {
        "context_length_exceeded",
        "maximum context length",
        "context length",
        "context window",
        "token limit",
        "too many tokens",
        "prompt is too long",
        "input is too long",
        "exceeds the maximum",
        "payload too large",
        "request entity too large",
        "Engine error -4",
        NULL
    };
    for (int i = 0; needles[i]; i++) {
        if (strcasestr(msg, needles[i]) != NULL) return 1;
    }
    return 0;
}

static int try_overflow_compact(ren_context_t *ctx, int *overflow_retries) {
    if (!ctx || *overflow_retries >= MAX_OVERFLOW_RETRIES) return 0;
    size_t before = ctx->history_len;
    size_t target = (before * 7) / 10;
    if (target < 1024) target = 1024;
    compact_history(ctx, target, "overflow");
    if (ctx->history_len < before) {
        (*overflow_retries)++;
        fprintf(stderr, "[ren] Context overflow; compacted history (%zu -> %zu bytes), retrying (%d/%d)\n",
                before, ctx->history_len, *overflow_retries, MAX_OVERFLOW_RETRIES);
        return 1;
    }
    return 0;
}

static char *build_native_turn_sig(const ren_tc_accum_t *tcs, int n_tcs) {
    size_t cap = 256, len = 0;
    char *sig = malloc(cap);
    if (!sig) return NULL;
    sig[0] = '\0';
    for (int i = 0; i < n_tcs; i++) {
        char *ua = unpack_tool_args(tcs[i].args);
        if (i > 0) buf_append(&sig, &len, &cap, "\x1e", 1);
        buf_append(&sig, &len, &cap, tcs[i].name, strlen(tcs[i].name));
        buf_append(&sig, &len, &cap, "\x1f", 1);
        if (ua) {
            buf_append(&sig, &len, &cap, ua, strlen(ua));
            free(ua);
        }
    }
    return sig;
}

static char *build_legacy_turn_sig(const char *tname, const char *targs) {
    size_t cap = 256, len = 0;
    char *sig = malloc(cap);
    if (!sig) return NULL;
    sig[0] = '\0';
    buf_append(&sig, &len, &cap, tname ? tname : "", tname ? strlen(tname) : 0);
    buf_append(&sig, &len, &cap, "\x1f", 1);
    buf_append(&sig, &len, &cap, targs ? targs : "", targs ? strlen(targs) : 0);
    return sig;
}

static int track_stall_repeat(char **prev_sig, int *stall_repeats, char *cur_sig) {
    if (!cur_sig) return 0;
    if (*prev_sig && strcmp(*prev_sig, cur_sig) == 0) {
        free(cur_sig);
        (*stall_repeats)++;
    } else {
        free(*prev_sig);
        *prev_sig = cur_sig;
        *stall_repeats = 1;
    }
    return *stall_repeats >= STALL_REPEAT_THRESHOLD;
}

int ren_chat_ex(ren_context_t *ctx, const char *prompt, const char *host, int port,
                ren_event_cb cb, void *userdata) {
    if (!ctx || !prompt || !*prompt) {
        emit_error(cb, userdata, "Invalid prompt or context");
        return -1;
    }
    reset_ctrl_state();
    sync_system_message(ctx);

    /* 1. Tier-0 Fast Deterministic Reflex Check (<1ms) */
    if (g_tier0_fn) {
        char *t0_op = NULL;
        char *t0_out = NULL;
        char *t0_spoken = NULL;
        if (g_tier0_fn(prompt, &t0_op, &t0_out, &t0_spoken) == 1) {
            const char *op_s = t0_op ? t0_op : "tier0";
            const char *out_s = t0_out ? t0_out : "ok";
            const char *spk_s = (t0_spoken && *t0_spoken) ? t0_spoken : out_s;

            printf("[tier-0] %s → %s\n", op_s, out_s);
            fflush(stdout);

            /* Record Tier-0 turn directly into ren's conversation history */
            append_message(ctx, "user", prompt);
            char hist_reply[1024];
            snprintf(hist_reply, sizeof(hist_reply), "[tier-0 %s] %s", op_s, out_s);
            append_message(ctx, "assistant", hist_reply);

            if (cb) {
                cb("TIER0", op_s, out_s, userdata);
                if (spk_s && *spk_s && strcmp(op_s, "tts_interrupt") != 0 && strcmp(op_s, "media_play") != 0) {
                    cb("CLAUSE", spk_s, "1", userdata);
                }
                cb("DONE", "1", out_s, userdata);
            }

            free(t0_op);
            free(t0_out);
            free(t0_spoken);
            reset_ctrl_state();
            return 0;
        }
        free(t0_op);
        free(t0_out);
        free(t0_spoken);
    }

    /* 2. Tier-1 Multi-Turn Agent Loop (axil-auren) */
    if (g_context_fn) {
        char *dctx = g_context_fn();
        if (dctx && *dctx) {
            size_t ucap = strlen(dctx) + strlen(prompt) + 64;
            char *umsg = malloc(ucap);
            snprintf(umsg, ucap, "[Live Desktop State: %s]\n%s", dctx, prompt);
            append_message(ctx, "user", umsg);
            free(umsg);
            free(dctx);
        } else {
            free(dctx);
            append_message(ctx, "user", prompt);
        }
    } else {
        append_message(ctx, "user", prompt);
    }

    int turn = 0;
    int overflow_retries = 0;
    const char *force_wrapup = NULL;
    char *prev_sig = NULL;
    int stall_repeats = 0;

    while (1) {
        if (is_aborted()) {
            free(prev_sig);
            emit_error(cb, userdata, "Interrupted");
            reset_ctrl_state();
            return -1;
        }

        char *steer_txt = NULL;
        if (consume_steer(&steer_txt)) {
            if (cb) cb("STEER", steer_txt, "", userdata);
            append_message(ctx, "user", steer_txt);
            free(steer_txt);
            overflow_retries = 0;
            stall_repeats = 0;
            force_wrapup = NULL;
        }

        const char *wrapup_reason = force_wrapup;
        if (!wrapup_reason && ctx->max_turns > 0 && turn >= ctx->max_turns) {
            wrapup_reason = "Tool turn limit reached.";
        }
        int wrapup = (wrapup_reason != NULL);

        if (ctx->max_history_chars > 0 && ctx->history_len > ctx->max_history_chars) {
            compact_history(ctx, ctx->max_history_chars, "limit");
        }

        struct json_object *req_obj = build_chat_request_json(ctx, wrapup_reason);
        const char *body = req_obj ? json_object_to_json_string_ext(req_obj, JSON_C_TO_STRING_PLAIN) : NULL;
        if (!body) {
            if (req_obj) json_object_put(req_obj);
            free(prev_sig);
            emit_error(cb, userdata, "Failed to serialize request JSON");
            reset_ctrl_state();
            return -1;
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            char emsg[256];
            snprintf(emsg, sizeof(emsg), "socket() failed: %s", strerror(errno));
            json_object_put(req_obj);
            free(prev_sig);
            emit_error(cb, userdata, emsg);
            reset_ctrl_state();
            return -1;
        }

        struct hostent *he = gethostbyname(host);
        if (!he) {
            char emsg[256];
            snprintf(emsg, sizeof(emsg), "Failed to resolve host %s", host);
            close(sock);
            json_object_put(req_obj);
            free(prev_sig);
            emit_error(cb, userdata, emsg);
            reset_ctrl_state();
            return -1;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            char emsg[256];
            snprintf(emsg, sizeof(emsg), "Failed to connect to axil-auren (%s:%d): %s",
                     host, port, strerror(errno));
            close(sock);
            json_object_put(req_obj);
            free(prev_sig);
            emit_error(cb, userdata, emsg);
            reset_ctrl_state();
            return -1;
        }

        size_t body_len = strlen(body);
        size_t req_cap = body_len + 512;
        char *request = malloc(req_cap);
        int req_len = snprintf(request, req_cap,
            "POST /v1/chat/completions HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n%s",
            host, port, body_len, body);
        json_object_put(req_obj);

        size_t sent = 0;
        int send_failed = 0;
        while (sent < (size_t)req_len) {
            if (is_aborted()) {
                send_failed = 1;
                break;
            }
            ssize_t n = send(sock, request + sent, (size_t)req_len - sent, 0);
            if (n < 0 && errno == EINTR && !is_aborted()) continue;
            if (n <= 0) {
                send_failed = 1;
                break;
            }
            sent += (size_t)n;
        }
        free(request);
        if (send_failed) {
            char emsg[256];
            if (is_aborted()) {
                snprintf(emsg, sizeof(emsg), "Interrupted");
            } else {
                snprintf(emsg, sizeof(emsg), "send() to axil-auren failed: %s", strerror(errno));
            }
            close(sock);
            free(prev_sig);
            emit_error(cb, userdata, emsg);
            reset_ctrl_state();
            return -1;
        }

        /* Read HTTP headers (and keep any body bytes that arrived in the same packet) */
        char buf[4096];
        size_t header_len = 0;
        size_t headers_cap = 8192;
        char *headers = malloc(headers_cap);
        headers[0] = '\0';
        char *body_start = NULL;
        int recv_err = 0;

        while (!body_start) {
            if (is_aborted() || has_pending_steer()) break;
            int prc = poll_sock_or_ctrl(sock, POLLIN);
            if (prc <= 0) {
                if (prc < 0) recv_err = errno;
                break;
            }
            ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n < 0) {
                if (errno == EINTR && !is_aborted() && !has_pending_steer()) continue;
                recv_err = errno;
                break;
            }
            if (n == 0) break;
            if (header_len + (size_t)n + 1 > headers_cap) {
                while (header_len + (size_t)n + 1 > headers_cap) headers_cap *= 2;
                headers = realloc(headers, headers_cap);
            }
            memcpy(headers + header_len, buf, (size_t)n);
            header_len += (size_t)n;
            headers[header_len] = '\0';
            body_start = strstr(headers, "\r\n\r\n");
        }

        if (!body_start) {
            if (is_aborted()) {
                free(headers);
                close(sock);
                free(prev_sig);
                emit_error(cb, userdata, "Interrupted");
                reset_ctrl_state();
                return -1;
            }
            char *steer_hdr = NULL;
            if (consume_steer(&steer_hdr)) {
                free(headers);
                close(sock);
                if (cb) cb("STEER", steer_hdr, "", userdata);
                append_message(ctx, "user", steer_hdr);
                free(steer_hdr);
                overflow_retries = 0;
                stall_repeats = 0;
                force_wrapup = NULL;
                continue;
            }
            char emsg[256];
            if (recv_err) {
                snprintf(emsg, sizeof(emsg), "recv() from axil-auren failed: %s", strerror(recv_err));
            } else {
                snprintf(emsg, sizeof(emsg), "axil-auren closed connection before HTTP headers");
            }
            free(headers);
            close(sock);
            free(prev_sig);
            emit_error(cb, userdata, emsg);
            reset_ctrl_state();
            return -1;
        }

        int http_status = 0;
        if (strncmp(headers, "HTTP/", 5) == 0) {
            const char *sp = strchr(headers, ' ');
            if (sp) http_status = atoi(sp + 1);
        }

        body_start += 4;
        size_t init_len = header_len - (size_t)(body_start - headers);

        if (http_status != 200) {
            char err_body[1024];
            size_t eblen = init_len < sizeof(err_body) - 1 ? init_len : sizeof(err_body) - 1;
            if (eblen > 0) memcpy(err_body, body_start, eblen);
            while (eblen < sizeof(err_body) - 1) {
                if (is_aborted() || has_pending_steer()) break;
                if (poll_sock_or_ctrl(sock, POLLIN) <= 0) break;
                ssize_t n = recv(sock, err_body + eblen, sizeof(err_body) - 1 - eblen, 0);
                if (n < 0 && errno == EINTR && !is_aborted() && !has_pending_steer()) continue;
                if (n <= 0) break;
                eblen += (size_t)n;
            }
            err_body[eblen] = '\0';
            free(headers);
            close(sock);

            if (is_aborted()) {
                free(prev_sig);
                emit_error(cb, userdata, "Interrupted");
                reset_ctrl_state();
                return -1;
            }

            char *steer_err = NULL;
            if (consume_steer(&steer_err)) {
                if (cb) cb("STEER", steer_err, "", userdata);
                append_message(ctx, "user", steer_err);
                free(steer_err);
                overflow_retries = 0;
                stall_repeats = 0;
                force_wrapup = NULL;
                continue;
            }

            if (is_context_overflow_error(http_status, err_body) &&
                try_overflow_compact(ctx, &overflow_retries)) {
                continue;
            }

            char emsg[1200];
            if (eblen > 0) {
                snprintf(emsg, sizeof(emsg), "Upstream HTTP %d: %s", http_status, err_body);
            } else {
                snprintf(emsg, sizeof(emsg), "Upstream HTTP %d", http_status);
            }
            free(prev_sig);
            emit_error(cb, userdata, emsg);
            reset_ctrl_state();
            return -1;
        }

        size_t event_cap = 16384;
        size_t event_len = 0;
        char *event_buf = malloc(event_cap);
        event_buf[0] = '\0';
        if (init_len > 0) {
            buf_append(&event_buf, &event_len, &event_cap, body_start, init_len);
        }
        free(headers);

        size_t full_cap = MAX_RESPONSE;
        char *full_response = malloc(full_cap);
        size_t full_len = 0;
        full_response[0] = '\0';

        char unspoken[8192];
        unspoken[0] = '\0';
        bool first_clause = true;

        ren_tc_accum_t tcs[MAX_TOOL_CALLS_PER_TURN];
        int n_tcs = 0;

        int got_sse_error = 0;
        char sse_err[512] = {0};
        int stream_recv_err = 0;
        int stop_reading = 0;
        int eof_seen = 0;

        char chunk[4096];
        while (1) {
            if (is_aborted() || has_pending_steer()) break;

            /* Parse complete newline-delimited SSE lines */
            char *ptr = event_buf;
            char *nl;
            while ((nl = memchr(ptr, '\n', event_len - (size_t)(ptr - event_buf))) != NULL) {
                *nl = '\0';
                size_t llen = (size_t)(nl - ptr);
                if (llen > 0 && ptr[llen - 1] == '\r') {
                    ptr[--llen] = '\0';
                }

                if (strncmp(ptr, "data: ", 6) == 0) {
                    const char *data = ptr + 6;
                    if (strcmp(data, "[DONE]") == 0) {
                        stop_reading = 1;
                        break;
                    }
                    struct json_object *jobj = json_tokener_parse(data);
                    if (jobj && json_object_is_type(jobj, json_type_object)) {
                        struct json_object *err_o = json_object_object_get(jobj, "error");
                        if (err_o) {
                            const char *emsg = NULL;
                            if (json_object_is_type(err_o, json_type_object)) {
                                struct json_object *mo = json_object_object_get(err_o, "message");
                                if (mo && json_object_is_type(mo, json_type_string)) {
                                    emsg = json_object_get_string(mo);
                                }
                            } else if (json_object_is_type(err_o, json_type_string)) {
                                emsg = json_object_get_string(err_o);
                            }
                            snprintf(sse_err, sizeof(sse_err), "%s",
                                     (emsg && *emsg) ? emsg : "Upstream SSE error");
                            got_sse_error = 1;
                            json_object_put(jobj);
                            stop_reading = 1;
                            break;
                        }

                        struct json_object *choices = json_object_object_get(jobj, "choices");
                        if (choices && json_object_is_type(choices, json_type_array) &&
                            json_object_array_length(choices) > 0) {
                            struct json_object *c0 = json_object_array_get_idx(choices, 0);
                            struct json_object *delta = c0 ? json_object_object_get(c0, "delta") : NULL;
                            if (delta && json_object_is_type(delta, json_type_object)) {
                                struct json_object *co = json_object_object_get(delta, "content");
                                if (co && json_object_is_type(co, json_type_string)) {
                                    const char *ds = json_object_get_string(co);
                                    if (ds && ds[0] != '\0') {
                                        printf("%s", ds);
                                        fflush(stdout);
                                        buf_append(&full_response, &full_len, &full_cap, ds, strlen(ds));
                                        if (!wrapup && n_tcs == 0) {
                                            char *tl = strstr(full_response, "TOOL: ");
                                            if (tl) {
                                                char *tnl = strchr(tl, '\n');
                                                if (tnl) {
                                                    *tnl = '\0';
                                                    full_len = (size_t)(tnl - full_response);
                                                    json_object_put(jobj);
                                                    stop_reading = 1;
                                                    break;
                                                }
                                            }
                                        }
                                        if (cb && (wrapup || n_tcs == 0) && strlen(unspoken) + strlen(ds) + 1 < sizeof(unspoken)) {
                                            strcat(unspoken, ds);
                                            char clause[512];
                                            while (pop_spoken_clause(unspoken, first_clause, clause, sizeof(clause))) {
                                                cb("CLAUSE", clause, "0", userdata);
                                                first_clause = false;
                                            }
                                        }
                                    }
                                }

                                struct json_object *tc_arr = json_object_object_get(delta, "tool_calls");
                                if (tc_arr && json_object_is_type(tc_arr, json_type_array)) {
                                    size_t n_tc_items = json_object_array_length(tc_arr);
                                    for (size_t ti = 0; ti < n_tc_items; ti++) {
                                        struct json_object *tc_item = json_object_array_get_idx(tc_arr, ti);
                                        if (!tc_item || !json_object_is_type(tc_item, json_type_object)) continue;

                                        struct json_object *id_o = json_object_object_get(tc_item, "id");
                                        const char *id_s = (id_o && json_object_is_type(id_o, json_type_string))
                                            ? json_object_get_string(id_o) : NULL;

                                        struct json_object *idx_o = json_object_object_get(tc_item, "index");
                                        int tc_idx = 0;
                                        if (idx_o && json_object_is_type(idx_o, json_type_int)) {
                                            tc_idx = json_object_get_int(idx_o);
                                        } else if (id_s && *id_s && n_tcs > 0 && strcmp(tcs[n_tcs - 1].id, id_s) != 0) {
                                            tc_idx = n_tcs;
                                        }

                                        ren_tc_accum_t *slot = find_or_add_tc(tcs, &n_tcs, tc_idx, id_s);
                                        if (!slot) continue;

                                        if (id_s && *id_s) {
                                            snprintf(slot->id, sizeof(slot->id), "%s", id_s);
                                        }
                                        struct json_object *fn_o = json_object_object_get(tc_item, "function");
                                        if (fn_o && json_object_is_type(fn_o, json_type_object)) {
                                            struct json_object *name_o = json_object_object_get(fn_o, "name");
                                            if (name_o && json_object_is_type(name_o, json_type_string)) {
                                                const char *nm = json_object_get_string(name_o);
                                                if (nm && *nm) {
                                                    snprintf(slot->name, sizeof(slot->name), "%s", nm);
                                                }
                                            }
                                            struct json_object *args_o = json_object_object_get(fn_o, "arguments");
                                            if (args_o && json_object_is_type(args_o, json_type_string)) {
                                                const char *as = json_object_get_string(args_o);
                                                if (as && *as) {
                                                    buf_append(&slot->args, &slot->args_len, &slot->args_cap, as, strlen(as));
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (jobj) json_object_put(jobj);
                }

                ptr = nl + 1;
            }

            if (stop_reading || eof_seen) break;

            size_t remaining = event_len - (size_t)(ptr - event_buf);
            if (ptr != event_buf) {
                memmove(event_buf, ptr, remaining);
                event_len = remaining;
                event_buf[event_len] = '\0';
            }

            if (is_aborted() || has_pending_steer()) break;
            int prc = poll_sock_or_ctrl(sock, POLLIN);
            if (prc < 0) {
                stream_recv_err = errno;
                break;
            }
            if (prc == 0) break;

            ssize_t n = recv(sock, chunk, sizeof(chunk) - 1, 0);
            if (n < 0) {
                if (errno == EINTR && !is_aborted() && !has_pending_steer()) continue;
                stream_recv_err = errno;
                break;
            }
            if (n == 0) {
                if (event_len > 0) {
                    buf_append(&event_buf, &event_len, &event_cap, "\n", 1);
                    eof_seen = 1;
                    continue;
                }
                break;
            }
            buf_append(&event_buf, &event_len, &event_cap, chunk, (size_t)n);
        }

        close(sock);
        free(event_buf);

        if (full_len > 0) {
            printf("\n");
            fflush(stdout);
        }

        if (is_aborted()) {
            free_tcs(tcs, n_tcs);
            free(full_response);
            free(prev_sig);
            emit_error(cb, userdata, "Interrupted");
            reset_ctrl_state();
            return -1;
        }

        char *steer_stream = NULL;
        if (consume_steer(&steer_stream)) {
            free_tcs(tcs, n_tcs);
            free(full_response);
            if (cb) cb("STEER", steer_stream, "", userdata);
            append_message(ctx, "user", steer_stream);
            free(steer_stream);
            overflow_retries = 0;
            stall_repeats = 0;
            force_wrapup = NULL;
            continue;
        }

        if (got_sse_error) {
            free_tcs(tcs, n_tcs);
            free(full_response);
            if (is_context_overflow_error(200, sse_err) &&
                try_overflow_compact(ctx, &overflow_retries)) {
                continue;
            }
            free(prev_sig);
            emit_error(cb, userdata, sse_err);
            reset_ctrl_state();
            return -1;
        }

        if (stream_recv_err && full_len == 0 && n_tcs == 0) {
            char emsg[256];
            snprintf(emsg, sizeof(emsg), "Stream recv() error: %s", strerror(stream_recv_err));
            free_tcs(tcs, n_tcs);
            free(full_response);
            free(prev_sig);
            emit_error(cb, userdata, emsg);
            reset_ctrl_state();
            return -1;
        }

        overflow_retries = 0;

        /* 2a. Native OpenAI tool_calls turn */
        if (!wrapup && n_tcs > 0) {
            if (track_stall_repeat(&prev_sig, &stall_repeats, build_native_turn_sig(tcs, n_tcs))) {
                fprintf(stderr, "[ren] Stall detected (%d identical tool turns); wrapping up.\n",
                        STALL_REPEAT_THRESHOLD);
                free_tcs(tcs, n_tcs);
                free(full_response);
                force_wrapup = "Repeated identical tool calls detected.";
                stall_repeats = 0;
                continue;
            }

            struct json_object *asst = json_object_new_object();
            json_object_object_add(asst, "role", json_object_new_string("assistant"));
            if (full_len > 0) {
                json_object_object_add(asst, "content", json_object_new_string(full_response));
            } else {
                json_object_object_add(asst, "content", NULL);
            }
            struct json_object *tc_arr = json_object_new_array();
            for (int i = 0; i < n_tcs; i++) {
                if (!tcs[i].id[0]) {
                    snprintf(tcs[i].id, sizeof(tcs[i].id), "call_%d_%d", turn, i);
                }
                struct json_object *tc = json_object_new_object();
                json_object_object_add(tc, "id", json_object_new_string(tcs[i].id));
                json_object_object_add(tc, "type", json_object_new_string("function"));
                struct json_object *fn = json_object_new_object();
                json_object_object_add(fn, "name", json_object_new_string(tcs[i].name));
                json_object_object_add(fn, "arguments",
                    json_object_new_string(tcs[i].args ? tcs[i].args : "{}"));
                json_object_object_add(tc, "function", fn);
                json_object_array_add(tc_arr, tc);
            }
            json_object_object_add(asst, "tool_calls", tc_arr);
            append_json_message(ctx, asst);
            json_object_put(asst);

            for (int i = 0; i < n_tcs; i++) {
                if (is_aborted() || has_pending_steer()) {
                    const char *skip_msg = is_aborted() ? "Aborted by user." : "Skipped: user steered.";
                    for (int j = i; j < n_tcs; j++) {
                        struct json_object *tr = json_object_new_object();
                        json_object_object_add(tr, "role", json_object_new_string("tool"));
                        json_object_object_add(tr, "tool_call_id", json_object_new_string(tcs[j].id));
                        json_object_object_add(tr, "name", json_object_new_string(tcs[j].name));
                        json_object_object_add(tr, "content", json_object_new_string(skip_msg));
                        append_json_message(ctx, tr);
                        json_object_put(tr);
                    }
                    break;
                }
                char *targs = unpack_tool_args(tcs[i].args);
                append_tool_result_turn(ctx, tcs[i].name, targs ? targs : "",
                                        tcs[i].id, cb, userdata, 1);
                free(targs);
            }

            free_tcs(tcs, n_tcs);
            free(full_response);

            if (is_aborted()) {
                free(prev_sig);
                emit_error(cb, userdata, "Interrupted");
                reset_ctrl_state();
                return -1;
            }

            char *steer_tools = NULL;
            if (consume_steer(&steer_tools)) {
                if (cb) cb("STEER", steer_tools, "", userdata);
                append_message(ctx, "user", steer_tools);
                free(steer_tools);
                overflow_retries = 0;
                stall_repeats = 0;
                force_wrapup = NULL;
            }

            turn++;
            continue;
        }
        free_tcs(tcs, n_tcs);

        /* 2b. Legacy/local model TOOL: fallback */
        char *tool_line = !wrapup ? strstr(full_response, "TOOL: ") : NULL;
        if (!tool_line) {
            if (full_len == 0) {
                free(full_response);
                free(prev_sig);
                const char *emsg = "Upstream returned empty response";
                if (wrapup) {
                    emsg = force_wrapup ? "Loop detected: repeated identical tool calls" : "Max tool turns reached";
                }
                emit_error(cb, userdata, emsg);
                reset_ctrl_state();
                return -1;
            }
            append_message(ctx, "assistant", full_response);
            char *steer_fin = NULL;
            if (consume_steer(&steer_fin)) {
                free(full_response);
                if (cb) cb("STEER", steer_fin, "", userdata);
                append_message(ctx, "user", steer_fin);
                free(steer_fin);
                overflow_retries = 0;
                stall_repeats = 0;
                force_wrapup = NULL;
                turn++;
                continue;
            }
            if (cb) {
                char *rem = unspoken;
                while (*rem && isspace((unsigned char)*rem)) rem++;
                if (*rem) {
                    cb("CLAUSE", rem, "1", userdata);
                }
                cb("DONE", "1", full_response, userdata);
            }
            free(full_response);
            free(prev_sig);
            reset_ctrl_state();
            return 0;
        }

        /* Extract tool name + args first so stall detection runs before mutating ctx->history */
        char *line_end = strchr(tool_line, '\n');
        if (line_end) *line_end = '\0';

        char *spec = tool_line + 6;
        while (*spec == ' ') spec++;
        char *space = strchr(spec, ' ');
        const char *tname = spec;
        const char *targs = "";
        if (space) {
            *space = '\0';
            targs = space + 1;
            while (*targs == ' ') targs++;
        }

        if (track_stall_repeat(&prev_sig, &stall_repeats, build_legacy_turn_sig(tname, targs))) {
            fprintf(stderr, "[ren] Stall detected (%d identical tool turns); wrapping up.\n",
                    STALL_REPEAT_THRESHOLD);
            free(full_response);
            force_wrapup = "Repeated identical tool calls detected.";
            stall_repeats = 0;
            continue;
        }

        if (space) *space = ' ';
        if (line_end) *line_end = '\n';
        append_message(ctx, "assistant", full_response);
        if (line_end) *line_end = '\0';
        if (space) *space = '\0';

        append_tool_result_turn(ctx, tname, targs, "", cb, userdata, 0);
        free(full_response);

        if (is_aborted()) {
            free(prev_sig);
            emit_error(cb, userdata, "Interrupted");
            reset_ctrl_state();
            return -1;
        }

        char *steer_leg = NULL;
        if (consume_steer(&steer_leg)) {
            if (cb) cb("STEER", steer_leg, "", userdata);
            append_message(ctx, "user", steer_leg);
            free(steer_leg);
            overflow_retries = 0;
            stall_repeats = 0;
            force_wrapup = NULL;
        }

        turn++;
    }
}

int ren_chat(ren_context_t *ctx, const char *prompt, const char *host, int port) {
    return ren_chat_ex(ctx, prompt, host, port, NULL, NULL);
}
