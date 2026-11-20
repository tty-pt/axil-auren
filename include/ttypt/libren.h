#ifndef LIBREN_H
#define LIBREN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <ttypt/xy.h>

typedef struct ren_context {
    char *session_id;
    char *history; /* JSON messages array */
    size_t history_len;
    size_t history_cap;
} ren_context_t;

/* Tool execution callback: returns 0 on success, <0 on error; caller frees *out_result */
typedef int (*ren_tool_func)(const char *args, char **out_result);

/* Optional Tier-0 reflex solver hook registered by mod_jarvis.
 * Returns 1 if Tier-0 deterministically handled the prompt (populates *out_op,
 * *out_output, *out_spoken — caller frees them), or 0 to fall through to Tier-1 LLM. */
typedef int (*ren_tier0_func)(const char *prompt, char **out_op, char **out_output, char **out_spoken);

/* Optional dynamic context provider registered by mod_jarvis (e.g. live desktop state).
 * Returns a heap-allocated summary string (caller frees) or NULL. */
typedef char *(*ren_context_func)(void);

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

/* Tool, Tier-0, and context registration */
void ren_register_tool(const char *name, const char *usage, ren_tool_func fn);
void ren_register_tier0(ren_tier0_func fn);
void ren_register_context_provider(ren_context_func fn);
int ren_load_module(const char *path);

void ren_xy_install(void);

#endif /* LIBREN_H */
