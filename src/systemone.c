/* systemone.c — Unified System One decision endpoint for axil-auren.
 *
 * Implements POST /api/v1/systemone/ and POST /v1/systemone
 * Evaluates decisions, choices, and exemplars purely using fast in-memory embeddings (QLLM_CAP_EMBED).
 */

#include "systemone.h"
#include "qllm-engine.h"
#include "qllm-registry.h"
#include "qllm-pending.h"

#include <ttypt/axil.h>
#include <json-c/json.h>

#include <stdbool.h>
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
	struct json_object *req = NULL, *mo;
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

	mo = json_object_object_get(req, "model");
	if (mo && json_object_is_type(mo, json_type_string))
		model_id = json_object_get_string(mo);

	/* All System One decisions rely strictly on embedding models (QLLM_CAP_EMBED) */
	const struct qllm_model_entry *me = qllm_registry_find_for_cap(QLLM_CAP_EMBED, model_id);
	if (!me) {
		me = qllm_registry_find_for_cap(QLLM_CAP_EMBED, NULL);
		if (!me) {
			systemone_respond_error(cfd, 503, "no embed model configured");
			goto out;
		}
		model_id = me->id;
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
