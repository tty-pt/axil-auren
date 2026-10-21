/* test_model_registry.c — Unit tests for qllm multi-model registry and runtime */

#include "../framework/test_utils.h"
#include "../../src/qllm-registry.h"
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

TEST(model_registry_add_and_find)
{
	struct qllm_config cfg;
	memset(&cfg, 0, sizeof(cfg));

	qllm_registry_init(NULL);

	int rc1 = qllm_registry_add("qwen-chat", "/models/qwen.gguf", QLLM_CAP_CHAT | QLLM_CAP_JUDGE, &cfg);
	ASSERT_EQ(rc1, 0);

	int rc2 = qllm_registry_add("bge-embed", "/models/bge.gguf", QLLM_CAP_EMBED, &cfg);
	ASSERT_EQ(rc2, 0);

	const struct qllm_model_entry *m1 = qllm_registry_find("qwen-chat");
	ASSERT_NOT_NULL(m1);
	ASSERT_STR_EQ(m1->id, "qwen-chat");
	ASSERT_STR_EQ(m1->path, "/models/qwen.gguf");
	ASSERT_EQ((int)m1->capabilities, (int)(QLLM_CAP_CHAT | QLLM_CAP_JUDGE));

	const struct qllm_model_entry *m2 = qllm_registry_find("bge-embed");
	ASSERT_NOT_NULL(m2);
	ASSERT_STR_EQ(m2->id, "bge-embed");
	ASSERT_EQ((int)m2->capabilities, (int)QLLM_CAP_EMBED);

	const struct qllm_model_entry *m3 = qllm_registry_find("nonexistent");
	ASSERT_NULL(m3);

	qllm_registry_shutdown();
}

TEST(model_registry_capability_merging)
{
	struct qllm_config cfg;
	memset(&cfg, 0, sizeof(cfg));

	qllm_registry_init(NULL);

	qllm_registry_add("qwen", "/models/qwen.gguf", QLLM_CAP_CHAT, &cfg);
	qllm_registry_add("qwen", "/models/qwen.gguf", QLLM_CAP_JUDGE, &cfg);

	const struct qllm_model_entry *m = qllm_registry_find("qwen");
	ASSERT_NOT_NULL(m);
	ASSERT_STR_EQ(m->id, "qwen");
	ASSERT_EQ((int)m->capabilities, (int)(QLLM_CAP_CHAT | QLLM_CAP_JUDGE));

	qllm_registry_shutdown();
}

TEST(model_registry_role_defaults)
{
	struct qllm_config cfg;
	memset(&cfg, 0, sizeof(cfg));

	qllm_registry_init(NULL);

	qllm_registry_add("gen-model", "/models/gen.gguf", QLLM_CAP_CHAT, &cfg);
	qllm_registry_add("judge-model", "/models/judge.gguf", QLLM_CAP_JUDGE, &cfg);
	qllm_registry_add("vec-model", "/models/vec.gguf", QLLM_CAP_EMBED, &cfg);

	qllm_registry_set_default(QLLM_ROLE_CHAT, "gen-model");
	qllm_registry_set_default(QLLM_ROLE_JUDGE, "judge-model");
	qllm_registry_set_default(QLLM_ROLE_EMBED, "vec-model");

	const char *def_chat = qllm_registry_default_id(QLLM_CAP_CHAT);
	ASSERT_NOT_NULL(def_chat);
	ASSERT_STR_EQ(def_chat, "gen-model");

	const char *def_judge = qllm_registry_default_id(QLLM_CAP_JUDGE);
	ASSERT_NOT_NULL(def_judge);
	ASSERT_STR_EQ(def_judge, "judge-model");

	const char *def_embed = qllm_registry_default_id(QLLM_CAP_EMBED);
	ASSERT_NOT_NULL(def_embed);
	ASSERT_STR_EQ(def_embed, "vec-model");

	qllm_registry_shutdown();
}

TEST(model_registry_capability_validation)
{
	struct qllm_config cfg;
	memset(&cfg, 0, sizeof(cfg));

	qllm_registry_init(NULL);

	qllm_registry_add("chat-only", "/models/chat.gguf", QLLM_CAP_CHAT, &cfg);
	qllm_registry_add("embed-only", "/models/embed.gguf", QLLM_CAP_EMBED, &cfg);

	struct qllm_context *ctx = NULL;
	char id_out[64];

	/* Unknown model */
	int rc = qllm_registry_acquire("ghost", QLLM_CAP_CHAT, &ctx, id_out, sizeof(id_out));
	ASSERT_EQ(rc, QLLM_REG_ERR_UNKNOWN);

	/* Capability mismatch: chat-only requested for embedding */
	rc = qllm_registry_acquire("chat-only", QLLM_CAP_EMBED, &ctx, id_out, sizeof(id_out));
	ASSERT_EQ(rc, QLLM_REG_ERR_CAP);

	/* Capability mismatch: embed-only requested for chat */
	rc = qllm_registry_acquire("embed-only", QLLM_CAP_CHAT, &ctx, id_out, sizeof(id_out));
	ASSERT_EQ(rc, QLLM_REG_ERR_CAP);

	/* Capability mismatch: embed-only requested for judge */
	rc = qllm_registry_acquire("embed-only", QLLM_CAP_JUDGE, &ctx, id_out, sizeof(id_out));
	ASSERT_EQ(rc, QLLM_REG_ERR_CAP);

	qllm_registry_shutdown();
}

TEST(model_registry_acquire_and_release)
{
	struct qllm_config cfg;
	memset(&cfg, 0, sizeof(cfg));

	qllm_registry_init(NULL);

	qllm_registry_add("my-model", "/models/test.gguf", QLLM_CAP_CHAT | QLLM_CAP_JUDGE, &cfg);

	struct qllm_context *ctx1 = NULL;
	char id1[64];
	int rc = qllm_registry_acquire("my-model", QLLM_CAP_CHAT, &ctx1, id1, sizeof(id1));
	ASSERT_EQ(rc, QLLM_REG_OK);
	ASSERT_NOT_NULL(ctx1);
	ASSERT_STR_EQ(id1, "my-model");

	const struct qllm_model_entry *entry = qllm_registry_find("my-model");
	ASSERT_NOT_NULL(entry);
	ASSERT_EQ(entry->in_flight, 1);

	/* Second concurrent acquire */
	struct qllm_context *ctx2 = NULL;
	rc = qllm_registry_acquire("my-model", QLLM_CAP_JUDGE, &ctx2, NULL, 0);
	ASSERT_EQ(rc, QLLM_REG_OK);
	ASSERT_EQ(entry->in_flight, 2);

	/* Releases */
	qllm_registry_release("my-model");
	ASSERT_EQ(entry->in_flight, 1);

	qllm_registry_release("my-model");
	ASSERT_EQ(entry->in_flight, 0);

	qllm_registry_shutdown();
}

TEST(model_registry_legacy_fallback)
{
	/* Single legacy model path fallback */
	qllm_registry_init("/models/legacy-model.gguf");

	const struct qllm_model_entry *m = qllm_registry_find("default");
	ASSERT_NOT_NULL(m);
	ASSERT_STR_EQ(m->path, "/models/legacy-model.gguf");
	ASSERT_EQ((int)m->capabilities, (int)(QLLM_CAP_CHAT | QLLM_CAP_JUDGE | QLLM_CAP_EMBED));

	const char *def_c = qllm_registry_default_id(QLLM_CAP_CHAT);
	ASSERT_STR_EQ(def_c, "default");
	const char *def_j = qllm_registry_default_id(QLLM_CAP_JUDGE);
	ASSERT_STR_EQ(def_j, "default");
	const char *def_e = qllm_registry_default_id(QLLM_CAP_EMBED);
	ASSERT_STR_EQ(def_e, "default");

	qllm_registry_shutdown();
}

TEST(model_registry_models_json)
{
	struct qllm_config cfg;
	memset(&cfg, 0, sizeof(cfg));

	qllm_registry_init(NULL);

	qllm_registry_add("qwen-chat", "/models/qwen.gguf", QLLM_CAP_CHAT, &cfg);
	qllm_registry_add("bge-embed", "/models/bge.gguf", QLLM_CAP_EMBED, &cfg);

	struct json_object *models = qllm_registry_models_json();
	ASSERT_NOT_NULL(models);

	struct json_object *obj_type = json_object_object_get(models, "object");
	ASSERT_NOT_NULL(obj_type);
	ASSERT_STR_EQ(json_object_get_string(obj_type), "list");

	struct json_object *data = json_object_object_get(models, "data");
	ASSERT_NOT_NULL(data);
	ASSERT_EQ((int)json_object_array_length(data), 2);

	struct json_object *m1 = json_object_array_get_idx(data, 0);
	ASSERT_NOT_NULL(m1);
	struct json_object *id1 = json_object_object_get(m1, "id");
	ASSERT_NOT_NULL(id1);
	ASSERT_STR_EQ(json_object_get_string(id1), "qwen-chat");

	struct json_object *m2 = json_object_array_get_idx(data, 1);
	ASSERT_NOT_NULL(m2);
	struct json_object *id2 = json_object_object_get(m2, "id");
	ASSERT_NOT_NULL(id2);
	ASSERT_STR_EQ(json_object_get_string(id2), "bge-embed");

	json_object_put(models);
	qllm_registry_shutdown();
}

TEST(model_registry_bare_array_json)
{
	const char *bare_array = "["
		"{\"id\":\"qwen-bare\",\"path\":\"/models/qwen-bare.gguf\",\"capabilities\":[\"chat\"]},"
		"{\"id\":\"bge-bare\",\"path\":\"/models/bge-bare.gguf\",\"capabilities\":[\"embed\"]}"
	"]";

	setenv("AUREN_MODELS", bare_array, 1);
	int rc = qllm_registry_init(NULL);
	unsetenv("AUREN_MODELS");

	ASSERT_EQ(rc, 0);

	const struct qllm_model_entry *m1 = qllm_registry_find("qwen-bare");
	ASSERT_NOT_NULL(m1);
	ASSERT_STR_EQ(m1->id, "qwen-bare");
	ASSERT_STR_EQ(m1->path, "/models/qwen-bare.gguf");
	ASSERT_EQ((int)m1->capabilities, (int)QLLM_CAP_CHAT);

	const struct qllm_model_entry *m2 = qllm_registry_find("bge-bare");
	ASSERT_NOT_NULL(m2);
	ASSERT_STR_EQ(m2->id, "bge-bare");
	ASSERT_STR_EQ(m2->path, "/models/bge-bare.gguf");
	ASSERT_EQ((int)m2->capabilities, (int)QLLM_CAP_EMBED);

	qllm_registry_shutdown();
}

TEST(model_registry_config_diagnostics)
{
	/* Test malformed JSON does not crash */
	setenv("AUREN_MODELS", "{ malformed json string: [", 1);
	int rc = qllm_registry_init(NULL);
	unsetenv("AUREN_MODELS");
	ASSERT_EQ(rc, -1);
	qllm_registry_shutdown();

	/* Test unreadable / nonexistent AUREN_CONFIG_PATH logs error and does not crash */
	setenv("AUREN_CONFIG_PATH", "/nonexistent/path/to/nonexistent-config.json", 1);
	rc = qllm_registry_init(NULL);
	unsetenv("AUREN_CONFIG_PATH");
	ASSERT_EQ(rc, -1);
	qllm_registry_shutdown();
}

TEST(model_registry_env_path_deduplication)
{
	setenv("AUREN_CHAT_PATH", "/models/qwen.gguf", 1);
	setenv("AUREN_JUDGE_PATH", "/models/qwen.gguf", 1);
	unsetenv("AUREN_CHAT_MODEL");
	unsetenv("AUREN_JUDGE_MODEL");

	int rc = qllm_registry_init(NULL);
	unsetenv("AUREN_CHAT_PATH");
	unsetenv("AUREN_JUDGE_PATH");

	ASSERT_EQ(rc, 0);

	/* Should be assigned shared ID 'qwen-shared' */
	const struct qllm_model_entry *m = qllm_registry_find("qwen-shared");
	ASSERT_NOT_NULL(m);
	ASSERT_STR_EQ(m->id, "qwen-shared");
	ASSERT_STR_EQ(m->path, "/models/qwen.gguf");
	ASSERT_EQ((int)m->capabilities, (int)(QLLM_CAP_CHAT | QLLM_CAP_JUDGE));

	const char *def_chat = qllm_registry_default_id(QLLM_CAP_CHAT);
	ASSERT_NOT_NULL(def_chat);
	ASSERT_STR_EQ(def_chat, "qwen-shared");

	const char *def_judge = qllm_registry_default_id(QLLM_CAP_JUDGE);
	ASSERT_NOT_NULL(def_judge);
	ASSERT_STR_EQ(def_judge, "qwen-shared");

	/* Total registered models should be exactly 1, not 2 */
	struct json_object *models = qllm_registry_models_json();
	ASSERT_NOT_NULL(models);
	struct json_object *data = json_object_object_get(models, "data");
	ASSERT_NOT_NULL(data);
	ASSERT_EQ((int)json_object_array_length(data), 1);
	json_object_put(models);

	qllm_registry_shutdown();
}
