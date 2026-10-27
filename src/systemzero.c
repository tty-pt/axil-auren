/* systemzero.c — High-efficiency binary decision endpoint for axil-auren.
 *
 * Implements POST /api/v1/systemzero/ and POST /v1/systemzero
 */

#include "systemzero.h"
#include "qllm-engine.h"
#include "qllm-registry.h"
#include "qllm-pending.h"

#include <ttypt/axil.h>
#include <json-c/json.h>

#include <stdlib.h>
#include <string.h>

static void
systemzero_respond_error(socket_t fd, int code, const char *msg)
{
	struct json_object *obj = json_object_new_object();
	json_object_object_add(obj, "error", json_object_new_string(msg ? msg : "error"));
	const char *s = json_object_to_json_string(obj);
	axil_header_set(fd, "Content-Type", "application/json");
	axil_respond(fd, code, s);
	json_object_put(obj);
}

static int
handler_systemzero(socket_t cfd, char *body)
{
	struct json_object *req = NULL, *po, *mo;
	const char *model_id = NULL;
	void *handle;

	if (!body || !body[0]) {
		systemzero_respond_error(cfd, 400, "empty request body");
		return 0;
	}

	req = json_tokener_parse(body);
	if (!req || !json_object_is_type(req, json_type_object)) {
		systemzero_respond_error(cfd, 400, "invalid JSON body");
		goto out;
	}

	po = json_object_object_get(req, "prompt");
	if (!po)
		po = json_object_object_get(req, "state");
	if (!po) {
		systemzero_respond_error(cfd, 400, "missing 'prompt' or 'state' field");
		goto out;
	}

	mo = json_object_object_get(req, "model");
	if (mo && json_object_is_type(mo, json_type_string))
		model_id = json_object_get_string(mo);

	const struct qllm_model_entry *me = qllm_registry_find_for_cap(QLLM_CAP_JUDGE, model_id);
	if (!me) {
		if (model_id && model_id[0] && strcmp(model_id, "default") != 0)
			systemzero_respond_error(cfd, 404, "unknown model");
		else
			systemzero_respond_error(cfd, 503, "no judge model configured");
		goto out;
	}
	if (!(me->capabilities & QLLM_CAP_JUDGE)) {
		systemzero_respond_error(cfd, 422, "model does not support judge capability");
		goto out;
	}

	axil_header_set(cfd, "Content-Type", "application/json");
	handle = axil_respond_defer(cfd, 200);
	if (!handle) {
		systemzero_respond_error(cfd, 503, "service unavailable");
		goto out;
	}

	if (qllm_engine_systemzero_model(model_id, body, make_pending(handle, 0, 0), NULL) != 0) {
		axil_respond_defer_abort(handle);
		systemzero_respond_error(cfd, 503, "engine busy");
		goto out;
	}

out:
	if (req)
		json_object_put(req);
	return 0;
}

int
systemzero_init(void)
{
	axil_register_handler("POST:/api/v1/systemzero/", handler_systemzero);
	axil_register_handler("POST:/api/v1/systemzero", handler_systemzero);
	axil_register_handler("POST:/v1/systemzero/", handler_systemzero);
	axil_register_handler("POST:/v1/systemzero", handler_systemzero);
	return 0;
}

void
systemzero_shutdown(void)
{
}
