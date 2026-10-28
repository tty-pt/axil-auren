/* openai_classify.c — In-memory nearest-exemplar classification endpoint for axil-auren.
 *
 * Implements POST /v1/classify and POST /api/v1/classify
 */

#include "openai_classify.h"
#include "qllm-engine.h"
#include "qllm-registry.h"
#include "qllm-pending.h"

#include <ttypt/axil.h>
#include <json-c/json.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static void
classify_respond_error(socket_t fd, int code, const char *msg)
{
	struct json_object *obj = json_object_new_object();
	json_object_object_add(obj, "error", json_object_new_string(msg ? msg : "internal error"));
	const char *s = json_object_to_json_string(obj);
	axil_header_set(fd, "Content-Type", "application/json");
	axil_respond(fd, code, s);
	json_object_put(obj);
}

static int
handler_classify(socket_t cfd, char *body)
{
	struct json_object *req = NULL, *text_obj, *labels_obj, *mo;
	const char *text = NULL;
	const char *model_id = NULL;
	void *handle;
	int num_valid_labels = 0;

	if (!body || !body[0]) {
		classify_respond_error(cfd, 400, "empty request body");
		return 0;
	}

	req = json_tokener_parse(body);
	if (!req || !json_object_is_type(req, json_type_object)) {
		classify_respond_error(cfd, 400, "invalid JSON body");
		goto out;
	}

	text_obj = json_object_object_get(req, "text");
	if (!text_obj || !json_object_is_type(text_obj, json_type_string)) {
		classify_respond_error(cfd, 400, "missing or invalid \"text\" field");
		goto out;
	}
	text = json_object_get_string(text_obj);
	if (!text || !text[0]) {
		classify_respond_error(cfd, 400, "\"text\" must not be empty");
		goto out;
	}

	labels_obj = json_object_object_get(req, "labels");
	if (!labels_obj || !json_object_is_type(labels_obj, json_type_object)) {
		classify_respond_error(cfd, 400, "missing or invalid \"labels\" field (expected object)");
		goto out;
	}

	struct json_object_iterator it = json_object_iter_begin(labels_obj);
	struct json_object_iterator it_end = json_object_iter_end(labels_obj);
	while (!json_object_iter_equal(&it, &it_end)) {
		struct json_object *val = json_object_iter_peek_value(&it);
		if (!val || !json_object_is_type(val, json_type_array)) {
			classify_respond_error(cfd, 400, "each label must map to an array of example strings");
			goto out;
		}
		size_t arr_len = json_object_array_length(val);
		if (arr_len == 0) {
			classify_respond_error(cfd, 400, "label example array must not be empty");
			goto out;
		}
		for (size_t i = 0; i < arr_len; ++i) {
			struct json_object *elem = json_object_array_get_idx(val, i);
			if (!elem || !json_object_is_type(elem, json_type_string)) {
				classify_respond_error(cfd, 400, "each exemplar in label array must be a string");
				goto out;
			}
		}
		num_valid_labels++;
		json_object_iter_next(&it);
	}

	if (num_valid_labels < 1) {
		classify_respond_error(cfd, 400, "\"labels\" must contain at least one label");
		goto out;
	}

	mo = json_object_object_get(req, "model");
	if (mo && json_object_is_type(mo, json_type_string))
		model_id = json_object_get_string(mo);

	if (model_id && model_id[0] && strcmp(model_id, "default") != 0) {
		const struct qllm_model_entry *me = qllm_registry_find_for_cap(QLLM_CAP_EMBED, model_id);
		if (!me) {
			classify_respond_error(cfd, 404, "unknown model");
			goto out;
		}
		if (!(me->capabilities & QLLM_CAP_EMBED)) {
			classify_respond_error(cfd, 400, "model does not support embed capability");
			goto out;
		}
	}

	axil_header_set(cfd, "Content-Type", "application/json");
	handle = axil_respond_defer(cfd, 200);
	if (!handle) {
		classify_respond_error(cfd, 503, "unavailable");
		goto out;
	}

	if (qllm_engine_submit_model(QE_JOB_CLASSIFY, model_id, body,
	    make_pending(handle, 0, 0), NULL) != 0) {
		axil_respond_defer_abort(handle);
		classify_respond_error(cfd, 503, "engine busy");
		goto out;
	}

out:
	if (req)
		json_object_put(req);
	return 0;
}

int
openai_classify_init(void)
{
	axil_register_handler("POST:/v1/classify", handler_classify);
	axil_register_handler("POST:/v1/classify/", handler_classify);
	axil_register_handler("POST:/api/v1/classify", handler_classify);
	axil_register_handler("POST:/api/v1/classify/", handler_classify);
	return 0;
}

void
openai_classify_shutdown(void)
{
	/* Handlers unregistered on axil shutdown */
}
