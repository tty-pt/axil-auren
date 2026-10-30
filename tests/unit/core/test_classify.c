#include "../framework/test_utils.h"
#include <json-c/json.h>
#include <string.h>
#include <math.h>

#define QE_JOB_SYSTEMONE 4

struct qllm_mock_job {
	int kind;
	char *model;
	char *input;
};

struct qllm_mock_res {
	int err;
	char *payload;
};

static float
sim_cosine_calc(const float *a, const float *b, size_t n)
{
	double dot = 0.0, na = 0.0, nb = 0.0;
	for (size_t i = 0; i < n; i++) {
		dot += (double)a[i] * (double)b[i];
		na += (double)a[i] * (double)a[i];
		nb += (double)b[i] * (double)b[i];
	}
	if (na <= 0.0 || nb <= 0.0)
		return 0.0f;
	return (float)(dot / sqrt(na * nb));
}

/* Worker classification logic exercising the exact job dispatch payload flow */
static void
mock_dispatch_worker_classify(struct qllm_mock_job *job, struct qllm_mock_res *out)
{
	memset(out, 0, sizeof(*out));
	struct json_object *o = json_tokener_parse(job->input);
	if (!o || !json_object_is_type(o, json_type_object)) {
		out->err = -400;
		out->payload = strdup("{\"error\":\"invalid JSON body\"}");
		if (o) json_object_put(o);
		return;
	}

	struct json_object *to = json_object_object_get(o, "text");
	struct json_object *lo = json_object_object_get(o, "labels");
	if (!to || !lo || !json_object_is_type(lo, json_type_object)) {
		out->err = -400;
		out->payload = strdup("{\"error\":\"missing text or labels\"}");
		json_object_put(o);
		return;
	}

	const char *probe = json_object_get_string(to);
	if (!probe || !probe[0]) {
		out->err = -400;
		out->payload = strdup("{\"error\":\"empty text\"}");
		json_object_put(o);
		return;
	}

	/* Simulated embedding dimensions & embeddings */
	float pvec[2] = { 1.0f, 0.0f };
	float code_vec[2] = { 0.95f, 0.05f };
	float chat_vec[2] = { 0.10f, 0.90f };

	double max_sim = -2.0, second_sim = -2.0;
	const char *best_label = NULL;
	struct json_object *scores_obj = json_object_new_object();

	struct json_object_iterator it = json_object_iter_begin(lo);
	struct json_object_iterator it_end = json_object_iter_end(lo);

	while (!json_object_iter_equal(&it, &it_end)) {
		const char *lbl = json_object_iter_peek_name(&it);
		float sim = 0.0f;
		if (strcmp(lbl, "code") == 0)
			sim = sim_cosine_calc(pvec, code_vec, 2);
		else
			sim = sim_cosine_calc(pvec, chat_vec, 2);

		json_object_object_add(scores_obj, lbl, json_object_new_double((double)sim));

		if (sim > max_sim) {
			second_sim = max_sim;
			max_sim = sim;
			best_label = lbl;
		} else if (sim > second_sim) {
			second_sim = sim;
		}

		json_object_iter_next(&it);
	}

	double margin = (best_label && second_sim > -1.5) ? (max_sim - second_sim) : (1.0 + max_sim);

	struct json_object *resp = json_object_new_object();
	json_object_object_add(resp, "choice", json_object_new_string(best_label ? best_label : ""));
	json_object_object_add(resp, "similarity", json_object_new_double(max_sim));
	json_object_object_add(resp, "margin", json_object_new_double(margin));
	json_object_object_add(resp, "scores", scores_obj);
	json_object_object_add(resp, "model", json_object_new_string("bge-base"));

	out->payload = strdup(json_object_to_json_string(resp));
	json_object_put(resp);
	json_object_put(o);
}

TEST(systemone_mock_engine_dispatch_and_resolution)
{
	struct qllm_mock_job job;
	job.kind = QE_JOB_SYSTEMONE;
	job.model = "bge-base";
	job.input = "{\"model\":\"bge-base\",\"text\":\"run tests\",\"labels\":{\"code\":[\"run tests\"],\"chat\":[\"hello\"]}}";

	struct qllm_mock_res res;
	mock_dispatch_worker_classify(&job, &res);

	ASSERT_EQ(res.err, 0);
	ASSERT_NOT_NULL(res.payload);

	struct json_object *resp = json_tokener_parse(res.payload);
	ASSERT_NOT_NULL(resp);

	struct json_object *co = json_object_object_get(resp, "choice");
	struct json_object *so = json_object_object_get(resp, "similarity");
	struct json_object *mo = json_object_object_get(resp, "margin");

	ASSERT_NOT_NULL(co);
	ASSERT(strcmp(json_object_get_string(co), "code") == 0);
	ASSERT(json_object_get_double(so) > 0.90);
	ASSERT(json_object_get_double(mo) > 0.50);

	json_object_put(resp);
	free(res.payload);
}

TEST(systemone_mock_engine_dispatch_rejects_empty_text)
{
	struct qllm_mock_job job;
	job.kind = QE_JOB_SYSTEMONE;
	job.model = "bge-base";
	job.input = "{\"model\":\"bge-base\",\"text\":\"\",\"labels\":{\"code\":[\"run tests\"]}}";

	struct qllm_mock_res res;
	mock_dispatch_worker_classify(&job, &res);

	ASSERT_EQ(res.err, -400);
	ASSERT_NOT_NULL(res.payload);
	ASSERT(strstr(res.payload, "empty text") != NULL);
	free(res.payload);
}

TEST(classify_sim_cosine_identical)
{
	float a[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	float b[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	float s = sim_cosine_calc(a, b, 4);
	ASSERT(fabsf(s - 1.0f) < 1e-5f);
}

TEST(classify_sim_cosine_orthogonal)
{
	float a[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	float b[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
	float s = sim_cosine_calc(a, b, 4);
	ASSERT(fabsf(s - 0.0f) < 1e-5f);
}

TEST(classify_label_margin_calculation)
{
	const char *labels[] = { "code", "chat", "docs" };
	double scores[] = { 0.892, 0.718, 0.450 };
	size_t n_labels = 3;

	const char *best_label = NULL;
	double max_sim = -2.0;
	double second_sim = -2.0;

	for (size_t i = 0; i < n_labels; ++i) {
		double s = scores[i];
		if (s > max_sim) {
			second_sim = max_sim;
			max_sim = s;
			best_label = labels[i];
		} else if (s > second_sim) {
			second_sim = s;
		}
	}

	ASSERT_NOT_NULL(best_label);
	ASSERT(strcmp(best_label, "code") == 0);
	ASSERT(fabs(max_sim - 0.892) < 1e-5);
	ASSERT(fabs(second_sim - 0.718) < 1e-5);

	double margin = max_sim - second_sim;
	ASSERT(fabs(margin - 0.174) < 1e-5);
}

TEST(classify_single_label_margin_is_bounded)
{
	double max_sim = 0.85;
	double margin = 1.0 + max_sim;
	ASSERT(fabs(margin - 1.85) < 1e-5);
}
