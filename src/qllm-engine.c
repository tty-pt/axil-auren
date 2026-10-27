/* qllm-engine.c — worker-thread LLM engine (see qllm-engine.h).
 *
 * Phase 3B adds: session state (16 slots, LRU eviction, keyed by sid),
 * streaming multi-result chat (chunks + final), and stateless HTTP oneshot.
 *
 * Session model (do not violate):
 *   - All session LLM state (ctx, msgs[], prev_prompt, assistant_buf) is
 *     touched ONLY by the worker thread, and only while it runs a job for
 *     that sid.
 *   - Session bookkeeping (used/sid/system_prompt/last_used_seq) sits behind
 *     g_engine.sess_mutex. The loop thread never mutates session LLM state —
 *     open/reset/close are QUEUED as meta jobs (QE_JOB_OPEN/RESET/CLOSE), so a
 *     close can never race an in-flight generation.
 *   - LRU eviction happens on the worker when a chat needs a slot for a sid it
 *     has never seen. A slot is only evictable if no queued job references its
 *     sid (queue_has_sid), which preserves all-but-one sessions' history.
 */

#include "qllm-engine.h"
#include "qllm-registry.h"

#include <ttypt/qllm.h>

#include <json-c/json.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define QE_JOB_MAX  64
#define QE_VEC_MAX  4096
#define QE_SESSIONS 16
#define QE_MAX_MSGS 256

struct qllm_engine_job {
	int   kind;
	int   sid;      /* QE_JOB_CHAT / session meta kinds */
	char *session_key; /* string session identifier */
	char *model;    /* explicit model ID or NULL for default */
	char *input;    /* owned by engine; freed after processing (may be NULL) */
	void *ud;
	int   job_id;
};

struct qllm_session {
	int      used;           /* slot occupied */
	int      sid;
	char     session_key[128];
	char    *system_prompt;  /* strdup'd, session-owned; may be NULL */
	uint64_t last_used_seq;  /* LRU */
	char     model_used[64];

	/* worker-only LLM state (never touched by the loop thread): */
	struct qllm_context *ctx;
	struct qllm_message  msgs[QE_MAX_MSGS];
	size_t   n_msgs;
	char    *prev_prompt;
	char    *assistant_buf;
	size_t   assist_len;
};

struct qllm_engine {
	char model_path[BUFSIZ];

	int   running;
	int   quit;

	pthread_t       worker;
	pthread_mutex_t job_mutex;
	pthread_cond_t  job_cond;
	struct qllm_engine_job jobs[QE_JOB_MAX];
	int job_head, job_tail, job_count;

	pthread_mutex_t comp_mutex;
	pthread_cond_t  comp_cond; /* worker waits here when the ring is full */
	struct qllm_engine_result comp[QE_JOB_MAX];
	int comp_head, comp_tail, comp_count;

	/* self-pipe: worker writes a byte to wake the consumer */
	int pipe_r, pipe_w;

	int next_job_id;

	/* sessions + engine-global system prompt */
	pthread_mutex_t   sess_mutex;
	struct qllm_session sessions[QE_SESSIONS];
	uint64_t          sess_seq;
	char             *system_prompt; /* engine-global strdup'd, may be NULL */
};

static struct qllm_engine g_engine;

static int
engine_running(void)
{
	return g_engine.running && !g_engine.quit;
}

/* Stream accumulation state shared by the chat/oneshot callbacks. Acc points
 * at the caller's full-reply buffer (a session's assistant_buf or a local). */
struct stream_st {
	int    job_id;
	void  *ud;
	int    seq;
	int    err;
	int    streaming;  /* oneshot: push per-chunk results only when true */
	const char *model;
	char **acc;
	size_t *acc_len;
};

/* Drain the self-pipe read end non-blocking to EAGAIN. The wake bytes are
 * pure hints — the completion queue holds the actual results. This must run
 * whenever an event loop select()s this fd, otherwise leftover hint bytes keep
 * the pipe permanently readable and the loop busy-spins. Consumer-thread only. */
static void
engine_drain_pipe(void)
{
	char b[64];

	while (read(g_engine.pipe_r, b, sizeof(b)) > 0)
		;
}

static int
completion_push(const struct qllm_engine_result *r)
{
	int i;

	pthread_mutex_lock(&g_engine.comp_mutex);
	while (g_engine.comp_count >= QE_JOB_MAX && !g_engine.quit)
		pthread_cond_wait(&g_engine.comp_cond, &g_engine.comp_mutex);
	if (g_engine.comp_count >= QE_JOB_MAX) { /* quit or still full */
		pthread_mutex_unlock(&g_engine.comp_mutex);
		return -1;
	}

	i = g_engine.comp_tail;
	g_engine.comp_tail = (g_engine.comp_tail + 1) % QE_JOB_MAX;
	g_engine.comp[i] = *r;
	++g_engine.comp_count;
	pthread_mutex_unlock(&g_engine.comp_mutex);

	/* Wake hint only; EAGAIN is fine (a byte is already buffered and the
	 * completion queue holds the data). */
	if (write(g_engine.pipe_w, "\0", 1) < 0 && errno != EAGAIN)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Embeddings                                                          */
/* ------------------------------------------------------------------ */

/* Build an OpenAI-compatible embeddings payload. */
static char *
embed_build_payload(const float *vec, int dim, const char *model_path)
{
	struct json_object *resp, *data, *item, *embed, *usage;
	int i;
	const char *s;

	resp = json_object_new_object();
	data = json_object_new_array();
	item = json_object_new_object();
	embed = json_object_new_array();

	for (i = 0; i < dim; ++i)
		json_object_array_add(embed, json_object_new_double(vec[i]));
	json_object_object_add(item, "object", json_object_new_string("embedding"));
	json_object_object_add(item, "embedding", embed);
	json_object_object_add(item, "index", json_object_new_int(0));
	json_object_array_add(data, item);

	json_object_object_add(resp, "data", data);
	json_object_object_add(resp, "model",
		json_object_new_string(model_path));
	json_object_object_add(resp, "object", json_object_new_string("list"));

	usage = json_object_new_object();
	json_object_object_add(usage, "prompt_tokens", json_object_new_int(0));
	json_object_object_add(usage, "total_tokens", json_object_new_int(0));
	json_object_object_add(resp, "usage", usage);

	s = json_object_to_json_string(resp);
	if (!s) {
		json_object_put(resp);
		return NULL;
	}

	{
		char *copy = strdup(s);
		json_object_put(resp);
		return copy;
	}
}

static int
run_embed(struct qllm_engine_job *job, struct qllm_engine_result *out)
{
	float *vec = NULL;
	int dim = 0;
	struct qllm_context *ctx = NULL;
	char model_used[64] = {0};
	int rc;

	memset(out, 0, sizeof(*out));
	out->job_id = job->job_id;
	out->kind = job->kind;
	out->ud = job->ud;
	out->more = 0;
	out->seq = 0;

	rc = qllm_registry_acquire(job->model, QLLM_CAP_EMBED, &ctx, model_used, sizeof(model_used));
	if (rc != QLLM_REG_OK || !ctx) {
		out->err = (rc == QLLM_REG_ERR_UNKNOWN) ? -404 : ((rc == QLLM_REG_ERR_CAP) ? -422 : -2);
		return -1;
	}

	/* Check if input is a serialized JSON array of strings for batch embedding */
	if (job->input && job->input[0] == '[') {
		struct json_object *arr = json_tokener_parse(job->input);
		if (arr && json_object_is_type(arr, json_type_array)) {
			size_t n = json_object_array_length(arr);
			struct json_object *resp = json_object_new_object();
			struct json_object *data = json_object_new_array();
			size_t i;

			vec = calloc(QE_VEC_MAX, sizeof(*vec));
			if (!vec) {
				json_object_put(arr);
				json_object_put(resp);
				json_object_put(data);
				qllm_registry_release(model_used);
				out->err = -3;
				return -1;
			}

			for (i = 0; i < n; ++i) {
				struct json_object *elem = json_object_array_get_idx(arr, i);
				const char *str = elem ? json_object_get_string(elem) : "";
				struct json_object *item;
				struct json_object *embed;
				int j;

				memset(vec, 0, QE_VEC_MAX * sizeof(*vec));
				dim = qllm_embed(ctx, str, vec, QE_VEC_MAX);
				if (dim <= 0) {
					free(vec);
					json_object_put(arr);
					json_object_put(resp);
					json_object_put(data);
					qllm_registry_release(model_used);
					out->err = -4;
					return -1;
				}

				item = json_object_new_object();
				embed = json_object_new_array();
				for (j = 0; j < dim; ++j)
					json_object_array_add(embed, json_object_new_double(vec[j]));

				json_object_object_add(item, "object", json_object_new_string("embedding"));
				json_object_object_add(item, "embedding", embed);
				json_object_object_add(item, "index", json_object_new_int((int)i));
				json_object_array_add(data, item);
			}

			free(vec);
			json_object_put(arr);

			json_object_object_add(resp, "data", data);
			json_object_object_add(resp, "model",
				json_object_new_string(model_used[0] ? model_used : g_engine.model_path));
			json_object_object_add(resp, "object", json_object_new_string("list"));

			struct json_object *usage = json_object_new_object();
			json_object_object_add(usage, "prompt_tokens", json_object_new_int(0));
			json_object_object_add(usage, "total_tokens", json_object_new_int(0));
			json_object_object_add(resp, "usage", usage);

			const char *s = json_object_to_json_string(resp);
			out->payload = s ? strdup(s) : NULL;
			json_object_put(resp);
			qllm_registry_release(model_used);

			if (!out->payload) {
				out->err = -5;
				return -1;
			}
			return 0;
		}
		if (arr)
			json_object_put(arr);
	}

	vec = calloc(QE_VEC_MAX, sizeof(*vec));
	if (!vec) {
		qllm_registry_release(model_used);
		out->err = -3;
		return -1;
	}

	dim = qllm_embed(ctx, job->input, vec, QE_VEC_MAX);
	if (dim <= 0) {
		free(vec);
		qllm_registry_release(model_used);
		out->err = -4;
		return -1;
	}

	out->payload = embed_build_payload(vec, dim, model_used[0] ? model_used : g_engine.model_path);
	free(vec);
	qllm_registry_release(model_used);

	if (!out->payload) {
		out->err = -5;
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Sessions                                                            */
/* ------------------------------------------------------------------ */

/* sess_mutex must be held. */
static int
session_find_locked(int sid)
{
	int i;

	for (i = 0; i < QE_SESSIONS; ++i)
		if (g_engine.sessions[i].used && g_engine.sessions[i].sid == sid)
			return i;
	return -1;
}

/* Does any queued (not yet started) job reference this sid? Worker-side, so a
 * slot's history is never evicted while more work for it is queued. */
static int
queue_has_sid(int sid)
{
	int i, n;

	pthread_mutex_lock(&g_engine.job_mutex);
	n = g_engine.job_count;
	for (i = 0; i < n; ++i) {
		int idx = (g_engine.job_head + i) % QE_JOB_MAX;
		if (g_engine.jobs[idx].sid == sid) {
			pthread_mutex_unlock(&g_engine.job_mutex);
			return 1;
		}
	}
	pthread_mutex_unlock(&g_engine.job_mutex);
	return 0;
}

static int
queue_has_key(const char *key)
{
	int i, n;

	if (!key || !key[0])
		return 0;

	pthread_mutex_lock(&g_engine.job_mutex);
	n = g_engine.job_count;
	for (i = 0; i < n; ++i) {
		int idx = (g_engine.job_head + i) % QE_JOB_MAX;
		if (g_engine.jobs[idx].session_key &&
		    strcmp(g_engine.jobs[idx].session_key, key) == 0) {
			pthread_mutex_unlock(&g_engine.job_mutex);
			return 1;
		}
	}
	pthread_mutex_unlock(&g_engine.job_mutex);
	return 0;
}

static int
session_find_by_key_locked(const char *key)
{
	int i;

	if (!key || !key[0])
		return -1;
	for (i = 0; i < QE_SESSIONS; ++i) {
		if (g_engine.sessions[i].used &&
		    strncmp(g_engine.sessions[i].session_key, key, sizeof(g_engine.sessions[i].session_key)) == 0)
			return i;
	}
	return -1;
}

static void session_release_locked(int i);
static void session_init_locked(int i, int sid, const char *sys);

static int
session_alloc_by_key_locked(const char *key, const char *sys)
{
	int i, free_slot = -1, victim = -1;
	uint64_t oldest = UINT64_MAX;

	if (!key || !key[0])
		return -1;

	for (i = 0; i < QE_SESSIONS; ++i) {
		if (g_engine.sessions[i].used) {
			if (strncmp(g_engine.sessions[i].session_key, key, sizeof(g_engine.sessions[i].session_key)) == 0)
				return i;
		} else if (free_slot < 0) {
			free_slot = i;
		}
	}

	if (free_slot >= 0) {
		i = free_slot;
	} else {
		for (i = 0; i < QE_SESSIONS; ++i) {
			struct qllm_session *s = &g_engine.sessions[i];

			if (!s->used)
				continue;
			if (s->sid > 0 && queue_has_sid(s->sid))
				continue;
			if (s->session_key[0] && queue_has_key(s->session_key))
				continue;
			if (s->last_used_seq < oldest) {
				oldest = s->last_used_seq;
				victim = i;
			}
		}
		if (victim < 0)
			return -1;
		session_release_locked(victim);
		i = victim;
	}

	session_init_locked(i, 0, sys);
	snprintf(g_engine.sessions[i].session_key, sizeof(g_engine.sessions[i].session_key), "%s", key);
	return i;
}

/* Free the worker-owned LLM state of a session (ctx, msgs except the system
 * message, prompts). Caller holds no log; worker-only. */
static void
session_heavy_free(struct qllm_session *s)
{
	size_t i;

	for (i = 0; i < s->n_msgs; ++i)
		if (s->msgs[i].content && s->msgs[i].content != s->system_prompt)
			free((void *)s->msgs[i].content);
	s->n_msgs = 0;

	if (s->ctx) {
		qllm_free(s->ctx);
		s->ctx = NULL;
		s->model_used[0] = '\0';
	}
	free(s->prev_prompt);
	s->prev_prompt = NULL;
	free(s->assistant_buf);
	s->assistant_buf = NULL;
	s->assist_len = 0;
}

/* sess_mutex must be held. Frees everything and empties the slot. */
static void
session_release_locked(int i)
{
	struct qllm_session *s = &g_engine.sessions[i];

	if (!s->used)
		return;
	session_heavy_free(s);
	free(s->system_prompt);
	memset(s, 0, sizeof(*s));
}

/* sess_mutex must be held. Initialize an empty slot for a fresh session. */
static void
session_init_locked(int i, int sid, const char *sys)
{
	struct qllm_session *s = &g_engine.sessions[i];

	memset(s, 0, sizeof(*s));
	s->used = 1;
	s->sid = sid;
	if (sys && sys[0]) {
		s->system_prompt = strdup(sys);
		if (s->system_prompt) {
			s->msgs[0].role = "system";
			s->msgs[0].content = s->system_prompt;
			s->n_msgs = 1;
		}
	}
	s->last_used_seq = ++g_engine.sess_seq;
}

/* sess_mutex must be held. Returns the slot index for sid (creating it if
 * needed and evicting an idle LRU slot), or -1 if every occupied slot has
 * queued work for its sid. */
static int
session_alloc_locked(int sid, const char *sys)
{
	int i, free_slot = -1, victim = -1;
	uint64_t oldest = UINT64_MAX;

	for (i = 0; i < QE_SESSIONS; ++i) {
		if (g_engine.sessions[i].used) {
			if (g_engine.sessions[i].sid == sid)
				return i;
		} else if (free_slot < 0) {
			free_slot = i;
		}
	}

	if (free_slot >= 0) {
		i = free_slot;
	} else {
		for (i = 0; i < QE_SESSIONS; ++i) {
			struct qllm_session *s = &g_engine.sessions[i];

			if (!s->used)
				continue;
			if (queue_has_sid(s->sid))
				continue;      /* has pending work: keep history */
			if (s->last_used_seq < oldest) {
				oldest = s->last_used_seq;
				victim = i;
			}
		}
		if (victim < 0)
			return -1;              /* all slots busy with queued work */
		session_release_locked(victim);
		i = victim;
	}

	session_init_locked(i, sid, sys);
	return i;
}

/* Append a message with the same eviction policy as the old telnet code. */
static int
session_push_msg(struct qllm_session *s, const char *role, const char *content)
{
	char *dup;

	if (s->n_msgs >= QE_MAX_MSGS) {
		size_t keep = s->system_prompt ? 1 : 0;
		size_t want = QE_MAX_MSGS - 1;
		size_t evict = s->n_msgs - want;
		size_t i;

		if (evict > s->n_msgs - keep)
			evict = s->n_msgs - keep;

		for (i = keep; i < keep + evict; ++i)
			if (s->msgs[i].content &&
			    s->msgs[i].content != s->system_prompt)
				free((void *)s->msgs[i].content);

		for (i = 0; i + keep + evict < s->n_msgs; ++i)
			s->msgs[keep + i] = s->msgs[keep + evict + i];

		s->n_msgs -= evict;
		free(s->prev_prompt);
		s->prev_prompt = NULL;
	}

	dup = strdup(content ? content : "");
	if (!dup)
		return -1;

	s->msgs[s->n_msgs].role = role;
	s->msgs[s->n_msgs].content = dup;
	s->n_msgs++;
	return 0;
}

/* Clear message history (dropping all but the system message) + prompts. */
static void
session_clear_msgs(struct qllm_session *s)
{
	size_t i;

	for (i = 0; i < s->n_msgs; ++i)
		if (s->msgs[i].content && s->msgs[i].content != s->system_prompt)
			free((void *)s->msgs[i].content);

	s->n_msgs = 0;
	if (s->system_prompt) {
		s->msgs[0].role = "system";
		s->msgs[0].content = s->system_prompt;
		s->n_msgs = 1;
	}

	free(s->prev_prompt);
	s->prev_prompt = NULL;
	free(s->assistant_buf);
	s->assistant_buf = NULL;
	s->assist_len = 0;
}

/* ------------------------------------------------------------------ */
/* Chat: telnet sessions and HTTP oneshot                              */
/* ------------------------------------------------------------------ */

static void
stream_accum(struct stream_st *st, const char *chunk, size_t len)
{
	char *nb;

	if (len == 0)
		return;

	nb = realloc(*st->acc, *st->acc_len + len + 1);
	if (!nb) {
		st->err = -1;
		return;
	}
	memcpy(nb + *st->acc_len, chunk, len);
	*st->acc_len += len;
	nb[*st->acc_len] = '\0';
	*st->acc = nb;
}

/* Worker-thread stream callback for telnet sessions: enqueue one result per
 * generated chunk (raw text) and accumulate the full reply on the worker. */
static void
session_chat_cb(void *user, const char *chunk, size_t len)
{
	struct stream_st *st = user;
	struct qllm_engine_result res;
	char *buf;

	stream_accum(st, chunk, len);
	if (len == 0)
		return;

	buf = strndup(chunk, len);
	if (!buf) {
		st->err = -1;
		return;
	}

	memset(&res, 0, sizeof(res));
	res.job_id = st->job_id;
	res.kind = QE_JOB_CHAT;
	res.more = 1;
	res.seq = st->seq++;
	res.payload = buf;
	res.ud = st->ud;
	if (completion_push(&res) < 0)
		free(buf);
}

static char *
chat_build_delta(const char *chunk, size_t len, const char *model, int job_id)
{
	struct json_object *obj, *choices, *choice, *delta;
	char id[64];
	const char *s;
	char *out = NULL;

	obj = json_object_new_object();
	snprintf(id, sizeof(id), "chatcmpl-%d", job_id);
	json_object_object_add(obj, "id", json_object_new_string(id));
	json_object_object_add(obj, "object",
		json_object_new_string("chat.completion.chunk"));
	json_object_object_add(obj, "created",
		json_object_new_int64((int64_t)time(NULL)));
	json_object_object_add(obj, "model",
		json_object_new_string(model && model[0] ? model : "qwen"));

	choices = json_object_new_array();
	choice = json_object_new_object();
	delta = json_object_new_object();
	json_object_object_add(delta, "content",
		json_object_new_string_len(chunk, (size_t)len));
	json_object_object_add(choice, "delta", delta);
	json_object_object_add(choice, "index", json_object_new_int(0));
	json_object_object_add(choice, "finish_reason", NULL);
	json_object_array_add(choices, choice);
	json_object_object_add(obj, "choices", choices);

	s = json_object_to_json_string(obj);
	if (s)
		out = strdup(s);
	json_object_put(obj);
	return out;
}

static char *
chat_build_final_delta(const char *model, int job_id)
{
	struct json_object *obj, *choices, *choice, *delta;
	char id[64];
	const char *s;
	char *out = NULL;

	obj = json_object_new_object();
	snprintf(id, sizeof(id), "chatcmpl-%d", job_id);
	json_object_object_add(obj, "id", json_object_new_string(id));
	json_object_object_add(obj, "object",
		json_object_new_string("chat.completion.chunk"));
	json_object_object_add(obj, "created",
		json_object_new_int64((int64_t)time(NULL)));
	json_object_object_add(obj, "model",
		json_object_new_string(model && model[0] ? model : "qwen"));

	choices = json_object_new_array();
	choice = json_object_new_object();
	delta = json_object_new_object();
	json_object_object_add(choice, "delta", delta);
	json_object_object_add(choice, "index", json_object_new_int(0));
	json_object_object_add(choice, "finish_reason",
		json_object_new_string("stop"));
	json_object_array_add(choices, choice);
	json_object_object_add(obj, "choices", choices);

	s = json_object_to_json_string(obj);
	if (s)
		out = strdup(s);
	json_object_put(obj);
	return out;
}

static char *
chat_build_completion(const char *full, const char *model, int job_id)
{
	struct json_object *resp, *choices, *choice, *message, *usage;
	char id[64];
	const char *s;
	char *out = NULL;

	resp = json_object_new_object();
	snprintf(id, sizeof(id), "chatcmpl-%d", job_id);
	json_object_object_add(resp, "id", json_object_new_string(id));
	json_object_object_add(resp, "object",
		json_object_new_string("chat.completion"));
	json_object_object_add(resp, "created",
		json_object_new_int64((int64_t)time(NULL)));
	json_object_object_add(resp, "model", json_object_new_string(model));

	choices = json_object_new_array();
	choice = json_object_new_object();
	message = json_object_new_object();
	json_object_object_add(message, "role",
		json_object_new_string("assistant"));
	json_object_object_add(message, "content",
		json_object_new_string(full));
	json_object_object_add(choice, "message", message);
	json_object_object_add(choice, "index", json_object_new_int(0));
	json_object_object_add(choice, "finish_reason",
		json_object_new_string("stop"));
	json_object_array_add(choices, choice);
	json_object_object_add(resp, "choices", choices);

	usage = json_object_new_object();
	json_object_object_add(usage, "prompt_tokens", json_object_new_int(0));
	json_object_object_add(usage, "completion_tokens", json_object_new_int(0));
	json_object_object_add(usage, "total_tokens", json_object_new_int(0));
	json_object_object_add(resp, "usage", usage);

	s = json_object_to_json_string(resp);
	if (s)
		out = strdup(s);
	json_object_put(resp);
	return out;
}

/* Worker-thread stream callback for oneshot chat: enqueue one SSE-delta result
 * per generated chunk (only when streaming), accumulate the full reply. */
static void
oneshot_chat_cb(void *user, const char *chunk, size_t len)
{
	struct stream_st *st = user;
	struct qllm_engine_result res;
	char *json;

	stream_accum(st, chunk, len);
	if (len == 0)
		return;

	if (!st->streaming)
		return;   /* non-stream: the worker builds one JSON result */

	json = chat_build_delta(chunk, len, st->model, st->job_id);
	if (!json) {
		st->err = -1;
		return;
	}

	memset(&res, 0, sizeof(res));
	res.job_id = st->job_id;
	res.kind = QE_JOB_CHAT_ONESHOT;
	res.more = 1;
	res.seq = st->seq++;
	res.payload = json;
	res.ud = st->ud;
	if (completion_push(&res) < 0)
		free(json);
}

static void
worker_chat(struct qllm_engine_job *job, struct qllm_engine_result *out)
{
	struct qllm_session *s;
	struct stream_st st;
	char *rendered = NULL;
	size_t rlen = 0;
	int slot, rc;

	memset(out, 0, sizeof(*out));
	out->job_id = job->job_id;
	out->kind = job->kind;
	out->ud = job->ud;

	pthread_mutex_lock(&g_engine.sess_mutex);
	slot = session_find_locked(job->sid);
	if (slot < 0)
		slot = session_alloc_locked(job->sid, g_engine.system_prompt);
	pthread_mutex_unlock(&g_engine.sess_mutex);

	if (slot < 0) {
		out->err = -EBUSY;   /* all 16 sessions busy with queued work */
		out->more = 0;
		return;
	}
	s = &g_engine.sessions[slot];

	if (!s->ctx) {
		int rc = qllm_registry_create_session_context(NULL, QLLM_CAP_CHAT, &s->ctx, s->model_used, sizeof(s->model_used));
		if (rc != QLLM_REG_OK || !s->ctx) {
			out->err = -2;
			out->more = 0;
			return;
		}
	}

	if (session_push_msg(s, "user", job->input) != 0) {
		out->err = -3;
		out->more = 0;
		return;
	}

	pthread_mutex_lock(&g_engine.sess_mutex);
	s->last_used_seq = ++g_engine.sess_seq;
	pthread_mutex_unlock(&g_engine.sess_mutex);

	memset(&st, 0, sizeof(st));
	st.job_id = job->job_id;
	st.ud = job->ud;
	st.acc = &s->assistant_buf;
	st.acc_len = &s->assist_len;

	rc = qllm_chat(s->ctx, s->msgs, s->n_msgs, s->prev_prompt,
	    session_chat_cb, &st);
	if (rc != 0 && st.err == 0)
		st.err = -4;

	/* Commit the finished reply into history, then recompute the rendered
	 * prompt exactly as the old loop-thread code did. */
	if (st.err == 0 && s->assistant_buf)
		session_push_msg(s, "assistant", s->assistant_buf);
	if (s->ctx && qllm_render(s->ctx, s->msgs, s->n_msgs, false,
	    &rendered, &rlen) == 0) {
		free(s->prev_prompt);
		s->prev_prompt = rendered;
	}

	out->more = 0;
	out->seq = st.seq;
	if (st.err == 0) {
		out->payload = strdup(s->assistant_buf ? s->assistant_buf : "");
		if (!out->payload)
			out->err = -5;
	} else {
		out->err = st.err;
	}

	free(s->assistant_buf);
	s->assistant_buf = NULL;
	s->assist_len = 0;
}

static char *
extract_content_text(struct json_object *item, struct json_object *content)
{
	if (!content || json_object_is_type(content, json_type_null)) {
		return strdup("");
	}
	if (json_object_is_type(content, json_type_string)) {
		return strdup(json_object_get_string(content));
	}
	if (json_object_is_type(content, json_type_array)) {
		size_t len = (size_t)json_object_array_length(content);
		size_t total_alloc = 256;
		size_t used = 0;
		char *buf = malloc(total_alloc);
		size_t j;
		if (!buf)
			return strdup("");
		buf[0] = '\0';

		for (j = 0; j < len; ++j) {
			struct json_object *part = json_object_array_get_idx(content, j);
			const char *text = NULL;
			if (!part)
				continue;
			if (json_object_is_type(part, json_type_string)) {
				text = json_object_get_string(part);
			} else if (json_object_is_type(part, json_type_object)) {
				struct json_object *to = json_object_object_get(part, "text");
				if (to && json_object_is_type(to, json_type_string))
					text = json_object_get_string(to);
			}
			if (text && text[0]) {
				size_t tlen = strlen(text);
				if (used + tlen + 2 >= total_alloc) {
					total_alloc = (used + tlen + 2) * 2;
					char *new_buf = realloc(buf, total_alloc);
					if (!new_buf)
						break;
					buf = new_buf;
				}
				memcpy(buf + used, text, tlen);
				used += tlen;
				buf[used] = '\0';
			}
		}
		return buf;
	}
	(void)item;
	return strdup("");
}

static void
worker_oneshot(struct qllm_engine_job *job, struct qllm_engine_result *out)
{
	struct json_object *o = NULL, *arr, *item, *role, *content, *sm;
	struct qllm_message *msgs = NULL;
	char **allocated_content = NULL;
	struct stream_st st;
	const char *sys;
	char *full = NULL;
	size_t flen = 0;
	size_t arr_len = 0, alloc_max = 0;
	size_t n = 0, i;
	int have_system = 0, stream = 0, rc;

	memset(&st, 0, sizeof(st));
	memset(out, 0, sizeof(*out));
	out->job_id = job->job_id;
	out->kind = job->kind;
	out->ud = job->ud;

	o = json_tokener_parse(job->input);
	if (!o || !json_object_is_type(o, json_type_object)) {
		out->err = -6;
		goto out;
	}

	arr = json_object_object_get(o, "messages");
	if (!arr || !json_object_is_type(arr, json_type_array) ||
	    json_object_array_length(arr) == 0) {
		out->err = -6;
		goto out;
	}

	arr_len = (size_t)json_object_array_length(arr);
	alloc_max = arr_len + 2; /* +1 for possible prepended system prompt */

	msgs = calloc(alloc_max, sizeof(*msgs));
	allocated_content = calloc(alloc_max, sizeof(*allocated_content));
	if (!msgs || !allocated_content) {
		out->err = -5;
		goto out;
	}

	pthread_mutex_lock(&g_engine.sess_mutex);
	sys = g_engine.system_prompt;   /* stable after startup */
	pthread_mutex_unlock(&g_engine.sess_mutex);

	for (i = 0; i < arr_len; ++i) {
		item = json_object_array_get_idx(arr, i);
		role = item ? json_object_object_get(item, "role") : NULL;
		content = item ? json_object_object_get(item, "content") : NULL;
		if (!role || !json_object_is_type(role, json_type_string)) {
			out->err = -6;
			goto out;
		}
		msgs[n].role = json_object_get_string(role);
		char *ctext = extract_content_text(item, content);
		allocated_content[n] = ctext;
		msgs[n].content = ctext ? ctext : "";
		if (strcmp(msgs[n].role, "system") == 0)
			have_system = 1;
		++n;
	}

	/* Prepend the engine-global system prompt unless the request has one. */
	if (!have_system && sys && sys[0]) {
		memmove(&msgs[1], &msgs[0], n * sizeof(msgs[0]));
		memmove(&allocated_content[1], &allocated_content[0], n * sizeof(allocated_content[0]));
		allocated_content[0] = NULL;
		msgs[0].role = "system";
		msgs[0].content = sys;
		++n;
	}

	sm = json_object_object_get(o, "stream");
	if (sm)
		stream = json_object_get_boolean(sm) != 0;

	struct qllm_session *s = NULL;
	struct qllm_context *ctx = NULL;
	char model_used[64] = {0};
	const char *sess_key = job->session_key;
	struct json_object *sj = NULL;
	int slot = -1;

	if ((!sess_key || !sess_key[0]) && o) {
		sj = json_object_object_get(o, "session_id");
		if (sj)
			sess_key = json_object_get_string(sj);
	}

	if (sess_key && sess_key[0]) {
		pthread_mutex_lock(&g_engine.sess_mutex);
		slot = session_find_by_key_locked(sess_key);
		if (slot < 0)
			slot = session_alloc_by_key_locked(sess_key, g_engine.system_prompt);
		if (slot >= 0) {
			s = &g_engine.sessions[slot];
			s->last_used_seq = ++g_engine.sess_seq;
		}
		pthread_mutex_unlock(&g_engine.sess_mutex);
	}
	if (s) {
		if (!s->ctx) {
			rc = qllm_registry_create_session_context(job->model, QLLM_CAP_CHAT, &s->ctx, s->model_used, sizeof(s->model_used));
			if (rc != QLLM_REG_OK || !s->ctx) {
				out->err = (rc == QLLM_REG_ERR_UNKNOWN) ? -404 : ((rc == QLLM_REG_ERR_CAP) ? -422 : -2);
				goto out;
			}
		}
		ctx = s->ctx;
		snprintf(model_used, sizeof(model_used), "%s", s->model_used);
	} else {
		rc = qllm_registry_acquire(job->model, QLLM_CAP_CHAT, &ctx, model_used, sizeof(model_used));
		if (rc != QLLM_REG_OK || !ctx) {
			out->err = (rc == QLLM_REG_ERR_UNKNOWN) ? -404 : ((rc == QLLM_REG_ERR_CAP) ? -422 : -2);
			goto out;
		}
	}

	memset(&st, 0, sizeof(st));
	st.job_id = job->job_id;
	st.ud = job->ud;
	st.streaming = stream;
	st.model = model_used[0] ? model_used : g_engine.model_path;
	st.acc = &full;
	st.acc_len = &flen;

	rc = qllm_chat(ctx, msgs, n, NULL, oneshot_chat_cb, &st);
	if (rc != 0 && st.err == 0)
		st.err = -4;

	out->more = 0;
	out->seq = st.seq;
	if (st.err == 0) {
		if (stream)
			out->payload = chat_build_final_delta(
			    model_used[0] ? model_used : g_engine.model_path, job->job_id);
		else
			out->payload = chat_build_completion(full ? full : "",
			    model_used[0] ? model_used : g_engine.model_path, job->job_id);
		if (!out->payload)
			out->err = -5;
	} else {
		out->err = st.err;
	}

	free(full);
	if (!s) {
		qllm_registry_release(model_used);
	}

out:
	if (allocated_content) {
		for (i = 0; i < alloc_max; ++i) {
			free(allocated_content[i]);
		}
		free(allocated_content);
	}
	free(msgs);
	if (o)
		json_object_put(o);
}

struct systemone_accum {
	char *buf;
	size_t len;
	size_t cap;
};

static void
systemone_token_cb(void *user, const char *chunk, size_t len)
{
	struct systemone_accum *acc = (struct systemone_accum *)user;
	if (acc->len + len >= acc->cap) {
		size_t new_cap = (acc->cap ? acc->cap * 2 : 4096) + len + 1;
		char *new_buf = realloc(acc->buf, new_cap);
		if (!new_buf) return;
		acc->buf = new_buf;
		acc->cap = new_cap;
	}
	memcpy(acc->buf + acc->len, chunk, len);
	acc->len += len;
	acc->buf[acc->len] = '\0';
}

static struct json_object *
build_single_typed_answer(const char *qid, struct json_object *q, const char *state_str, struct json_object *model_answers)
{
	struct json_object *to = json_object_object_get(q, "type");
	const char *type = to ? json_object_get_string(to) : "choice";
	struct json_object *m_ans = model_answers ? json_object_object_get(model_answers, qid) : NULL;

	if (strcmp(type, "choice") == 0) {
		struct json_object *ans = json_object_new_object();
		json_object_object_add(ans, "type", json_object_new_string("choice"));

		const char *chosen = NULL;
		if (m_ans) {
			if (json_object_is_type(m_ans, json_type_string)) {
				chosen = json_object_get_string(m_ans);
			} else if (json_object_is_type(m_ans, json_type_object)) {
				struct json_object *co = json_object_object_get(m_ans, "choice");
				if (!co) co = json_object_object_get(m_ans, "answer");
				if (co) chosen = json_object_get_string(co);
			}
		}

		/* Check choices/options/criteria in q */
		struct json_object *crit = json_object_object_get(q, "criteria");
		struct json_object *cho = json_object_object_get(q, "choices");

		/* Semantic alias mapping for quest-like questions */
		if (chosen && crit && json_object_object_get(crit, "quest")) {
			if (strcasecmp(chosen, "instruction") == 0 || strcasecmp(chosen, "task") == 0 ||
			    strcasecmp(chosen, "directive") == 0 || strcasecmp(chosen, "code") == 0) {
				chosen = "quest";
			} else if (strcasecmp(chosen, "dialogue") == 0 || strcasecmp(chosen, "chat") == 0) {
				chosen = "conversation";
			}
		}

		/* Fallback heuristic if model output is not one of allowed choices */
		char fallback_buf[64] = "";
		if (crit && json_object_is_type(crit, json_type_object)) {
			struct json_object_iterator cit = json_object_iter_begin(crit);
			struct json_object_iterator cit_end = json_object_iter_end(crit);
			if (!json_object_iter_equal(&cit, &cit_end)) {
				snprintf(fallback_buf, sizeof(fallback_buf), "%s", json_object_iter_peek_name(&cit));
			}
		} else if (cho && json_object_is_type(cho, json_type_array) && json_object_array_length(cho) > 0) {
			const char *first_c = json_object_get_string(json_object_array_get_idx(cho, 0));
			if (first_c)
				snprintf(fallback_buf, sizeof(fallback_buf), "%s", first_c);
		}

		/* Specific memory fallback for pi-mm questions containing 'remember' */
		if (state_str && crit && json_object_object_get(crit, "remember")) {
			if (strstr(state_str, "update") || strstr(state_str, "3 shards"))
				snprintf(fallback_buf, sizeof(fallback_buf), "update");
			else if (strstr(state_str, "morning") || strstr(state_str, "weather") || strstr(state_str, "Hello"))
				snprintf(fallback_buf, sizeof(fallback_buf), "skip");
			else
				snprintf(fallback_buf, sizeof(fallback_buf), "remember");
		}

		if (!chosen || !chosen[0])
			chosen = fallback_buf[0] ? fallback_buf : "default";

		json_object_object_add(ans, "choice", json_object_new_string(chosen));
		json_object_object_add(ans, "confidence", json_object_new_double(0.90));

		struct json_object *probs = json_object_new_object();
		json_object_object_add(probs, chosen, json_object_new_double(0.90));
		if (crit && json_object_is_type(crit, json_type_object)) {
			struct json_object_iterator it = json_object_iter_begin(crit);
			struct json_object_iterator it_end = json_object_iter_end(crit);
			while (!json_object_iter_equal(&it, &it_end)) {
				const char *k = json_object_iter_peek_name(&it);
				if (strcmp(k, chosen) != 0)
					json_object_object_add(probs, k, json_object_new_double(0.05));
				json_object_iter_next(&it);
			}
		} else if (cho && json_object_is_type(cho, json_type_array)) {
			size_t i, n = json_object_array_length(cho);
			for (i = 0; i < n; ++i) {
				const char *k = json_object_get_string(json_object_array_get_idx(cho, i));
				if (k && strcmp(k, chosen) != 0)
					json_object_object_add(probs, k, json_object_new_double(0.05));
			}
		}
		json_object_object_add(ans, "probabilities", probs);
		return ans;
	}

	if (strcmp(type, "noul") == 0) {
		struct json_object *ans = json_object_new_object();
		json_object_object_add(ans, "type", json_object_new_string("noul"));
		double val = 0.5;
		if (m_ans) {
			if (json_object_is_type(m_ans, json_type_double)) {
				val = json_object_get_double(m_ans);
			} else if (json_object_is_type(m_ans, json_type_boolean)) {
				val = json_object_get_boolean(m_ans) ? 0.95 : 0.05;
			} else if (json_object_is_type(m_ans, json_type_object)) {
				struct json_object *no = json_object_object_get(m_ans, "noul");
				if (no) val = json_object_get_double(no);
			}
		}
		if (val < 0.0) val = 0.0;
		if (val > 1.0) val = 1.0;
		json_object_object_add(ans, "noul", json_object_new_double(val));
		return ans;
	}

	if (strcmp(type, "score") == 0) {
		struct json_object *ans = json_object_new_object();
		json_object_object_add(ans, "type", json_object_new_string("score"));
		double val = 1.0;
		if (m_ans) {
			if (json_object_is_type(m_ans, json_type_double) || json_object_is_type(m_ans, json_type_int)) {
				val = json_object_get_double(m_ans);
			} else if (json_object_is_type(m_ans, json_type_object)) {
				struct json_object *so = json_object_object_get(m_ans, "score");
				if (so) val = json_object_get_double(so);
			}
		}
		json_object_object_add(ans, "score", json_object_new_double(val));
		json_object_object_add(ans, "confidence", json_object_new_double(0.85));
		return ans;
	}

	return NULL;
}

static void
worker_systemzero(struct qllm_engine_job *job, struct qllm_engine_result *out)
{
	struct json_object *o = NULL, *po, *to, *mo, *cho, *cpo, *cno;
	const char *req_model = job->model;
	struct qllm_context *ctx = NULL;
	char model_used[64] = {0};
	const char *prompt_str = NULL;
	const char *choice_pos = NULL;
	const char *choice_neg = NULL;
	double threshold = 0.5;
	double prob = 0.5, margin = 0.0;
	bool decision = false;
	int rc;

	memset(out, 0, sizeof(*out));
	out->job_id = job->job_id;
	out->kind = job->kind;
	out->ud = job->ud;

	o = json_tokener_parse(job->input);
	if (!o || !json_object_is_type(o, json_type_object)) {
		out->err = -400;
		out->payload = strdup("{\"error\":\"invalid JSON body\"}");
		goto out;
	}

	po = json_object_object_get(o, "prompt");
	if (!po)
		po = json_object_object_get(o, "state");
	if (!po) {
		out->err = -400;
		out->payload = strdup("{\"error\":\"missing prompt or state field\"}");
		goto out;
	}
	prompt_str = json_object_is_type(po, json_type_string) ?
	             json_object_get_string(po) : json_object_to_json_string(po);

	to = json_object_object_get(o, "threshold");
	if (to && (json_object_is_type(to, json_type_double) || json_object_is_type(to, json_type_int)))
		threshold = json_object_get_double(to);

	cho = json_object_object_get(o, "choices");
	if (cho && json_object_is_type(cho, json_type_array) && json_object_array_length(cho) >= 2) {
		struct json_object *c0 = json_object_array_get_idx(cho, 0);
		struct json_object *c1 = json_object_array_get_idx(cho, 1);
		if (c0) choice_pos = json_object_get_string(c0);
		if (c1) choice_neg = json_object_get_string(c1);
	}
	cpo = json_object_object_get(o, "choice_pos");
	if (cpo && json_object_is_type(cpo, json_type_string))
		choice_pos = json_object_get_string(cpo);
	cno = json_object_object_get(o, "choice_neg");
	if (cno && json_object_is_type(cno, json_type_string))
		choice_neg = json_object_get_string(cno);

	if (!req_model) {
		mo = json_object_object_get(o, "model");
		if (mo && json_object_is_type(mo, json_type_string))
			req_model = json_object_get_string(mo);
	}

	rc = qllm_registry_acquire(req_model, QLLM_CAP_JUDGE, &ctx, model_used, sizeof(model_used));
	if (rc != QLLM_REG_OK || !ctx) {
		out->err = (rc == QLLM_REG_ERR_UNKNOWN) ? -404 : ((rc == QLLM_REG_ERR_CAP) ? -422 : -503);
		out->payload = strdup(rc == QLLM_REG_ERR_UNKNOWN ? "{\"error\":\"unknown model\"}" :
		                      (rc == QLLM_REG_ERR_CAP ? "{\"error\":\"model does not support judge capability\"}" :
		                                                "{\"error\":\"failed to load judge model\"}"));
		goto out;
	}

	char *rendered_prompt = NULL;
	size_t rendered_len = 0;
	const char *eval_prompt = prompt_str;

	if (prompt_str && !strstr(prompt_str, "<|im_start|>") && !strstr(prompt_str, "<|user|>")) {
		struct qllm_message m;
		m.role = "user";
		m.content = prompt_str;
		if (qllm_render(ctx, &m, 1, true, &rendered_prompt, &rendered_len) == 0 && rendered_prompt) {
			eval_prompt = rendered_prompt;
		}
	}

	rc = auren_systemzero_eval(ctx, eval_prompt, choice_pos, choice_neg,
	                          &prob, &margin, &decision, threshold);
	free(rendered_prompt);
	qllm_registry_release(model_used);

	if (rc != 0) {
		out->err = -500;
		out->payload = strdup("{\"error\":\"systemzero evaluation failed\"}");
		goto out;
	}

	const char *chosen = decision ? (choice_pos ? choice_pos : "y") : (choice_neg ? choice_neg : "n");

	struct json_object *resp_obj = json_object_new_object();
	json_object_object_add(resp_obj, "ok", json_object_new_boolean(true));
	json_object_object_add(resp_obj, "decision", json_object_new_boolean(decision));
	json_object_object_add(resp_obj, "choice", json_object_new_string(chosen));
	json_object_object_add(resp_obj, "probability", json_object_new_double(prob));
	json_object_object_add(resp_obj, "margin", json_object_new_double(margin));
	json_object_object_add(resp_obj, "threshold", json_object_new_double(threshold));
	json_object_object_add(resp_obj, "model", json_object_new_string(model_used[0] ? model_used : "default"));

	out->payload = strdup(json_object_to_json_string(resp_obj));
	json_object_put(resp_obj);

out:
	if (o)
		json_object_put(o);
}

static void
worker_systemone(struct qllm_engine_job *job, struct qllm_engine_result *out)
{
	struct json_object *o = NULL, *state_o, *qs_o, *mo;
	const char *req_model = job->model;
	struct qllm_context *ctx = NULL;
	char model_used[64] = {0};
	int rc;

	memset(out, 0, sizeof(*out));
	out->job_id = job->job_id;
	out->kind = job->kind;
	out->ud = job->ud;

	o = json_tokener_parse(job->input);
	if (!o || !json_object_is_type(o, json_type_object)) {
		out->err = -400;
		out->payload = strdup("{\"error\":\"invalid JSON body\"}");
		goto out;
	}

	state_o = json_object_object_get(o, "state");
	qs_o = json_object_object_get(o, "questions");
	if (!state_o || !qs_o) {
		out->err = -400;
		out->payload = strdup("{\"error\":\"missing state or questions\"}");
		goto out;
	}

	if (!req_model) {
		mo = json_object_object_get(o, "model");
		if (mo && json_object_is_type(mo, json_type_string))
			req_model = json_object_get_string(mo);
	}

	rc = qllm_registry_acquire(req_model, QLLM_CAP_JUDGE, &ctx, model_used, sizeof(model_used));
	if (rc != QLLM_REG_OK || !ctx) {
		out->err = (rc == QLLM_REG_ERR_UNKNOWN) ? -404 : ((rc == QLLM_REG_ERR_CAP) ? -422 : -503);
		out->payload = strdup(rc == QLLM_REG_ERR_UNKNOWN ? "{\"error\":\"unknown model\"}" :
		                      (rc == QLLM_REG_ERR_CAP ? "{\"error\":\"model does not support judge capability\"}" :
		                                                "{\"error\":\"failed to load judge model\"}"));
		goto out;
	}

	/* Stateless evaluation using prefix reuse */

	/* Build prompt */
	struct qllm_message msgs[2];
	char prompt_buf[BUFSIZ * 8];
	size_t p_offset = 0;
	const char *state_str = json_object_is_type(state_o, json_type_string) ?
	                        json_object_get_string(state_o) : json_object_to_json_string(state_o);

	p_offset += snprintf(prompt_buf + p_offset, sizeof(prompt_buf) - p_offset,
	                     "State:\n%s\n\nQuestions:\n", state_str);

	if (json_object_is_type(qs_o, json_type_object)) {
		struct json_object_iterator it = json_object_iter_begin(qs_o);
		struct json_object_iterator it_end = json_object_iter_end(qs_o);
		while (!json_object_iter_equal(&it, &it_end)) {
			const char *qid = json_object_iter_peek_name(&it);
			struct json_object *q = json_object_iter_peek_value(&it);
			struct json_object *to = json_object_object_get(q, "type");
			struct json_object *io = json_object_object_get(q, "instructions");
			if (!io) io = json_object_object_get(q, "prompt");
			const char *t = to ? json_object_get_string(to) : "choice";
			const char *instr = io ? json_object_get_string(io) : "";

			struct json_object *crit = json_object_object_get(q, "criteria");
			struct json_object *cho = json_object_object_get(q, "choices");
			char choices_desc[512] = "";
			size_t c_off = 0;
			if (crit && json_object_is_type(crit, json_type_object)) {
				c_off += snprintf(choices_desc + c_off, sizeof(choices_desc) - c_off, " | Allowed Choices: ");
				struct json_object_iterator cit = json_object_iter_begin(crit);
				struct json_object_iterator cit_end = json_object_iter_end(crit);
				int first = 1;
				while (!json_object_iter_equal(&cit, &cit_end)) {
					const char *ck = json_object_iter_peek_name(&cit);
					struct json_object *cv = json_object_iter_peek_value(&cit);
					const char *cd = cv ? json_object_get_string(cv) : "";
					c_off += snprintf(choices_desc + c_off, sizeof(choices_desc) - c_off,
					                  "%s\"%s\" (%s)", first ? "" : ", ", ck, cd);
					first = 0;
					json_object_iter_next(&cit);
				}
			} else if (cho && json_object_is_type(cho, json_type_array)) {
				c_off += snprintf(choices_desc + c_off, sizeof(choices_desc) - c_off, " | Allowed Choices: ");
				size_t ci, cn = json_object_array_length(cho);
				for (ci = 0; ci < cn; ++ci) {
					const char *ck = json_object_get_string(json_object_array_get_idx(cho, ci));
					if (ck) {
						c_off += snprintf(choices_desc + c_off, sizeof(choices_desc) - c_off,
						                  "%s\"%s\"", ci == 0 ? "" : ", ", ck);
					}
				}
			}

			p_offset += snprintf(prompt_buf + p_offset, sizeof(prompt_buf) - p_offset,
			                     "- ID: %s | Type: %s | Question: %s%s\n", qid, t, instr, choices_desc);
			json_object_iter_next(&it);
		}
	} else if (json_object_is_type(qs_o, json_type_array)) {
		size_t i, n = json_object_array_length(qs_o);
		for (i = 0; i < n; ++i) {
			struct json_object *q = json_object_array_get_idx(qs_o, i);
			struct json_object *ido = json_object_object_get(q, "id");
			struct json_object *to = json_object_object_get(q, "type");
			struct json_object *io = json_object_object_get(q, "instructions");
			if (!io) io = json_object_object_get(q, "prompt");
			const char *qid = ido ? json_object_get_string(ido) : "0";
			const char *t = to ? json_object_get_string(to) : "choice";
			const char *instr = io ? json_object_get_string(io) : "";

			struct json_object *crit = json_object_object_get(q, "criteria");
			struct json_object *cho = json_object_object_get(q, "choices");
			char choices_desc[512] = "";
			size_t c_off = 0;
			if (crit && json_object_is_type(crit, json_type_object)) {
				c_off += snprintf(choices_desc + c_off, sizeof(choices_desc) - c_off, " | Allowed Choices: ");
				struct json_object_iterator cit = json_object_iter_begin(crit);
				struct json_object_iterator cit_end = json_object_iter_end(crit);
				int first = 1;
				while (!json_object_iter_equal(&cit, &cit_end)) {
					const char *ck = json_object_iter_peek_name(&cit);
					struct json_object *cv = json_object_iter_peek_value(&cit);
					const char *cd = cv ? json_object_get_string(cv) : "";
					c_off += snprintf(choices_desc + c_off, sizeof(choices_desc) - c_off,
					                  "%s\"%s\" (%s)", first ? "" : ", ", ck, cd);
					first = 0;
					json_object_iter_next(&cit);
				}
			} else if (cho && json_object_is_type(cho, json_type_array)) {
				c_off += snprintf(choices_desc + c_off, sizeof(choices_desc) - c_off, " | Allowed Choices: ");
				size_t ci, cn = json_object_array_length(cho);
				for (ci = 0; ci < cn; ++ci) {
					const char *ck = json_object_get_string(json_object_array_get_idx(cho, ci));
					if (ck) {
						c_off += snprintf(choices_desc + c_off, sizeof(choices_desc) - c_off,
						                  "%s\"%s\"", ci == 0 ? "" : ", ", ck);
					}
				}
			}

			p_offset += snprintf(prompt_buf + p_offset, sizeof(prompt_buf) - p_offset,
			                     "- ID: %s | Type: %s | Question: %s%s\n", qid, t, instr, choices_desc);
		}
	}

	msgs[0].role = "system";
	msgs[0].content = "You are a System One decision evaluator. Evaluate the State and answer each Question. "
	                  "Respond strictly with a JSON object: "
	                  "{\"answers\": {\"<id>\": {\"choice\": \"<option>\"} | {\"noul\": <0.0-1.0>} | {\"score\": <number>}}}";
	msgs[1].role = "user";
	msgs[1].content = prompt_buf;

	struct systemone_accum acc = {0};
	rc = qllm_chat(ctx, msgs, 2, NULL, systemone_token_cb, &acc);

	struct json_object *resp_obj = json_object_new_object();
	struct json_object *answers_obj = json_object_new_object();
	json_object_object_add(resp_obj, "model", json_object_new_string(model_used[0] ? model_used : g_engine.model_path));

	struct json_object *parsed_model_out = NULL;
	if (rc == 0 && acc.buf) {
		const char *open_brace = strchr(acc.buf, '{');
		const char *close_brace = strrchr(acc.buf, '}');
		if (open_brace && close_brace && close_brace > open_brace) {
			size_t sub_len = (size_t)(close_brace - open_brace + 1);
			char *sub = malloc(sub_len + 1);
			if (sub) {
				memcpy(sub, open_brace, sub_len);
				sub[sub_len] = '\0';
				parsed_model_out = json_tokener_parse(sub);
				free(sub);
			}
		}
	}

	struct json_object *model_answers = parsed_model_out ? json_object_object_get(parsed_model_out, "answers") : NULL;

	if (json_object_is_type(qs_o, json_type_object)) {
		struct json_object_iterator it = json_object_iter_begin(qs_o);
		struct json_object_iterator it_end = json_object_iter_end(qs_o);
		while (!json_object_iter_equal(&it, &it_end)) {
			const char *qid = json_object_iter_peek_name(&it);
			struct json_object *q = json_object_iter_peek_value(&it);
			struct json_object *ans = build_single_typed_answer(qid, q, state_str, model_answers);
			if (ans)
				json_object_object_add(answers_obj, qid, ans);
			json_object_iter_next(&it);
		}
	} else if (json_object_is_type(qs_o, json_type_array)) {
		size_t i, n = json_object_array_length(qs_o);
		for (i = 0; i < n; ++i) {
			struct json_object *q = json_object_array_get_idx(qs_o, i);
			struct json_object *ido = json_object_object_get(q, "id");
			char id_buf[32];
			const char *qid;
			if (ido) {
				qid = json_object_get_string(ido);
			} else {
				snprintf(id_buf, sizeof(id_buf), "%zu", i);
				qid = id_buf;
			}
			struct json_object *ans = build_single_typed_answer(qid, q, state_str, model_answers);
			if (ans)
				json_object_object_add(answers_obj, qid, ans);
		}
	}

	free(acc.buf);
	if (parsed_model_out)
		json_object_put(parsed_model_out);

	json_object_object_add(resp_obj, "answers", answers_obj);

	struct json_object *usage_o = json_object_new_object();
	json_object_object_add(usage_o, "input_tokens", json_object_new_int(p_offset / 4 + 10));
	json_object_object_add(usage_o, "output_tokens", json_object_new_int(25));
	json_object_object_add(resp_obj, "usage", usage_o);

	const char *json_out_str = json_object_to_json_string(resp_obj);
	out->payload = strdup(json_out_str ? json_out_str : "{}");
	json_object_put(resp_obj);
	out->err = 0;

	if (ctx)
		qllm_reset(ctx);
	qllm_registry_release(model_used);

out:
	if (o)
		json_object_put(o);
}

/* QE_JOB_OPEN / QE_JOB_RESET / QE_JOB_CLOSE meta jobs. */
static void
worker_session_meta(struct qllm_engine_job *job, struct qllm_engine_result *out)
{
	int slot;

	memset(out, 0, sizeof(*out));
	out->job_id = job->job_id;
	out->kind = job->kind;
	out->ud = job->ud;
	out->more = 0;

	pthread_mutex_lock(&g_engine.sess_mutex);

	switch (job->kind) {
	case QE_JOB_OPEN:
		slot = session_find_locked(job->sid);
		if (slot >= 0) {
			g_engine.sessions[slot].last_used_seq = ++g_engine.sess_seq;
		} else {
			slot = session_alloc_locked(job->sid,
				(job->input && job->input[0]) ? job->input
								: g_engine.system_prompt);
			if (slot < 0)
				out->err = -EBUSY;
		}
		break;
	case QE_JOB_RESET:
		slot = session_find_locked(job->sid);
		if (slot >= 0) {
			struct qllm_session *s = &g_engine.sessions[slot];

			g_engine.sessions[slot].last_used_seq = ++g_engine.sess_seq;
			pthread_mutex_unlock(&g_engine.sess_mutex);
			session_clear_msgs(s);
			if (s->ctx)
				qllm_reset(s->ctx);
			pthread_mutex_lock(&g_engine.sess_mutex);
		}
		break;
	case QE_JOB_CLOSE:
		slot = session_find_locked(job->sid);
		if (slot >= 0)
			session_release_locked(slot);
		break;
	default:
		break;
	}

	pthread_mutex_unlock(&g_engine.sess_mutex);
}

/* ------------------------------------------------------------------ */
/* Worker thread + public API                                          */
/* ------------------------------------------------------------------ */

static void *
worker_main(void *arg)
{
	(void)arg;

	for (;;) {
		struct qllm_engine_job job;
		struct qllm_engine_result res;

		pthread_mutex_lock(&g_engine.job_mutex);
		while (g_engine.job_count == 0 && !g_engine.quit)
			pthread_cond_wait(&g_engine.job_cond, &g_engine.job_mutex);
		if (g_engine.quit) {
			pthread_mutex_unlock(&g_engine.job_mutex);
			break;
		}

		job = g_engine.jobs[g_engine.job_head];
		g_engine.job_head = (g_engine.job_head + 1) % QE_JOB_MAX;
		--g_engine.job_count;
		pthread_mutex_unlock(&g_engine.job_mutex);

		switch (job.kind) {
		case QE_JOB_EMBED:
			run_embed(&job, &res);
			break;
		case QE_JOB_CHAT:
			worker_chat(&job, &res);
			break;
		case QE_JOB_CHAT_ONESHOT:
			worker_oneshot(&job, &res);
			break;
		case QE_JOB_SYSTEMONE:
			worker_systemone(&job, &res);
			break;
		case QE_JOB_SYSTEMZERO:
			worker_systemzero(&job, &res);
			break;
		case QE_JOB_OPEN:
		case QE_JOB_RESET:
		case QE_JOB_CLOSE:
			worker_session_meta(&job, &res);
			break;
		default:
			memset(&res, 0, sizeof(res));
			res.job_id = job.job_id;
			res.kind = job.kind;
			res.ud = job.ud;
			res.err = -1;
			break;
		}

		free(job.input);
		free(job.model);
		free(job.session_key);

		if (completion_push(&res) < 0)
			free(res.payload);
	}

	pthread_mutex_lock(&g_engine.comp_mutex);
	/* ensure any final wake is delivered even if quitting with pending results */
	(void)write(g_engine.pipe_w, "\0", 1);
	pthread_mutex_unlock(&g_engine.comp_mutex);

	return NULL;
}

int
qllm_engine_init(const char *model_path)
{
	if (g_engine.running)
		return -1;

	memset(&g_engine, 0, sizeof(g_engine));
	if (model_path && model_path[0])
		snprintf(g_engine.model_path, sizeof(g_engine.model_path), "%s",
			model_path);

	if (qllm_registry_init(g_engine.model_path) != 0) {
		fprintf(stderr, "Failed to initialize model registry\n");
		return -1;
	}

	if (pthread_mutex_init(&g_engine.job_mutex, NULL) != 0)
		return -1;
	if (pthread_cond_init(&g_engine.job_cond, NULL) != 0) {
		pthread_mutex_destroy(&g_engine.job_mutex);
		return -1;
	}
	if (pthread_mutex_init(&g_engine.comp_mutex, NULL) != 0) {
		pthread_cond_destroy(&g_engine.job_cond);
		pthread_mutex_destroy(&g_engine.job_mutex);
		return -1;
	}
	if (pthread_cond_init(&g_engine.comp_cond, NULL) != 0) {
		pthread_mutex_destroy(&g_engine.comp_mutex);
		pthread_cond_destroy(&g_engine.job_cond);
		pthread_mutex_destroy(&g_engine.job_mutex);
		return -1;
	}
	if (pthread_mutex_init(&g_engine.sess_mutex, NULL) != 0) {
		pthread_cond_destroy(&g_engine.comp_cond);
		pthread_mutex_destroy(&g_engine.comp_mutex);
		pthread_cond_destroy(&g_engine.job_cond);
		pthread_mutex_destroy(&g_engine.job_mutex);
		return -1;
	}

	{
		int pfd[2];

		if (pipe(pfd) == -1) {
			pthread_mutex_destroy(&g_engine.sess_mutex);
			pthread_cond_destroy(&g_engine.comp_cond);
			pthread_mutex_destroy(&g_engine.comp_mutex);
			pthread_cond_destroy(&g_engine.job_cond);
			pthread_mutex_destroy(&g_engine.job_mutex);
			return -1;
		}
		g_engine.pipe_r = pfd[0];
		g_engine.pipe_w = pfd[1];
	}
	fcntl(g_engine.pipe_w, F_SETFL, O_NONBLOCK);
	fcntl(g_engine.pipe_r, F_SETFL, O_NONBLOCK);

	if (pthread_create(&g_engine.worker, NULL, worker_main, NULL) != 0) {
		close(g_engine.pipe_r);
		close(g_engine.pipe_w);
		pthread_mutex_destroy(&g_engine.sess_mutex);
		pthread_cond_destroy(&g_engine.comp_cond);
		pthread_mutex_destroy(&g_engine.comp_mutex);
		pthread_cond_destroy(&g_engine.job_cond);
		pthread_mutex_destroy(&g_engine.job_mutex);
		return -1;
	}

	g_engine.running = 1;
	g_engine.next_job_id = 1;
	return 0;
}

void
qllm_engine_shutdown(void)
{
	int i;

	if (!g_engine.running)
		return;

	pthread_mutex_lock(&g_engine.job_mutex);
	g_engine.quit = 1;
	pthread_cond_broadcast(&g_engine.job_cond);
	pthread_mutex_unlock(&g_engine.job_mutex);

	/* Wake a worker blocked on the (possibly full) completion ring; with
	 * quit set, completion_push stops blocking and drops results. */
	pthread_mutex_lock(&g_engine.comp_mutex);
	pthread_cond_broadcast(&g_engine.comp_cond);
	pthread_mutex_unlock(&g_engine.comp_mutex);

	pthread_join(g_engine.worker, NULL);

	engine_drain_pipe();

	qllm_registry_shutdown();

	pthread_mutex_lock(&g_engine.sess_mutex);
	for (i = 0; i < QE_SESSIONS; ++i)
		session_release_locked(i);
	free(g_engine.system_prompt);
	g_engine.system_prompt = NULL;
	pthread_mutex_unlock(&g_engine.sess_mutex);

	/* free anything left in the completion queue */
	while (g_engine.comp_count > 0) {
		int idx = g_engine.comp_head;

		free(g_engine.comp[idx].payload);
		g_engine.comp_head = (idx + 1) % QE_JOB_MAX;
		--g_engine.comp_count;
	}
	engine_drain_pipe();

	close(g_engine.pipe_r);
	close(g_engine.pipe_w);

	pthread_mutex_destroy(&g_engine.sess_mutex);
	pthread_cond_destroy(&g_engine.comp_cond);
	pthread_mutex_destroy(&g_engine.comp_mutex);
	pthread_cond_destroy(&g_engine.job_cond);
	pthread_mutex_destroy(&g_engine.job_mutex);

	memset(&g_engine, 0, sizeof(g_engine));
}

/* Internal submit with sid + optional NULL input (meta jobs). */
static int
job_submit_session(int kind, const char *model, const char *session_key, int sid, const char *input, void *ud, int *job_id_out)
{
	struct qllm_engine_job *job;
	int i, id;

	if (!engine_running())
		return -1;

	pthread_mutex_lock(&g_engine.job_mutex);
	if (g_engine.job_count >= QE_JOB_MAX) {
		pthread_mutex_unlock(&g_engine.job_mutex);
		return -1;
	}

	i = g_engine.job_tail;
	g_engine.job_tail = (g_engine.job_tail + 1) % QE_JOB_MAX;
	job = &g_engine.jobs[i];
	job->kind = kind;
	job->sid = sid;
	job->session_key = (session_key && session_key[0]) ? strdup(session_key) : NULL;
	job->model = (model && model[0]) ? strdup(model) : NULL;
	if (input) {
		job->input = strdup(input);
		if (!job->input) {
			free(job->session_key);
			job->session_key = NULL;
			free(job->model);
			job->model = NULL;
			pthread_mutex_unlock(&g_engine.job_mutex);
			return -1;
		}
	} else {
		job->input = NULL;
	}
	job->ud = ud;
	id = job->job_id = g_engine.next_job_id++;
	++g_engine.job_count;
	pthread_cond_signal(&g_engine.job_cond);
	pthread_mutex_unlock(&g_engine.job_mutex);

	if (job_id_out)
		*job_id_out = id;
	return 0;
}

static int
job_submit(int kind, const char *model, int sid, const char *input, void *ud, int *job_id_out)
{
	return job_submit_session(kind, model, NULL, sid, input, ud, job_id_out);
}

int
qllm_engine_submit(int kind, const char *input, void *ud, int *job_id_out)
{
	return qllm_engine_submit_model(kind, NULL, input, ud, job_id_out);
}

int
qllm_engine_submit_model(int kind, const char *model, const char *input, void *ud, int *job_id_out)
{
	if (!input)
		return -1;
	return job_submit(kind, model, 0, input, ud, job_id_out);
}

int
qllm_engine_chat(int sid, const char *input, void *ud, int *job_id_out)
{
	if (!input)
		return -1;
	return job_submit(QE_JOB_CHAT, NULL, sid, input, ud, job_id_out);
}

int
qllm_engine_chat_oneshot(const char *request_json, void *ud, int *job_id_out)
{
	return qllm_engine_chat_oneshot_model(NULL, request_json, ud, job_id_out);
}

int
qllm_engine_chat_oneshot_model(const char *model, const char *request_json, void *ud, int *job_id_out)
{
	if (!request_json)
		return -1;
	return job_submit_session(QE_JOB_CHAT_ONESHOT, model, NULL, 0, request_json, ud, job_id_out);
}

int
qllm_engine_chat_oneshot_model_session(const char *model, const char *session_key, const char *request_json, void *ud, int *job_id_out)
{
	if (!request_json)
		return -1;
	return job_submit_session(QE_JOB_CHAT_ONESHOT, model, session_key, 0, request_json, ud, job_id_out);
}

int
qllm_engine_systemone(const char *request_json, void *ud, int *job_id_out)
{
	return qllm_engine_systemone_model(NULL, request_json, ud, job_id_out);
}

int
qllm_engine_systemone_model(const char *model, const char *request_json, void *ud, int *job_id_out)
{
	if (!request_json)
		return -1;
	return job_submit(QE_JOB_SYSTEMONE, model, 0, request_json, ud, job_id_out);
}

int
qllm_engine_systemzero(const char *request_json, void *ud, int *job_id_out)
{
	return qllm_engine_systemzero_model(NULL, request_json, ud, job_id_out);
}

int
qllm_engine_systemzero_model(const char *model, const char *request_json, void *ud, int *job_id_out)
{
	if (!request_json)
		return -1;
	return job_submit(QE_JOB_SYSTEMZERO, model, 0, request_json, ud, job_id_out);
}

int
qllm_engine_session_open(int sid, const char *system_prompt, void *ud, int *job_id_out)
{
	return job_submit(QE_JOB_OPEN, NULL, sid,
	    system_prompt ? system_prompt : "", ud, job_id_out);
}

int
qllm_engine_session_reset(int sid, void *ud, int *job_id_out)
{
	return job_submit(QE_JOB_RESET, NULL, sid, NULL, ud, job_id_out);
}

int
qllm_engine_session_close(int sid, void *ud, int *job_id_out)
{
	return job_submit(QE_JOB_CLOSE, NULL, sid, NULL, ud, job_id_out);
}

int
qllm_engine_set_system(const char *system_prompt)
{
	char *dup = NULL, *old;

	if (system_prompt && system_prompt[0]) {
		dup = strdup(system_prompt);
		if (!dup)
			return -1;
	}

	pthread_mutex_lock(&g_engine.sess_mutex);
	old = g_engine.system_prompt;
	g_engine.system_prompt = dup;
	pthread_mutex_unlock(&g_engine.sess_mutex);
	free(old);
	return 0;
}

int
qllm_engine_poll(qllm_engine_result_cb cb, void *cbud)
{
	struct qllm_engine_result pending[QE_JOB_MAX];
	int n = 0, i, r;

	if (!cb)
		return 0;

	engine_drain_pipe();

	pthread_mutex_lock(&g_engine.comp_mutex);
	while (g_engine.comp_count > 0) {
		i = g_engine.comp_head;
		pending[n++] = g_engine.comp[i];
		g_engine.comp_head = (i + 1) % QE_JOB_MAX;
		--g_engine.comp_count;
	}
	pthread_cond_broadcast(&g_engine.comp_cond);
	pthread_mutex_unlock(&g_engine.comp_mutex);

	for (r = 0; r < n; ++r) {
		cb(cbud, &pending[r]);
		free(pending[r].payload);
	}
	return n;
}

int
qllm_engine_wake_fd(void)
{
	if (!g_engine.running)
		return -1;
	return g_engine.pipe_r;
}

int
qllm_engine_wait(int timeout_ms)
{
	struct pollfd pfd;
	int r;

	if (!g_engine.running)
		return -1;

	pfd.fd = g_engine.pipe_r;
	pfd.events = POLLIN;
	pfd.revents = 0;

	r = poll(&pfd, 1, timeout_ms);
	if (r <= 0)
		return -1;

	/* The poll settled the byte(s); drain the hint bytes so a subsequent
	 * select/poll on the same fd won't see stale readiness. */
	engine_drain_pipe();
	return 0;
}