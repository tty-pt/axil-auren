#ifndef LIBREN_H
#define LIBREN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <signal.h>
#include <ttypt/xy.h>

#define REN_DEFAULT_TOOL_OUTPUT_CHARS 32768

typedef struct ren_context {
    char *session_id;
    char *history; /* JSON messages array */
    size_t history_len;
    size_t history_cap;
    int max_turns;            /* 0 = unlimited (default); >0 = turn cap before wrap-up */
    size_t max_history_chars; /* 0 = overflow-driven (default); >0 = proactive client cap */
    size_t max_tool_output;   /* 0 = unlimited; >0 = per-tool-result char cap */
} ren_context_t;

/* Tool execution callback: returns 0 on success, <0 on error; caller frees *out_result */
typedef int (*ren_tool_func)(const char *args, char **out_result);

/* Optional Tier-0 reflex solver hook registered by an extension module.
 * Returns 1 if Tier-0 deterministically handled the prompt (populates *out_op,
 * *out_output, *out_spoken — caller frees them), or 0 to fall through to Tier-1 LLM. */
typedef int (*ren_tier0_func)(const char *prompt, char **out_op, char **out_output, char **out_spoken);

/* Optional dynamic context provider registered by an extension module.
 * Returns a heap-allocated summary string (caller frees) or NULL. */
typedef char *(*ren_context_func)(void);

/* Optional history compactor hook registered by a xylem extension.
 * Called when max_history_chars is exceeded ("limit") or when upstream rejects
 * the payload for context length ("overflow").
 * Returns >0 if ctx->history was reduced, 0 to fall back to ren_compact_default(),
 * or <0 on error (also falls back safely on overflow). */
typedef int (*ren_compact_func)(ren_context_t *ctx, size_t target_bytes, const char *reason);

/* Streaming event callback for socket IPC / TTS streaming:
 * event_type: "TIER0", "TOOL", "CLAUSE", "DONE" */
typedef void (*ren_event_cb)(const char *event_type, const char *arg1, const char *arg2, void *userdata);

typedef struct ren_module {
    const char *name;
    const char *description;
    const char *schema;
    ren_tool_func execute;
} ren_module_t;

void ren_init(ren_context_t *ctx);
void ren_free(ren_context_t *ctx);
void ren_reset(ren_context_t *ctx);

/* Core loop */
int ren_chat(ren_context_t *ctx, const char *prompt, const char *host, int port);
int ren_chat_ex(ren_context_t *ctx, const char *prompt, const char *host, int port,
                ren_event_cb cb, void *userdata);

/* Built-in atomic oldest-group history compactor (usable directly by extensions) */
int ren_compact_default(ren_context_t *ctx, size_t target_bytes);

/* Tool, prompt-hint, Tier-0, context, compactor, and interrupt registration */
void ren_register_tool(const char *name, const char *usage, ren_tool_func fn);
void ren_register_prompt_hint(const char *hint);
void ren_register_tier0(ren_tier0_func fn);
void ren_register_context_provider(ren_context_func fn);
void ren_register_compactor(ren_compact_func fn);
void ren_set_system_prompt(const char *prompt);
void ren_set_model(const char *model);
void ren_set_abort_flag(volatile sig_atomic_t *flag);
void ren_steer(const char *text);
void ren_abort(void);
int ren_load_module(const char *path);
int ren_call_tool(const char *name, const char *args, char **out_result);

void ren_xy_install(void);

#endif /* LIBREN_H */
