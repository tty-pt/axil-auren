/* systemone.c — Jev-compatible System One decision endpoint for axil-qllm.
 *
 * Implements POST /api/v1/systemone/ and POST /v1/systemone
 */

#include "systemone.h"
#include "qllm-engine.h"
#include "qllm-registry.h"
#include "qllm-pending.h"

#include <ttypt/axil.h>
#include <json-c/json.h>

#include <stdlib.h>
#include <string.h>

static void
systemone_respond_error(socket_t fd, int code, const char *msg)
{
	struct json_object *obj = json_object_new_object();
	json_object_object_add(obj, "error", json_object_new_string(msg ? msg : "error"));
	const char *s = json_object_to_json_string(obj);
	axil_header_set(fd, "Content-Type", "application/json");
	axil_respond(fd, code, s);
	json_object_put(obj);
}

static int
handler_systemone(socket_t cfd, char *body)
{
	struct json_object *req = NULL, *state_o, *qs_o, *mo;
	const char *model_id = NULL;
	void *handle;

	if (!body || !body[0]) {
		systemone_respond_error(cfd, 400, "empty request body");
		return 0;
	}

	req = json_tokener_parse(body);
	if (!req || !json_object_is_type(req, json_type_object)) {
		systemone_respond_error(cfd, 400, "invalid JSON body");
		goto out;
	}

	state_o = json_object_object_get(req, "state");
	if (!state_o) {
		systemone_respond_error(cfd, 400, "missing 'state' field");
		goto out;
	}

	qs_o = json_object_object_get(req, "questions");
	if (!qs_o || (!json_object_is_type(qs_o, json_type_object) && !json_object_is_type(qs_o, json_type_array))) {
		systemone_respond_error(cfd, 400, "missing or invalid 'questions' field");
		goto out;
	}

	if (json_object_is_type(qs_o, json_type_object)) {
		if (json_object_object_length(qs_o) == 0) {
			systemone_respond_error(cfd, 400, "no questions provided");
			goto out;
		}
	} else if (json_object_is_type(qs_o, json_type_array)) {
		if (json_object_array_length(qs_o) == 0) {
			systemone_respond_error(cfd, 400, "no questions provided");
			goto out;
		}
	}

	/* Validate questions types */
	if (json_object_is_type(qs_o, json_type_object)) {
		struct json_object_iterator it = json_object_iter_begin(qs_o);
		struct json_object_iterator it_end = json_object_iter_end(qs_o);
		while (!json_object_iter_equal(&it, &it_end)) {
			struct json_object *val = json_object_iter_peek_value(&it);
			struct json_object *type_o = val ? json_object_object_get(val, "type") : NULL;
			const char *t = type_o ? json_object_get_string(type_o) : NULL;
			if (!t || (strcmp(t, "choice") != 0 && strcmp(t, "noul") != 0 && strcmp(t, "score") != 0)) {
				systemone_respond_error(cfd, 400, "unsupported question type (expected choice, noul, or score)");
				goto out;
			}
			json_object_iter_next(&it);
		}
	} else {
		size_t i, n = json_object_array_length(qs_o);
		for (i = 0; i < n; ++i) {
			struct json_object *item = json_object_array_get_idx(qs_o, i);
			struct json_object *type_o = item ? json_object_object_get(item, "type") : NULL;
			const char *t = type_o ? json_object_get_string(type_o) : NULL;
			if (!t || (strcmp(t, "choice") != 0 && strcmp(t, "noul") != 0 && strcmp(t, "score") != 0)) {
				systemone_respond_error(cfd, 400, "unsupported question type (expected choice, noul, or score)");
				goto out;
			}
		}
	}

	mo = json_object_object_get(req, "model");
	if (mo && json_object_is_type(mo, json_type_string))
		model_id = json_object_get_string(mo);

	const struct qllm_model_entry *me = qllm_registry_find_for_cap(QLLM_CAP_JUDGE, model_id);
	if (!me) {
		if (model_id && model_id[0] && strcmp(model_id, "default") != 0)
			systemone_respond_error(cfd, 404, "unknown model");
		else
			systemone_respond_error(cfd, 503, "no judge model configured");
		goto out;
	}
	if (!(me->capabilities & QLLM_CAP_JUDGE)) {
		systemone_respond_error(cfd, 422, "model does not support judge capability");
		goto out;
	}

	axil_header_set(cfd, "Content-Type", "application/json");
	handle = axil_respond_defer(cfd, 200);
	if (!handle) {
		systemone_respond_error(cfd, 503, "service unavailable");
		goto out;
	}

	if (qllm_engine_systemone_model(model_id, body, make_pending(handle, 0, 0), NULL) != 0) {
		axil_respond_defer_abort(handle);
		systemone_respond_error(cfd, 503, "engine busy");
		goto out;
	}

out:
	if (req)
		json_object_put(req);
	return 0;
}

int
systemone_init(void)
{
	axil_register_handler("POST:/api/v1/systemone/", handler_systemone);
	axil_register_handler("POST:/api/v1/systemone", handler_systemone);
	axil_register_handler("POST:/v1/systemone/", handler_systemone);
	axil_register_handler("POST:/v1/systemone", handler_systemone);
	return 0;
}

void
systemone_shutdown(void)
{
}
