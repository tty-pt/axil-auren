#include "../framework/test_utils.h"
#include "../../../src/systemone-choice.h"
#include <string.h>

static struct json_object *
question(const char *json)
{
	return json_tokener_parse(json);
}

TEST(systemone_choice_exact_key)
{
	struct json_object *q = question("{\"criteria\":{\"refinement\":\"a\",\"advance\":\"b\"}}");
	const char *r = systemone_resolve_choice("advance", q);

	ASSERT_NOT_NULL(r);
	ASSERT(strcmp(r, "advance") == 0);
	json_object_put(q);
}

TEST(systemone_choice_normalises_case_and_space)
{
	struct json_object *q = question("{\"criteria\":{\"refinement\":\"a\",\"advance\":\"b\"}}");
	const char *r = systemone_resolve_choice("  ADVANCE\n", q);

	ASSERT_NOT_NULL(r);
	ASSERT(strcmp(r, "advance") == 0);
	json_object_put(q);
}

TEST(systemone_choice_rejects_unknown_and_prefixes)
{
	struct json_object *q = question("{\"criteria\":{\"refinement\":\"a\",\"advance\":\"b\"}}");

	ASSERT_NULL(systemone_resolve_choice("advan", q));
	ASSERT_NULL(systemone_resolve_choice("advance now", q));
	ASSERT_NULL(systemone_resolve_choice("", q));
	ASSERT_NULL(systemone_resolve_choice("   ", q));
	ASSERT_NULL(systemone_resolve_choice(NULL, q));
	json_object_put(q);
}

TEST(systemone_choice_array_choices)
{
	struct json_object *q = question("{\"choices\":[\"yes\",\"no\"]}");
	const char *r = systemone_resolve_choice("No", q);

	ASSERT_NOT_NULL(r);
	ASSERT(strcmp(r, "no") == 0);
	ASSERT_NULL(systemone_resolve_choice("maybe", q));
	json_object_put(q);
}

TEST(systemone_choice_free_form_passthrough)
{
	struct json_object *q = question("{\"type\":\"choice\"}");
	const char *r = systemone_resolve_choice("whatever", q);

	ASSERT_NOT_NULL(r);
	ASSERT(strcmp(r, "whatever") == 0);
	json_object_put(q);
}
