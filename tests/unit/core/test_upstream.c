/* test_upstream.c — Unit tests for auren-upstream router and protocol translator */

#include "../framework/test_utils.h"
#include "../../src/auren-upstream.h"
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

TEST(upstream_resolve_aliases)
{
	struct auren_upstream_route r;

	/* "fast" alias */
	enum auren_upstream_provider p1 = auren_upstream_resolve_model("fast", NULL, &r);
	ASSERT_EQ(p1, AUREN_UPSTREAM_ALIAS_FAST);
	ASSERT_STR_EQ(r.runtime_model, "gemini-3.8-flash-low");
	ASSERT_STR_EQ(r.effort, "low");
	ASSERT_EQ(r.is_thinking, 1);
	ASSERT_EQ(r.model_enum, 320);

	/* "auto" alias */
	enum auren_upstream_provider p2 = auren_upstream_resolve_model("auto", NULL, &r);
	ASSERT_EQ(p2, AUREN_UPSTREAM_ALIAS_FAST);
	ASSERT_STR_EQ(r.runtime_model, "gemini-3.8-flash-low");

	/* "smart" alias */
	enum auren_upstream_provider p3 = auren_upstream_resolve_model("smart", NULL, &r);
	ASSERT_EQ(p3, AUREN_UPSTREAM_ALIAS_SMART);
	ASSERT_STR_EQ(r.runtime_model, "gemini-4-hydrogen-high");
	ASSERT_STR_EQ(r.effort, "high");
	ASSERT_EQ(r.is_thinking, 1);
	ASSERT_EQ(r.model_enum, 525);
}

TEST(upstream_resolve_antigravity_models)
{
	struct auren_upstream_route r;

	/* Default effort */
	enum auren_upstream_provider p1 = auren_upstream_resolve_model("gemini-3.8-flash", NULL, &r);
	ASSERT_EQ(p1, AUREN_UPSTREAM_ANTIGRAVITY);
	ASSERT_STR_EQ(r.runtime_model, "gemini-3.8-flash-low");

	/* Suffix effort */
	enum auren_upstream_provider p2 = auren_upstream_resolve_model("gemini-3.8-flash-medium", NULL, &r);
	ASSERT_EQ(p2, AUREN_UPSTREAM_ANTIGRAVITY);
	ASSERT_STR_EQ(r.runtime_model, "gemini-3.8-flash-medium");
	ASSERT_STR_EQ(r.effort, "medium");

	/* Hydrogen xhigh */
	enum auren_upstream_provider p3 = auren_upstream_resolve_model("gemini-4-hydrogen-xhigh", NULL, &r);
	ASSERT_EQ(p3, AUREN_UPSTREAM_ANTIGRAVITY);
	ASSERT_STR_EQ(r.runtime_model, "gemini-4-hydrogen-xhigh");
	ASSERT_STR_EQ(r.effort, "xhigh");

	/* Claude Sonnet */
	enum auren_upstream_provider p4 = auren_upstream_resolve_model("claude-sonnet-4-6", NULL, &r);
	ASSERT_EQ(p4, AUREN_UPSTREAM_ANTIGRAVITY);
	ASSERT_EQ(r.is_claude, 1);

	/* GPT-OSS 120B */
	enum auren_upstream_provider p5 = auren_upstream_resolve_model("gpt-oss-120b", NULL, &r);
	ASSERT_EQ(p5, AUREN_UPSTREAM_ANTIGRAVITY);
	ASSERT_EQ(r.is_gpt_oss, 1);
	ASSERT_STR_EQ(r.runtime_model, "gpt-oss-120b-medium");
}

TEST(upstream_resolve_kilo_models)
{
	struct auren_upstream_route r;

	/* kilo-auto/free */
	enum auren_upstream_provider p1 = auren_upstream_resolve_model("kilo-auto/free", NULL, &r);
	ASSERT_EQ(p1, AUREN_UPSTREAM_KILO);
	ASSERT_STR_EQ(r.canonical_id, "kilo-auto/free");

	/* kilo/prefix */
	enum auren_upstream_provider p2 = auren_upstream_resolve_model("kilo/anthropic/claude-3.5-haiku", NULL, &r);
	ASSERT_EQ(p2, AUREN_UPSTREAM_KILO);
	ASSERT_STR_EQ(r.canonical_id, "anthropic/claude-3.5-haiku");
}

TEST(upstream_sanitize_system_prompt)
{
	const char *orig = "You are an assistant running inside pi, a coding agent harness. Be practical, and tool-aware.";
	char *clean = auren_upstream_sanitize_system_prompt(orig);
	ASSERT_NOT_NULL(clean);
	ASSERT_NULL(strstr(clean, "inside pi"));
	ASSERT_NOT_NULL(strstr(clean, "running inside your development environment"));
	ASSERT_NOT_NULL(strstr(clean, "practical, and tool aware."));
	free(clean);
}

TEST(upstream_thought_signature_cache)
{
	const char *cid = "call_abc_123";
	const char *sig = "sig_bytes_encrypted_xyz987";
	char lookup[512] = {0};

	ASSERT_EQ(auren_upstream_lookup_thought_sig(cid, lookup, sizeof(lookup)), 0);

	auren_upstream_cache_thought_sig(cid, sig);

	ASSERT_EQ(auren_upstream_lookup_thought_sig(cid, lookup, sizeof(lookup)), 1);
	ASSERT_STR_EQ(lookup, sig);
}

TEST(upstream_build_antigravity_envelope)
{
	struct auren_upstream_route r;
	auren_upstream_resolve_model("gemini-3.8-flash-low", NULL, &r);

	const char *req_str = "{\"model\":\"gemini-3.8-flash\",\"messages\":[{\"role\":\"system\",\"content\":\"You are helpful inside pi.\"},{\"role\":\"user\",\"content\":\"hello\"}],\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"test_tool\",\"description\":\"a test\",\"parameters\":{\"$schema\":\"http://json-schema.org/draft-07/schema#\",\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"string\"}}}}}]}";
	struct json_object *openai_req = json_tokener_parse(req_str);
	ASSERT_NOT_NULL(openai_req);

	struct json_object *agy_req = auren_upstream_build_antigravity_request(openai_req, &r, "aicode-consumers");
	ASSERT_NOT_NULL(agy_req);

	struct json_object *model_o = json_object_object_get(agy_req, "model");
	ASSERT_NOT_NULL(model_o);
	ASSERT_STR_EQ(json_object_get_string(model_o), "gemini-3.8-flash-low");

	struct json_object *req = json_object_object_get(agy_req, "request");
	ASSERT_NOT_NULL(req);

	struct json_object *sys = json_object_object_get(req, "systemInstruction");
	ASSERT_NOT_NULL(sys);

	struct json_object *tools = json_object_object_get(req, "tools");
	ASSERT_NOT_NULL(tools);
	ASSERT_EQ(json_object_array_length(tools), 1);

	json_object_put(openai_req);
	json_object_put(agy_req);
}

TEST(upstream_append_models_list)
{
	struct json_object *data = json_object_new_array();
	auren_upstream_append_models_json(data);
	size_t len = json_object_array_length(data);
	ASSERT_TRUE(len > 10);

	int found_flash = 0;
	int found_kilo = 0;
	int found_fast = 0;

	for (size_t i = 0; i < len; ++i) {
		struct json_object *item = json_object_array_get_idx(data, i);
		struct json_object *id_o = json_object_object_get(item, "id");
		if (id_o) {
			const char *id_s = json_object_get_string(id_o);
			if (strcmp(id_s, "gemini-3.8-flash") == 0) found_flash = 1;
			if (strcmp(id_s, "kilo-auto/free") == 0) found_kilo = 1;
			if (strcmp(id_s, "fast") == 0) found_fast = 1;
		}
	}
	ASSERT_EQ(found_flash, 1);
	ASSERT_EQ(found_kilo, 1);
	ASSERT_EQ(found_fast, 1);

	json_object_put(data);
}
