/* auren-engine.h — worker-thread LLM engine for auren (axil-agnostic).
 *
 * Decouples LLM work from HTTP/transport behind a single worker thread,
 * a job queue, and a completion queue that a consumer drains after a self-pipe wake.
 *
 * Thread model (do not violate):
 *   - auren_engine_init / auren_engine_submit   may be called from any thread.
 *   - The worker thread executes all llama inference and builds result payloads.
 *   - Results are delivered ONLY via auren_engine_poll, called from a single
 *     consumer thread (the axil loop thread), after the consumer
 *     wakes on auren_engine_wake_fd() (or blocks in auren_engine_wait).
 *   - auren_engine_poll must not be called concurrently from two consumers.
 *   - This header must never pull in axil headers.
 */

#ifndef AUREN_ENGINE_H
#define AUREN_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Job kinds. */
enum {
	AUREN_JOB_EMBED = 1, /* OpenAI-style embeddings for a single text string */
	AUREN_JOB_CHAT,      /* telnet: incremental session chat (sid) */
	AUREN_JOB_CHAT_ONESHOT, /* HTTP: stateless chat, input = full request JSON */
	AUREN_JOB_SYSTEMONE, /* HTTP: System One reflexive decision (choices, exemplars, questions) */
	AUREN_JOB_OPEN,      /* meta: open/ensure a session slot for sid */
	AUREN_JOB_RESET,     /* meta: reset session history + KV */
	AUREN_JOB_CLOSE,     /* meta: close/free a session slot */
};

#define QE_JOB_EMBED AUREN_JOB_EMBED
#define QE_JOB_CHAT AUREN_JOB_CHAT
#define QE_JOB_CHAT_ONESHOT AUREN_JOB_CHAT_ONESHOT
#define QE_JOB_SYSTEMONE AUREN_JOB_SYSTEMONE
#define QE_JOB_OPEN AUREN_JOB_OPEN
#define QE_JOB_RESET AUREN_JOB_RESET
#define QE_JOB_CLOSE AUREN_JOB_CLOSE

/* Result of one completed job. Valid only during the received callback; the
 * engine frees `payload` after the callback returns (copy if you need it).
 * A chat job produces MULTIPLE results: `more=1` for every streamed chunk,
 * then one final result with `more=0` (the full reply text for AUREN_JOB_CHAT,
 * or the tail JSON for a streamed oneshot). `seq` counts per-job results
 * starting at 0. Non-chat jobs produce exactly one result (more=0, seq=0). */
struct auren_engine_result {
	int   job_id;   /* echoed from the submit/job_id_out */
	int   kind;     /* AUREN_JOB_* */
	int   err;      /* 0 = ok, else negative error code */
	int   more;     /* 1 = more chunk(s) follow, 0 = final result for the job */
	int   seq;      /* per-job result sequence, starting at 0 */
	char *payload;  /* NUL-terminated text/JSON on success; NULL on error */
	void *ud;       /* echoed opaque pointer from the submit call */
};
#define qllm_engine_result auren_engine_result

typedef void (*auren_engine_result_cb)(
	    void *ud, const struct auren_engine_result *r);
#define qllm_engine_result_cb auren_engine_result_cb

/*
 * Initialize the engine with a single worker thread.
 * Returns 0 on success, -1 if already initialized or model_path is empty.
 */
int auren_engine_init(const char *model_path);

/*
 * Shut the engine down: signal the worker, join it, free the model context and
 * any queued jobs/results. Idempotent; safe to call with no active engine.
 */
void auren_engine_shutdown(void);

/*
 * Submit a one-shot job. `input` is copied; `ud` is echoed into the result.
 * On success sets *job_id_out (may be NULL) and returns 0; returns -1 if the
 * engine is not initialized or the job queue is full.
 */
int auren_engine_submit(int kind, const char *input, void *ud, int *job_id_out);
int auren_engine_submit_model(int kind, const char *model, const char *input, void *ud, int *job_id_out);

/*
 * Submit a telnet chat turn for an already-open (or to-be-opened) session.
 */
int auren_engine_chat(int sid, const char *input, void *ud, int *job_id_out);

/*
 * Submit a stateless HTTP chat request.
 */
int auren_engine_chat_oneshot(const char *request_json, void *ud, int *job_id_out);
int auren_engine_chat_oneshot_model(const char *model, const char *request_json, void *ud, int *job_id_out);
int auren_engine_chat_oneshot_model_session(const char *model, const char *session_key, const char *request_json, void *ud, int *job_id_out);

/*
 * Submit a System One reflexive decision request (choices gating, exemplar matching, or questions).
 */
int auren_engine_systemone(const char *request_json, void *ud, int *job_id_out);
int auren_engine_systemone_model(const char *model, const char *request_json, void *ud, int *job_id_out);

/*
 * Session bookkeeping.
 */
int auren_engine_session_open(int sid, const char *system_prompt, void *ud, int *job_id_out);
int auren_engine_session_reset(int sid, void *ud, int *job_id_out);
int auren_engine_session_close(int sid, void *ud, int *job_id_out);

/*
 * Provide the engine-global system prompt.
 */
int auren_engine_set_system(const char *system_prompt);

/*
 * Drain completed results. Returns the number of results delivered.
 */
int auren_engine_poll(auren_engine_result_cb cb, void *cbud);

/*
 * The self-pipe read end.
 */
int auren_engine_wake_fd(void);

/*
 * Block up to timeout_ms for a completion wake.
 */
int auren_engine_wait(int timeout_ms);

/* Compatibility aliases */
#define qllm_engine_init auren_engine_init
#define qllm_engine_shutdown auren_engine_shutdown
#define qllm_engine_submit auren_engine_submit
#define qllm_engine_submit_model auren_engine_submit_model
#define qllm_engine_chat auren_engine_chat
#define qllm_engine_chat_oneshot auren_engine_chat_oneshot
#define qllm_engine_chat_oneshot_model auren_engine_chat_oneshot_model
#define qllm_engine_chat_oneshot_model_session auren_engine_chat_oneshot_model_session
#define qllm_engine_systemone auren_engine_systemone
#define qllm_engine_systemone_model auren_engine_systemone_model
#define qllm_engine_session_open auren_engine_session_open
#define qllm_engine_session_reset auren_engine_session_reset
#define qllm_engine_session_close auren_engine_session_close
#define qllm_engine_set_system auren_engine_set_system
#define qllm_engine_poll auren_engine_poll
#define qllm_engine_wake_fd auren_engine_wake_fd
#define qllm_engine_wait auren_engine_wait

#ifdef __cplusplus
}
#endif

#endif /* AUREN_ENGINE_H */
