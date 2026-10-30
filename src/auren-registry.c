/* qllm-registry.c — Model registry and lifecycle implementation for axil-qllm. */

#include "auren-registry.h"
#include <ttypt/qsys.h>

#include <json-c/json.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef MOCK_BUILD
/* Declared in vulkan.c */
extern void qllm_backend_mem_check(int gpu, size_t *free_b, size_t *total_b);
#endif

static struct qllm_registry g_reg;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t
parse_capabilities(struct json_object *caps_obj)
{
	uint32_t caps = 0;
	size_t i, n;

	if (!caps_obj)
		return QLLM_CAP_CHAT | QLLM_CAP_JUDGE | QLLM_CAP_EMBED;

	if (json_object_is_type(caps_obj, json_type_string)) {
		const char *s = json_object_get_string(caps_obj);
		if (strstr(s, "chat"))  caps |= QLLM_CAP_CHAT;
		if (strstr(s, "judge")) caps |= QLLM_CAP_JUDGE;
		if (strstr(s, "embed")) caps |= QLLM_CAP_EMBED;
		return caps ? caps : (QLLM_CAP_CHAT | QLLM_CAP_JUDGE);
	}

	if (!json_object_is_type(caps_obj, json_type_array))
		return QLLM_CAP_CHAT | QLLM_CAP_JUDGE | QLLM_CAP_EMBED;

	n = (size_t)json_object_array_length(caps_obj);
	for (i = 0; i < n; ++i) {
		struct json_object *item = json_object_array_get_idx(caps_obj, i);
		const char *str = item ? json_object_get_string(item) : NULL;
		if (!str) continue;
		if (strcmp(str, "chat") == 0 || strcmp(str, "generation") == 0)
			caps |= QLLM_CAP_CHAT;
		else if (strcmp(str, "judge") == 0 || strcmp(str, "systemone") == 0)
			caps |= QLLM_CAP_JUDGE;
		else if (strcmp(str, "embed") == 0 || strcmp(str, "embeddings") == 0)
			caps |= QLLM_CAP_EMBED;
	}
	return caps;
}

static void
expand_path(const char *in, char *out, size_t out_sz)
{
	if (!in) {
		if (out_sz > 0) out[0] = '\0';
		return;
	}
	if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
		const char *home = getenv("HOME");
		if (home && home[0]) {
			snprintf(out, out_sz, "%s%s", home, in + 1);
			return;
		}
	}
	snprintf(out, out_sz, "%s", in);
}

static void
derive_shared_id(const char *path, char *out, size_t out_sz)
{
	const char *slash;
	const char *base;
	const char *dot;
	char stem[64];
	size_t len;

	if (!path || !path[0]) {
		snprintf(out, out_sz, "shared-model");
		return;
	}

	slash = strrchr(path, '/');
	base = slash ? slash + 1 : path;
	dot = strrchr(base, '.');
	if (dot && dot != base)
		len = (size_t)(dot - base);
	else
		len = strlen(base);

	if (len == 0) {
		snprintf(out, out_sz, "shared-model");
		return;
	}

	if (len >= sizeof(stem))
		len = sizeof(stem) - 1;
	memcpy(stem, base, len);
	stem[len] = '\0';

	if (strstr(stem, "shared"))
		snprintf(out, out_sz, "%s", stem);
	else
		snprintf(out, out_sz, "%s-shared", stem);
}

int
qllm_registry_add(const char *id, const char *path, uint32_t capabilities, const struct qllm_config *cfg)
{
	int i;
	struct qllm_model_entry *e;
	char resolved_path[BUFSIZ];

	if (!id || !id[0] || !path || !path[0])
		return -1;

	expand_path(path, resolved_path, sizeof(resolved_path));

	pthread_mutex_lock(&g_reg_lock);

	for (i = 0; i < g_reg.count; ++i) {
		if (strcmp(g_reg.entries[i].id, id) == 0) {
			/* Update existing */
			e = &g_reg.entries[i];
			snprintf(e->path, sizeof(e->path), "%s", resolved_path);
			e->capabilities |= capabilities;
			if (cfg) {
				if (cfg->n_ctx > 0) e->cfg.n_ctx = cfg->n_ctx;
				if (cfg->n_gpu_layers > 0) e->cfg.n_gpu_layers = cfg->n_gpu_layers;
				if (cfg->temperature > 0.0f) e->cfg.temperature = cfg->temperature;
				if (cfg->enable_embeddings) e->cfg.enable_embeddings = cfg->enable_embeddings;
			}
			e->cfg.model_path = e->path;
			pthread_mutex_unlock(&g_reg_lock);
			return 0;
		}
	}

	if (g_reg.count >= QLLM_MAX_MODELS) {
		pthread_mutex_unlock(&g_reg_lock);
		return -1;
	}

	e = &g_reg.entries[g_reg.count++];
	memset(e, 0, sizeof(*e));
	snprintf(e->id, sizeof(e->id), "%s", id);
	snprintf(e->path, sizeof(e->path), "%s", resolved_path);
	e->capabilities = capabilities;
	if (cfg)
		e->cfg = *cfg;
	e->cfg.model_path = e->path;
	e->last_used = time(NULL);

	pthread_mutex_unlock(&g_reg_lock);
	return 0;
}

int
qllm_registry_set_default(const char *role, const char *model_id)
{
	if (!role || !model_id)
		return -1;

	pthread_mutex_lock(&g_reg_lock);
	if (strcmp(role, QLLM_ROLE_CHAT) == 0)
		snprintf(g_reg.default_chat, sizeof(g_reg.default_chat), "%s", model_id);
	else if (strcmp(role, QLLM_ROLE_JUDGE) == 0)
		snprintf(g_reg.default_judge, sizeof(g_reg.default_judge), "%s", model_id);
	else if (strcmp(role, QLLM_ROLE_EMBED) == 0)
		snprintf(g_reg.default_embed, sizeof(g_reg.default_embed), "%s", model_id);
	else {
		pthread_mutex_unlock(&g_reg_lock);
		return -1;
	}
	pthread_mutex_unlock(&g_reg_lock);
	return 0;
}

const char *
qllm_registry_default_id(uint32_t cap)
{
	const char *def = NULL;

	pthread_mutex_lock(&g_reg_lock);
	if (cap & QLLM_CAP_EMBED)
		def = g_reg.default_embed[0] ? g_reg.default_embed : NULL;
	else if (cap & QLLM_CAP_JUDGE)
		def = g_reg.default_judge[0] ? g_reg.default_judge : NULL;
	else if (cap & QLLM_CAP_CHAT)
		def = g_reg.default_chat[0] ? g_reg.default_chat : NULL;

	/* If specific role default is unset, pick first model matching capability */
	if (!def) {
		int i;
		for (i = 0; i < g_reg.count; ++i) {
			if (g_reg.entries[i].capabilities & cap) {
				def = g_reg.entries[i].id;
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_reg_lock);
	return def;
}

const struct qllm_model_entry *
qllm_registry_find(const char *model_id)
{
	int i;
	if (!model_id)
		return NULL;

	pthread_mutex_lock(&g_reg_lock);
	for (i = 0; i < g_reg.count; ++i) {
		if (strcmp(g_reg.entries[i].id, model_id) == 0) {
			pthread_mutex_unlock(&g_reg_lock);
			return &g_reg.entries[i];
		}
	}
	if (strcmp(model_id, "default") == 0 && g_reg.count > 0) {
		const char *def = g_reg.default_chat[0] ? g_reg.default_chat :
		                  (g_reg.default_judge[0] ? g_reg.default_judge : g_reg.entries[0].id);
		for (i = 0; i < g_reg.count; ++i) {
			if (strcmp(g_reg.entries[i].id, def) == 0) {
				pthread_mutex_unlock(&g_reg_lock);
				return &g_reg.entries[i];
			}
		}
	}
	pthread_mutex_unlock(&g_reg_lock);
	return NULL;
}

const struct qllm_model_entry *
qllm_registry_find_for_cap(uint32_t cap, const char *model_id)
{
	int i;
	const char *resolved_id = model_id;

	pthread_mutex_lock(&g_reg_lock);
	if (!resolved_id || !resolved_id[0] || strcmp(resolved_id, "default") == 0) {
		if (cap & QLLM_CAP_EMBED && g_reg.default_embed[0])
			resolved_id = g_reg.default_embed;
		else if (cap & QLLM_CAP_JUDGE && g_reg.default_judge[0])
			resolved_id = g_reg.default_judge;
		else if (cap & QLLM_CAP_CHAT && g_reg.default_chat[0])
			resolved_id = g_reg.default_chat;
	}

	for (i = 0; i < g_reg.count; ++i) {
		if (resolved_id && strcmp(g_reg.entries[i].id, resolved_id) == 0) {
			pthread_mutex_unlock(&g_reg_lock);
			return &g_reg.entries[i];
		}
	}
	if (!model_id || !model_id[0] || strcmp(model_id, "default") == 0) {
		for (i = 0; i < g_reg.count; ++i) {
			if (g_reg.entries[i].capabilities & cap) {
				pthread_mutex_unlock(&g_reg_lock);
				return &g_reg.entries[i];
			}
		}
	}
	pthread_mutex_unlock(&g_reg_lock);
	return NULL;
}

static void
parse_models_array(struct json_object *models_arr)
{
	size_t i, n;

	if (!models_arr || !json_object_is_type(models_arr, json_type_array))
		return;

	n = (size_t)json_object_array_length(models_arr);
	for (i = 0; i < n; ++i) {
		struct json_object *item = json_object_array_get_idx(models_arr, i);
		struct json_object *id_o, *path_o, *caps_o, *n_ctx_o, *n_gpu_o, *temp_o, *emb_o;
		const char *id_str, *path_str;
		uint32_t caps;
		struct qllm_config cfg;

		if (!item || !json_object_is_type(item, json_type_object))
			continue;

		id_o = json_object_object_get(item, "id");
		path_o = json_object_object_get(item, "path");
		if (!id_o || !path_o) continue;

		id_str = json_object_get_string(id_o);
		path_str = json_object_get_string(path_o);
		caps_o = json_object_object_get(item, "capabilities");
		caps = parse_capabilities(caps_o);

		memset(&cfg, 0, sizeof(cfg));
		n_ctx_o = json_object_object_get(item, "n_ctx");
		if (n_ctx_o) cfg.n_ctx = json_object_get_int(n_ctx_o);

		n_gpu_o = json_object_object_get(item, "n_gpu_layers");
		if (n_gpu_o) cfg.n_gpu_layers = json_object_get_int(n_gpu_o);

		temp_o = json_object_object_get(item, "temperature");
		if (temp_o) cfg.temperature = (float)json_object_get_double(temp_o);
		else cfg.temperature = 0.7f;

		emb_o = json_object_object_get(item, "enable_embeddings");
		if (emb_o) cfg.enable_embeddings = json_object_get_int(emb_o);
		else if (caps & QLLM_CAP_EMBED) cfg.enable_embeddings = 1;

		qllm_registry_add(id_str, path_str, caps, &cfg);
	}
}

static void
parse_json_config(const char *json_text)
{
	struct json_object *root;

	if (!json_text || !json_text[0])
		return;

	root = json_tokener_parse(json_text);
	if (!root) {
		qsyslog(QLOG_ERR, "qllm: failed to parse JSON configuration\n");
		return;
	}

	if (json_object_is_type(root, json_type_array)) {
		parse_models_array(root);
	} else if (json_object_is_type(root, json_type_object)) {
		struct json_object *max_res_obj = json_object_object_get(root, "max_resident");
		if (max_res_obj && json_object_is_type(max_res_obj, json_type_int)) {
			int m = json_object_get_int(max_res_obj);
			if (m > 0) g_reg.max_resident = m;
		}

		struct json_object *models_arr = json_object_object_get(root, "models");
		if (models_arr)
			parse_models_array(models_arr);

		struct json_object *defs_obj = json_object_object_get(root, "defaults");
		if (defs_obj && json_object_is_type(defs_obj, json_type_object)) {
			struct json_object *c = json_object_object_get(defs_obj, "chat");
			struct json_object *j = json_object_object_get(defs_obj, "judge");
			struct json_object *e = json_object_object_get(defs_obj, "embed");
			if (c) qllm_registry_set_default(QLLM_ROLE_CHAT, json_object_get_string(c));
			if (j) qllm_registry_set_default(QLLM_ROLE_JUDGE, json_object_get_string(j));
			if (e) qllm_registry_set_default(QLLM_ROLE_EMBED, json_object_get_string(e));
		}
	}

	json_object_put(root);
}

static char *
read_file_text(const char *path)
{
	FILE *f = fopen(path, "rb");
	long sz;
	char *buf;

	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	if (sz <= 0 || sz > 1024 * 1024) {
		fclose(f);
		return NULL;
	}
	fseek(f, 0, SEEK_SET);
	buf = malloc(sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[sz] = '\0';
	fclose(f);
	return buf;
}

int
qllm_registry_init(const char *legacy_fallback_path)
{
	const char *cfg_path = getenv("AUREN_CONFIG_PATH");
	const char *models_json = getenv("AUREN_MODELS");
	const char *chat_path = getenv("AUREN_CHAT_PATH");
	const char *judge_path = getenv("AUREN_JUDGE_PATH");
	const char *embed_path = getenv("AUREN_EMBED_PATH");
	const char *chat_id = getenv("AUREN_CHAT_MODEL");
	const char *judge_id = getenv("AUREN_JUDGE_MODEL");
	const char *embed_id = getenv("AUREN_EMBED_MODEL");
	int same_chat_judge = 0;
	int def_ctx = 0;
	int def_ngl = -1;
	const char *env_ctx;
	const char *env_gpu;

	env_gpu = getenv("AUREN_GPU_LAYERS");
	if (!env_gpu) env_gpu = getenv("AUREN_N_GPU_LAYERS");
	if (env_gpu) def_ngl = atoi(env_gpu);

	pthread_mutex_lock(&g_reg_lock);
	memset(&g_reg, 0, sizeof(g_reg));
	g_reg.max_resident = 2; /* Default: up to 2 models resident simultaneously */
	pthread_mutex_unlock(&g_reg_lock);

	/* If caller explicitly passed a model path (e.g. engine-test), map that single model */
	if (legacy_fallback_path && legacy_fallback_path[0]) {
		pthread_mutex_lock(&g_reg_lock);
		char resolved_lp[BUFSIZ];
		expand_path(legacy_fallback_path, resolved_lp, sizeof(resolved_lp));
		struct qllm_model_entry *e = &g_reg.entries[g_reg.count++];
		memset(e, 0, sizeof(*e));
		snprintf(e->id, sizeof(e->id), "default");
		snprintf(e->path, sizeof(e->path), "%s", resolved_lp);
		e->capabilities = QLLM_CAP_CHAT | QLLM_CAP_JUDGE | QLLM_CAP_EMBED;
		e->cfg.model_path = e->path;
		e->cfg.n_ctx = def_ctx;
		e->cfg.enable_embeddings = 1;
		e->last_used = time(NULL);

		snprintf(g_reg.default_chat, sizeof(g_reg.default_chat), "default");
		snprintf(g_reg.default_judge, sizeof(g_reg.default_judge), "default");
		snprintf(g_reg.default_embed, sizeof(g_reg.default_embed), "default");
		pthread_mutex_unlock(&g_reg_lock);
		return 0;
	}

	/* 1. Try AUREN_CONFIG_PATH or default candidate search order */
	if (cfg_path && cfg_path[0]) {
		char resolved_cfg[BUFSIZ];
		expand_path(cfg_path, resolved_cfg, sizeof(resolved_cfg));
		char *text = read_file_text(resolved_cfg);
		if (text) {
			parse_json_config(text);
			free(text);
		} else {
			qsyslog(QLOG_ERR, "auren: failed to open config at '%s'\n", resolved_cfg);
		}
	} else {
		static const char *candidate_paths[] = {
			"./auren.json",
			"~/.config/auren/config.json",
			"~/.auren.json"
		};
		size_t i;
		for (i = 0; i < sizeof(candidate_paths) / sizeof(candidate_paths[0]); ++i) {
			char resolved_cfg[BUFSIZ];
			expand_path(candidate_paths[i], resolved_cfg, sizeof(resolved_cfg));
			if (access(resolved_cfg, R_OK) == 0) {
				char *text = read_file_text(resolved_cfg);
				if (text) {
					parse_json_config(text);
					free(text);
					break;
				}
			}
		}
	}

	/* 2. Try inline AUREN_MODELS JSON string */
	if (models_json && models_json[0]) {
		parse_json_config(models_json);
	}

	env_ctx = getenv("AUREN_N_CTX");
	if (env_ctx) {
		int parsed = atoi(env_ctx);
		if (parsed > 0) def_ctx = parsed;
	}

	/* 3. Handle specific role paths from environment */
	if (chat_path && chat_path[0] && judge_path && judge_path[0]) {
		char exp_chat[BUFSIZ], exp_judge[BUFSIZ];
		char real_chat[PATH_MAX], real_judge[PATH_MAX];
		expand_path(chat_path, exp_chat, sizeof(exp_chat));
		expand_path(judge_path, exp_judge, sizeof(exp_judge));

		if (realpath(exp_chat, real_chat) && realpath(exp_judge, real_judge)) {
			if (strcmp(real_chat, real_judge) == 0)
				same_chat_judge = 1;
		} else {
			if (strcmp(exp_chat, exp_judge) == 0)
				same_chat_judge = 1;
		}
	}

	if (same_chat_judge && !(chat_id && chat_id[0]) && !(judge_id && judge_id[0])) {
		char shared_id[64];
		derive_shared_id(chat_path, shared_id, sizeof(shared_id));
		struct qllm_config cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.n_ctx = def_ctx;
		if (def_ngl >= 0) cfg.n_gpu_layers = def_ngl;
		qllm_registry_add(shared_id, chat_path, QLLM_CAP_CHAT | QLLM_CAP_JUDGE, &cfg);
		qllm_registry_set_default(QLLM_ROLE_CHAT, shared_id);
		qllm_registry_set_default(QLLM_ROLE_JUDGE, shared_id);
	} else {
		if (chat_path && chat_path[0]) {
			struct qllm_config cfg;
			memset(&cfg, 0, sizeof(cfg));
			cfg.n_ctx = def_ctx;
			if (def_ngl >= 0) cfg.n_gpu_layers = def_ngl;
			qllm_registry_add(chat_id && chat_id[0] ? chat_id : "chat-model", chat_path, QLLM_CAP_CHAT, &cfg);
			qllm_registry_set_default(QLLM_ROLE_CHAT, chat_id && chat_id[0] ? chat_id : "chat-model");
		}
		if (judge_path && judge_path[0]) {
			struct qllm_config cfg;
			memset(&cfg, 0, sizeof(cfg));
			cfg.n_ctx = def_ctx;
			if (def_ngl >= 0) cfg.n_gpu_layers = def_ngl;
			qllm_registry_add(judge_id && judge_id[0] ? judge_id : "judge-model", judge_path, QLLM_CAP_JUDGE, &cfg);
			qllm_registry_set_default(QLLM_ROLE_JUDGE, judge_id && judge_id[0] ? judge_id : "judge-model");
		}
	}
	if (embed_path && embed_path[0]) {
		struct qllm_config cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.n_ctx = 512;
		cfg.enable_embeddings = 1;
		qllm_registry_add(embed_id && embed_id[0] ? embed_id : "embed-model", embed_path, QLLM_CAP_EMBED, &cfg);
		qllm_registry_set_default(QLLM_ROLE_EMBED, embed_id && embed_id[0] ? embed_id : "embed-model");
	}

	/* 4. If registry is still empty, map legacy single model configuration */
	pthread_mutex_lock(&g_reg_lock);
	if (g_reg.count == 0) {
		const char *lp = (legacy_fallback_path && legacy_fallback_path[0])
			? legacy_fallback_path
			: getenv("AUREN_MODEL_PATH");
		if (lp && lp[0]) {
			char resolved_lp[BUFSIZ];
			expand_path(lp, resolved_lp, sizeof(resolved_lp));
			struct qllm_model_entry *e = &g_reg.entries[g_reg.count++];
			memset(e, 0, sizeof(*e));
			snprintf(e->id, sizeof(e->id), "default");
			snprintf(e->path, sizeof(e->path), "%s", resolved_lp);
			e->capabilities = QLLM_CAP_CHAT | QLLM_CAP_JUDGE | QLLM_CAP_EMBED;
			e->cfg.model_path = e->path;
			e->cfg.n_ctx = def_ctx;
			e->cfg.enable_embeddings = 1;
			e->last_used = time(NULL);

			snprintf(g_reg.default_chat, sizeof(g_reg.default_chat), "default");
			snprintf(g_reg.default_judge, sizeof(g_reg.default_judge), "default");
			snprintf(g_reg.default_embed, sizeof(g_reg.default_embed), "default");
		}
	}
	int final_count = g_reg.count;
	pthread_mutex_unlock(&g_reg_lock);

	return final_count > 0 ? 0 : -1;
}

/* Evict one idle model if needed to stay within max_resident */
static void
evict_idle_lru_locked(void)
{
	int resident_count = 0;
	int oldest_idx = -1;
	time_t oldest_time = (time_t)0x7fffffff;
	int i;

	for (i = 0; i < g_reg.count; ++i) {
		if (g_reg.entries[i].ctx != NULL) {
			resident_count++;
			if (g_reg.entries[i].in_flight == 0) {
				if (g_reg.entries[i].last_used < oldest_time) {
					oldest_time = g_reg.entries[i].last_used;
					oldest_idx = i;
				}
			}
		}
	}

	if (resident_count >= g_reg.max_resident && oldest_idx >= 0) {
		struct qllm_model_entry *e = &g_reg.entries[oldest_idx];
		fprintf(stderr, "[qllm-registry] Evicting idle model '%s' (ctx=%p)\n", e->id, (void*)e->ctx);
		qllm_free(e->ctx);
		e->ctx = NULL;
	}
}

int
qllm_registry_acquire(const char *model_id, uint32_t required_cap, struct qllm_context **ctx_out, char *id_out, size_t id_out_sz)
{
	int i;
	struct qllm_model_entry *target = NULL;
	const char *resolved_id = model_id;

	pthread_mutex_lock(&g_reg_lock);

	/* If no model_id specified, resolve role default */
	if (!resolved_id || !resolved_id[0]) {
		if (required_cap & QLLM_CAP_EMBED)
			resolved_id = g_reg.default_embed;
		else if (required_cap & QLLM_CAP_JUDGE)
			resolved_id = g_reg.default_judge;
		else if (required_cap & QLLM_CAP_CHAT)
			resolved_id = g_reg.default_chat;
	}

	/* Find target */
	for (i = 0; i < g_reg.count; ++i) {
		if (resolved_id && strcmp(g_reg.entries[i].id, resolved_id) == 0) {
			target = &g_reg.entries[i];
			break;
		}
	}

	/* Fallback: if resolved_id was not found but empty/default, try first matching cap */
	if (!target && (!resolved_id || !resolved_id[0] || strcmp(resolved_id, "default") == 0)) {
		for (i = 0; i < g_reg.count; ++i) {
			if (g_reg.entries[i].capabilities & required_cap) {
				target = &g_reg.entries[i];
				break;
			}
		}
	}

	if (!target) {
		pthread_mutex_unlock(&g_reg_lock);
		return QLLM_REG_ERR_UNKNOWN;
	}

	/* Validate capability */
	if (!(target->capabilities & required_cap)) {
		pthread_mutex_unlock(&g_reg_lock);
		return QLLM_REG_ERR_CAP;
	}

	/* Load context on-demand if not loaded */
	if (!target->ctx) {
		evict_idle_lru_locked();

		target->ctx = qllm_create(&target->cfg);
		if (!target->ctx) {
			/* On load failure, try evicting any other idle models and retry once */
			int any_evicted = 0;
			for (i = 0; i < g_reg.count; ++i) {
				if (g_reg.entries[i].ctx != NULL && g_reg.entries[i].in_flight == 0) {
					qllm_free(g_reg.entries[i].ctx);
					g_reg.entries[i].ctx = NULL;
					any_evicted = 1;
				}
			}
			if (any_evicted)
				target->ctx = qllm_create(&target->cfg);

			if (!target->ctx) {
				pthread_mutex_unlock(&g_reg_lock);
				return QLLM_REG_ERR_LOAD;
			}
		}
	}

	target->in_flight++;
	target->last_used = time(NULL);

	if (ctx_out)
		*ctx_out = target->ctx;
	if (id_out && id_out_sz > 0)
		snprintf(id_out, id_out_sz, "%s", target->id);

	pthread_mutex_unlock(&g_reg_lock);
	return QLLM_REG_OK;
}

int
qllm_registry_create_session_context(const char *model_id, uint32_t required_cap, struct qllm_context **ctx_out, char *id_out, size_t id_out_sz)
{
	int i;
	struct qllm_model_entry *target = NULL;
	const char *resolved_id = model_id;
	struct qllm_config cfg_copy;
	struct qllm_context *ctx = NULL;

	pthread_mutex_lock(&g_reg_lock);

	/* If no model_id specified, resolve role default */
	if (!resolved_id || !resolved_id[0]) {
		if (required_cap & QLLM_CAP_CHAT)
			resolved_id = g_reg.default_chat;
		else if (required_cap & QLLM_CAP_JUDGE)
			resolved_id = g_reg.default_judge;
		else if (required_cap & QLLM_CAP_EMBED)
			resolved_id = g_reg.default_embed;
	}

	/* Find target */
	for (i = 0; i < g_reg.count; ++i) {
		if (resolved_id && strcmp(g_reg.entries[i].id, resolved_id) == 0) {
			target = &g_reg.entries[i];
			break;
		}
	}

	/* Fallback */
	if (!target && (!resolved_id || !resolved_id[0] || strcmp(resolved_id, "default") == 0)) {
		for (i = 0; i < g_reg.count; ++i) {
			if (g_reg.entries[i].capabilities & required_cap) {
				target = &g_reg.entries[i];
				break;
			}
		}
	}

	if (!target) {
		pthread_mutex_unlock(&g_reg_lock);
		return QLLM_REG_ERR_UNKNOWN;
	}

	if (!(target->capabilities & required_cap)) {
		pthread_mutex_unlock(&g_reg_lock);
		return QLLM_REG_ERR_CAP;
	}

	if (id_out && id_out_sz > 0)
		snprintf(id_out, id_out_sz, "%s", target->id);

	cfg_copy = target->cfg;
	target->last_used = time(NULL);
	pthread_mutex_unlock(&g_reg_lock);

	ctx = qllm_create(&cfg_copy);
	if (!ctx)
		return QLLM_REG_ERR_LOAD;

	*ctx_out = ctx;
	return QLLM_REG_OK;
}

void
qllm_registry_release(const char *model_id)
{
	int i;
	if (!model_id)
		return;

	pthread_mutex_lock(&g_reg_lock);
	for (i = 0; i < g_reg.count; ++i) {
		if (strcmp(g_reg.entries[i].id, model_id) == 0) {
			if (g_reg.entries[i].in_flight > 0)
				g_reg.entries[i].in_flight--;
			g_reg.entries[i].last_used = time(NULL);
			break;
		}
	}
	pthread_mutex_unlock(&g_reg_lock);
}

struct json_object *
qllm_registry_models_json(void)
{
	struct json_object *root = json_object_new_object();
	struct json_object *data = json_object_new_array();
	int i;

	pthread_mutex_lock(&g_reg_lock);
	for (i = 0; i < g_reg.count; ++i) {
		struct json_object *m = json_object_new_object();
		time_t created = g_reg.entries[i].last_used ? g_reg.entries[i].last_used : time(NULL);
		json_object_object_add(m, "id", json_object_new_string(g_reg.entries[i].id));
		json_object_object_add(m, "object", json_object_new_string("model"));
		json_object_object_add(m, "created", json_object_new_int64((int64_t)created));
		json_object_object_add(m, "owned_by", json_object_new_string("local"));
		json_object_array_add(data, m);
	}
	pthread_mutex_unlock(&g_reg_lock);

	json_object_object_add(root, "object", json_object_new_string("list"));
	json_object_object_add(root, "data", data);
	return root;
}

void
qllm_registry_shutdown(void)
{
	int i;
	pthread_mutex_lock(&g_reg_lock);
	for (i = 0; i < g_reg.count; ++i) {
		if (g_reg.entries[i].ctx) {
			qllm_free(g_reg.entries[i].ctx);
			g_reg.entries[i].ctx = NULL;
		}
		g_reg.entries[i].in_flight = 0;
	}
	g_reg.count = 0;
	pthread_mutex_unlock(&g_reg_lock);
}
