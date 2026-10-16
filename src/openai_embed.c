/* openai_embed.c — OpenAI-compatible embeddings endpoint for qllmd. */

#include "openai_embed.h"
#include "qllm-engine.h"
#include "qllm-registry.h"
#include "qllm-pending.h"

#include <ttypt/axil.h>

#include <json-c/json.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static void
embed_respond_error(socket_t fd, int code, const char *msg)
{
	struct json_object *obj;
	const char *s;

	obj = json_object_new_object();
	json_object_object_add(obj, "error",
	    json_object_new_string(msg ? msg : "internal error"));
	s = json_object_to_json_string(obj);
	axil_header_set(fd, "Content-Type", "application/json");
	axil_respond(fd, code, s);
	json_object_put(obj);
}

static int
handler_embeddings(socket_t cfd, char *body)
{
	struct json_object *req = NULL, *input_obj;
	const char *input;
	void *handle;

	if (!body || !body[0]) {
		embed_respond_error(cfd, 400, "empty request body");
		return 0;
	}

	req = json_tokener_parse(body);
	if (!req || !json_object_is_type(req, json_type_object)) {
		embed_respond_error(cfd, 400, "invalid JSON body");
		goto out;
	}

	input_obj = json_object_object_get(req, "input");
	if (!input_obj || !json_object_is_type(input_obj, json_type_string)) {
		embed_respond_error(cfd, 400,
		    "missing or invalid \"input\" (expected a string)");
		goto out;
	}

	input = json_object_get_string(input_obj);
	if (!input || !input[0]) {
		embed_respond_error(cfd, 400, "\"input\" must not be empty");
		goto out;
	}

	struct json_object *mo = json_object_object_get(req, "model");
	const char *model_id = mo && json_object_is_type(mo, json_type_string) ? json_object_get_string(mo) : NULL;
	if (model_id && model_id[0] && strcmp(model_id, "default") != 0) {
		const struct qllm_model_entry *me = qllm_registry_find_for_cap(QLLM_CAP_EMBED, model_id);
		if (!me) {
			embed_respond_error(cfd, 404, "unknown model");
			goto out;
		}
		if (!(me->capabilities & QLLM_CAP_EMBED)) {
			embed_respond_error(cfd, 400, "model does not support embed capability");
			goto out;
		}
	}

	/* Send status + headers now, complete the body later from the loop
	 * thread when the engine's worker finishes (`drain` in qllmd.c). */
	axil_header_set(cfd, "Content-Type", "application/json");
	handle = axil_respond_defer(cfd, 200);
	if (!handle) {
		embed_respond_error(cfd, 503, "unavailable");
		goto out;
	}

	if (qllm_engine_submit_model(QE_JOB_EMBED, model_id, input,
	    make_pending(handle, 0, 0), NULL) != 0) {
		axil_respond_defer_abort(handle);
		embed_respond_error(cfd, 503, "engine busy");
		goto out;
	}

out:
	if (req)
		json_object_put(req);
	return 0;
}

int
openai_embed_init(const char *model_path)
{
	(void)model_path;
	axil_register_handler("POST:/v1/embeddings", handler_embeddings);
	return 0;
}

void
openai_embed_shutdown(void)
{
}
