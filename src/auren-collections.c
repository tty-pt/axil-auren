/* auren-collections.c — Partitioned multi-tenant collections using libsepal and libcorm.
 *
 * Implements:
 *   - POST   /v1/collections/:name/documents
 *   - GET    /v1/collections/:name/search
 *   - DELETE /v1/collections/:name/documents/:id
 */

#include "auren-collections.h"
#include "qllm-engine.h"
#include "qllm-registry.h"

#include <ttypt/axil.h>
#include <ttypt/corm.h>
#include <ttypt/sepal.h>

#include <json-c/json.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_COLLECTIONS 64
#define MAX_NAME_LEN 64
#define MAX_HITS 50
#define VEC_DIM_MAX 4096

struct collection {
	char name[MAX_NAME_LEN + 1];
	sepal_vecstore_t *vs;
	uint32_t corm_hd;
	uint64_t next_ref;
	pthread_mutex_t lock;
	int active;
};

static struct {
	struct collection cols[MAX_COLLECTIONS];
	size_t count;
	pthread_mutex_t lock;
	int inited;
} g_col_mgr;

static int
validate_collection_name(const char *name)
{
	if (!name || !name[0])
		return 0;
	size_t len = strlen(name);
	if (len > MAX_NAME_LEN)
		return 0;
	for (size_t i = 0; i < len; ++i) {
		char c = name[i];
		if (!isalnum((unsigned char)c) && c != '_' && c != '-')
			return 0;
	}
	return 1;
}

static void
collections_respond_error(socket_t fd, int code, const char *msg)
{
	struct json_object *obj = json_object_new_object();
	json_object_object_add(obj, "error", json_object_new_string(msg ? msg : "error"));
	const char *s = json_object_to_json_string(obj);
	axil_header_set(fd, "Content-Type", "application/json");
	axil_respond(fd, code, s);
	json_object_put(obj);
}

static void
ensure_collections_dir(void)
{
	(void)mkdir("var", 0755);
	(void)mkdir("var/collections", 0755);
}

static struct collection *
get_or_open_collection(const char *name)
{
	if (!validate_collection_name(name))
		return NULL;

	if (!g_col_mgr.inited) {
		pthread_mutex_init(&g_col_mgr.lock, NULL);
		g_col_mgr.inited = 1;
	}

	pthread_mutex_lock(&g_col_mgr.lock);
	for (size_t i = 0; i < g_col_mgr.count; ++i) {
		if (g_col_mgr.cols[i].active && strcmp(g_col_mgr.cols[i].name, name) == 0) {
			pthread_mutex_unlock(&g_col_mgr.lock);
			return &g_col_mgr.cols[i];
		}
	}

	if (g_col_mgr.count >= MAX_COLLECTIONS) {
		pthread_mutex_unlock(&g_col_mgr.lock);
		return NULL;
	}

	ensure_collections_dir();
	char dir_path[256];
	snprintf(dir_path, sizeof(dir_path), "var/collections/%s", name);
	(void)mkdir(dir_path, 0755);

	char sepal_path[512];
	char corm_path[512];
	snprintf(sepal_path, sizeof(sepal_path), "%s/%s.sepal", dir_path, name);
	snprintf(corm_path, sizeof(corm_path), "%s/%s.corm", dir_path, name);

	int err = 0;
	sepal_vecstore_t *vs = sepal_open(sepal_path, &err);
	if (!vs) {
		pthread_mutex_unlock(&g_col_mgr.lock);
		return NULL;
	}

	uint32_t hd = corm_open(corm_path, NULL, CM_STR, CM_STR, 0xFF, 0);
	if (hd == CM_MISS) {
		sepal_close(vs);
		pthread_mutex_unlock(&g_col_mgr.lock);
		return NULL;
	}

	size_t idx = g_col_mgr.count++;
	struct collection *col = &g_col_mgr.cols[idx];
	snprintf(col->name, sizeof(col->name), "%s", name);
	col->vs = vs;
	col->corm_hd = hd;
	col->next_ref = 1;

	/* Determine next_ref from stored counter */
	const char *cnt_str = corm_get(hd, "__next_ref__");
	if (cnt_str) {
		uint64_t n = strtoull(cnt_str, NULL, 10);
		if (n >= 1) col->next_ref = n;
	}

	pthread_mutex_init(&col->lock, NULL);
	col->active = 1;

	pthread_mutex_unlock(&g_col_mgr.lock);
	return col;
}

static void
get_param_val(socket_t cfd, const char *body, const char *env_key, const char *param_name, char *buf, size_t buf_len)
{
	buf[0] = '\0';
	const char *ev = getenv(env_key);
	if (ev && ev[0]) {
		snprintf(buf, buf_len, "%s", ev);
		return;
	}
	(void)axil_req_param(cfd, body, param_name, buf, buf_len);
}

static int
handler_collections_post_document(socket_t cfd, char *body)
{
	char name[MAX_NAME_LEN + 1] = {0};
	get_param_val(cfd, body, "PATTERN_PARAM_NAME", "name", name, sizeof(name));

	if (!validate_collection_name(name)) {
		collections_respond_error(cfd, 400, "invalid collection name (alphanumeric/hyphen/underscore only)");
		return 0;
	}

	struct collection *col = get_or_open_collection(name);
	if (!col) {
		collections_respond_error(cfd, 500, "failed to open collection store");
		return 0;
	}

	if (!body || !body[0]) {
		collections_respond_error(cfd, 400, "empty request body");
		return 0;
	}

	struct json_object *req = json_tokener_parse(body);
	if (!req || !json_object_is_type(req, json_type_object)) {
		collections_respond_error(cfd, 400, "invalid JSON body");
		if (req) json_object_put(req);
		return 0;
	}

	struct json_object *to = json_object_object_get(req, "text");
	struct json_object *io = json_object_object_get(req, "id");
	if (!to || !json_object_is_type(to, json_type_string)) {
		collections_respond_error(cfd, 400, "missing or invalid 'text' field");
		json_object_put(req);
		return 0;
	}

	const char *text = json_object_get_string(to);
	const char *doc_id = io && json_object_is_type(io, json_type_string) ? json_object_get_string(io) : NULL;

	char auto_id[64];
	pthread_mutex_lock(&col->lock);
	if (!doc_id || !doc_id[0]) {
		snprintf(auto_id, sizeof(auto_id), "%llu", (unsigned long long)col->next_ref);
		doc_id = auto_id;
	}

	/* Acquire embed model */
	struct qllm_context *ctx = NULL;
	char model_used[64] = {0};
	int rc = qllm_registry_acquire(NULL, QLLM_CAP_EMBED, &ctx, model_used, sizeof(model_used));
	if (rc != QLLM_REG_OK || !ctx) {
		pthread_mutex_unlock(&col->lock);
		json_object_put(req);
		collections_respond_error(cfd, 503, "embedding model unavailable");
		return 0;
	}

	float vec[VEC_DIM_MAX];
	int dim = qllm_embed(ctx, text, vec, VEC_DIM_MAX);
	qllm_registry_release(model_used);
	if (dim <= 0) {
		pthread_mutex_unlock(&col->lock);
		json_object_put(req);
		collections_respond_error(cfd, 500, "failed to embed text");
		return 0;
	}

	/* Check if doc_id already exists in this collection; if so, delete old vector */
	char id_key[256];
	snprintf(id_key, sizeof(id_key), "id:%s", doc_id);
	const char *old_ref_str = corm_get(col->corm_hd, id_key);
	uint64_t ref;
	if (old_ref_str) {
		ref = strtoull(old_ref_str, NULL, 10);
		sepal_del(col->vs, ref);
	} else {
		ref = col->next_ref++;
		char next_buf[64];
		snprintf(next_buf, sizeof(next_buf), "%llu", (unsigned long long)col->next_ref);
		corm_put(col->corm_hd, "__next_ref__", next_buf);
	}

	char ref_str[64];
	snprintf(ref_str, sizeof(ref_str), "%llu", (unsigned long long)ref);

	/* Store id -> ref, and ref -> text and ref -> id */
	corm_put(col->corm_hd, id_key, ref_str);

	char ref_key[256];
	snprintf(ref_key, sizeof(ref_key), "ref:%llu", (unsigned long long)ref);
	corm_put(col->corm_hd, ref_key, text);

	char rev_key[256];
	snprintf(rev_key, sizeof(rev_key), "rev:%llu", (unsigned long long)ref);
	corm_put(col->corm_hd, rev_key, doc_id);

	/* Persist vector in sepal */
	sepal_put(col->vs, ref, vec, (size_t)dim);
	corm_save();

	pthread_mutex_unlock(&col->lock);

	struct json_object *resp = json_object_new_object();
	json_object_object_add(resp, "id", json_object_new_string(doc_id));
	json_object_object_add(resp, "collection", json_object_new_string(col->name));
	json_object_object_add(resp, "status", json_object_new_string("indexed"));

	const char *resp_s = json_object_to_json_string(resp);
	axil_header_set(cfd, "Content-Type", "application/json");
	axil_respond(cfd, 200, resp_s);

	json_object_put(resp);
	json_object_put(req);
	return 0;
}

static int
handler_collections_search(socket_t cfd, char *body)
{
	char name[MAX_NAME_LEN + 1] = {0};
	get_param_val(cfd, body, "PATTERN_PARAM_NAME", "name", name, sizeof(name));

	if (!validate_collection_name(name)) {
		collections_respond_error(cfd, 400, "invalid collection name");
		return 0;
	}

	struct collection *col = get_or_open_collection(name);
	if (!col) {
		collections_respond_error(cfd, 500, "failed to open collection store");
		return 0;
	}

	/* Extract query params: q and limit */
	char qbuf[1024] = {0};
	if (axil_req_param(cfd, body, "q", qbuf, sizeof(qbuf)) < 0 || !qbuf[0]) {
		collections_respond_error(cfd, 400, "missing or empty 'q' parameter");
		return 0;
	}

	char limit_buf[32] = {0};
	size_t limit = 10;
	if (axil_req_param(cfd, body, "limit", limit_buf, sizeof(limit_buf)) >= 0 && limit_buf[0]) {
		int l = atoi(limit_buf);
		if (l > 0 && l <= MAX_HITS) limit = (size_t)l;
	}

	/* Embed query */
	struct qllm_context *ctx = NULL;
	char model_used[64] = {0};
	int rc = qllm_registry_acquire(NULL, QLLM_CAP_EMBED, &ctx, model_used, sizeof(model_used));
	if (rc != QLLM_REG_OK || !ctx) {
		collections_respond_error(cfd, 503, "embedding model unavailable");
		return 0;
	}

	float qvec[VEC_DIM_MAX];
	int qdim = qllm_embed(ctx, qbuf, qvec, VEC_DIM_MAX);
	qllm_registry_release(model_used);
	if (qdim <= 0) {
		collections_respond_error(cfd, 500, "failed to embed query");
		return 0;
	}

	pthread_mutex_lock(&col->lock);
	sepal_hit_t hits[MAX_HITS];
	size_t nhits = sepal_search(col->vs, qvec, (size_t)qdim, limit, 0.0f, 0, hits);

	struct json_object *resp = json_object_new_object();
	struct json_object *data = json_object_new_array();

	for (size_t i = 0; i < nhits; ++i) {
		char ref_key[256];
		char rev_key[256];
		snprintf(ref_key, sizeof(ref_key), "ref:%llu", (unsigned long long)hits[i].ref);
		snprintf(rev_key, sizeof(rev_key), "rev:%llu", (unsigned long long)hits[i].ref);

		const char *text = corm_get(col->corm_hd, ref_key);
		const char *doc_id = corm_get(col->corm_hd, rev_key);

		struct json_object *item = json_object_new_object();
		json_object_object_add(item, "id", json_object_new_string(doc_id ? doc_id : ""));
		json_object_object_add(item, "text", json_object_new_string(text ? text : ""));
		json_object_object_add(item, "score", json_object_new_double((double)hits[i].score));
		json_object_array_add(data, item);
	}
	pthread_mutex_unlock(&col->lock);

	json_object_object_add(resp, "collection", json_object_new_string(col->name));
	json_object_object_add(resp, "query", json_object_new_string(qbuf));
	json_object_object_add(resp, "results", data);

	const char *resp_s = json_object_to_json_string(resp);
	axil_header_set(cfd, "Content-Type", "application/json");
	axil_respond(cfd, 200, resp_s);

	json_object_put(resp);
	return 0;
}

static int
handler_collections_delete_document(socket_t cfd, char *body)
{
	char name[MAX_NAME_LEN + 1] = {0};
	char id_buf[128] = {0};
	get_param_val(cfd, body, "PATTERN_PARAM_NAME", "name", name, sizeof(name));
	get_param_val(cfd, body, "PATTERN_PARAM_ID", "id", id_buf, sizeof(id_buf));

	if (!validate_collection_name(name)) {
		collections_respond_error(cfd, 400, "invalid collection name");
		return 0;
	}
	if (!id_buf[0]) {
		collections_respond_error(cfd, 400, "missing document ID");
		return 0;
	}

	struct collection *col = get_or_open_collection(name);
	if (!col) {
		collections_respond_error(cfd, 500, "failed to open collection store");
		return 0;
	}

	pthread_mutex_lock(&col->lock);
	char id_key[256];
	snprintf(id_key, sizeof(id_key), "id:%s", id_buf);
	const char *ref_str = corm_get(col->corm_hd, id_key);
	if (!ref_str) {
		pthread_mutex_unlock(&col->lock);
		collections_respond_error(cfd, 404, "document not found in collection");
		return 0;
	}

	uint64_t ref = strtoull(ref_str, NULL, 10);
	char ref_key[256];
	char rev_key[256];
	snprintf(ref_key, sizeof(ref_key), "ref:%llu", (unsigned long long)ref);
	snprintf(rev_key, sizeof(rev_key), "rev:%llu", (unsigned long long)ref);

	sepal_del(col->vs, ref);
	corm_del(col->corm_hd, id_key);
	corm_del(col->corm_hd, ref_key);
	corm_del(col->corm_hd, rev_key);
	corm_save();
	pthread_mutex_unlock(&col->lock);

	struct json_object *resp = json_object_new_object();
	json_object_object_add(resp, "id", json_object_new_string(id_buf));
	json_object_object_add(resp, "collection", json_object_new_string(col->name));
	json_object_object_add(resp, "status", json_object_new_string("deleted"));

	const char *resp_s = json_object_to_json_string(resp);
	axil_header_set(cfd, "Content-Type", "application/json");
	axil_respond(cfd, 200, resp_s);

	json_object_put(resp);
	return 0;
}

int
auren_collections_init(void)
{
	axil_register_handler("POST:/v1/collections/:name/documents", handler_collections_post_document);
	axil_register_handler("POST:/api/v1/collections/:name/documents", handler_collections_post_document);
	axil_register_handler("GET:/v1/collections/:name/search", handler_collections_search);
	axil_register_handler("GET:/api/v1/collections/:name/search", handler_collections_search);
	axil_register_handler("DELETE:/v1/collections/:name/documents/:id", handler_collections_delete_document);
	axil_register_handler("DELETE:/api/v1/collections/:name/documents/:id", handler_collections_delete_document);
	return 0;
}

void
auren_collections_shutdown(void)
{
	if (!g_col_mgr.inited) return;
	pthread_mutex_lock(&g_col_mgr.lock);
	for (size_t i = 0; i < g_col_mgr.count; ++i) {
		if (g_col_mgr.cols[i].active) {
			if (g_col_mgr.cols[i].vs) sepal_close(g_col_mgr.cols[i].vs);
			if (g_col_mgr.cols[i].corm_hd != CM_MISS) corm_close(g_col_mgr.cols[i].corm_hd);
			g_col_mgr.cols[i].active = 0;
		}
	}
	pthread_mutex_unlock(&g_col_mgr.lock);
}
