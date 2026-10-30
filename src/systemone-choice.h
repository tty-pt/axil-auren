#ifndef SYSTEMONE_CHOICE_H
#define SYSTEMONE_CHOICE_H

#include <json-c/json.h>

/*
 * Resolve a model-produced answer to exactly one option declared by the
 * caller's question (`criteria` object keys or `choices` array entries).
 *
 * Matching ignores case and surrounding whitespace; there is no synonym or
 * substring matching. Returns the declared key (owned by `q`, valid while `q`
 * lives) or NULL when the answer is empty or not a declared option — callers
 * must report that as "unparseable", never substitute a default.
 *
 * When the question declares no options at all, any non-empty answer passes
 * through unchanged (free-form choice).
 */
const char *systemone_resolve_choice(const char *chosen, struct json_object *q);

#endif
