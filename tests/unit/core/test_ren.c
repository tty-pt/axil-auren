/* test_ren.c — Unit & integration tests for libren tool calling, history, and errors */

#define _GNU_SOURCE
#include "../framework/test_utils.h"
#include "ttypt/libren.h"
#include <arpa/inet.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_MOCK_TURNS 40

struct mock_http_server {
	int listen_fd;
	int port;
	int n_turns;
	const char *responses[MAX_MOCK_TURNS];
	char *req_bodies[MAX_MOCK_TURNS];
	pthread_t tid;
};

static char *read_http_request_body(int cfd) {
	size_t cap = 8192, len = 0;
	char *buf = malloc(cap);
	if (!buf) return NULL;
	buf[0] = '\0';

	int has_hdr = 0;
	size_t body_off = 0;
	size_t content_len = 0;

	while (1) {
		if (len + 4096 + 1 > cap) {
			cap *= 2;
			buf = realloc(buf, cap);
		}
		ssize_t n = recv(cfd, buf + len, 4096, 0);
		if (n <= 0) break;
		len += (size_t)n;
		buf[len] = '\0';

		if (!has_hdr) {
			char *hdr_end = strstr(buf, "\r\n\r\n");
			if (hdr_end) {
				has_hdr = 1;
				body_off = (size_t)((hdr_end + 4) - buf);
				const char *cl = strcasestr(buf, "Content-Length:");
				if (cl && cl < hdr_end) {
					content_len = (size_t)strtoul(cl + 15, NULL, 10);
				}
			}
		}
		if (has_hdr) {
			if (len - body_off >= content_len) {
				char *body = strndup(buf + body_off, content_len);
				free(buf);
				return body;
			}
		}
	}
	free(buf);
	return NULL;
}

static void *mock_server_thread(void *arg) {
	struct mock_http_server *s = arg;
	for (int i = 0; i < s->n_turns; i++) {
		int cfd = accept(s->listen_fd, NULL, NULL);
		if (cfd < 0) break;
		s->req_bodies[i] = read_http_request_body(cfd);
		const char *resp = s->responses[i] ? s->responses[i] : "HTTP/1.1 500 Internal\r\nConnection: close\r\n\r\n";
		size_t rlen = strlen(resp);
		size_t sent = 0;
		while (sent < rlen) {
			ssize_t n = send(cfd, resp + sent, rlen - sent, 0);
			if (n <= 0) break;
			sent += (size_t)n;
		}
		close(cfd);
	}
	return NULL;
}

static int mock_server_start(struct mock_http_server *s, int n_turns) {
	memset(s, 0, sizeof(*s));
	s->n_turns = n_turns;
	s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (s->listen_fd < 0) return -1;

	int opt = 1;
	setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(s->listen_fd);
		return -1;
	}
	if (listen(s->listen_fd, 4) < 0) {
		close(s->listen_fd);
		return -1;
	}

	socklen_t alen = sizeof(addr);
	if (getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen) < 0) {
		close(s->listen_fd);
		return -1;
	}
	s->port = ntohs(addr.sin_port);
	return 0;
}

static void mock_server_launch(struct mock_http_server *s) {
	pthread_create(&s->tid, NULL, mock_server_thread, s);
}

static void mock_server_join(struct mock_http_server *s) {
	if (s->listen_fd >= 0) {
		pthread_join(s->tid, NULL);
		close(s->listen_fd);
		s->listen_fd = -1;
	}
}

static void mock_server_stop(struct mock_http_server *s) {
	mock_server_join(s);
	for (int i = 0; i < s->n_turns; i++) {
		free(s->req_bodies[i]);
		s->req_bodies[i] = NULL;
	}
}

struct captured_events {
	int tool_calls;
	char tool_name[128];
	char tool_args[512];
	int tier0_calls;
	char tier0_op[128];
	char tier0_out[512];
	int clause_calls;
	char clause_text[512];
	int steer_calls;
	char steer_msg[512];
	int done_calls;
	char done_ok[16];
	char done_msg[1024];
};

static void test_event_cb(const char *ev, const char *a1, const char *a2, void *ud) {
	struct captured_events *c = ud;
	if (strcmp(ev, "TIER0") == 0) {
		c->tier0_calls++;
		snprintf(c->tier0_op, sizeof(c->tier0_op), "%s", a1 ? a1 : "");
		snprintf(c->tier0_out, sizeof(c->tier0_out), "%s", a2 ? a2 : "");
	} else if (strcmp(ev, "TOOL") == 0) {
		c->tool_calls++;
		snprintf(c->tool_name, sizeof(c->tool_name), "%s", a1 ? a1 : "");
		snprintf(c->tool_args, sizeof(c->tool_args), "%s", a2 ? a2 : "");
	} else if (strcmp(ev, "CLAUSE") == 0) {
		c->clause_calls++;
		snprintf(c->clause_text, sizeof(c->clause_text), "%s", a1 ? a1 : "");
	} else if (strcmp(ev, "STEER") == 0) {
		c->steer_calls++;
		snprintf(c->steer_msg, sizeof(c->steer_msg), "%s", a1 ? a1 : "");
	} else if (strcmp(ev, "DONE") == 0) {
		c->done_calls++;
		snprintf(c->done_ok, sizeof(c->done_ok), "%s", a1 ? a1 : "");
		snprintf(c->done_msg, sizeof(c->done_msg), "%s", a2 ? a2 : "");
	}
}

static char g_mock_tool_output[512] = "total 4496\n-rw-r--r-- 1 user user 10 README.md";

static int mock_bash_tool(const char *args, char **out) {
	(void)args;
	*out = strdup(g_mock_tool_output);
	return 0;
}

TEST(ren_native_tool_calls_multi_turn)
{
	signal(SIGPIPE, SIG_IGN);
	snprintf(g_mock_tool_output, sizeof(g_mock_tool_output), "total 42\n-rw-r--r-- 1 user user 10 README.md");

	ren_context_t ctx;
	ren_init(&ctx);
	ren_register_tool("bash", "bash <command>", mock_bash_tool);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 2), 0);

	/* Turn 1: chunked delta.tool_calls */
	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_abc1\",\"type\":\"function\",\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"args\\\":\"}}]}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"ls -la\\\"}\"}}]}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
		"data: [DONE]\n\n";

	/* Turn 2: final assistant content */
	srv.responses[1] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"There is one file: README.md.\"}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
		"data: [DONE]\n\n";

	mock_server_launch(&srv);

	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "List the files", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_join(&srv);

	ASSERT_EQ(rc, 0);
	ASSERT_EQ(ev.tool_calls, 1);
	ASSERT_STR_EQ(ev.tool_name, "bash");
	ASSERT_STR_EQ(ev.tool_args, "ls -la");
	ASSERT_EQ(ev.done_calls, 1);
	ASSERT_STR_EQ(ev.done_ok, "1");
	ASSERT_STR_EQ(ev.done_msg, "There is one file: README.md.");

	/* Turn 1 request should declare tools */
	ASSERT_NOT_NULL(srv.req_bodies[0]);
	struct json_object *req0 = json_tokener_parse(srv.req_bodies[0]);
	ASSERT_NOT_NULL(req0);
	struct json_object *tools0 = json_object_object_get(req0, "tools");
	ASSERT_NOT_NULL(tools0);
	ASSERT_TRUE(json_object_array_length(tools0) >= 1);
	json_object_put(req0);

	/* Turn 2 request should contain assistant(tool_calls) + role:tool with call_abc1 */
	ASSERT_NOT_NULL(srv.req_bodies[1]);
	struct json_object *req1 = json_tokener_parse(srv.req_bodies[1]);
	ASSERT_NOT_NULL(req1);
	struct json_object *msgs1 = json_object_object_get(req1, "messages");
	ASSERT_NOT_NULL(msgs1);
	size_t n1 = json_object_array_length(msgs1);
	ASSERT_TRUE(n1 >= 4);
	struct json_object *last_m = json_object_array_get_idx(msgs1, n1 - 1);
	ASSERT_STR_EQ(json_object_get_string(json_object_object_get(last_m, "role")), "tool");
	ASSERT_STR_EQ(json_object_get_string(json_object_object_get(last_m, "tool_call_id")), "call_abc1");
	ASSERT_NOT_NULL(strstr(json_object_get_string(json_object_object_get(last_m, "content")), "README.md"));
	json_object_put(req1);

	mock_server_stop(&srv);
	ren_free(&ctx);
}

TEST(ren_empty_sse_fails_without_poisoning_history)
{
	signal(SIGPIPE, SIG_IGN);
	snprintf(g_mock_tool_output, sizeof(g_mock_tool_output), "total 4496\ndrwxr-xr-x 2 user user 4096 .");

	ren_context_t ctx;
	ren_init(&ctx);
	ren_register_tool("bash", "bash <command>", mock_bash_tool);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 2), 0);

	/* Turn 1: native tool call */
	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_e1\",\"type\":\"function\",\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"args\\\":\\\"ls -la\\\"}\"}}]}}]}\n\n"
		"data: [DONE]\n\n";

	/* Turn 2: upstream drops reply and emits empty finish + [DONE] */
	srv.responses[1] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
		"data: [DONE]\n\n";

	mock_server_launch(&srv);

	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "List directory", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_stop(&srv);

	ASSERT_EQ(rc, -1);
	ASSERT_EQ(ev.done_calls, 1);
	ASSERT_STR_EQ(ev.done_ok, "0");
	ASSERT_NOT_NULL(strstr(ev.done_msg, "empty"));

	/* Ensure history did NOT get poisoned with an assistant message saying "total 4496" */
	size_t wlen = ctx.history_len + 3;
	char *wrapped = malloc(wlen);
	snprintf(wrapped, wlen, "[%s]", ctx.history);
	struct json_object *arr = json_tokener_parse(wrapped);
	free(wrapped);
	ASSERT_NOT_NULL(arr);
	size_t n = json_object_array_length(arr);
	struct json_object *last = json_object_array_get_idx(arr, n - 1);
	ASSERT_STR_EQ(json_object_get_string(json_object_object_get(last, "role")), "tool");
	json_object_put(arr);

	ren_free(&ctx);
}

TEST(ren_sse_error_frame_reports_error)
{
	signal(SIGPIPE, SIG_IGN);

	ren_context_t ctx;
	ren_init(&ctx);
	ren_register_tool("bash", "bash <command>", mock_bash_tool);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 1), 0);

	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"error\":{\"message\":\"antigravity gemini-4-hydrogen-low returned MALFORMED_FUNCTION_CALL\",\"type\":\"server_error\"}}\n\n"
		"data: [DONE]\n\n";

	mock_server_launch(&srv);

	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Run a check", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_stop(&srv);

	ASSERT_EQ(rc, -1);
	ASSERT_EQ(ev.done_calls, 1);
	ASSERT_STR_EQ(ev.done_ok, "0");
	ASSERT_NOT_NULL(strstr(ev.done_msg, "MALFORMED_FUNCTION_CALL"));

	ren_free(&ctx);
}

TEST(ren_http_502_reports_error_body)
{
	signal(SIGPIPE, SIG_IGN);

	ren_context_t ctx;
	ren_init(&ctx);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 1), 0);

	srv.responses[0] =
		"HTTP/1.1 502 Bad Gateway\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
		"{\"error\":\"upstream overloaded\"}";

	mock_server_launch(&srv);

	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Hello", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_stop(&srv);

	ASSERT_EQ(rc, -1);
	ASSERT_EQ(ev.done_calls, 1);
	ASSERT_STR_EQ(ev.done_ok, "0");
	ASSERT_NOT_NULL(strstr(ev.done_msg, "502"));
	ASSERT_NOT_NULL(strstr(ev.done_msg, "upstream overloaded"));

	ren_free(&ctx);
}

TEST(ren_history_trim_preserves_json_and_tool_parity)
{
	signal(SIGPIPE, SIG_IGN);

	ren_context_t ctx;
	ren_init(&ctx);
	ctx.max_history_chars = 262144;
	ren_register_tool("bash", "bash <command>", mock_bash_tool);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 2), 0);

	/* Seed history with literal delimiter pattern inside content and >262KB so proactive compaction runs */
	char big_chunk[18000];
	memset(big_chunk, 'X', sizeof(big_chunk) - 1);
	big_chunk[sizeof(big_chunk) - 1] = '\0';
	memcpy(big_chunk, ",{\"role\":\"fake_injection\"", 24);

	snprintf(g_mock_tool_output, sizeof(g_mock_tool_output), "ok");

	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_t1\",\"type\":\"function\",\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"args\\\":\\\"echo hi\\\"}\"}}]}}]}\n\n"
		"data: [DONE]\n\n";

	srv.responses[1] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Trimmed cleanly.\"}}]}\n\n"
		"data: [DONE]\n\n";

	struct json_object *arr = json_object_new_array();
	for (int i = 0; i < 16; i++) {
		struct json_object *u = json_object_new_object();
		json_object_object_add(u, "role", json_object_new_string("user"));
		json_object_object_add(u, "content", json_object_new_string(big_chunk));
		json_object_array_add(arr, u);

		struct json_object *a = json_object_new_object();
		json_object_object_add(a, "role", json_object_new_string("assistant"));
		json_object_object_add(a, "content", json_object_new_string("ack"));
		json_object_array_add(arr, a);
	}
	const char *ser = json_object_to_json_string_ext(arr, JSON_C_TO_STRING_PLAIN);
	size_t slen = strlen(ser);
	size_t inner = slen - 2;
	free(ctx.history);
	ctx.history_cap = inner + 16;
	ctx.history = malloc(ctx.history_cap);
	memcpy(ctx.history, ser + 1, inner);
	ctx.history[inner] = '\0';
	ctx.history_len = inner;
	json_object_put(arr);

	mock_server_launch(&srv);
	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Check trimmed history", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_join(&srv);

	ASSERT_EQ(rc, 0);
	ASSERT_STR_EQ(ev.done_msg, "Trimmed cleanly.");

	/* Verify turn 0 request messages were trimmed, started with system, then user */
	struct json_object *req0 = json_tokener_parse(srv.req_bodies[0]);
	ASSERT_NOT_NULL(req0);
	struct json_object *msgs0 = json_object_object_get(req0, "messages");
	ASSERT_NOT_NULL(msgs0);
	size_t m0_len = json_object_array_length(msgs0);
	ASSERT_TRUE(m0_len >= 2);
	ASSERT_TRUE(m0_len < 34);
	struct json_object *m0 = json_object_array_get_idx(msgs0, 0);
	struct json_object *m1 = json_object_array_get_idx(msgs0, 1);
	ASSERT_STR_EQ(json_object_get_string(json_object_object_get(m0, "role")), "system");
	ASSERT_STR_EQ(json_object_get_string(json_object_object_get(m1, "role")), "user");
	json_object_put(req0);

	mock_server_stop(&srv);
	ren_free(&ctx);
}

TEST(ren_max_tool_turns_triggers_graceful_wrapup)
{
	signal(SIGPIPE, SIG_IGN);
	snprintf(g_mock_tool_output, sizeof(g_mock_tool_output), "file_entry");

	ren_context_t ctx;
	ren_init(&ctx);
	ctx.max_turns = 8;
	ren_register_tool("bash", "bash <command>", mock_bash_tool);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 9), 0);

	char *resps[8];
	for (int i = 0; i < 8; i++) {
		resps[i] = malloc(512);
		snprintf(resps[i], 512,
			"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
			"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"type\":\"function\",\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"args\\\":\\\"ls_%d\\\"}\"}}]}}]}\n\n"
			"data: [DONE]\n\n", i);
		srv.responses[i] = resps[i];
	}
	srv.responses[8] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Summary after 8 tool turns.\"}}]}\n\n"
		"data: [DONE]\n\n";

	mock_server_launch(&srv);
	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Explore project", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_join(&srv);

	for (int i = 0; i < 8; i++) free(resps[i]);

	ASSERT_EQ(rc, 0);
	ASSERT_EQ(ev.tool_calls, 8);
	ASSERT_EQ(ev.done_calls, 1);
	ASSERT_STR_EQ(ev.done_ok, "1");
	ASSERT_STR_EQ(ev.done_msg, "Summary after 8 tool turns.");

	/* Turn 8 wrap-up request should include tool_choice: "none" and transient wrap-up nudge */
	ASSERT_NOT_NULL(srv.req_bodies[8]);
	struct json_object *req_w = json_tokener_parse(srv.req_bodies[8]);
	ASSERT_NOT_NULL(req_w);
	struct json_object *tc_o = json_object_object_get(req_w, "tool_choice");
	ASSERT_NOT_NULL(tc_o);
	ASSERT_STR_EQ(json_object_get_string(tc_o), "none");
	struct json_object *msgs_w = json_object_object_get(req_w, "messages");
	ASSERT_NOT_NULL(msgs_w);
	size_t nw = json_object_array_length(msgs_w);
	struct json_object *last_w = json_object_array_get_idx(msgs_w, nw - 1);
	ASSERT_STR_EQ(json_object_get_string(json_object_object_get(last_w, "role")), "user");
	ASSERT_NOT_NULL(strstr(json_object_get_string(json_object_object_get(last_w, "content")), "Tool turn limit reached"));
	json_object_put(req_w);

	mock_server_stop(&srv);

	/* Persisted history should end with assistant summary, not transient nudge or dangling tool */
	size_t wlen = ctx.history_len + 3;
	char *wrapped = malloc(wlen);
	snprintf(wrapped, wlen, "[%s]", ctx.history);
	struct json_object *arr = json_tokener_parse(wrapped);
	free(wrapped);
	ASSERT_NOT_NULL(arr);
	size_t hn = json_object_array_length(arr);
	struct json_object *last_h = json_object_array_get_idx(arr, hn - 1);
	ASSERT_STR_EQ(json_object_get_string(json_object_object_get(last_h, "role")), "assistant");
	ASSERT_STR_EQ(json_object_get_string(json_object_object_get(last_h, "content")), "Summary after 8 tool turns.");
	json_object_put(arr);

	ren_free(&ctx);
}

TEST(ren_unlimited_turns_exceeds_32_by_default)
{
	signal(SIGPIPE, SIG_IGN);
	snprintf(g_mock_tool_output, sizeof(g_mock_tool_output), "ok");

	ren_context_t ctx;
	ren_init(&ctx);
	ASSERT_EQ(ctx.max_turns, 0);
	ASSERT_EQ(ctx.max_history_chars, (size_t)0);
	ren_register_tool("bash", "bash <command>", mock_bash_tool);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 36), 0);

	char *resps[35];
	for (int i = 0; i < 35; i++) {
		resps[i] = malloc(512);
		snprintf(resps[i], 512,
			"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
			"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"type\":\"function\",\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"args\\\":\\\"step_%d\\\"}\"}}]}}]}\n\n"
			"data: [DONE]\n\n", i);
		srv.responses[i] = resps[i];
	}
	srv.responses[35] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Completed 35 turns.\"}}]}\n\n"
		"data: [DONE]\n\n";

	mock_server_launch(&srv);
	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Explore deeply", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_join(&srv);

	for (int i = 0; i < 35; i++) free(resps[i]);

	ASSERT_EQ(rc, 0);
	ASSERT_EQ(ev.tool_calls, 35);
	ASSERT_STR_EQ(ev.done_ok, "1");
	ASSERT_STR_EQ(ev.done_msg, "Completed 35 turns.");

	mock_server_stop(&srv);
	ren_free(&ctx);
}

TEST(ren_stall_detection_wraps_up_repeated_calls)
{
	signal(SIGPIPE, SIG_IGN);
	snprintf(g_mock_tool_output, sizeof(g_mock_tool_output), "same_dir_listing");

	ren_context_t ctx;
	ren_init(&ctx);
	ren_register_tool("bash", "bash <command>", mock_bash_tool);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 4), 0);

	const char *repeat_tc =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"type\":\"function\",\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"args\\\":\\\"ls -la\\\"}\"}}]}}]}\n\n"
		"data: [DONE]\n\n";

	srv.responses[0] = repeat_tc;
	srv.responses[1] = repeat_tc;
	srv.responses[2] = repeat_tc; /* 3rd identical turn triggers stall wrap-up without executing tool */
	srv.responses[3] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Stopped repeating and answered.\"}}]}\n\n"
		"data: [DONE]\n\n";

	mock_server_launch(&srv);
	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Inspect", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_join(&srv);

	ASSERT_EQ(rc, 0);
	ASSERT_EQ(ev.tool_calls, 2);
	ASSERT_STR_EQ(ev.done_ok, "1");
	ASSERT_STR_EQ(ev.done_msg, "Stopped repeating and answered.");

	ASSERT_NOT_NULL(srv.req_bodies[3]);
	struct json_object *req_w = json_tokener_parse(srv.req_bodies[3]);
	ASSERT_NOT_NULL(req_w);
	ASSERT_STR_EQ(json_object_get_string(json_object_object_get(req_w, "tool_choice")), "none");
	struct json_object *msgs_w = json_object_object_get(req_w, "messages");
	size_t nw = json_object_array_length(msgs_w);
	struct json_object *last_w = json_object_array_get_idx(msgs_w, nw - 1);
	ASSERT_NOT_NULL(strstr(json_object_get_string(json_object_object_get(last_w, "content")), "Repeated identical tool calls detected"));
	json_object_put(req_w);

	mock_server_stop(&srv);
	ren_free(&ctx);
}

TEST(ren_overflow_compaction_retries_and_recovers)
{
	signal(SIGPIPE, SIG_IGN);

	ren_context_t ctx;
	ren_init(&ctx);

	/* Seed multi-turn history without setting max_history_chars (default 0 = no upfront trim) */
	char big_chunk[4096];
	memset(big_chunk, 'A', sizeof(big_chunk) - 1);
	big_chunk[sizeof(big_chunk) - 1] = '\0';

	struct json_object *arr = json_object_new_array();
	for (int i = 0; i < 6; i++) {
		struct json_object *u = json_object_new_object();
		json_object_object_add(u, "role", json_object_new_string("user"));
		json_object_object_add(u, "content", json_object_new_string(big_chunk));
		json_object_array_add(arr, u);

		struct json_object *a = json_object_new_object();
		json_object_object_add(a, "role", json_object_new_string("assistant"));
		json_object_object_add(a, "content", json_object_new_string("ok"));
		json_object_array_add(arr, a);
	}
	const char *ser = json_object_to_json_string_ext(arr, JSON_C_TO_STRING_PLAIN);
	size_t inner = strlen(ser) - 2;
	free(ctx.history);
	ctx.history_cap = inner + 16;
	ctx.history = malloc(ctx.history_cap);
	memcpy(ctx.history, ser + 1, inner);
	ctx.history[inner] = '\0';
	ctx.history_len = inner;
	json_object_put(arr);

	size_t seeded_len = ctx.history_len;

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 2), 0);

	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"error\":{\"message\":\"antigravity failed (http=400): maximum context length exceeded\",\"type\":\"server_error\"}}\n\n"
		"data: [DONE]\n\n";

	srv.responses[1] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Recovered after compaction.\"}}]}\n\n"
		"data: [DONE]\n\n";

	mock_server_launch(&srv);
	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Continue", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_join(&srv);

	ASSERT_EQ(rc, 0);
	ASSERT_STR_EQ(ev.done_ok, "1");
	ASSERT_STR_EQ(ev.done_msg, "Recovered after compaction.");
	ASSERT_TRUE(ctx.history_len < seeded_len);
	ASSERT_TRUE(strlen(srv.req_bodies[1]) < strlen(srv.req_bodies[0]));

	mock_server_stop(&srv);
	ren_free(&ctx);
}

static int g_custom_compact_calls = 0;
static char g_custom_compact_reason[64] = {0};

static int mock_xylem_compactor(ren_context_t *ctx, size_t target_bytes, const char *reason)
{
	(void)target_bytes;
	g_custom_compact_calls++;
	snprintf(g_custom_compact_reason, sizeof(g_custom_compact_reason), "%s", reason ? reason : "");

	const char *replacement =
		"{\"role\":\"system\",\"content\":\"You are ren.\"},"
		"{\"role\":\"user\",\"content\":\"[compacted summary] Continue\"}";
	size_t rlen = strlen(replacement);
	if (rlen + 1 > ctx->history_cap) {
		ctx->history_cap = rlen + 16;
		ctx->history = realloc(ctx->history, ctx->history_cap);
	}
	memcpy(ctx->history, replacement, rlen + 1);
	ctx->history_len = rlen;
	return 1;
}

TEST(ren_custom_compactor_extension_hook)
{
	signal(SIGPIPE, SIG_IGN);
	g_custom_compact_calls = 0;
	g_custom_compact_reason[0] = '\0';

	ren_context_t ctx;
	ren_init(&ctx);
	ren_register_compactor(mock_xylem_compactor);

	char big_chunk[4096];
	memset(big_chunk, 'Z', sizeof(big_chunk) - 1);
	big_chunk[sizeof(big_chunk) - 1] = '\0';

	struct json_object *arr = json_object_new_array();
	for (int i = 0; i < 4; i++) {
		struct json_object *u = json_object_new_object();
		json_object_object_add(u, "role", json_object_new_string("user"));
		json_object_object_add(u, "content", json_object_new_string(big_chunk));
		json_object_array_add(arr, u);
	}
	const char *ser = json_object_to_json_string_ext(arr, JSON_C_TO_STRING_PLAIN);
	size_t inner = strlen(ser) - 2;
	free(ctx.history);
	ctx.history_cap = inner + 16;
	ctx.history = malloc(ctx.history_cap);
	memcpy(ctx.history, ser + 1, inner);
	ctx.history[inner] = '\0';
	ctx.history_len = inner;
	json_object_put(arr);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 2), 0);

	srv.responses[0] =
		"HTTP/1.1 413 Payload Too Large\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
		"{\"error\":\"request entity too large\"}";
	srv.responses[1] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Custom compactor worked.\"}}]}\n\n"
		"data: [DONE]\n\n";

	mock_server_launch(&srv);
	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Continue", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_join(&srv);

	ren_register_compactor(NULL);

	ASSERT_EQ(rc, 0);
	ASSERT_EQ(g_custom_compact_calls, 1);
	ASSERT_STR_EQ(g_custom_compact_reason, "overflow");
	ASSERT_STR_EQ(ev.done_msg, "Custom compactor worked.");
	ASSERT_NOT_NULL(strstr(srv.req_bodies[1], "[compacted summary]"));

	mock_server_stop(&srv);
	ren_free(&ctx);
}

TEST(ren_default_prompt_excludes_optional_mods)
{
	ren_context_t ctx;
	ren_init(&ctx);

	ASSERT_NOT_NULL(ctx.history);
	ASSERT_NOT_NULL(strstr(ctx.history, "You are ren, a pure-C coding assistant."));
	ASSERT_NULL(strstr(ctx.history, "Jarvis"));
	ASSERT_NULL(strstr(ctx.history, "WhatsApp"));
	ASSERT_NULL(strstr(ctx.history, "record"));
	ASSERT_NULL(strstr(ctx.history, "recall"));
	ASSERT_NULL(strstr(ctx.history, "inspect"));

	ren_free(&ctx);
}

TEST(ren_prompt_hint_updates_system_prompt)
{
	signal(SIGPIPE, SIG_IGN);

	ren_context_t ctx;
	ren_init(&ctx);
	ren_register_prompt_hint("Test hint: custom module active.");
	ren_register_prompt_hint("Test hint: custom module active.");

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 1), 0);

	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Hint received.\"}}]}\n\n"
		"data: [DONE]\n\n";

	mock_server_launch(&srv);
	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Check prompt", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_join(&srv);

	ASSERT_EQ(rc, 0);
	ASSERT_NOT_NULL(srv.req_bodies[0]);
	const char *first_hit = strstr(srv.req_bodies[0], "Test hint: custom module active.");
	ASSERT_NOT_NULL(first_hit);
	ASSERT_NULL(strstr(first_hit + 1, "Test hint: custom module active."));

	mock_server_stop(&srv);
	ren_free(&ctx);
}

TEST(ren_set_system_prompt_overrides_identity)
{
	signal(SIGPIPE, SIG_IGN);

	ren_context_t ctx;
	ren_init(&ctx);
	ren_set_system_prompt("You are Jarvis, the user's personal voice assistant. Never identify yourself as ren.");
	ren_register_prompt_hint("Test hint: custom module active.");

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 1), 0);

	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"I am Jarvis.\"}}]}\n\n"
		"data: [DONE]\n\n";

	mock_server_launch(&srv);
	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Who are you?", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_join(&srv);

	ASSERT_EQ(rc, 0);
	ASSERT_NOT_NULL(srv.req_bodies[0]);
	struct json_object *req0 = json_tokener_parse(srv.req_bodies[0]);
	ASSERT_NOT_NULL(req0);
	struct json_object *msgs0 = json_object_object_get(req0, "messages");
	ASSERT_NOT_NULL(msgs0);
	struct json_object *sys0 = json_object_array_get_idx(msgs0, 0);
	ASSERT_STR_EQ(json_object_get_string(json_object_object_get(sys0, "role")), "system");
	const char *sc = json_object_get_string(json_object_object_get(sys0, "content"));
	ASSERT_NOT_NULL(sc);
	ASSERT_NOT_NULL(strstr(sc, "You are Jarvis"));
	ASSERT_NULL(strstr(sc, "You are ren, a pure-C coding assistant."));
	ASSERT_NOT_NULL(strstr(sc, "Available tools:"));
	ASSERT_NOT_NULL(strstr(sc, "Test hint: custom module active."));
	json_object_put(req0);

	/* ren_reset must keep the active plugin identity prompt */
	ren_reset(&ctx);
	ASSERT_NOT_NULL(strstr(ctx.history, "You are Jarvis"));
	ASSERT_NULL(strstr(ctx.history, "You are ren, a pure-C coding assistant."));

	/* NULL restores default ren identity */
	ren_set_system_prompt(NULL);
	ren_reset(&ctx);
	ASSERT_NOT_NULL(strstr(ctx.history, "You are ren, a pure-C coding assistant."));

	mock_server_stop(&srv);
	ren_free(&ctx);
}

struct slow_stream_mock {
	int listen_fd;
	int port;
	int ready_pipe[2];
	int n_turns;
	const char *turn0_head;
	const char *turn1_resp;
	char *req_bodies[2];
	pthread_t srv_tid;
	pthread_t ctrl_tid;
	int do_abort;
	const char *steer_text;
};

static void *slow_stream_server_fn(void *arg)
{
	struct slow_stream_mock *s = arg;
	for (int i = 0; i < s->n_turns; i++) {
		int cfd = accept(s->listen_fd, NULL, NULL);
		if (cfd < 0) break;
		s->req_bodies[i] = read_http_request_body(cfd);
		if (i == 0) {
			size_t len = strlen(s->turn0_head);
			size_t off = 0;
			while (off < len) {
				ssize_t w = send(cfd, s->turn0_head + off, len - off, 0);
				if (w <= 0) break;
				off += (size_t)w;
			}
			char b = 1;
			ssize_t pw = write(s->ready_pipe[1], &b, 1);
			(void)pw;

			struct pollfd pfd;
			pfd.fd = cfd;
			pfd.events = POLLIN | POLLRDHUP;
			pfd.revents = 0;
			(void)poll(&pfd, 1, 2000);
			close(cfd);
		} else {
			size_t len = strlen(s->turn1_resp);
			size_t off = 0;
			while (off < len) {
				ssize_t w = send(cfd, s->turn1_resp + off, len - off, 0);
				if (w <= 0) break;
				off += (size_t)w;
			}
			close(cfd);
		}
	}
	return NULL;
}

static void *slow_stream_ctrl_fn(void *arg)
{
	struct slow_stream_mock *s = arg;
	char b = 0;
	ssize_t r = read(s->ready_pipe[0], &b, 1);
	(void)r;
	usleep(15000);
	if (s->do_abort) {
		ren_abort();
	} else if (s->steer_text) {
		ren_steer(s->steer_text);
	}
	return NULL;
}

TEST(ren_steer_mid_stream_redirects_without_corrupting_history)
{
	signal(SIGPIPE, SIG_IGN);

	ren_context_t ctx;
	ren_init(&ctx);

	struct slow_stream_mock sm;
	memset(&sm, 0, sizeof(sm));
	sm.n_turns = 2;
	sm.steer_text = "Actually check foo.c";
	sm.turn0_head =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Partial answer that must be discarded.\"}}]}\n\n";
	sm.turn1_resp =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Switched to foo.c.\"}}]}\n\n"
		"data: [DONE]\n\n";

	ASSERT_EQ(pipe(sm.ready_pipe), 0);
	sm.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_TRUE(sm.listen_fd >= 0);
	int opt = 1;
	setsockopt(sm.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ASSERT_EQ(bind(sm.listen_fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(listen(sm.listen_fd, 4), 0);
	socklen_t alen = sizeof(addr);
	ASSERT_EQ(getsockname(sm.listen_fd, (struct sockaddr *)&addr, &alen), 0);
	sm.port = ntohs(addr.sin_port);

	pthread_create(&sm.srv_tid, NULL, slow_stream_server_fn, &sm);
	pthread_create(&sm.ctrl_tid, NULL, slow_stream_ctrl_fn, &sm);

	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Check bar.c", "127.0.0.1", sm.port, test_event_cb, &ev);

	pthread_join(sm.ctrl_tid, NULL);
	pthread_join(sm.srv_tid, NULL);
	close(sm.listen_fd);
	close(sm.ready_pipe[0]);
	close(sm.ready_pipe[1]);

	ASSERT_EQ(rc, 0);
	ASSERT_EQ(ev.steer_calls, 1);
	ASSERT_STR_EQ(ev.steer_msg, "Actually check foo.c");
	ASSERT_STR_EQ(ev.done_ok, "1");
	ASSERT_STR_EQ(ev.done_msg, "Switched to foo.c.");
	ASSERT_NOT_NULL(sm.req_bodies[1]);
	ASSERT_NOT_NULL(strstr(sm.req_bodies[1], "Actually check foo.c"));
	ASSERT_NULL(strstr(sm.req_bodies[1], "Partial answer that must be discarded."));
	ASSERT_NULL(strstr(ctx.history, "Partial answer that must be discarded."));

	free(sm.req_bodies[0]);
	free(sm.req_bodies[1]);
	ren_free(&ctx);
}

static int g_steer_t1_calls = 0;
static int g_steer_t2_calls = 0;

static int mock_steer_tool1(const char *args, char **out)
{
	(void)args;
	g_steer_t1_calls++;
	ren_steer("Stop after first tool and answer now");
	*out = strdup("tool1 ok");
	return 0;
}

static int mock_steer_tool2(const char *args, char **out)
{
	(void)args;
	g_steer_t2_calls++;
	*out = strdup("tool2 should not run");
	return 0;
}

TEST(ren_steer_mid_tool_batch_skips_remaining_preserves_ids)
{
	signal(SIGPIPE, SIG_IGN);
	g_steer_t1_calls = 0;
	g_steer_t2_calls = 0;

	ren_context_t ctx;
	ren_init(&ctx);
	ren_register_tool("st_tool1", "st_tool1", mock_steer_tool1);
	ren_register_tool("st_tool2", "st_tool2", mock_steer_tool2);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 2), 0);

	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":["
		"{\"index\":0,\"id\":\"call_t1\",\"type\":\"function\",\"function\":{\"name\":\"st_tool1\",\"arguments\":\"{}\"}},"
		"{\"index\":1,\"id\":\"call_t2\",\"type\":\"function\",\"function\":{\"name\":\"st_tool2\",\"arguments\":\"{}\"}}"
		"]}}]}\n\n"
		"data: [DONE]\n\n";

	srv.responses[1] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Stopped after tool1.\"}}]}\n\n"
		"data: [DONE]\n\n";

	mock_server_launch(&srv);
	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Run both tools", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_join(&srv);

	ASSERT_EQ(rc, 0);
	ASSERT_EQ(g_steer_t1_calls, 1);
	ASSERT_EQ(g_steer_t2_calls, 0);
	ASSERT_EQ(ev.steer_calls, 1);
	ASSERT_STR_EQ(ev.steer_msg, "Stop after first tool and answer now");
	ASSERT_STR_EQ(ev.done_msg, "Stopped after tool1.");

	ASSERT_NOT_NULL(srv.req_bodies[1]);
	struct json_object *req1 = json_tokener_parse(srv.req_bodies[1]);
	ASSERT_NOT_NULL(req1);
	struct json_object *msgs1 = json_object_object_get(req1, "messages");
	ASSERT_NOT_NULL(msgs1);

	int found_t1 = 0, found_t2_skip = 0, found_steer_user = 0;
	size_t nm = json_object_array_length(msgs1);
	for (size_t i = 0; i < nm; i++) {
		struct json_object *m = json_object_array_get_idx(msgs1, i);
		const char *role = json_object_get_string(json_object_object_get(m, "role"));
		const char *content = json_object_get_string(json_object_object_get(m, "content"));
		if (role && strcmp(role, "tool") == 0) {
			const char *tid = json_object_get_string(json_object_object_get(m, "tool_call_id"));
			if (tid && strcmp(tid, "call_t1") == 0 && content && strcmp(content, "tool1 ok") == 0)
				found_t1 = 1;
			if (tid && strcmp(tid, "call_t2") == 0 && content && strcmp(content, "Skipped: user steered.") == 0)
				found_t2_skip = 1;
		} else if (role && strcmp(role, "user") == 0 && content &&
		           strcmp(content, "[steer] Stop after first tool and answer now") == 0) {
			found_steer_user = 1;
		}
	}
	json_object_put(req1);

	ASSERT_EQ(found_t1, 1);
	ASSERT_EQ(found_t2_skip, 1);
	ASSERT_EQ(found_steer_user, 1);

	mock_server_stop(&srv);
	ren_free(&ctx);
}

TEST(ren_abort_mid_stream_returns_immediately)
{
	signal(SIGPIPE, SIG_IGN);

	ren_context_t ctx;
	ren_init(&ctx);

	struct slow_stream_mock sm;
	memset(&sm, 0, sizeof(sm));
	sm.n_turns = 1;
	sm.do_abort = 1;
	sm.turn0_head =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Streaming before abort...\"}}]}\n\n";

	ASSERT_EQ(pipe(sm.ready_pipe), 0);
	sm.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_TRUE(sm.listen_fd >= 0);
	int opt = 1;
	setsockopt(sm.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ASSERT_EQ(bind(sm.listen_fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(listen(sm.listen_fd, 4), 0);
	socklen_t alen = sizeof(addr);
	ASSERT_EQ(getsockname(sm.listen_fd, (struct sockaddr *)&addr, &alen), 0);
	sm.port = ntohs(addr.sin_port);

	pthread_create(&sm.srv_tid, NULL, slow_stream_server_fn, &sm);
	pthread_create(&sm.ctrl_tid, NULL, slow_stream_ctrl_fn, &sm);

	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "Start long job", "127.0.0.1", sm.port, test_event_cb, &ev);

	pthread_join(sm.ctrl_tid, NULL);
	pthread_join(sm.srv_tid, NULL);
	close(sm.listen_fd);
	close(sm.ready_pipe[0]);
	close(sm.ready_pipe[1]);
	free(sm.req_bodies[0]);

	ASSERT_EQ(rc, -1);
	ASSERT_STR_EQ(ev.done_ok, "0");
	ASSERT_STR_EQ(ev.done_msg, "Interrupted");

	/* Subsequent turn must not remain aborted */
	struct mock_http_server srv2;
	ASSERT_EQ(mock_server_start(&srv2, 1), 0);
	srv2.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Fresh turn ok.\"}}]}\n\n"
		"data: [DONE]\n\n";
	mock_server_launch(&srv2);
	struct captured_events ev2 = {0};
	int rc2 = ren_chat_ex(&ctx, "Next question", "127.0.0.1", srv2.port, test_event_cb, &ev2);
	mock_server_join(&srv2);
	ASSERT_EQ(rc2, 0);
	ASSERT_STR_EQ(ev2.done_ok, "1");
	ASSERT_STR_EQ(ev2.done_msg, "Fresh turn ok.");
	mock_server_stop(&srv2);

	ren_free(&ctx);
}

TEST(ren_set_model_overrides_request_model)
{
	ren_context_t ctx;
	ren_init(&ctx);
	ren_set_model("gemini-4-hydrogen-extra-low");

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 1), 0);
	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"OK\"}}]}\n\n"
		"data: [DONE]\n\n";
	mock_server_launch(&srv);

	ASSERT_EQ(ren_chat(&ctx, "hi", "127.0.0.1", srv.port), 0);
	mock_server_join(&srv);
	ASSERT_NOT_NULL(srv.req_bodies[0]);
	ASSERT_NOT_NULL(strstr(srv.req_bodies[0], "\"gemini-4-hydrogen-extra-low\""));

	mock_server_stop(&srv);
	ren_set_model(NULL);
	ren_free(&ctx);
}

static char *mock_ctx_a(void) { return strdup("x11_focused='Firefox'"); }
static char *mock_ctx_empty(void) { return strdup(""); }
static char *mock_ctx_b(void) { return strdup("browser_tabs=2"); }

TEST(ren_multiple_context_providers_joined)
{
	signal(SIGPIPE, SIG_IGN);

	ren_context_t ctx;
	ren_init(&ctx);
	ren_register_context_provider(NULL);
	ren_register_context_provider(mock_ctx_a);
	ren_register_context_provider(mock_ctx_a); /* duplicate ignored */
	ren_register_context_provider(mock_ctx_empty);
	ren_register_context_provider(mock_ctx_b);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 1), 0);
	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Noted.\"}}]}\n\n"
		"data: [DONE]\n\n";
	mock_server_launch(&srv);

	struct captured_events ev = {0};
	int rc = ren_chat_ex(&ctx, "What is open?", "127.0.0.1", srv.port, test_event_cb, &ev);
	mock_server_join(&srv);

	ren_register_context_provider(NULL);

	ASSERT_EQ(rc, 0);
	ASSERT_NOT_NULL(srv.req_bodies[0]);
	ASSERT_NOT_NULL(strstr(srv.req_bodies[0], "[Live Desktop State: x11_focused='Firefox' browser_tabs=2]"));
	/* Deduplicated: should appear once */
	const char *hit1 = strstr(srv.req_bodies[0], "x11_focused='Firefox'");
	ASSERT_NOT_NULL(hit1);
	ASSERT_NULL(strstr(hit1 + 1, "x11_focused='Firefox'"));

	mock_server_stop(&srv);
	ren_free(&ctx);
}

extern void xy_install(void);

static char g_reflex_last_ws[128] = {0};
static char g_reflex_last_media[128] = {0};
static int g_reflex_fail_ws = 0;

static int mock_reflex_ws(const char *args, char **out) {
	if (g_reflex_fail_ws) {
		*out = strdup("Error: simulated workspace failure");
		return -1;
	}
	snprintf(g_reflex_last_ws, sizeof(g_reflex_last_ws), "%s", args ? args : "");
	*out = strdup("switched");
	return 0;
}

static int mock_reflex_media(const char *args, char **out) {
	snprintf(g_reflex_last_media, sizeof(g_reflex_last_media), "%s", args ? args : "");
	*out = strdup("media_ok");
	return 0;
}

static int g_reflex_fail_open_app = 0;
static char g_reflex_last_browser[128] = {0};

static int mock_reflex_open_app(const char *args, char **out) {
	if (g_reflex_fail_open_app || !args || strcmp(args, "firefox") != 0) {
		*out = strdup("Error: unknown application");
		return -1;
	}
	*out = strdup("launched");
	return 0;
}

static int mock_reflex_browser(const char *args, char **out) {
	snprintf(g_reflex_last_browser, sizeof(g_reflex_last_browser), "%s", args ? args : "");
	*out = strdup("browser_ok");
	return 0;
}

TEST(ren_reflex_deterministic_and_fallthrough)
{
	signal(SIGPIPE, SIG_IGN);
	g_reflex_last_ws[0] = '\0';
	g_reflex_last_media[0] = '\0';
	g_reflex_last_browser[0] = '\0';
	g_reflex_fail_ws = 0;
	g_reflex_fail_open_app = 0;

	/* Seed custom user TSV rule inside test HOME */
	const char *home = getenv("HOME");
	ASSERT_NOT_NULL(home);
	char cfg_dir[512], ren_dir[512], tsv_path[512];
	snprintf(cfg_dir, sizeof(cfg_dir), "%s/.config", home);
	snprintf(ren_dir, sizeof(ren_dir), "%s/.config/ren", home);
	snprintf(tsv_path, sizeof(tsv_path), "%s/.config/ren/reflex.tsv", home);
	mkdir(cfg_dir, 0755);
	mkdir(ren_dir, 0755);
	FILE *f = fopen(tsv_path, "w");
	ASSERT_NOT_NULL(f);
	fprintf(f, "# user override\nswitch tag {n}\tworkspace\tmove {n}\tCustom tag {n} -> {n}.\n");
	fclose(f);

	ren_context_t ctx;
	ren_init(&ctx);
	ren_register_tool("workspace", "workspace <1..9 | move 1..9>", mock_reflex_ws);
	ren_register_tool("media", "media <play|pause|mute|volume_up>", mock_reflex_media);
	ren_register_tool("open_app", "open_app <app>", mock_reflex_open_app);
	ren_register_tool("browser", "browser <close>", mock_reflex_browser);
	xy_install();

	/* 1. Built-in exact/number word match (never contacts server) */
	struct captured_events ev1 = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "Go to workspace tres!", "127.0.0.1", 1, test_event_cb, &ev1), 0);
	ASSERT_EQ(ev1.tier0_calls, 1);
	ASSERT_STR_EQ(ev1.tier0_op, "workspace");
	ASSERT_STR_EQ(g_reflex_last_ws, "3");

	/* 2. Portuguese move workspace & concise shortcuts */
	struct captured_events ev2 = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "Manda para a área de trabalho dois.", "127.0.0.1", 1, test_event_cb, &ev2), 0);
	ASSERT_EQ(ev2.tier0_calls, 1);
	ASSERT_STR_EQ(g_reflex_last_ws, "move 2");

	struct captured_events ev_ws_short = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "Vai para a 2.", "127.0.0.1", 1, test_event_cb, &ev_ws_short), 0);
	ASSERT_EQ(ev_ws_short.tier0_calls, 1);
	ASSERT_STR_EQ(ev_ws_short.tier0_op, "workspace");
	ASSERT_STR_EQ(g_reflex_last_ws, "2");

	/* 3. Custom TSV rule match with repeated placeholders */
	struct captured_events ev3 = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "switch tag 4", "127.0.0.1", 1, test_event_cb, &ev3), 0);
	ASSERT_EQ(ev3.tier0_calls, 1);
	ASSERT_STR_EQ(g_reflex_last_ws, "move 4");
	ASSERT_STR_EQ(ev3.clause_text, "Custom tag 4 -> 4.");

	/* 4. media_play suppresses CLAUSE TTS; tira o som + browser close + abre o browser hit Tier-0 */
	struct captured_events ev4 = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "resume music", "127.0.0.1", 1, test_event_cb, &ev4), 0);
	ASSERT_EQ(ev4.tier0_calls, 1);
	ASSERT_STR_EQ(ev4.tier0_op, "media_play");
	ASSERT_STR_EQ(g_reflex_last_media, "play");
	ASSERT_EQ(ev4.clause_calls, 0);

	struct captured_events ev_mute = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "tira o som", "127.0.0.1", 1, test_event_cb, &ev_mute), 0);
	ASSERT_EQ(ev_mute.tier0_calls, 1);
	ASSERT_STR_EQ(ev_mute.tier0_op, "media");
	ASSERT_STR_EQ(g_reflex_last_media, "mute");

	struct captured_events ev_tab1 = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "close tab", "127.0.0.1", 1, test_event_cb, &ev_tab1), 0);
	ASSERT_EQ(ev_tab1.tier0_calls, 1);
	ASSERT_STR_EQ(ev_tab1.tier0_op, "browser");
	ASSERT_STR_EQ(g_reflex_last_browser, "close");

	struct captured_events ev_tab2 = {0};
	g_reflex_last_browser[0] = '\0';
	ASSERT_EQ(ren_chat_ex(&ctx, "fecha o separador!", "127.0.0.1", 1, test_event_cb, &ev_tab2), 0);
	ASSERT_EQ(ev_tab2.tier0_calls, 1);
	ASSERT_STR_EQ(ev_tab2.tier0_op, "browser");
	ASSERT_STR_EQ(g_reflex_last_browser, "close");

	struct captured_events ev_browser_open = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "abre o browser", "127.0.0.1", 1, test_event_cb, &ev_browser_open), 0);
	ASSERT_EQ(ev_browser_open.tier0_calls, 1);
	ASSERT_STR_EQ(ev_browser_open.tier0_op, "open_app");

	struct captured_events ev_polite_en = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "Can you please go to workspace 3?", "127.0.0.1", 1, test_event_cb, &ev_polite_en), -1); /* trailing '?' fails open to Tier-1 LLM */
	ASSERT_EQ(ev_polite_en.tier0_calls, 0);
	struct captured_events ev_polite_en_cmd = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "Please pause music", "127.0.0.1", 1, test_event_cb, &ev_polite_en_cmd), 0);
	ASSERT_EQ(ev_polite_en_cmd.tier0_calls, 1);
	ASSERT_STR_EQ(g_reflex_last_media, "pause");

	struct captured_events ev_polite_pt = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "Podes mudar para a Área de Trabalho 3!", "127.0.0.1", 1, test_event_cb, &ev_polite_pt), 0);
	ASSERT_EQ(ev_polite_pt.tier0_calls, 1);
	ASSERT_STR_EQ(g_reflex_last_ws, "3");

	struct captured_events ev_pt_accent = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "Próxima música", "127.0.0.1", 1, test_event_cb, &ev_pt_accent), 0);
	ASSERT_EQ(ev_pt_accent.tier0_calls, 1);
	ASSERT_STR_EQ(g_reflex_last_media, "next");

	/* 5. Builtin conversation_control runs without external tool */
	struct captured_events ev_conv = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "fim de conversa", "127.0.0.1", 1, test_event_cb, &ev_conv), 0);
	ASSERT_EQ(ev_conv.tier0_calls, 1);
	ASSERT_STR_EQ(ev_conv.tier0_op, "conversation_control");
	ASSERT_STR_EQ(ev_conv.clause_text, "Certo.");

	/* 6. Question, multi-clause, non-app 'open ...', failed open_app, and failed tool fail open to Tier-1 LLM */
	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 5), 0);
	for (int i = 0; i < 5; i++) {
		srv.responses[i] =
			"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
			"data: {\"choices\":[{\"delta\":{\"content\":\"LLM handled.\"}}]}\n\n"
			"data: [DONE]\n\n";
	}
	mock_server_launch(&srv);

	struct captured_events ev5 = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "workspace 2?", "127.0.0.1", srv.port, test_event_cb, &ev5), 0);
	ASSERT_EQ(ev5.tier0_calls, 0);

	struct captured_events ev6 = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "pause and workspace 5", "127.0.0.1", srv.port, test_event_cb, &ev6), 0);
	ASSERT_EQ(ev6.tier0_calls, 0);

	struct captured_events ev7 = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "open the architecture docs link", "127.0.0.1", srv.port, test_event_cb, &ev7), 0);
	ASSERT_EQ(ev7.tier0_calls, 0);

	struct captured_events ev_unknown_app = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "launch my dev server", "127.0.0.1", srv.port, test_event_cb, &ev_unknown_app), 0);
	ASSERT_EQ(ev_unknown_app.tier0_calls, 0);

	g_reflex_fail_ws = 1;
	struct captured_events ev8 = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "workspace 6", "127.0.0.1", srv.port, test_event_cb, &ev8), 0);
	ASSERT_EQ(ev8.tier0_calls, 0);
	g_reflex_fail_ws = 0;

	mock_server_stop(&srv);
	ren_register_tier0(NULL);
	unlink(tsv_path);
	ren_free(&ctx);
}

static int g_probe_before = -1;
static int g_probe_after_abort = -1;
static int g_probe_after_steer = -1;

static int mock_interrupt_probe_tool(const char *args, char **out)
{
	g_probe_before = ren_is_interrupted();
	if (args && strstr(args, "steer")) {
		ren_steer("Steer mid-tool");
		g_probe_after_steer = ren_is_interrupted();
	} else {
		ren_abort();
		g_probe_after_abort = ren_is_interrupted();
	}
	*out = strdup("probed");
	return 0;
}

TEST(ren_is_interrupted_reflects_abort_and_steer_and_resets)
{
	signal(SIGPIPE, SIG_IGN);
	g_probe_before = -1;
	g_probe_after_abort = -1;
	g_probe_after_steer = -1;

	ASSERT_EQ(ren_is_interrupted(), 0);

	ren_context_t ctx;
	ren_init(&ctx);
	ren_register_tool("int_probe", "int_probe <abort|steer>", mock_interrupt_probe_tool);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 3), 0);
	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":["
		"{\"index\":0,\"id\":\"call_p1\",\"type\":\"function\",\"function\":{\"name\":\"int_probe\",\"arguments\":\"{\\\"args\\\":\\\"abort\\\"}\"}}"
		"]}}]}\n\n"
		"data: [DONE]\n\n";
	srv.responses[1] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":["
		"{\"index\":0,\"id\":\"call_p2\",\"type\":\"function\",\"function\":{\"name\":\"int_probe\",\"arguments\":\"{\\\"args\\\":\\\"steer\\\"}\"}}"
		"]}}]}\n\n"
		"data: [DONE]\n\n";
	srv.responses[2] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Steered ok.\"}}]}\n\n"
		"data: [DONE]\n\n";
	mock_server_launch(&srv);

	struct captured_events ev1 = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "Probe abort", "127.0.0.1", srv.port, test_event_cb, &ev1), -1);
	ASSERT_EQ(g_probe_before, 0);
	ASSERT_EQ(g_probe_after_abort, 1);
	ASSERT_STR_EQ(ev1.done_msg, "Interrupted");
	ASSERT_EQ(ren_is_interrupted(), 0);

	g_probe_before = -1;
	struct captured_events ev2 = {0};
	ASSERT_EQ(ren_chat_ex(&ctx, "Probe steer", "127.0.0.1", srv.port, test_event_cb, &ev2), 0);
	mock_server_join(&srv);
	ASSERT_EQ(g_probe_before, 0);
	ASSERT_EQ(g_probe_after_steer, 1);
	ASSERT_EQ(ev2.steer_calls, 1);
	ASSERT_STR_EQ(ev2.done_msg, "Steered ok.");
	ASSERT_EQ(ren_is_interrupted(), 0);

	mock_server_stop(&srv);
	ren_free(&ctx);
}

struct clause_log {
	int count;
	char clauses[16][512];
};

static void clause_capture_cb(const char *ev, const char *a1, const char *a2, void *ud)
{
	(void)a2;
	struct clause_log *cl = ud;
	if (strcmp(ev, "CLAUSE") == 0 && cl->count < 16) {
		snprintf(cl->clauses[cl->count++], sizeof(cl->clauses[0]), "%s", a1 ? a1 : "");
	}
}

TEST(ren_clause_comma_boundary_no_midphrase_cut)
{
	signal(SIGPIPE, SIG_IGN);
	ren_context_t ctx;
	ren_init(&ctx);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 1), 0);
	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Não, sou o Jarbas! \"}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Quanto à física quântica, é o ramo da física que estuda o comportamento \"}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"da matéria e da energia à escala atómica e subatómica, \"}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"onde as partículas se comportam também como ondas.\"}}]}\n\n"
		"data: [DONE]\n\n";
	mock_server_launch(&srv);

	struct clause_log cl = {0};
	int rc = ren_chat_ex(&ctx, "És a Maria?", "127.0.0.1", srv.port, clause_capture_cb, &cl);
	mock_server_join(&srv);

	ASSERT_EQ(rc, 0);
	ASSERT_TRUE(cl.count >= 3);
	ASSERT_STR_EQ(cl.clauses[0], "Não, sou o Jarbas!");
	/* Second clause must split at a comma boundary, never mid-verb ("onde as partículas se") */
	for (int i = 0; i < cl.count; i++) {
		size_t n = strlen(cl.clauses[i]);
		ASSERT_TRUE(n > 0);
		if (i + 1 < cl.count) {
			char last = cl.clauses[i][n - 1];
			ASSERT_TRUE(last == '!' || last == '.' || last == '?' || last == ',' || last == ';' || last == ':');
		}
	}
	ASSERT_NOT_NULL(strstr(cl.clauses[cl.count - 1], "partículas se comportam"));

	mock_server_stop(&srv);
	ren_free(&ctx);
}

static int mock_schema_tool(const char *args, char **out_result)
{
	*out_result = strdup(args);
	return 0;
}

TEST(ren_tool_schema_emitted_in_tools_array)
{
	signal(SIGPIPE, SIG_IGN);
	ren_context_t ctx;
	ren_init(&ctx);

	const char *custom_schema =
		"{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
	ren_register_tool_with_schema("custom_tool", "custom_tool <path>", custom_schema, mock_schema_tool);

	struct mock_http_server srv;
	ASSERT_EQ(mock_server_start(&srv, 1), 0);
	srv.responses[0] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
		"data: [DONE]\n\n";
	mock_server_launch(&srv);

	int rc = ren_chat(&ctx, "hi", "127.0.0.1", srv.port);
	mock_server_join(&srv);
	ASSERT_EQ(rc, 0);

	ASSERT_NOT_NULL(srv.req_bodies[0]);
	ASSERT_NOT_NULL(strstr(srv.req_bodies[0], "\"name\":\"custom_tool\""));
	ASSERT_NOT_NULL(strstr(srv.req_bodies[0], "\"required\":[\"path\"]"));

	mock_server_stop(&srv);
	ren_free(&ctx);
}

#define xy_install fs_xy_install
#include "../../../mods/fs/ren-fs.c"
#undef xy_install

#define xy_install bash_xy_install
#include "../../../mods/bash/ren-bash.c"
#undef xy_install

TEST(ren_call_tool_read_json_offset_limit)
{
	ren_register_tool_with_schema("read", "read <path>", READ_SCHEMA, read_file);

	char tmpfile[] = "/tmp/ren_test_read_XXXXXX";
	int fd = mkstemp(tmpfile);
	ASSERT_TRUE(fd >= 0);
	const char *lines = "line 1\nline 2\nline 3\nline 4\nline 5\n";
	(void)write(fd, lines, strlen(lines));
	close(fd);

	/* Call read with offset=2, limit=2 */
	char req[256];
	snprintf(req, sizeof(req), "{\"path\":\"%s\",\"offset\":2,\"limit\":2}", tmpfile);
	char *out = NULL;
	int rc = ren_call_tool("read", req, &out);
	unlink(tmpfile);

	ASSERT_EQ(rc, 0);
	ASSERT_NOT_NULL(out);
	ASSERT_STR_EQ(out, "line 2\nline 3\n");
	free(out);
}

TEST(ren_call_tool_edit_batch_json)
{
	ren_register_tool_with_schema("edit", "edit <path>", EDIT_SCHEMA, edit_file);
	ren_register_tool_with_schema("read", "read <path>", READ_SCHEMA, read_file);

	char tmpfile[] = "/tmp/ren_test_edit_XXXXXX";
	int fd = mkstemp(tmpfile);
	ASSERT_TRUE(fd >= 0);
	const char *content = "alpha beta gamma delta";
	(void)write(fd, content, strlen(content));
	close(fd);

	/* Call edit with batch edits array */
	char req[512];
	snprintf(req, sizeof(req),
		"{\"path\":\"%s\",\"edits\":[{\"oldText\":\"alpha\",\"newText\":\"AAA\"},{\"oldText\":\"gamma\",\"newText\":\"CCC\"}]}",
		tmpfile);
	char *out = NULL;
	int rc = ren_call_tool("edit", req, &out);

	ASSERT_EQ(rc, 0);
	ASSERT_NOT_NULL(out);
	ASSERT_STR_EQ(out, "OK");
	free(out);

	char rreq[256];
	snprintf(rreq, sizeof(rreq), "{\"path\":\"%s\"}", tmpfile);
	char *final_content = NULL;
	rc = ren_call_tool("read", rreq, &final_content);
	unlink(tmpfile);

	ASSERT_EQ(rc, 0);
	ASSERT_NOT_NULL(final_content);
	ASSERT_STR_EQ(final_content, "AAA beta CCC delta");
	free(final_content);
}

TEST(ren_call_tool_bash_json_command)
{
	ren_register_tool_with_schema("bash", "bash <command>", BASH_SCHEMA, exec_bash);

	const char *req = "{\"command\":\"echo ren_bash_ok\"}";
	char *out = NULL;
	int rc = ren_call_tool("bash", req, &out);
	ASSERT_EQ(rc, 0);
	ASSERT_NOT_NULL(out);
	ASSERT_NOT_NULL(strstr(out, "ren_bash_ok"));
	free(out);
}
