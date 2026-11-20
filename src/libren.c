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
#include "ttypt/libren.h"

#define MAX_RESPONSE 65536
#define MAX_TOOL_TURNS 8
#define MAX_HISTORY_CHARS 16000
#define MAX_TOOLS 64

typedef struct {
    char *name;
    char *usage;
    ren_tool_func fn;
} ren_tool_entry_t;

static ren_tool_entry_t g_tools[MAX_TOOLS];
static int g_num_tools = 0;
static char *g_system_prompt = NULL;
static int g_tools_loaded = 0;
static ren_tier0_func g_tier0_fn = NULL;
static ren_context_func g_context_fn = NULL;

static void rebuild_system_prompt(void) {
    free(g_system_prompt);
    size_t cap = 1024;
    g_system_prompt = malloc(cap);
    snprintf(g_system_prompt, cap,
        "You are ren (Jarvis's executive brain), a pure-C coding and Linux desktop assistant. Available tools:\n");
    size_t len = strlen(g_system_prompt);
    for (int i = 0; i < g_num_tools; i++) {
        size_t need = len + strlen(g_tools[i].usage) + 32;
        if (need > cap) {
            while (need > cap) cap *= 2;
            g_system_prompt = realloc(g_system_prompt, cap);
        }
        snprintf(g_system_prompt + len, cap - len, "- %s\n", g_tools[i].usage);
        len = strlen(g_system_prompt);
    }
    const char *tail =
        "To run a tool, output ONLY a single line starting with: TOOL: <name> <args>\n"
        "Wait for TOOL_RESULT before answering.\n"
        "You have full live GUI and browser control via 'inspect', 'browser', 'window', and 'input'. Never claim you cannot access or interact with browser tabs, WhatsApp Web, or desktop apps.\n"
        "To interact with an open browser tab or send a message (e.g. on WhatsApp Web):\n"
        "1. Run 'TOOL: inspect browser' (or 'TOOL: browser tab whatsapp') to focus the browser and see tabs and UI elements.\n"
        "2. Run 'TOOL: input send <contact_or_entry> | <message>' (or 'TOOL: input click <contact>' then 'TOOL: input submit <message>') to select the conversation, type the text, and press Enter.\n"
        "After receiving a final TOOL_RESULT that completes the user's request, always output a brief 1-sentence confirmation to the user (never an empty reply).\n"
        "When answering the user directly (no TOOL: line), keep spoken replies concise, natural, and direct.\n"
        "Context is limited. Use 'record' to save important findings before they scroll out.";
    if (len + strlen(tail) + 2 > cap) {
        cap += strlen(tail) + 64;
        g_system_prompt = realloc(g_system_prompt, cap);
    }
    strcat(g_system_prompt, tail);
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

void ren_register_tier0(ren_tier0_func fn) {
    g_tier0_fn = fn;
}

void ren_register_context_provider(ren_context_func fn) {
    g_context_fn = fn;
}

int ren_load_module(const char *path) {
    char clean[512];
    snprintf(clean, sizeof(clean), "%s", path);
    size_t len = strlen(clean);
    if (len > 3 && strcmp(clean + len - 3, ".so") == 0) {
        clean[len - 3] = '\0';
    }

    char probe[1050];
    snprintf(probe, sizeof(probe), "%s.so", clean);
    if (access(probe, R_OK) == 0) {
        return xy_load(clean);
    }

    const char *home = getenv("HOME");
    if (home && *home && path[0] != '/') {
        const char *base = strrchr(clean, '/');
        base = base ? (base + 1) : clean;
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s/.local/lib/ren/%s", home, base);
        snprintf(probe, sizeof(probe), "%s.so", candidate);
        if (access(probe, R_OK) == 0) {
            return xy_load(candidate);
        }
        snprintf(candidate, sizeof(candidate), "%s/site/external/axil-auren/%s", home, clean);
        snprintf(probe, sizeof(probe), "%s.so", candidate);
        if (access(probe, R_OK) == 0) {
            return xy_load(candidate);
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
    ren_load_module("mods/mm/mod_mm");
    ren_load_module("mods/jarvis/mod_jarvis");
}

static void buf_append(char **buf, size_t *len, size_t *cap, const char *str, size_t slen) {
    if (*len + slen + 1 > *cap) {
        while (*len + slen + 1 > *cap) *cap *= 2;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, str, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

static void buf_append_json_escaped(char **buf, size_t *len, size_t *cap, const char *s) {
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  buf_append(buf, len, cap, "\\\"", 2); break;
            case '\\': buf_append(buf, len, cap, "\\\\", 2); break;
            case '\n': buf_append(buf, len, cap, "\\n", 2); break;
            case '\r': buf_append(buf, len, cap, "\\r", 2); break;
            case '\t': buf_append(buf, len, cap, "\\t", 2); break;
            default:
                if (c >= 0x20) {
                    buf_append(buf, len, cap, p, 1);
                }
                break;
        }
    }
}

static void trim_history(ren_context_t *ctx) {
    while (ctx->history_len > MAX_HISTORY_CHARS) {
        char *p = strstr(ctx->history, "},{\"role\"");
        if (!p) break;
        char *q = strstr(p + 2, "},{\"role\"");
        if (!q) break;
        size_t remove_len = (size_t)(q - (p + 1));
        memmove(p + 1, q, ctx->history_len - (q - ctx->history) + 1);
        ctx->history_len -= remove_len;
    }
}

static void append_message(ren_context_t *ctx, const char *role, const char *content) {
    if (ctx->history_len > 0) {
        buf_append(&ctx->history, &ctx->history_len, &ctx->history_cap, ",", 1);
    }
    buf_append(&ctx->history, &ctx->history_len, &ctx->history_cap, "{\"role\":\"", 9);
    buf_append(&ctx->history, &ctx->history_len, &ctx->history_cap, role, strlen(role));
    buf_append(&ctx->history, &ctx->history_len, &ctx->history_cap, "\",\"content\":\"", 13);
    buf_append_json_escaped(&ctx->history, &ctx->history_len, &ctx->history_cap, content);
    buf_append(&ctx->history, &ctx->history_len, &ctx->history_cap, "\"}", 2);
}

static void sync_system_message(ren_context_t *ctx) {
    if (!g_system_prompt) return;
    if (ctx->history_len == 0) {
        append_message(ctx, "system", g_system_prompt);
        return;
    }
    /* Replace leading system message if tools were added after session load */
    if (strncmp(ctx->history, "{\"role\":\"system\",\"content\":\"", 28) == 0) {
        char *end = strstr(ctx->history, "\"},{\"role\":");
        if (end) {
            char *rest = strdup(end + 2); /* starts with ,{"role":... */
            ctx->history_len = 0;
            ctx->history[0] = '\0';
            append_message(ctx, "system", g_system_prompt);
            buf_append(&ctx->history, &ctx->history_len, &ctx->history_cap, rest, strlen(rest));
            free(rest);
        } else {
            ctx->history_len = 0;
            ctx->history[0] = '\0';
            append_message(ctx, "system", g_system_prompt);
        }
    }
}

void ren_init(ren_context_t *ctx) {
    load_default_tools();
    ctx->session_id = strdup("session-1");
    ctx->history_cap = 4096;
    ctx->history = malloc(ctx->history_cap);
    ctx->history[0] = '\0';
    ctx->history_len = 0;

    append_message(ctx, "system", g_system_prompt ? g_system_prompt : "You are ren.");
}

void ren_reset(ren_context_t *ctx) {
    if (!ctx || !ctx->history) return;
    ctx->history_len = 0;
    ctx->history[0] = '\0';
    append_message(ctx, "system", g_system_prompt ? g_system_prompt : "You are ren.");
}

void ren_free(ren_context_t *ctx) {
    free(ctx->session_id);
    free(ctx->history);
}

static void extract_delta(const char *json, char *out, size_t out_cap) {
    out[0] = '\0';
    const char *c = strstr(json, "\"content\"");
    if (!c) return;
    c = strchr(c, ':');
    if (!c) return;
    c++;
    while (*c == ' ' || *c == '\t') c++;
    if (*c != '"') return;
    c++;
    size_t olen = 0;
    for (const char *p = c; *p && *p != '"' && olen < out_cap - 1; p++) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n': out[olen++] = '\n'; break;
                case 'r': out[olen++] = '\r'; break;
                case 't': out[olen++] = '\t'; break;
                case '"': out[olen++] = '"'; break;
                case '\\': out[olen++] = '\\'; break;
                default: out[olen++] = *p; break;
            }
        } else {
            out[olen++] = *p;
        }
    }
    out[olen] = '\0';
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

int ren_chat_ex(ren_context_t *ctx, const char *prompt, const char *host, int port,
                ren_event_cb cb, void *userdata) {
    if (!ctx || !prompt || !*prompt) return -1;
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

    char last_tool_result[256] = {0};

    for (int turn = 0; turn < MAX_TOOL_TURNS; turn++) {
        trim_history(ctx);
        size_t body_cap = ctx->history_len + 128;
        char *body = malloc(body_cap);
        snprintf(body, body_cap,
            "{\"model\":\"fast\",\"messages\":[%s],\"stream\":true}",
            ctx->history);

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { free(body); return -1; }

        struct hostent *he = gethostbyname(host);
        if (!he) { close(sock); free(body); return -1; }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(sock); free(body);
            if (cb) cb("DONE", "0", "Failed to connect to axil-auren", userdata);
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

        size_t sent = 0;
        while (sent < (size_t)req_len) {
            ssize_t n = send(sock, request + sent, req_len - sent, 0);
            if (n <= 0) { free(request); free(body); close(sock); return -1; }
            sent += n;
        }
        free(request);
        free(body);

        /* Read headers (and keep any body bytes that arrived in the same packet) */
        char buf[4096];
        size_t header_len = 0;
        char *headers = malloc(8192);
        size_t headers_cap = 8192;
        char *body_start = NULL;

        while (!body_start) {
            ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (header_len + n + 1 > headers_cap) {
                headers_cap *= 2;
                headers = realloc(headers, headers_cap);
            }
            memcpy(headers + header_len, buf, n);
            header_len += n;
            headers[header_len] = '\0';
            body_start = strstr(headers, "\r\n\r\n");
        }

        /* Process remaining data as SSE */
        size_t full_cap = MAX_RESPONSE;
        char *full_response = malloc(full_cap);
        size_t full_len = 0;
        full_response[0] = '\0';

        char unspoken[8192];
        unspoken[0] = '\0';
        bool first_clause = true;

        char event_buf[16384];
        size_t event_len = 0;
        event_buf[0] = '\0';

        if (body_start) {
            body_start += 4;
            size_t init_len = header_len - (size_t)(body_start - headers);
            if (init_len > 0 && init_len < sizeof(event_buf) - 1) {
                memcpy(event_buf, body_start, init_len);
                event_len = init_len;
                event_buf[event_len] = '\0';
            }
        }
        free(headers);

        char chunk[4096];
        while (1) {
            /* First parse any complete events already in event_buf */
            char *ptr = event_buf;
            char *end;
            while ((end = strstr(ptr, "\n\n")) != NULL) {
                *end = '\0';
                char *data = strstr(ptr, "data: ");
                if (data) {
                    data += 6;
                    if (strncmp(data, "[DONE]", 6) == 0) {
                        goto done;
                    }
                    char delta[4096];
                    extract_delta(data, delta, sizeof(delta));
                    if (delta[0] != '\0') {
                        printf("%s", delta);
                        fflush(stdout);
                        buf_append(&full_response, &full_len, &full_cap, delta, strlen(delta));
                        char *tl = strstr(full_response, "TOOL: ");
                        if (tl) {
                            char *nl = strchr(tl, '\n');
                            if (nl) {
                                *nl = '\0';
                                full_len = (size_t)(nl - full_response);
                                goto done;
                            }
                        }
                        if (cb && strlen(unspoken) + strlen(delta) + 1 < sizeof(unspoken)) {
                            strcat(unspoken, delta);
                            char clause[512];
                            while (pop_spoken_clause(unspoken, first_clause, clause, sizeof(clause))) {
                                cb("CLAUSE", clause, "0", userdata);
                                first_clause = false;
                            }
                        }
                    }
                }
                ptr = end + 2;
            }
            size_t remaining = event_len - (size_t)(ptr - event_buf);
            if (ptr != event_buf) {
                memmove(event_buf, ptr, remaining);
                event_len = remaining;
                event_buf[event_len] = '\0';
            }

            ssize_t n = recv(sock, chunk, sizeof(chunk) - 1, 0);
            if (n <= 0) break;
            chunk[n] = '\0';

            if (event_len + n + 1 > sizeof(event_buf)) {
                event_len = 0;
            }
            memcpy(event_buf + event_len, chunk, n);
            event_len += n;
            event_buf[event_len] = '\0';
        }
    done:
        close(sock);

        printf("\n");
        fflush(stdout);

        /* Check for TOOL: */
        char *tool_line = strstr(full_response, "TOOL: ");
        if (!tool_line) {
            const char *final_resp = full_response;
            if (!final_resp[0] && last_tool_result[0]) {
                final_resp = last_tool_result;
            }
            append_message(ctx, "assistant", final_resp);
            /* Flush any remaining unspoken text to TTS */
            if (cb) {
                char *rem = unspoken;
                while (*rem && isspace((unsigned char)*rem)) rem++;
                if (*rem) {
                    cb("CLAUSE", rem, "1", userdata);
                }
                cb("DONE", "1", final_resp, userdata);
            }
            free(full_response);
            return 0;
        }

        append_message(ctx, "assistant", full_response);

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

        fprintf(stderr, "[tool] %s %s\n", tname, targs);
        if (cb) {
            cb("TOOL", tname, targs, userdata);
        }

        char *tres = NULL;
        dispatch_tool(tname, targs, &tres);
        if (!tres) tres = strdup("OK");

        snprintf(last_tool_result, sizeof(last_tool_result), "%.250s", tres);
        char *nl_pos = strchr(last_tool_result, '\n');
        if (nl_pos) *nl_pos = '\0';

        printf("[tool-result] %.240s%s\n", tres, strlen(tres) > 240 ? "..." : "");
        fflush(stdout);

        size_t msg_cap = strlen(tres) + 32;
        char *tmsg = malloc(msg_cap);
        snprintf(tmsg, msg_cap, "TOOL_RESULT:\n%s", tres);
        append_message(ctx, "user", tmsg);

        free(tmsg);
        free(tres);
        free(full_response);
    }

    if (cb) cb("DONE", "1", "Max tool turns reached", userdata);
    return 0;
}

int ren_chat(ren_context_t *ctx, const char *prompt, const char *host, int port) {
    return ren_chat_ex(ctx, prompt, host, port, NULL, NULL);
}
