#include "systemone-choice.h"

#include <string.h>
#include <strings.h>

static const char *
trim_span(const char *s, size_t *len)
{
	size_t n = strlen(s);

	while (n > 0 && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
		s++;
		n--;
	}
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
	                 s[n - 1] == '\n' || s[n - 1] == '\r'))
		n--;
	*len = n;
	return s;
}

static int
same_key(const char *chosen, size_t clen, const char *key)
{
	return key && strlen(key) == clen && strncasecmp(chosen, key, clen) == 0;
}

const char *
systemone_resolve_choice(const char *chosen, struct json_object *q)
{
	struct json_object *crit, *cho;
	size_t clen;

	if (!chosen)
		return NULL;
	chosen = trim_span(chosen, &clen);
	if (clen == 0)
		return NULL;

	crit = q ? json_object_object_get(q, "criteria") : NULL;
	cho = q ? json_object_object_get(q, "choices") : NULL;

	if (crit && json_object_is_type(crit, json_type_object)) {
		struct json_object_iterator it = json_object_iter_begin(crit);
		struct json_object_iterator end = json_object_iter_end(crit);

		while (!json_object_iter_equal(&it, &end)) {
			const char *k = json_object_iter_peek_name(&it);

			if (same_key(chosen, clen, k))
				return k;
			json_object_iter_next(&it);
		}
		return NULL;
	}

	if (cho && json_object_is_type(cho, json_type_array)) {
		size_t i, n = json_object_array_length(cho);

		for (i = 0; i < n; ++i) {
			struct json_object *e = json_object_array_get_idx(cho, i);
			const char *k = e ? json_object_get_string(e) : NULL;

			if (same_key(chosen, clen, k))
				return k;
		}
		return NULL;
	}

	return chosen;
}
