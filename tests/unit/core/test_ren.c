/* test_ren.c — Unit & integration tests for libren tool calling, history, and errors */

#define _GNU_SOURCE
#include "../framework/test_utils.h"
#include "ttypt/libren.h"
#include <arpa/inet.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
	int done_calls;
	char done_ok[16];
	char done_msg[1024];
};

static void test_event_cb(const char *ev, const char *a1, const char *a2, void *ud) {
	struct captured_events *c = ud;
	if (strcmp(ev, "TOOL") == 0) {
		c->tool_calls++;
		snprintf(c->tool_name, sizeof(c->tool_name), "%s", a1 ? a1 : "");
		snprintf(c->tool_args, sizeof(c->tool_args), "%s", a2 ? a2 : "");
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

