/* auren-upstream.h — Cloud upstream router & protocol translator for axil-auren.
 *
 * Routes OpenAI-compatible /v1/chat/completions and /v1/models across:
 *   1. Antigravity Direct Cloud Code API (Google Cloud Code v1internal SSE)
 *   2. Kilo Gateway / OpenRouter API
 *   3. GitHub Copilot Chat Completions API
 *   4. Smart aliases ("very-fast", "fast", "smart", "genius", "auto") with
 *      tiered fallback chains, circuit breakers, and SystemOne intent routing.
 *
 * Upstream HTTP requests run on detached worker pthreads via libcurl and push
 * completion chunks into auren-engine's thread-safe completion queue so they
 * never block the local llama.cpp worker (embeddings / SystemOne / local chat).
 *
 * Performance optimizations:
 *   - Persistent curl connection pool (HTTP/2 keep-alive)
 *   - Per-model circuit breakers with exponential backoff
 *   - Session affinity hash for provider-side prompt cache hits
 *   - SystemOne bge-base intent classification (< 2 ms) for "auto" routing
 */

#ifndef AUREN_UPSTREAM_H
#define AUREN_UPSTREAM_H

#include <json-c/json.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum auren_upstream_provider {
	AUREN_UPSTREAM_NONE = 0,
	AUREN_UPSTREAM_ANTIGRAVITY = 1,
	AUREN_UPSTREAM_KILO = 2,
	AUREN_UPSTREAM_COPILOT = 3,
	AUREN_UPSTREAM_ALIAS_VERY_FAST = 4,
	AUREN_UPSTREAM_ALIAS_FAST = 5,
	AUREN_UPSTREAM_ALIAS_SMART = 6,
	AUREN_UPSTREAM_ALIAS_GENIUS = 7,
};

struct auren_upstream_route {
	enum auren_upstream_provider provider;
	char requested_model[128]; /* model ID requested by caller */
	char canonical_id[128];    /* base model ID without provider prefix */
	char runtime_model[128];   /* backend wire model ID */
	char effort[32];           /* "low", "medium", "high", "xhigh", or "" */
	int  is_thinking;          /* 1 if model uses thinking/reasoning config */
	int  is_claude;            /* 1 if Claude model on Antigravity */
	int  is_gpt_oss;           /* 1 if GPT-OSS model on Antigravity */
	int  model_enum;           /* Cloud Code model_enum label (>0 if known) */
};

/* Initialize upstream router state (curl global init, caches). */
int auren_upstream_init(void);

/* Shutdown upstream router state. */
void auren_upstream_shutdown(void);

/* Returns 1 if an upstream auth.json file exists or upstream env vars are set. */
int auren_upstream_has_credentials(void);

/* Classify a requested model_id into an upstream route.
 * Returns AUREN_UPSTREAM_NONE (0) if model_id is not an upstream model.
 * Note: callers should check local auren_registry_find() first if local models
 * take precedence, except for explicit provider prefixes ("antigravity/",
 * "kilo/", "copilot/", "github-copilot/") or aliases ("very-fast", "fast",
 * "smart", "genius", "auto"). */
enum auren_upstream_provider auren_upstream_resolve_model(
	const char *model_id,
	const char *requested_effort,
	struct auren_upstream_route *out);

/* Append available upstream models to a /v1/models `data` JSON array. */
void auren_upstream_append_models_json(struct json_object *data_arr);

/* Sanitize system prompt strings for Antigravity Cloud Code compatibility.
 * Returns a newly allocated string (caller must free). */
char *auren_upstream_sanitize_system_prompt(const char *input);

/* Build an Antigravity Cloud Code v1internal request envelope JSON object from
 * an OpenAI /v1/chat/completions request JSON object and resolved route.
 * Caller owns the returned json_object and must json_object_put() it. */
struct json_object *auren_upstream_build_antigravity_request(
	struct json_object *openai_req,
	const struct auren_upstream_route *route,
	const char *project_id);

/* Record a tool_call_id -> thoughtSignature mapping in the in-memory ring cache
 * so subsequent multi-turn requests can re-attach thoughtSignature on Gemini 3/4. */
void auren_upstream_cache_thought_sig(const char *tool_call_id, const char *sig);

/* Lookup a cached thoughtSignature by tool_call_id. Returns 1 if found and copies
 * into sig_out, or 0 if not found. */
int auren_upstream_lookup_thought_sig(const char *tool_call_id, char *sig_out, size_t sig_out_sz);

/* Submit an asynchronous upstream chat completions request.
 * Spawns a detached pthread that streams or buffers results and pushes
 * AUREN_JOB_CHAT_ONESHOT results with `ud` into auren-engine's completion queue.
 * Returns 0 on success, -1 on error. */
int auren_upstream_submit_chat(
	const struct auren_upstream_route *route,
	const char *request_json,
	int stream,
	void *ud);

/* Submit an asynchronous SystemOne classification job for intent routing.
 * Uses bge-base embeddings to route "auto" requests to very-fast/fast/smart/genius.
 * Returns 0 on success, -1 on error. */
int auren_upstream_classify_intent(const char *prompt, void *ud);

/* Get the resolved tier for a given session affinity hash.
 * Returns one of: "very-fast", "fast", "smart", "genius". */
const char *auren_upstream_get_affinity_tier(uint64_t hash);

/* Set the affinity tier for a hash (called after successful upstream call). */
void auren_upstream_set_affinity_tier(uint64_t hash, const char *tier);

/* Build an Antigravity Cloud Code v1internal request envelope JSON object from
 * an OpenAI /v1/chat/completions request JSON object and resolved route.
 * Caller owns the returned json_object and must json_object_put() it. */
struct json_object *auren_upstream_build_antigravity_request(
	struct json_object *openai_req,
	const struct auren_upstream_route *route,
	const char *project_id);

/* Record a tool_call_id -> thoughtSignature mapping in the in-memory ring cache
 * so subsequent multi-turn requests can re-attach thoughtSignature on Gemini 3/4. */
void auren_upstream_cache_thought_sig(const char *tool_call_id, const char *sig);

/* Lookup a cached thoughtSignature by tool_call_id. Returns 1 if found and copies
 * into sig_out, or 0 if not found. */
int auren_upstream_lookup_thought_sig(const char *tool_call_id, char *sig_out, size_t sig_out_sz);

/* Submit an asynchronous upstream chat completions request.
 * Spawns a detached pthread that streams or buffers results and pushes
 * AUREN_JOB_CHAT_ONESHOT results with `ud` into auren-engine's completion queue.
 * Returns 0 on success, -1 on error. */
int auren_upstream_submit_chat(
	const struct auren_upstream_route *route,
	const char *request_json,
	int stream,
	void *ud);

#ifdef __cplusplus
}
#endif

#endif /* AUREN_UPSTREAM_H */
