#include <ttypt/axil.h>
#include <ttypt/stele.h>
#ifdef _WIN32
#pragma push_macro("weak")
#define weak
#include <ttypt/xy-mod.h>
#pragma pop_macro("weak")
#else
#include <ttypt/xy-mod.h>
#endif

#include "openai_chat.h"
#include "openai_embed.h"
#include "auren-collections.h"
#include "systemone.h"
#include "auren-engine.h"
#include "auren-pending.h"
#include "auren-upstream.h"

#include <json-c/json.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
static char *
auren_strndup(const char *s, size_t n)
{
	size_t len = 0;
	while (len < n && s[len])
		len++;
	char *out = malloc(len + 1);
	if (!out)
		return NULL;
	memcpy(out, s, len);
	out[len] = '\0';
	return out;
}
#define strndup auren_strndup
#endif

#define REC_END "\r\n.\r\n"

/* Per-telnet-connection state. LLM state lives in the engine (sessions keyed
 * by the connection's fd); the loop thread only keeps the input line being
 * assembled (for "$ " command execution inside streamed output) and a busy
 * flag so overlapping ask turns are rejected at submit time. */
typedef struct fd_info {
	char			line_buf[BUFSIZ * 4];
	unsigned		line_pos;
	unsigned		busy;
	unsigned		has_session;
	unsigned long		epoch;	/* bumps on each accept; guards stale jobs */
} fdi_t;

fdi_t fdis[FD_SETSIZE];

size_t crb_len = 0;
char *crb_mapped = NULL;
char *crb_system = NULL;	/* NUL-terminated heap copy of crb.txt */

static char auren_model_path[BUFSIZ];

pending_t *
make_pending(void *handle, int mode, int sid)
{
	pending_t *p = malloc(sizeof(*p));

	if (p) {
		socket_t fd = handle ? (socket_t)(intptr_t)handle : (socket_t)sid;
		p->handle = handle;
		p->mode = mode;
		p->sid = sid;
		p->epoch = (fd >= 0 && fd < FD_SETSIZE) ? fdis[fd].epoch : 0;
		p->cancelled = 0;
	}
	return p;
}

static inline void
append_to_line(fdi_t *fdi, const char *s, size_t len)
{
	size_t space;

	if (len == 0)
		return;

	if (fdi->line_pos >= sizeof(fdi->line_buf) - 1)
		return;

	space = sizeof(fdi->line_buf) - 1 - fdi->line_pos;
	if (len > space)
		len = space;

	memcpy(fdi->line_buf + fdi->line_pos, s, len);
	fdi->line_pos += len;
	fdi->line_buf[fdi->line_pos] = '\0';
}

static inline void
line_reset(fdi_t *fdi)
{
	fdi->line_pos = 0;
	memset(fdi->line_buf, 0, sizeof(fdi->line_buf));
}

static void
cmd_cb(
	socket_t fd,
	char *buf,
	size_t len,
	int ofd __attribute__((unused)))
{
	/* Just echo command output back to client. */
	axil_write(fd, buf, len);
}

static inline void
cmd_exec(socket_t fd, fdi_t *fdi)
{
	char argsbuf[BUFSIZ], *space;
	int argc = 0;
	char *args[8];
	char *pound;

	if (!fdi->line_pos)
		return;

	pound = strstr(fdi->line_buf, "$ ");
	if (!pound)
		return;

	snprintf(argsbuf, sizeof(argsbuf), "%s", pound + 2);
	space = argsbuf;

	do {
		args[argc] = space;
		argc++;
		space = strchr(space, ' ');
		if (!space)
			break;
		*space = '\0';
		space++;
	} while (argc < 7);

	args[argc] = NULL;
	space = strchr(args[argc - 1], '\n');
	if (space)
		*space = '\0';

	axil_exec(fd, args, cmd_cb, NULL, 0);
	axil_write(fd, "\n", 1);
}

static void
do_ASK(socket_t fd, int argc, char *argv[])
{
	fdi_t *fdi = &fdis[fd];
	char *user;
	size_t total = 1;
	int i;

	if (fdi->busy) {
		axil_write(fd, "Busy\n", 5);
		return;
	}

	for (i = 1; i < argc; ++i)
		total += strlen(argv[i]) + (i > 1 ? 1 : 0);

	user = malloc(total);
	if (!user) {
		axil_writef(fd, "Out of memory\n");
		return;
	}

	user[0] = '\0';
	for (i = 1; i < argc; ++i) {
		if (i > 1)
			strcat(user, " ");
		strcat(user, argv[i]);
	}

	fdi->busy = 1;
	fdi->has_session = 1;
	{
		pending_t *p = make_pending(NULL, 0, (int)fd);

		if (!p) {
			fdi->busy = 0;
			axil_writef(fd, "Out of memory\n");
			free(user);
			return;
		}
		p->epoch = fdi->epoch;
		if (qllm_engine_chat((int)fd, user, p, NULL) != 0) {
			free(p);
			fdi->busy = 0;
			axil_writef(fd, "Engine busy\n");
		}
	}
	free(user);
}

static void
do_CHAT(socket_t fd, int argc __attribute__((unused)),
    char *argv[] __attribute__((unused)))
{
	if (fd >= 0 && fd < FD_SETSIZE)
		fdis[fd].has_session = 1;
	/* Clear the connection's session history + KV in the engine. */
	if (qllm_engine_session_reset((int)fd, NULL, NULL) != 0)
		stl_syslog(STL_LOG_ERR, "chat reset queue full\n");
}

/* Feed a streamed chunk's newlines through "$ " command execution. */
static void
chat_handle_chunk(socket_t fd, const char *chunk, size_t len)
{
	fdi_t *fdi = &fdis[fd];
	size_t i;

	for (i = 0; i < len; ++i) {
		append_to_line(fdi, &chunk[i], 1);
		if (chunk[i] == '\n') {
			cmd_exec(fd, fdi);
			line_reset(fdi);
		}
	}
}

/* Loop-thread completion drain: complete deferred responses from the engine.
 * Owns/frees the pending_t attached to the job's ud. Meta results (open/reset/
 * close) carry ud = NULL and need no response. */
static void
drain(void *cbud __attribute__((unused)),
    const struct qllm_engine_result *r)
{
	pending_t *p = r->ud;

	if (!p)
		return;

	if (r->kind == QE_JOB_EMBED) {
		socket_t fd = (socket_t)(intptr_t)p->handle;
		if (fd < 0 || fd >= FD_SETSIZE || fdis[fd].epoch != p->epoch) {
			free(p);
			return;
		}
		if (r->err)
			axil_respond_defer_abort(p->handle);
		else
			axil_respond_defer_finish(p->handle, r->payload);
		free(p);
		return;
	}

	if (r->kind == QE_JOB_CHAT) {
		socket_t fd = (socket_t)p->sid;

		/* The connection moved on (closed and its fd reused): these
		 * results belong to a dead generation — mark cancelled and
		 * only free p on the terminal result (!r->more). */
		if (fd < 0 || fd >= FD_SETSIZE || fdis[fd].epoch != p->epoch) {
			__sync_lock_test_and_set(&p->cancelled, 1);
			if (!r->more)
				free(p);
			return;
		}

		if (r->more) {
			size_t len = r->payload ? strlen(r->payload) : 0;

			if (len)
				axil_write(fd, (char *)r->payload, len);
			chat_handle_chunk(fd, r->payload ? r->payload : "", len);
			return;   /* keep pending until the final result */
		}

		/* Final: end the streaming body; the chunks are already sent. */
		axil_write(fd, (void *)REC_END, strlen(REC_END));
		fdis[fd].busy = 0;
		free(p);
		return;
	}

	if (r->kind == QE_JOB_CHAT_ONESHOT) {
		socket_t fd = (socket_t)(intptr_t)p->handle;

		if (fd < 0 || fd >= FD_SETSIZE || fdis[fd].epoch != p->epoch) {
			__sync_lock_test_and_set(&p->cancelled, 1);
			if (!r->more)
				free(p);
			return;
		}

		if (p->mode && r->more) {
			axil_write(fd, "data: ", 6);
			if (r->payload)
				axil_write(fd, r->payload, strlen(r->payload));
			axil_write(fd, "\n\n", 2);
			return;   /* keep pending until the final result */
		}

		if (p->mode) {
			if (r->err) {
				char fallback[128];
				snprintf(fallback, sizeof(fallback),
					"Engine error %d: generation or context failed", r->err);
				const char *msg = (r->payload && r->payload[0]) ? r->payload : fallback;
				struct json_object *eo = json_object_new_object();
				struct json_object *ei = json_object_new_object();
				json_object_object_add(ei, "message", json_object_new_string(msg));
				json_object_object_add(ei, "type", json_object_new_string("server_error"));
				json_object_object_add(eo, "error", ei);
				const char *es = json_object_to_json_string(eo);
				if (es) {
					axil_write(fd, "data: ", 6);
					axil_write(fd, (char *)es, strlen(es));
					axil_write(fd, "\n\n", 2);
				}
				json_object_put(eo);
			} else if (r->payload) {
				axil_write(fd, "data: ", 6);
				axil_write(fd, r->payload, strlen(r->payload));
				axil_write(fd, "\n\n", 2);
			}
			axil_write(fd, "data: [DONE]\n\n", 14);
			axil_respond_defer_done(p->handle);
		} else if (r->err) {
			axil_respond_defer_abort(p->handle);
		} else {
			axil_respond_defer_finish(p->handle, r->payload);
		}
		free(p);
		return;
	}

	if (r->kind == QE_JOB_SYSTEMONE) {
		socket_t fd = (socket_t)(intptr_t)p->handle;
		if (fd < 0 || fd >= FD_SETSIZE || fdis[fd].epoch != p->epoch) {
			free(p);
			return;
		}
		if (r->err)
			axil_respond_defer_abort(p->handle);
		else
			axil_respond_defer_finish(p->handle, r->payload);
		free(p);
		return;
	}

	if (!r->more)
		free(p);
}

/* The library's terms (this module) implement the axil host hooks. The engine
 * worker wakes the loop via qllm_engine_wake_fd(); every readable fd also
 * drains whatever completed (idempotent on the loop thread). */
XY_IMPL(int, on_axil_connect, socket_t, fd)
{
	fdis[fd].busy = 0;
	fdis[fd].has_session = 0;
	fdis[fd].epoch++;
	line_reset(&fdis[fd]);
	return 0;
}

XY_IMPL(int, on_axil_disconnect, socket_t, fd)
{
	fdi_t *fdi = &fdis[fd];

	/* Release the engine session only if this connection opened one. */
	if (fdi->has_session) {
		if (qllm_engine_session_close((int)fd, NULL, NULL) != 0)
			stl_syslog(STL_LOG_ERR, "session close queue full\n");
		fdi->has_session = 0;
	}

	fdi->busy = 0;
	fdi->epoch++;
	line_reset(fdi);
	return 0;
}

XY_IMPL(int, on_axil_tick, socket_t, fd)
{
	(void)fd;
	qllm_engine_poll(drain, NULL);
	return 0;
}

XY_IMPL(int, on_axil_update, unsigned long long, dt)
{
	(void)dt;
	qllm_engine_poll(drain, NULL);
	return 0;
}

static void
setup_crb(const char *path)
{
	ssize_t r;

	r = axil_mmap(&crb_mapped, (char *)path);
	if (r > 0) {
		crb_len = (size_t)r;
		crb_system = strndup(crb_mapped, crb_len);
	}
}

#ifndef _WIN32
#include <dirent.h>
static int
has_auto_models_dir(void)
{
	const char *env_dir = getenv("AUREN_MODELS_DIR");
	const char *home = getenv("HOME");
	char dir_path[512];
	if (env_dir && env_dir[0])
		snprintf(dir_path, sizeof(dir_path), "%s", env_dir);
	else if (home && home[0])
		snprintf(dir_path, sizeof(dir_path), "%s/.models", home);
	else
		return 0;

	DIR *dp = opendir(dir_path);
	if (!dp)
		return 0;
	struct dirent *de;
	int found = 0;
	while ((de = readdir(dp)) != NULL) {
		size_t nl = strlen(de->d_name);
		if (nl > 5 && strcasecmp(de->d_name + nl - 5, ".gguf") == 0) {
			char fp[1024];
			snprintf(fp, sizeof(fp), "%s/%s", dir_path, de->d_name);
			if (access(fp, R_OK) == 0) {
				found = 1;
				break;
			}
		}
	}
	closedir(dp);
	return found;
}
#else
static int has_auto_models_dir(void) { return 0; }
#endif

static int
auren_init(void)
{
	const char *model_path = getenv("AUREN_MODEL_PATH");
	const char *crb_path = getenv("AUREN_CRB_PATH");
	const char *chat_path = getenv("AUREN_CHAT_PATH");
	const char *judge_path = getenv("AUREN_JUDGE_PATH");
	const char *embed_path = getenv("AUREN_EMBED_PATH");
	const char *cfg_path = getenv("AUREN_CONFIG_PATH");
	const char *models_json = getenv("AUREN_MODELS");
	char home_auren[512] = {0};
	char home_dot_auren[512] = {0};
	char home_config_auren[512] = {0};
	const char *home = getenv("HOME");
	if (home && home[0]) {
		snprintf(home_auren, sizeof(home_auren), "%s/auren.json", home);
		snprintf(home_dot_auren, sizeof(home_dot_auren), "%s/.auren.json", home);
		snprintf(home_config_auren, sizeof(home_config_auren), "%s/.config/auren/config.json", home);
	}

	int has_models = (model_path && model_path[0]) ||
	                 (chat_path && chat_path[0]) ||
	                 (judge_path && judge_path[0]) ||
	                 (embed_path && embed_path[0]) ||
	                 (cfg_path && cfg_path[0]) ||
	                 (models_json && models_json[0]) ||
	                 (access("auren.json", R_OK) == 0) ||
	                 (home_auren[0] && access(home_auren, R_OK) == 0) ||
	                 (home_dot_auren[0] && access(home_dot_auren, R_OK) == 0) ||
	                 (home_config_auren[0] && access(home_config_auren, R_OK) == 0) ||
	                 auren_upstream_has_credentials() ||
	                 has_auto_models_dir();

	if (!has_models) {
		stl_syslog(STL_LOG_ERR, "No model configuration set; auren engine disabled\n");
		return -1;
	}

	if (model_path && model_path[0])
		snprintf(auren_model_path, sizeof(auren_model_path), "%s", model_path);
	else
		auren_model_path[0] = '\0';

	setup_crb(crb_path && crb_path[0] ? crb_path : "crb.txt");

	axil_register("ask", do_ASK, CF_NOAUTH | CF_NOTRIM);
	axil_register("chat", do_CHAT, CF_NOAUTH | CF_NOTRIM);

	if (auren_engine_init(auren_model_path) != 0) {
		stl_syslog(STL_LOG_ERR, "Failed to init engine\n");
		return -1;
	}

	auren_engine_set_system(crb_system);

#ifndef _WIN32
	axil_fd_watch(auren_engine_wake_fd());
#endif

	auren_upstream_init();
	openai_embed_init(auren_model_path);
	openai_chat_init(auren_model_path);
	auren_collections_init();
	systemone_init();

	return 0;
}

XY_MODULE_API void
xy_install(void)
{
	if (auren_init() != 0)
		stl_syslog(STL_LOG_ERR, "libaxil-auren: init failed\n");
}

XY_MODULE_API void
xy_shutdown(void)
{
	auren_upstream_shutdown();
	auren_engine_shutdown();
	openai_embed_shutdown();
	openai_chat_shutdown();
	auren_collections_shutdown();
	systemone_shutdown();
	free(crb_system);
	crb_system = NULL;
}
