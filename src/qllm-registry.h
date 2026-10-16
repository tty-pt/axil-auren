/* qllm-registry.h — Model registry and lifecycle management for axil-qllm.
 *
 * Supports multiple named model configurations, explicit capability tracking,
 * independent defaults per role (chat, judge, embed), and safe on-demand loading
 * and LRU eviction.
 */

#ifndef QLLM_REGISTRY_H
#define QLLM_REGISTRY_H

#include <ttypt/qllm.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#define QLLM_CAP_CHAT  (1 << 0)
#define QLLM_CAP_JUDGE (1 << 1)
#define QLLM_CAP_EMBED (1 << 2)

#define QLLM_ROLE_CHAT  "chat"
#define QLLM_ROLE_JUDGE "judge"
#define QLLM_ROLE_EMBED "embed"

#define QLLM_MAX_MODELS 16

/* Return codes */
#define QLLM_REG_OK           0
#define QLLM_REG_ERR_UNKNOWN -1
#define QLLM_REG_ERR_CAP     -2
#define QLLM_REG_ERR_LOAD    -3
#define QLLM_REG_ERR_OOM     -4

struct qllm_model_entry {
	char id[64];
	char path[BUFSIZ];
	uint32_t capabilities; /* bitmask of QLLM_CAP_* */
	struct qllm_config cfg;
	struct qllm_context *ctx;
	int in_flight;
	time_t last_used;
};

struct qllm_registry {
	struct qllm_model_entry entries[QLLM_MAX_MODELS];
	int count;
	char default_chat[64];
	char default_judge[64];
	char default_embed[64];
	int max_resident;
};

/* Initialize the registry from JSON config, env vars, or legacy fallback path. */
int qllm_registry_init(const char *legacy_fallback_path);

/* Register a model profile explicitly. */
int qllm_registry_add(const char *id, const char *path, uint32_t capabilities, const struct qllm_config *cfg);

/* Set default model id for a role ("chat", "judge", "embed"). */
int qllm_registry_set_default(const char *role, const char *model_id);

/* Get default model id for a capability. */
const char *qllm_registry_default_id(uint32_t cap);

/* Acquire a model context for inference. Checks capability, loads on-demand if needed.
 * Increments in_flight. */
int qllm_registry_acquire(const char *model_id, uint32_t required_cap, struct qllm_context **ctx_out, char *id_out, size_t id_out_sz);

/* Release a previously acquired model. Decrements in_flight and updates last_used. */
void qllm_registry_release(const char *model_id);

/* Lookup model entry (read-only). */
const struct qllm_model_entry *qllm_registry_find(const char *model_id);
const struct qllm_model_entry *qllm_registry_find_for_cap(uint32_t cap, const char *model_id);

/* Unload all loaded contexts and reset registry. */
void qllm_registry_shutdown(void);

#endif
