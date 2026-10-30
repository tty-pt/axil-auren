/* auren-registry.h — Model registry and lifecycle management for auren.
 *
 * Supports multiple named model configurations, explicit capability tracking,
 * independent defaults per role (chat, judge, embed), and safe on-demand loading
 * and LRU eviction.
 */

#ifndef AUREN_REGISTRY_H
#define AUREN_REGISTRY_H

#include <ttypt/auren.h>
#include <ttypt/qllm.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#define AUREN_CAP_CHAT  (1 << 0)
#define AUREN_CAP_JUDGE (1 << 1)
#define AUREN_CAP_EMBED (1 << 2)

#define AUREN_ROLE_CHAT  "chat"
#define AUREN_ROLE_JUDGE "judge"
#define AUREN_ROLE_EMBED "embed"

#define AUREN_MAX_MODELS 16

/* Return codes */
#define AUREN_REG_OK           0
#define AUREN_REG_ERR_UNKNOWN -1
#define AUREN_REG_ERR_CAP     -2
#define AUREN_REG_ERR_LOAD    -3
#define AUREN_REG_ERR_OOM     -4

struct auren_model_entry {
	char id[64];
	char path[BUFSIZ];
	uint32_t capabilities; /* bitmask of AUREN_CAP_* */
	struct auren_config cfg;
	struct auren_context *ctx;
	int in_flight;
	time_t last_used;
};

struct auren_registry {
	struct auren_model_entry entries[AUREN_MAX_MODELS];
	int count;
	char default_chat[64];
	char default_judge[64];
	char default_embed[64];
	int max_resident;
};

/* Compatibility type aliases */
typedef struct auren_model_entry qllm_model_entry_t;
#define qllm_model_entry auren_model_entry
#define qllm_registry auren_registry

#define QLLM_CAP_CHAT  AUREN_CAP_CHAT
#define QLLM_CAP_JUDGE AUREN_CAP_JUDGE
#define QLLM_CAP_EMBED AUREN_CAP_EMBED

#define QLLM_ROLE_CHAT  AUREN_ROLE_CHAT
#define QLLM_ROLE_JUDGE AUREN_ROLE_JUDGE
#define QLLM_ROLE_EMBED AUREN_ROLE_EMBED

#define QLLM_MAX_MODELS AUREN_MAX_MODELS

#define QLLM_REG_OK           AUREN_REG_OK
#define QLLM_REG_ERR_UNKNOWN AUREN_REG_ERR_UNKNOWN
#define QLLM_REG_ERR_CAP     AUREN_REG_ERR_CAP
#define QLLM_REG_ERR_LOAD    AUREN_REG_ERR_LOAD
#define QLLM_REG_ERR_OOM     AUREN_REG_ERR_OOM

/* Initialize the registry from JSON config, env vars, or legacy fallback path. */
int auren_registry_init(const char *legacy_fallback_path);

/* Register a model profile explicitly. */
int auren_registry_add(const char *id, const char *path, uint32_t capabilities, const struct auren_config *cfg);

/* Set default model id for a role ("chat", "judge", "embed"). */
int auren_registry_set_default(const char *role, const char *model_id);

/* Get default model id for a capability. */
const char *auren_registry_default_id(uint32_t cap);

/* Acquire a model context for inference. Checks capability, loads on-demand if needed.
 * Increments in_flight. */
int auren_registry_acquire(const char *model_id, uint32_t required_cap, struct auren_context **ctx_out, char *id_out, size_t id_out_sz);

/* Create a dedicated session context for a model. Shares loaded model weights,
 * but maintains an independent KV cache and context. Caller must auren_free(). */
int auren_registry_create_session_context(const char *model_id, uint32_t required_cap, struct auren_context **ctx_out, char *id_out, size_t id_out_sz);

/* Release a previously acquired model. Decrements in_flight and updates last_used. */
void auren_registry_release(const char *model_id);

/* Lookup model entry (read-only). */
const struct auren_model_entry *auren_registry_find(const char *model_id);
const struct auren_model_entry *auren_registry_find_for_cap(uint32_t cap, const char *model_id);

struct json_object;

/* Build OpenAI-compatible JSON list of all registered models. Caller must json_object_put(). */
struct json_object *auren_registry_models_json(void);

/* Unload all loaded contexts and reset registry. */
void auren_registry_shutdown(void);

/* Compatibility function aliases */
#define qllm_registry_init auren_registry_init
#define qllm_registry_add auren_registry_add
#define qllm_registry_set_default auren_registry_set_default
#define qllm_registry_default_id auren_registry_default_id
#define qllm_registry_acquire auren_registry_acquire
#define qllm_registry_create_session_context auren_registry_create_session_context
#define qllm_registry_release auren_registry_release
#define qllm_registry_find auren_registry_find
#define qllm_registry_find_for_cap auren_registry_find_for_cap
#define qllm_registry_models_json auren_registry_models_json
#define qllm_registry_shutdown auren_registry_shutdown

#endif
