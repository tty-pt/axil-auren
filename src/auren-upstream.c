/* auren-upstream.c — Cloud upstream router & protocol translator for axil-auren. */

#include "auren-upstream.h"
#include "auren-engine.h"
#include "auren-registry.h"
#include "auren-pending.h"

#include <ttypt/stele.h>

#include <curl/curl.h>
#include <json-c/json.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define AGY_DEFAULT_CLIENT_ID ""
#define AGY_DEFAULT_CLIENT_SECRET ""
#define AGY_DEFAULT_PROJECT_ID "aicode-consumers"
#define AGY_DEFAULT_ENDPOINT "https://daily-cloudcode-pa.googleapis.com"
#define AGY_FALLBACK_ENDPOINT "https://cloudcode-pa.googleapis.com"
#define AGY_USER_AGENT "antigravity/cli/1.5.5 (aidev_client; os_type=linux; arch=amd64; cl=982146307; auth_method=consumer)"

#define KILO_DEFAULT_GATEWAY "https://api.kilo.ai/api/gateway/chat/completions"
#define KILO_FALLBACK_GATEWAY "https://api.kilo.ai/api/openrouter/chat/completions"
#define KILO_USER_AGENT "pi-free-providers"

#define COPILOT_DEFAULT_API "https://api.individual.githubcopilot.com/chat/completions"
#define COPILOT_TOKEN_URL "https://api.github.com/copilot_internal/v2/token"
#define COPILOT_USER_AGENT "GitHubCopilotChat/0.35.0"

/* Ring cache for tool_call_id -> thoughtSignature */
#define SIG_CACHE_SIZE 512

struct sig_cache_entry {
	char call_id[128];
	char sig[512];
};

static struct sig_cache_entry g_sig_cache[SIG_CACHE_SIZE];
static int g_sig_cache_idx = 0;
static pthread_mutex_t g_sig_lock = PTHREAD_MUTEX_INITIALIZER;

/* In-memory auth cache */
struct auth_state {
	pthread_mutex_t lock;
	char auth_path[512];

	/* Antigravity */
	char agy_access[1024];
	char agy_refresh[512];
	uint64_t agy_expires_ms;
	char agy_project_id[128];

	/* Kilo */
	char kilo_access[1024];
	uint64_t kilo_expires_ms;

	/* Copilot */
	char copilot_access[2048];
	char copilot_refresh[512];
	uint64_t copilot_expires_ms;
	char copilot_api_url[256];
	char copilot_models[32][64];
	int copilot_models_count;
};

static struct auth_state g_auth = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.copilot_models_count = 0,
};

static uint64_t
current_time_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
}

static void
generate_uuid(char *out, size_t sz)
{
	unsigned char b[16];
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		if (fread(b, 1, 16, f) != 16)
			memset(b, 0x42, 16);
		fclose(f);
	} else {
		for (int i = 0; i < 16; ++i)
			b[i] = (unsigned char)(rand() & 0xff);
	}
	b[6] = (b[6] & 0x0f) | 0x40; /* version 4 */
	b[8] = (b[8] & 0x3f) | 0x80; /* variant */
	snprintf(out, sz,
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
		b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void
auren_upstream_cache_thought_sig(const char *tool_call_id, const char *sig)
{
	if (!tool_call_id || !tool_call_id[0] || !sig || !sig[0])
		return;

	pthread_mutex_lock(&g_sig_lock);
	int idx = g_sig_cache_idx % SIG_CACHE_SIZE;
	snprintf(g_sig_cache[idx].call_id, sizeof(g_sig_cache[idx].call_id), "%s", tool_call_id);
	snprintf(g_sig_cache[idx].sig, sizeof(g_sig_cache[idx].sig), "%s", sig);
	g_sig_cache_idx++;
	pthread_mutex_unlock(&g_sig_lock);
}

int
auren_upstream_lookup_thought_sig(const char *tool_call_id, char *sig_out, size_t sig_out_sz)
{
	if (!tool_call_id || !tool_call_id[0] || !sig_out || sig_out_sz == 0)
		return 0;

	pthread_mutex_lock(&g_sig_lock);
	for (int i = 0; i < SIG_CACHE_SIZE; ++i) {
		if (strcmp(g_sig_cache[i].call_id, tool_call_id) == 0) {
			snprintf(sig_out, sig_out_sz, "%s", g_sig_cache[i].sig);
			pthread_mutex_unlock(&g_sig_lock);
			return 1;
		}
	}
	pthread_mutex_unlock(&g_sig_lock);
	return 0;
}

static void
resolve_auth_file_path(char *out, size_t sz)
{
	const char *env_path = getenv("AUREN_AUTH_PATH");
	if (env_path && env_path[0]) {
		snprintf(out, sz, "%s", env_path);
		return;
	}
	const char *home = getenv("HOME");
	if (home && home[0]) {
		snprintf(out, sz, "%s/.pi/agent/auth.json", home);
		return;
	}
	snprintf(out, sz, "auth.json");
}

int
auren_upstream_has_credentials(void)
{
	char path[512];
	resolve_auth_file_path(path, sizeof(path));
	return (access(path, R_OK) == 0);
}

/* Load auth from disk under g_auth.lock */
static int
auth_load_locked(void)
{
	resolve_auth_file_path(g_auth.auth_path, sizeof(g_auth.auth_path));
	FILE *f = fopen(g_auth.auth_path, "rb");
	if (!f)
		return -1;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > 2 * 1024 * 1024) {
		fclose(f);
		return -1;
	}

	char *buf = malloc(len + 1);
	if (!buf) {
		fclose(f);
		return -1;
	}
	if (fread(buf, 1, len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return -1;
	}
	buf[len] = '\0';
	fclose(f);

	struct json_object *root = json_tokener_parse(buf);
	free(buf);
	if (!root || !json_object_is_type(root, json_type_object)) {
		if (root) json_object_put(root);
		return -1;
	}

	/* Antigravity */
	struct json_object *agy = json_object_object_get(root, "antigravity");
	if (agy && json_object_is_type(agy, json_type_object)) {
		struct json_object *acc = json_object_object_get(agy, "access");
		struct json_object *ref = json_object_object_get(agy, "refresh");
		struct json_object *exp = json_object_object_get(agy, "expires");
		struct json_object *prj = json_object_object_get(agy, "projectId");

		if (acc && json_object_is_type(acc, json_type_string))
			snprintf(g_auth.agy_access, sizeof(g_auth.agy_access), "%s", json_object_get_string(acc));
		if (ref && json_object_is_type(ref, json_type_string)) {
			const char *rstr = json_object_get_string(ref);
			/* Refresh token might have pipe prefix/suffix */
			const char *pipe = strchr(rstr, '|');
			if (pipe)
				snprintf(g_auth.agy_refresh, sizeof(g_auth.agy_refresh), "%s", pipe + 1);
			else
				snprintf(g_auth.agy_refresh, sizeof(g_auth.agy_refresh), "%s", rstr);
		}
		if (exp && (json_object_is_type(exp, json_type_int) || json_object_is_type(exp, json_type_double)))
			g_auth.agy_expires_ms = (uint64_t)json_object_get_int64(exp);
		if (prj && json_object_is_type(prj, json_type_string))
			snprintf(g_auth.agy_project_id, sizeof(g_auth.agy_project_id), "%s", json_object_get_string(prj));
		else if (!g_auth.agy_project_id[0])
			snprintf(g_auth.agy_project_id, sizeof(g_auth.agy_project_id), "%s", AGY_DEFAULT_PROJECT_ID);
	}

	/* Kilo */
	struct json_object *kilo = json_object_object_get(root, "kilo");
	if (kilo && json_object_is_type(kilo, json_type_object)) {
		struct json_object *acc = json_object_object_get(kilo, "access");
		struct json_object *exp = json_object_object_get(kilo, "expires");
		if (acc && json_object_is_type(acc, json_type_string))
			snprintf(g_auth.kilo_access, sizeof(g_auth.kilo_access), "%s", json_object_get_string(acc));
		if (exp)
			g_auth.kilo_expires_ms = (uint64_t)json_object_get_int64(exp);
	}

	/* Copilot */
	struct json_object *cop = json_object_object_get(root, "github-copilot");
	if (cop && json_object_is_type(cop, json_type_object)) {
		struct json_object *acc = json_object_object_get(cop, "access");
		struct json_object *ref = json_object_object_get(cop, "refresh");
		struct json_object *exp = json_object_object_get(cop, "expires");
		struct json_object *mods = json_object_object_get(cop, "availableModelIds");

		if (acc && json_object_is_type(acc, json_type_string)) {
			snprintf(g_auth.copilot_access, sizeof(g_auth.copilot_access), "%s", json_object_get_string(acc));
			/* Check proxy-ep in token */
			const char *pep = strstr(g_auth.copilot_access, "proxy-ep=");
			if (pep) {
				char host[128] = {0};
				pep += 9;
				const char *semi = strchr(pep, ';');
				size_t hlen = semi ? (size_t)(semi - pep) : strlen(pep);
				if (hlen < sizeof(host)) {
					memcpy(host, pep, hlen);
					host[hlen] = '\0';
					/* replace proxy. with api. */
					if (strncmp(host, "proxy.", 6) == 0)
						snprintf(g_auth.copilot_api_url, sizeof(g_auth.copilot_api_url), "https://api.%s/chat/completions", host + 6);
					else
						snprintf(g_auth.copilot_api_url, sizeof(g_auth.copilot_api_url), "https://%s/chat/completions", host);
				}
			}
			if (!g_auth.copilot_api_url[0])
				snprintf(g_auth.copilot_api_url, sizeof(g_auth.copilot_api_url), "%s", COPILOT_DEFAULT_API);
		}
		if (ref && json_object_is_type(ref, json_type_string))
			snprintf(g_auth.copilot_refresh, sizeof(g_auth.copilot_refresh), "%s", json_object_get_string(ref));
		if (exp)
			g_auth.copilot_expires_ms = (uint64_t)json_object_get_int64(exp);

		if (mods && json_object_is_type(mods, json_type_array)) {
			size_t n = json_object_array_length(mods);
			g_auth.copilot_models_count = 0;
			for (size_t i = 0; i < n && g_auth.copilot_models_count < 32; ++i) {
				struct json_object *m = json_object_array_get_idx(mods, i);
				if (m && json_object_is_type(m, json_type_string)) {
					snprintf(g_auth.copilot_models[g_auth.copilot_models_count++],
						sizeof(g_auth.copilot_models[0]), "%s", json_object_get_string(m));
				}
			}
		}
	}

	json_object_put(root);
	return 0;
}

static void
auth_save_antigravity_locked(const char *access, uint64_t expires_ms)
{
	FILE *f = fopen(g_auth.auth_path, "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(len + 1);
	if (!buf) { fclose(f); return; }
	if (fread(buf, 1, len, f) != (size_t)len) { free(buf); fclose(f); return; }
	buf[len] = '\0';
	fclose(f);

	struct json_object *root = json_tokener_parse(buf);
	free(buf);
	if (!root) return;

	struct json_object *agy = json_object_object_get(root, "antigravity");
	if (!agy) {
		agy = json_object_new_object();
		json_object_object_add(root, "antigravity", agy);
	}
	json_object_object_add(agy, "access", json_object_new_string(access));
	json_object_object_add(agy, "expires", json_object_new_int64((int64_t)expires_ms));

	char tmp_path[1024];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", g_auth.auth_path, (int)getpid());
	FILE *out = fopen(tmp_path, "wb");
	if (out) {
		const char *s = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
		fwrite(s, 1, strlen(s), out);
		fclose(out);
		rename(tmp_path, g_auth.auth_path);
	}
	json_object_put(root);
}

static void
auth_save_copilot_locked(const char *access, uint64_t expires_ms)
{
	FILE *f = fopen(g_auth.auth_path, "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(len + 1);
	if (!buf) { fclose(f); return; }
	if (fread(buf, 1, len, f) != (size_t)len) { free(buf); fclose(f); return; }
	buf[len] = '\0';
	fclose(f);

	struct json_object *root = json_tokener_parse(buf);
	free(buf);
	if (!root) return;

	struct json_object *cop = json_object_object_get(root, "github-copilot");
	if (!cop) {
		cop = json_object_new_object();
		json_object_object_add(root, "github-copilot", cop);
	}
	json_object_object_add(cop, "access", json_object_new_string(access));
	json_object_object_add(cop, "expires", json_object_new_int64((int64_t)expires_ms));

	char tmp_path[1024];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", g_auth.auth_path, (int)getpid());
	FILE *out = fopen(tmp_path, "wb");
	if (out) {
		const char *s = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
		fwrite(s, 1, strlen(s), out);
		fclose(out);
		rename(tmp_path, g_auth.auth_path);
	}
	json_object_put(root);
}

struct mem_buf {
	char *data;
	size_t len;
	size_t cap;
};

static size_t
curl_write_mem(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t total = size * nmemb;
	struct mem_buf *mb = userdata;
	if (mb->len + total + 1 > mb->cap) {
		size_t ncap = (mb->cap == 0) ? 4096 : mb->cap * 2;
		while (ncap < mb->len + total + 1)
			ncap *= 2;
		char *nd = realloc(mb->data, ncap);
		if (!nd) return 0;
		mb->data = nd;
		mb->cap = ncap;
	}
	memcpy(mb->data + mb->len, ptr, total);
	mb->len += total;
	mb->data[mb->len] = '\0';
	return total;
}

static int
antigravity_refresh_token_locked(void)
{
	uint64_t now = current_time_ms();
	if (g_auth.agy_expires_ms > now + 60000 && g_auth.agy_access[0])
		return 0; /* token still valid for > 60s */

	if (!g_auth.agy_refresh[0])
		return -1;

	const char *cid = getenv("ANTIGRAVITY_CLIENT_ID");
	if (!cid || !cid[0]) cid = AGY_DEFAULT_CLIENT_ID;
	const char *csec = getenv("ANTIGRAVITY_CLIENT_SECRET");
	if (!csec || !csec[0]) csec = AGY_DEFAULT_CLIENT_SECRET;

	char post_data[2048];
	snprintf(post_data, sizeof(post_data),
		"client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
		cid, csec, g_auth.agy_refresh);

	CURL *curl = curl_easy_init();
	if (!curl) return -1;

	struct mem_buf mb = {0};
	curl_easy_setopt(curl, CURLOPT_URL, "https://oauth2.googleapis.com/token");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_mem);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

	CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK || http_code != 200 || !mb.data) {
		free(mb.data);
		return -1;
	}

	struct json_object *root = json_tokener_parse(mb.data);
	free(mb.data);
	if (!root) return -1;

	struct json_object *acc = json_object_object_get(root, "access_token");
	struct json_object *exp = json_object_object_get(root, "expires_in");
	if (!acc || !json_object_is_type(acc, json_type_string)) {
		json_object_put(root);
		return -1;
	}

	int expires_in = exp ? json_object_get_int(exp) : 3600;
	snprintf(g_auth.agy_access, sizeof(g_auth.agy_access), "%s", json_object_get_string(acc));
	g_auth.agy_expires_ms = current_time_ms() + (uint64_t)expires_in * 1000ULL - 300000ULL;

	auth_save_antigravity_locked(g_auth.agy_access, g_auth.agy_expires_ms);
	json_object_put(root);
	return 0;
}

static int
copilot_refresh_token_locked(void)
{
	uint64_t now = current_time_ms();
	if (g_auth.copilot_expires_ms > now + 60000 && g_auth.copilot_access[0])
		return 0;

	if (!g_auth.copilot_refresh[0])
		return -1;

	CURL *curl = curl_easy_init();
	if (!curl) return -1;

	struct curl_slist *headers = NULL;
	char auth_hdr[1024];
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", g_auth.copilot_refresh);
	headers = curl_slist_append(headers, auth_hdr);
	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, "User-Agent: " COPILOT_USER_AGENT);
	headers = curl_slist_append(headers, "Editor-Version: vscode/1.107.0");
	headers = curl_slist_append(headers, "Editor-Plugin-Version: copilot-chat/0.35.0");
	headers = curl_slist_append(headers, "Copilot-Integration-Id: vscode-chat");

	struct mem_buf mb = {0};
	curl_easy_setopt(curl, CURLOPT_URL, COPILOT_TOKEN_URL);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_mem);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

	CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK || http_code != 200 || !mb.data) {
		free(mb.data);
		return -1;
	}

	struct json_object *root = json_tokener_parse(mb.data);
	free(mb.data);
	if (!root) return -1;

	struct json_object *tok = json_object_object_get(root, "token");
	struct json_object *exp = json_object_object_get(root, "expires_at");
	if (!tok || !json_object_is_type(tok, json_type_string)) {
		json_object_put(root);
		return -1;
	}

	int64_t exp_sec = exp ? json_object_get_int64(exp) : (int64_t)(time(NULL) + 1800);
	snprintf(g_auth.copilot_access, sizeof(g_auth.copilot_access), "%s", json_object_get_string(tok));
	g_auth.copilot_expires_ms = (uint64_t)exp_sec * 1000ULL - 300000ULL;

	auth_save_copilot_locked(g_auth.copilot_access, g_auth.copilot_expires_ms);
	json_object_put(root);
	return 0;
}

int
auren_upstream_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	pthread_mutex_lock(&g_auth.lock);
	auth_load_locked();
	pthread_mutex_unlock(&g_auth.lock);
	return 0;
}

void
auren_upstream_shutdown(void)
{
	curl_global_cleanup();
}

static int
copilot_has_model(const char *model_id)
{
	pthread_mutex_lock(&g_auth.lock);
	for (int i = 0; i < g_auth.copilot_models_count; ++i) {
		if (strcmp(g_auth.copilot_models[i], model_id) == 0) {
			pthread_mutex_unlock(&g_auth.lock);
			return 1;
		}
	}
	pthread_mutex_unlock(&g_auth.lock);
	return 0;
}

/* Mapping table for Antigravity models and runtime wire IDs */
struct agy_model_map {
	const char *public_id;
	const char *default_wire;
	const char *low_wire;
	const char *med_wire;
	const char *high_wire;
	const char *xhigh_wire;
	int is_claude;
	int is_gpt_oss;
	int model_enum;
};

static const struct agy_model_map AGY_MAPS[] = {
	{ "gemini-3.8-flash", "gemini-3.8-flash-low", "gemini-3.8-flash-low", "gemini-3.8-flash-medium", "gemini-3.8-flash-high", "gemini-3.8-flash-high", 0, 0, 318 },
	{ "gemini-4-hydrogen", "gemini-4-hydrogen-high", "gemini-4-hydrogen-low", "gemini-4-hydrogen-medium", "gemini-4-hydrogen-high", "gemini-4-hydrogen-xhigh", 0, 0, 527 },
	{ "gemini-4-argon", "gemini-4-argon-high", "gemini-4-argon-low", "gemini-4-argon-medium", "gemini-4-argon-high", "gemini-4-argon-high", 0, 0, 0 },
	{ "gemini-4-carbon", "gemini-4-carbon", "gemini-4-carbon", "gemini-4-carbon", "gemini-4-carbon", "gemini-4-carbon", 0, 0, 0 },
	{ "gemini-3.1-pro", "gemini-3.1-pro-low", "gemini-3.1-pro-low", "gemini-3.1-pro-low", "gemini-pro-agent", "gemini-pro-agent", 0, 0, 36 },
	{ "gemini-pro-agent", "gemini-pro-agent", "gemini-3.1-pro-low", "gemini-3.1-pro-low", "gemini-pro-agent", "gemini-pro-agent", 0, 0, 16 },
	{ "gemini-3.7-flash", "gemini-3.7-flash-low", "gemini-3.7-flash-low", "gemini-3.7-flash-medium", "gemini-3.7-flash-high", "gemini-3.7-flash-high", 0, 0, 298 },
	{ "gemini-3.6-flash", "gemini-3.6-flash-low", "gemini-3.6-flash-low", "gemini-3.6-flash-medium", "gemini-3.6-flash-high", "gemini-3.6-flash-high", 0, 0, 71 },
	{ "gemini-3.5-flash", "gemini-3.5-flash-extra-low", "gemini-3.5-flash-extra-low", "gemini-3.5-flash-low", "gemini-3-flash-agent", "gemini-3-flash-agent", 0, 0, 20 },
	{ "claude-sonnet-4-6", "claude-sonnet-4-6", "claude-sonnet-4-6", "claude-sonnet-4-6", "claude-sonnet-4-6", "claude-sonnet-4-6", 1, 0, 35 },
	{ "claude-opus-4-6", "claude-opus-4-6-thinking", "claude-opus-4-6", "claude-opus-4-6-thinking", "claude-opus-4-6-thinking", "claude-opus-4-6-thinking", 1, 0, 26 },
	{ "claude-opus-4-6-thinking", "claude-opus-4-6-thinking", "claude-opus-4-6", "claude-opus-4-6-thinking", "claude-opus-4-6-thinking", "claude-opus-4-6-thinking", 1, 0, 26 },
	{ "gpt-oss-120b", "gpt-oss-120b-medium", "gpt-oss-120b-medium", "gpt-oss-120b-medium", "gpt-oss-120b-medium", "gpt-oss-120b-medium", 0, 1, 0 },
	{ "gpt-oss-120b-medium", "gpt-oss-120b-medium", "gpt-oss-120b-medium", "gpt-oss-120b-medium", "gpt-oss-120b-medium", "gpt-oss-120b-medium", 0, 1, 0 },
	{ NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0 }
};

enum auren_upstream_provider
auren_upstream_resolve_model(
	const char *model_id,
	const char *requested_effort,
	struct auren_upstream_route *out)
{
	if (!model_id || !model_id[0])
		return AUREN_UPSTREAM_NONE;

	if (out)
		memset(out, 0, sizeof(*out));

	const char *bare = model_id;
	enum auren_upstream_provider explicit_prov = AUREN_UPSTREAM_NONE;

	if (strncmp(bare, "antigravity/", 12) == 0) {
		explicit_prov = AUREN_UPSTREAM_ANTIGRAVITY;
		bare += 12;
	} else if (strncmp(bare, "kilo/", 5) == 0) {
		explicit_prov = AUREN_UPSTREAM_KILO;
		bare += 5;
	} else if (strncmp(bare, "copilot/", 8) == 0) {
		explicit_prov = AUREN_UPSTREAM_COPILOT;
		bare += 8;
	} else if (strncmp(bare, "github-copilot/", 15) == 0) {
		explicit_prov = AUREN_UPSTREAM_COPILOT;
		bare += 15;
	}

	if (out) {
		snprintf(out->requested_model, sizeof(out->requested_model), "%s", model_id);
		snprintf(out->canonical_id, sizeof(out->canonical_id), "%s", bare);
	}

	/* Smart Aliases */
	if (strcmp(bare, "fast") == 0 || strcmp(bare, "auto") == 0) {
		if (out) {
			out->provider = AUREN_UPSTREAM_ALIAS_FAST;
			snprintf(out->runtime_model, sizeof(out->runtime_model), "gemini-3.8-flash-low");
			snprintf(out->effort, sizeof(out->effort), "low");
			out->is_thinking = 1;
			out->model_enum = 320;
		}
		return AUREN_UPSTREAM_ALIAS_FAST;
	}

	if (strcmp(bare, "smart") == 0) {
		if (out) {
			out->provider = AUREN_UPSTREAM_ALIAS_SMART;
			snprintf(out->runtime_model, sizeof(out->runtime_model), "gemini-4-hydrogen-high");
			snprintf(out->effort, sizeof(out->effort), "high");
			out->is_thinking = 1;
			out->model_enum = 525;
		}
		return AUREN_UPSTREAM_ALIAS_SMART;
	}

	/* Kilo models */
	if (explicit_prov == AUREN_UPSTREAM_KILO || strcmp(bare, "kilo-auto/free") == 0 ||
	    strncmp(bare, "kilo-auto/", 10) == 0 || strncmp(model_id, "kilo/", 5) == 0) {
		if (out) {
			out->provider = AUREN_UPSTREAM_KILO;
			snprintf(out->runtime_model, sizeof(out->runtime_model), "%s", bare);
		}
		return AUREN_UPSTREAM_KILO;
	}

	/* Copilot explicit */
	if (explicit_prov == AUREN_UPSTREAM_COPILOT) {
		if (out) {
			out->provider = AUREN_UPSTREAM_COPILOT;
			snprintf(out->runtime_model, sizeof(out->runtime_model), "%s", bare);
		}
		return AUREN_UPSTREAM_COPILOT;
	}

	/* Parse effort suffix (e.g. "gemini-3.8-flash-medium" or requested_effort) */
	char base_clean[128];
	snprintf(base_clean, sizeof(base_clean), "%s", bare);
	char parsed_effort[32] = {0};

	if (requested_effort && requested_effort[0])
		snprintf(parsed_effort, sizeof(parsed_effort), "%s", requested_effort);

	static const char *suffixes[] = { "-extra-low", "-low", "-medium", "-high", "-xhigh", "-thinking", NULL };
	for (int s = 0; suffixes[s]; ++s) {
		size_t slen = strlen(suffixes[s]);
		size_t blen = strlen(base_clean);
		if (blen > slen && strcmp(base_clean + blen - slen, suffixes[s]) == 0) {
			if (!parsed_effort[0])
				snprintf(parsed_effort, sizeof(parsed_effort), "%s", suffixes[s] + 1);
			base_clean[blen - slen] = '\0';
			break;
		}
	}

	/* Check Antigravity catalog */
	for (int i = 0; AGY_MAPS[i].public_id; ++i) {
		if (strcmp(base_clean, AGY_MAPS[i].public_id) == 0 ||
		    strcmp(bare, AGY_MAPS[i].public_id) == 0) {
			if (out) {
				out->provider = AUREN_UPSTREAM_ANTIGRAVITY;
				out->is_claude = AGY_MAPS[i].is_claude;
				out->is_gpt_oss = AGY_MAPS[i].is_gpt_oss;
				out->is_thinking = 1;
				out->model_enum = AGY_MAPS[i].model_enum;
				snprintf(out->effort, sizeof(out->effort), "%s", parsed_effort[0] ? parsed_effort : "low");

				const char *target_wire = AGY_MAPS[i].default_wire;
				if (strcmp(parsed_effort, "high") == 0) target_wire = AGY_MAPS[i].high_wire;
				else if (strcmp(parsed_effort, "xhigh") == 0) target_wire = AGY_MAPS[i].xhigh_wire;
				else if (strcmp(parsed_effort, "medium") == 0) target_wire = AGY_MAPS[i].med_wire;
				else if (strcmp(parsed_effort, "low") == 0 || strcmp(parsed_effort, "extra-low") == 0)
					target_wire = AGY_MAPS[i].low_wire;

				snprintf(out->runtime_model, sizeof(out->runtime_model), "%s", target_wire);
			}
			return AUREN_UPSTREAM_ANTIGRAVITY;
		}
	}

	/* If explicit antigravity prefix was used for an unmapped model */
	if (explicit_prov == AUREN_UPSTREAM_ANTIGRAVITY) {
		if (out) {
			out->provider = AUREN_UPSTREAM_ANTIGRAVITY;
			snprintf(out->runtime_model, sizeof(out->runtime_model), "%s", bare);
			out->is_thinking = (strstr(bare, "gemini-") != NULL);
		}
		return AUREN_UPSTREAM_ANTIGRAVITY;
	}

	/* Check Copilot model list */
	if (copilot_has_model(bare)) {
		if (out) {
			out->provider = AUREN_UPSTREAM_COPILOT;
			snprintf(out->runtime_model, sizeof(out->runtime_model), "%s", bare);
		}
		return AUREN_UPSTREAM_COPILOT;
	}

	return AUREN_UPSTREAM_NONE;
}

void
auren_upstream_append_models_json(struct json_object *data_arr)
{
	if (!data_arr || !json_object_is_type(data_arr, json_type_array))
		return;

	time_t now = time(NULL);

	static const char *cloud_models[] = {
		"fast",
		"smart",
		"gemini-3.8-flash",
		"gemini-3.8-flash-low",
		"gemini-3.8-flash-medium",
		"gemini-3.8-flash-high",
		"gemini-4-hydrogen",
		"gemini-4-hydrogen-low",
		"gemini-4-hydrogen-medium",
		"gemini-4-hydrogen-high",
		"gemini-4-hydrogen-xhigh",
		"gemini-4-argon",
		"gemini-4-carbon",
		"gemini-3.1-pro",
		"gemini-pro-agent",
		"gemini-3.7-flash",
		"gemini-3.6-flash",
		"gemini-3.5-flash",
		"claude-sonnet-4-6",
		"claude-opus-4-6",
		"gpt-oss-120b",
		"kilo-auto/free",
		NULL
	};

	for (int i = 0; cloud_models[i]; ++i) {
		struct json_object *m = json_object_new_object();
		json_object_object_add(m, "id", json_object_new_string(cloud_models[i]));
		json_object_object_add(m, "object", json_object_new_string("model"));
		json_object_object_add(m, "created", json_object_new_int64((int64_t)now));
		const char *owner = "antigravity";
		if (strncmp(cloud_models[i], "kilo", 4) == 0) owner = "kilo";
		else if (strcmp(cloud_models[i], "fast") == 0 || strcmp(cloud_models[i], "smart") == 0) owner = "auren-router";
		json_object_object_add(m, "owned_by", json_object_new_string(owner));
		json_object_array_add(data_arr, m);
	}

	pthread_mutex_lock(&g_auth.lock);
	for (int i = 0; i < g_auth.copilot_models_count; ++i) {
		struct json_object *m = json_object_new_object();
		json_object_object_add(m, "id", json_object_new_string(g_auth.copilot_models[i]));
		json_object_object_add(m, "object", json_object_new_string("model"));
		json_object_object_add(m, "created", json_object_new_int64((int64_t)now));
		json_object_object_add(m, "owned_by", json_object_new_string("github-copilot"));
		json_object_array_add(data_arr, m);
	}
	pthread_mutex_unlock(&g_auth.lock);
}

char *
auren_upstream_sanitize_system_prompt(const char *input)
{
	if (!input)
		return strdup("");

	static const struct {
		const char *target;
		const char *repl;
	} rules[] = {
		{ "running inside pi, a coding agent harness", "running inside your development environment" },
		{ "inside pi, a coding agent harness", "inside your development environment" },
		{ "running inside pi", "running inside your development environment" },
		{ "inside pi", "inside your development environment" },
		{ "practical, and tool-aware.", "practical, and tool aware." },
		{ NULL, NULL }
	};

	char *curr = strdup(input);
	if (!curr) return NULL;

	for (int r = 0; rules[r].target; ++r) {
		char *pos = strstr(curr, rules[r].target);
		while (pos) {
			size_t prefix_len = (size_t)(pos - curr);
			size_t target_len = strlen(rules[r].target);
			size_t repl_len = strlen(rules[r].repl);
			size_t suffix_len = strlen(pos + target_len);

			char *next = malloc(prefix_len + repl_len + suffix_len + 1);
			if (!next) break;

			memcpy(next, curr, prefix_len);
			memcpy(next + prefix_len, rules[r].repl, repl_len);
			memcpy(next + prefix_len + repl_len, pos + target_len, suffix_len + 1);

			free(curr);
			curr = next;
			pos = strstr(curr + prefix_len + repl_len, rules[r].target);
		}
	}
	return curr;
}

/* Strip meta schema keys ($schema, $id, etc.) recursively */
static void
clean_schema_object(struct json_object *obj)
{
	if (!obj || !json_object_is_type(obj, json_type_object))
		return;

	json_object_object_del(obj, "$schema");
	json_object_object_del(obj, "$id");
	json_object_object_del(obj, "$defs");
	json_object_object_del(obj, "definitions");

	json_object_object_foreach(obj, key, val) {
		(void)key;
		if (json_object_is_type(val, json_type_object))
			clean_schema_object(val);
		else if (json_object_is_type(val, json_type_array)) {
			size_t len = json_object_array_length(val);
			for (size_t i = 0; i < len; ++i) {
				struct json_object *item = json_object_array_get_idx(val, i);
				if (json_object_is_type(item, json_type_object))
					clean_schema_object(item);
			}
		}
	}
}

struct json_object *
auren_upstream_build_antigravity_request(
	struct json_object *openai_req,
	const struct auren_upstream_route *route,
	const char *project_id)
{
	if (!openai_req || !route)
		return NULL;

	struct json_object *env = json_object_new_object();
	json_object_object_add(env, "project", json_object_new_string(project_id && project_id[0] ? project_id : AGY_DEFAULT_PROJECT_ID));
	json_object_object_add(env, "model", json_object_new_string(route->runtime_model));
	json_object_object_add(env, "requestType", json_object_new_string("agent"));
	json_object_object_add(env, "userAgent", json_object_new_string("antigravity"));

	char uuid_buf[64], traj_buf[64], req_id[256];
	generate_uuid(uuid_buf, sizeof(uuid_buf));
	generate_uuid(traj_buf, sizeof(traj_buf));
	snprintf(req_id, sizeof(req_id), "agent/%s/%" PRIu64 "/%s/1", uuid_buf, current_time_ms(), traj_buf);
	json_object_object_add(env, "requestId", json_object_new_string(req_id));

	struct json_object *credits = json_object_new_array();
	json_object_array_add(credits, json_object_new_string("GOOGLE_ONE_AI"));
	json_object_object_add(env, "enabledCreditTypes", credits);

	/* Inner request object */
	struct json_object *req = json_object_new_object();

	/* Labels */
	struct json_object *labels = json_object_new_object();
	json_object_object_add(labels, "last_step_index", json_object_new_string("0"));
	char step_req_id[128];
	snprintf(step_req_id, sizeof(step_req_id), "%s-0", traj_buf);
	json_object_object_add(labels, "request_id", json_object_new_string(step_req_id));
	json_object_object_add(labels, "trajectory_id", json_object_new_string(traj_buf));
	json_object_object_add(labels, "used_claude", json_object_new_string(route->is_claude ? "true" : "false"));
	json_object_object_add(labels, "used_claude_conservative", json_object_new_string(route->is_claude ? "true" : "false"));
	json_object_object_add(labels, "used_non_gemini_model", json_object_new_string((route->is_claude || route->is_gpt_oss) ? "true" : "false"));
	if (route->model_enum > 0) {
		char enum_buf[64];
		snprintf(enum_buf, sizeof(enum_buf), "MODEL_PLACEHOLDER_M%d", route->model_enum);
		json_object_object_add(labels, "model_enum", json_object_new_string(enum_buf));
	}
	json_object_object_add(req, "labels", labels);

	/* Session ID */
	char sess_id_str[64];
	uint64_t snum = (current_time_ms() ^ 0x5a5a5a5a5a5a5a5aULL);
	snprintf(sess_id_str, sizeof(sess_id_str), "%" PRIu64, snum);
	json_object_object_add(req, "sessionId", json_object_new_string(sess_id_str));

	/* Parse Messages */
	struct json_object *messages = json_object_object_get(openai_req, "messages");
	struct json_object *contents = json_object_new_array();
	/* Pre-scan tool_calls to resolve tool_call_id -> tool_name for tool responses */
	struct {
		char id[128];
		char name[128];
	} tool_id_map[64];
	int tool_id_map_count = 0;

	if (messages && json_object_is_type(messages, json_type_array)) {
		size_t n_msgs = json_object_array_length(messages);
		for (size_t i = 0; i < n_msgs; ++i) {
			struct json_object *m = json_object_array_get_idx(messages, i);
			struct json_object *tc_arr = m ? json_object_object_get(m, "tool_calls") : NULL;
			if (tc_arr && json_object_is_type(tc_arr, json_type_array)) {
				size_t n_tc = json_object_array_length(tc_arr);
				for (size_t j = 0; j < n_tc && tool_id_map_count < 64; ++j) {
					struct json_object *tc = json_object_array_get_idx(tc_arr, j);
					struct json_object *id_o = tc ? json_object_object_get(tc, "id") : NULL;
					struct json_object *fn = tc ? json_object_object_get(tc, "function") : NULL;
					struct json_object *fn_name = fn ? json_object_object_get(fn, "name") : NULL;
					if (id_o && fn_name) {
						snprintf(tool_id_map[tool_id_map_count].id, sizeof(tool_id_map[0].id), "%s", json_object_get_string(id_o));
						snprintf(tool_id_map[tool_id_map_count].name, sizeof(tool_id_map[0].name), "%s", json_object_get_string(fn_name));
						tool_id_map_count++;
					}
				}
			}
		}
	}

	char system_acc[16384] = {0};
	if (messages && json_object_is_type(messages, json_type_array)) {
		size_t n_msgs = json_object_array_length(messages);
		for (size_t i = 0; i < n_msgs; ++i) {
			struct json_object *msg = json_object_array_get_idx(messages, i);
			if (!msg || !json_object_is_type(msg, json_type_object))
				continue;

			struct json_object *r_obj = json_object_object_get(msg, "role");
			const char *role = r_obj ? json_object_get_string(r_obj) : "user";
			struct json_object *c_obj = json_object_object_get(msg, "content");

			if (strcmp(role, "system") == 0 || strcmp(role, "developer") == 0) {
				const char *text = c_obj ? json_object_get_string(c_obj) : "";
				if (text && text[0]) {
					if (system_acc[0]) strncat(system_acc, "\n\n", sizeof(system_acc) - strlen(system_acc) - 1);
					strncat(system_acc, text, sizeof(system_acc) - strlen(system_acc) - 1);
				}
				continue;
			}

			/* Non-system message -> contents */
			struct json_object *content_turn = json_object_new_object();
			struct json_object *parts = json_object_new_array();

			if (strcmp(role, "assistant") == 0) {
				json_object_object_add(content_turn, "role", json_object_new_string("model"));
				/* Check text */
				if (c_obj && json_object_is_type(c_obj, json_type_string)) {
					const char *txt = json_object_get_string(c_obj);
					if (txt && txt[0]) {
						struct json_object *p = json_object_new_object();
						json_object_object_add(p, "text", json_object_new_string(txt));
						json_object_array_add(parts, p);
					}
				}
				/* Check tool_calls */
				struct json_object *tc_arr = json_object_object_get(msg, "tool_calls");
				if (tc_arr && json_object_is_type(tc_arr, json_type_array)) {
					size_t num_tc = json_object_array_length(tc_arr);
					for (size_t tc_i = 0; tc_i < num_tc; ++tc_i) {
						struct json_object *tc = json_object_array_get_idx(tc_arr, tc_i);
						struct json_object *fn = tc ? json_object_object_get(tc, "function") : NULL;
						struct json_object *fn_name = fn ? json_object_object_get(fn, "name") : NULL;
						struct json_object *fn_args = fn ? json_object_object_get(fn, "arguments") : NULL;
						struct json_object *fn_id = tc ? json_object_object_get(tc, "id") : NULL;

						const char *name_str = fn_name ? json_object_get_string(fn_name) : "tool";
						const char *args_str = fn_args ? json_object_get_string(fn_args) : "{}";
						const char *id_str = fn_id ? json_object_get_string(fn_id) : "";

						/* Gemini 3/4 requires thoughtSignature. Check if cached */
						char sig_buf[512] = {0};
						int has_sig = auren_upstream_lookup_thought_sig(id_str, sig_buf, sizeof(sig_buf));

						struct json_object *part = json_object_new_object();
						struct json_object *fc = json_object_new_object();
						json_object_object_add(fc, "name", json_object_new_string(name_str));
						struct json_object *parsed_args = json_tokener_parse(args_str);
						if (!parsed_args) parsed_args = json_object_new_object();
						json_object_object_add(fc, "args", parsed_args);
						if (id_str[0])
							json_object_object_add(fc, "id", json_object_new_string(id_str));

						json_object_object_add(part, "functionCall", fc);
						if (has_sig)
							json_object_object_add(part, "thoughtSignature", json_object_new_string(sig_buf));

						json_object_array_add(parts, part);
					}
				}
			} else if (strcmp(role, "tool") == 0) {
				json_object_object_add(content_turn, "role", json_object_new_string("user"));
				struct json_object *tid = json_object_object_get(msg, "tool_call_id");
				const char *tid_str = tid ? json_object_get_string(tid) : "";
				const char *raw_res = c_obj ? json_object_get_string(c_obj) : "";
				struct json_object *name_obj = json_object_object_get(msg, "name");
				const char *tool_name = name_obj ? json_object_get_string(name_obj) : NULL;
				if (!tool_name && tid_str && tid_str[0]) {
					for (int m = 0; m < tool_id_map_count; ++m) {
						if (strcmp(tool_id_map[m].id, tid_str) == 0) {
							tool_name = tool_id_map[m].name;
							break;
						}
					}
				}
				if (!tool_name) tool_name = "tool";

				struct json_object *part = json_object_new_object();
				struct json_object *fr = json_object_new_object();
				json_object_object_add(fr, "name", json_object_new_string(tool_name));
				if (route->is_claude || route->is_gpt_oss) {
					if (tid_str && tid_str[0])
						json_object_object_add(fr, "id", json_object_new_string(tid_str));
				}

				struct json_object *resp_obj = NULL;
				if (raw_res && raw_res[0] == '{') {
					resp_obj = json_tokener_parse(raw_res);
					if (resp_obj && !json_object_is_type(resp_obj, json_type_object)) {
						json_object_put(resp_obj);
						resp_obj = NULL;
					}
				}
				if (!resp_obj) {
					resp_obj = json_object_new_object();
					json_object_object_add(resp_obj, "output", json_object_new_string(raw_res ? raw_res : ""));
				}
				json_object_object_add(fr, "response", resp_obj);
				json_object_object_add(part, "functionResponse", fr);
				json_object_array_add(parts, part);
			} else {
				/* user */
				json_object_object_add(content_turn, "role", json_object_new_string("user"));
				if (c_obj && json_object_is_type(c_obj, json_type_string)) {
					struct json_object *p = json_object_new_object();
					json_object_object_add(p, "text", json_object_new_string(json_object_get_string(c_obj)));
					json_object_array_add(parts, p);
				} else if (c_obj && json_object_is_type(c_obj, json_type_array)) {
					size_t n_parts = json_object_array_length(c_obj);
					for (size_t p_i = 0; p_i < n_parts; ++p_i) {
						struct json_object *elem = json_object_array_get_idx(c_obj, p_i);
						struct json_object *type_o = elem ? json_object_object_get(elem, "type") : NULL;
						const char *type_s = type_o ? json_object_get_string(type_o) : "text";

						if (strcmp(type_s, "text") == 0) {
							struct json_object *to = json_object_object_get(elem, "text");
							if (to) {
								struct json_object *p = json_object_new_object();
								json_object_object_add(p, "text", json_object_new_string(json_object_get_string(to)));
								json_object_array_add(parts, p);
							}
						} else if (strcmp(type_s, "image_url") == 0) {
							struct json_object *iu = json_object_object_get(elem, "image_url");
							struct json_object *urlo = iu ? json_object_object_get(iu, "url") : NULL;
							const char *url_s = urlo ? json_object_get_string(urlo) : "";
							if (strncmp(url_s, "data:", 5) == 0) {
								const char *comma = strchr(url_s, ',');
								if (comma) {
									char mime[64] = "image/png";
									const char *semi = strchr(url_s + 5, ';');
									if (semi && semi > url_s + 5 && (size_t)(semi - (url_s + 5)) < sizeof(mime)) {
										memcpy(mime, url_s + 5, (size_t)(semi - (url_s + 5)));
										mime[semi - (url_s + 5)] = '\0';
									}
									struct json_object *p = json_object_new_object();
									struct json_object *id = json_object_new_object();
									json_object_object_add(id, "mimeType", json_object_new_string(mime));
									json_object_object_add(id, "data", json_object_new_string(comma + 1));
									json_object_object_add(p, "inlineData", id);
									json_object_array_add(parts, p);
								}
							}
						}
					}
				}
			}

			if (json_object_array_length(parts) > 0) {
				json_object_object_add(content_turn, "parts", parts);
				json_object_array_add(contents, content_turn);
			} else {
				json_object_put(parts);
				json_object_put(content_turn);
			}
		}
	}
	json_object_object_add(req, "contents", contents);

	/* System Instruction */
	if (system_acc[0]) {
		char *sanitized = auren_upstream_sanitize_system_prompt(system_acc);
		struct json_object *sys_inst = json_object_new_object();
		json_object_object_add(sys_inst, "role", json_object_new_string("user"));
		struct json_object *sparts = json_object_new_array();
		struct json_object *sp = json_object_new_object();
		json_object_object_add(sp, "text", json_object_new_string(sanitized ? sanitized : system_acc));
		json_object_array_add(sparts, sp);
		json_object_object_add(sys_inst, "parts", sparts);
		json_object_object_add(req, "systemInstruction", sys_inst);
		free(sanitized);
	}

	/* Tools declarations */
	struct json_object *openai_tools = json_object_object_get(openai_req, "tools");
	if (openai_tools && json_object_is_type(openai_tools, json_type_array) &&
	    json_object_array_length(openai_tools) > 0) {
		struct json_object *tools_arr = json_object_new_array();
		struct json_object *tool_entry = json_object_new_object();
		struct json_object *fn_decls = json_object_new_array();

		size_t n_tools = json_object_array_length(openai_tools);
		for (size_t i = 0; i < n_tools; ++i) {
			struct json_object *item = json_object_array_get_idx(openai_tools, i);
			struct json_object *fn = item ? json_object_object_get(item, "function") : NULL;
			if (!fn) continue;

			struct json_object *decl = json_object_new_object();
			struct json_object *name = json_object_object_get(fn, "name");
			struct json_object *desc = json_object_object_get(fn, "description");
			struct json_object *params = json_object_object_get(fn, "parameters");

			if (name) json_object_object_add(decl, "name", json_object_get(name));
			if (desc) json_object_object_add(decl, "description", json_object_get(desc));

			if (params && json_object_is_type(params, json_type_object)) {
				struct json_object *params_copy = json_tokener_parse(json_object_to_json_string(params));
				clean_schema_object(params_copy);
				if (route->is_claude || route->is_gpt_oss) {
					json_object_object_add(decl, "parameters", params_copy);
				} else {
					json_object_object_add(decl, "parametersJsonSchema", params_copy);
				}
			}
			json_object_array_add(fn_decls, decl);
		}

		json_object_object_add(tool_entry, "functionDeclarations", fn_decls);
		json_object_array_add(tools_arr, tool_entry);
		json_object_object_add(req, "tools", tools_arr);
	}

	/* Generation Config */
	struct json_object *gen_cfg = json_object_new_object();
	int max_out = 65536;
	if (route->is_claude) max_out = 64000;
	else if (route->is_gpt_oss) max_out = 32768;
	else if (strstr(route->runtime_model, "gemini-4-hydrogen")) max_out = 262144;
	json_object_object_add(gen_cfg, "maxOutputTokens", json_object_new_int(max_out));

	if (route->is_thinking) {
		struct json_object *tc = json_object_new_object();
		json_object_object_add(tc, "includeThoughts", json_object_new_boolean(1));
		int budget = -1;
		if (strcmp(route->effort, "low") == 0) budget = 1000;
		else if (strcmp(route->effort, "medium") == 0) budget = 4000;
		else if (strcmp(route->effort, "high") == 0 || strcmp(route->effort, "xhigh") == 0) budget = -1;
		json_object_object_add(tc, "thinkingBudget", json_object_new_int(budget));
		json_object_object_add(gen_cfg, "thinkingConfig", tc);
	}
	json_object_object_add(req, "generationConfig", gen_cfg);

	json_object_object_add(env, "request", req);
	return env;
}

/* Worker context for asynchronous curl request */
struct upstream_worker_job {
	struct auren_upstream_route route;
	char *request_json;
	int stream;
	void *ud;
	int job_id;
};

/* State for processing streamed Cloud Code SSE */
struct agy_stream_parser {
	int stream;
	int job_id;
	void *ud;
	const char *model_id;
	char line_buf[65536];
	size_t line_len;
	char full_content[131072];
	size_t full_content_len;
	int tc_index;
	int emitted_any_chunk;
	int has_tool_calls;
	struct json_object *tool_calls_arr;
};

static void
agy_emit_chunk(struct agy_stream_parser *p, const char *delta_key, const char *delta_val)
{
	if (!p->stream) {
		if (strcmp(delta_key, "content") == 0 && delta_val) {
			size_t vlen = strlen(delta_val);
			if (p->full_content_len + vlen < sizeof(p->full_content) - 1) {
				memcpy(p->full_content + p->full_content_len, delta_val, vlen);
				p->full_content_len += vlen;
				p->full_content[p->full_content_len] = '\0';
			}
		}
		return;
	}

	struct json_object *obj = json_object_new_object();
	char id[64];
	snprintf(id, sizeof(id), "chatcmpl-%d", p->job_id);
	json_object_object_add(obj, "id", json_object_new_string(id));
	json_object_object_add(obj, "object", json_object_new_string("chat.completion.chunk"));
	json_object_object_add(obj, "created", json_object_new_int64((int64_t)time(NULL)));
	json_object_object_add(obj, "model", json_object_new_string(p->model_id ? p->model_id : "gemini"));

	struct json_object *choices = json_object_new_array();
	struct json_object *choice = json_object_new_object();
	struct json_object *delta = json_object_new_object();

	json_object_object_add(delta, delta_key, json_object_new_string(delta_val ? delta_val : ""));
	json_object_object_add(choice, "delta", delta);
	json_object_object_add(choice, "index", json_object_new_int(0));
	json_object_object_add(choice, "finish_reason", NULL);
	json_object_array_add(choices, choice);
	json_object_object_add(obj, "choices", choices);

	const char *s = json_object_to_json_string(obj);
	if (s) {
		struct auren_engine_result r = {
			.job_id = p->job_id,
			.kind = AUREN_JOB_CHAT_ONESHOT,
			.err = 0,
			.more = 1,
			.payload = strdup(s),
			.ud = p->ud,
		};
		auren_engine_push_external_result(&r);
		p->emitted_any_chunk = 1;
	}
	json_object_put(obj);
}

static void
agy_emit_tool_call(struct agy_stream_parser *p, const char *call_id, const char *name, const char *args_json)
{
	p->has_tool_calls = 1;

	if (!p->tool_calls_arr)
		p->tool_calls_arr = json_object_new_array();

	struct json_object *saved_tc = json_object_new_object();
	json_object_object_add(saved_tc, "id", json_object_new_string(call_id));
	json_object_object_add(saved_tc, "type", json_object_new_string("function"));
	struct json_object *saved_fn = json_object_new_object();
	json_object_object_add(saved_fn, "name", json_object_new_string(name));
	json_object_object_add(saved_fn, "arguments", json_object_new_string(args_json));
	json_object_object_add(saved_tc, "function", saved_fn);
	json_object_array_add(p->tool_calls_arr, saved_tc);

	if (!p->stream)
		return; /* handled in final completion if non-streaming */

	struct json_object *obj = json_object_new_object();
	char id[64];
	snprintf(id, sizeof(id), "chatcmpl-%d", p->job_id);
	json_object_object_add(obj, "id", json_object_new_string(id));
	json_object_object_add(obj, "object", json_object_new_string("chat.completion.chunk"));
	json_object_object_add(obj, "created", json_object_new_int64((int64_t)time(NULL)));
	json_object_object_add(obj, "model", json_object_new_string(p->model_id ? p->model_id : "gemini"));

	struct json_object *choices = json_object_new_array();
	struct json_object *choice = json_object_new_object();
	struct json_object *delta = json_object_new_object();
	struct json_object *tc_arr = json_object_new_array();
	struct json_object *tc = json_object_new_object();

	json_object_object_add(tc, "index", json_object_new_int(p->tc_index++));
	json_object_object_add(tc, "id", json_object_new_string(call_id));
	json_object_object_add(tc, "type", json_object_new_string("function"));

	struct json_object *fn = json_object_new_object();
	json_object_object_add(fn, "name", json_object_new_string(name));
	json_object_object_add(fn, "arguments", json_object_new_string(args_json));
	json_object_object_add(tc, "function", fn);

	json_object_array_add(tc_arr, tc);
	json_object_object_add(delta, "tool_calls", tc_arr);
	json_object_object_add(choice, "delta", delta);
	json_object_object_add(choice, "index", json_object_new_int(0));
	json_object_object_add(choice, "finish_reason", NULL);
	json_object_array_add(choices, choice);
	json_object_object_add(obj, "choices", choices);

	const char *s = json_object_to_json_string(obj);
	if (s) {
		struct auren_engine_result r = {
			.job_id = p->job_id,
			.kind = AUREN_JOB_CHAT_ONESHOT,
			.err = 0,
			.more = 1,
			.payload = strdup(s),
			.ud = p->ud,
		};
		auren_engine_push_external_result(&r);
		p->emitted_any_chunk = 1;
	}
	json_object_put(obj);
}

static void
agy_parse_sse_line(struct agy_stream_parser *p, const char *line)
{
	if (strncmp(line, "data: ", 6) != 0)
		return;

	const char *json_str = line + 6;
	if (strcmp(json_str, "[DONE]") == 0)
		return;

	struct json_object *root = json_tokener_parse(json_str);
	if (!root || !json_object_is_type(root, json_type_object)) {
		if (root) json_object_put(root);
		return;
	}

	struct json_object *resp = json_object_object_get(root, "response");
	if (!resp) resp = root;

	struct json_object *cands = json_object_object_get(resp, "candidates");
	if (cands && json_object_is_type(cands, json_type_array) && json_object_array_length(cands) > 0) {
		struct json_object *cand = json_object_array_get_idx(cands, 0);
		struct json_object *content = cand ? json_object_object_get(cand, "content") : NULL;
		struct json_object *parts = content ? json_object_object_get(content, "parts") : NULL;

		if (parts && json_object_is_type(parts, json_type_array)) {
			size_t n_parts = json_object_array_length(parts);
			for (size_t i = 0; i < n_parts; ++i) {
				struct json_object *part = json_object_array_get_idx(parts, i);
				if (!part) continue;

				struct json_object *th = json_object_object_get(part, "thought");
				int is_thought = (th && json_object_get_boolean(th));

				struct json_object *to = json_object_object_get(part, "text");
				if (to && json_object_is_type(to, json_type_string)) {
					const char *txt = json_object_get_string(to);
					if (txt && txt[0]) {
						if (is_thought)
							agy_emit_chunk(p, "reasoning_content", txt);
						else
							agy_emit_chunk(p, "content", txt);
					}
				}

				struct json_object *fc = json_object_object_get(part, "functionCall");
				if (fc && json_object_is_type(fc, json_type_object)) {
					struct json_object *name_o = json_object_object_get(fc, "name");
					struct json_object *args_o = json_object_object_get(fc, "args");
					struct json_object *id_o = json_object_object_get(fc, "id");
					struct json_object *tsig_o = json_object_object_get(part, "thoughtSignature");

					const char *fn_name = name_o ? json_object_get_string(name_o) : "tool";
					const char *fn_args = args_o ? json_object_to_json_string(args_o) : "{}";
					char call_id[64];
					if (id_o && json_object_is_type(id_o, json_type_string))
						snprintf(call_id, sizeof(call_id), "%s", json_object_get_string(id_o));
					else
						snprintf(call_id, sizeof(call_id), "call_%s_%d", fn_name, p->tc_index + 1);

					if (tsig_o && json_object_is_type(tsig_o, json_type_string))
						auren_upstream_cache_thought_sig(call_id, json_object_get_string(tsig_o));

					agy_emit_tool_call(p, call_id, fn_name, fn_args);
				}
			}
		}
	}
	json_object_put(root);
}

static size_t
curl_write_agy_stream(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t total = size * nmemb;
	struct agy_stream_parser *p = userdata;
	const char *src = ptr;

	for (size_t i = 0; i < total; ++i) {
		char c = src[i];
		if (c == '\n') {
			p->line_buf[p->line_len] = '\0';
			if (p->line_len > 0)
				agy_parse_sse_line(p, p->line_buf);
			p->line_len = 0;
		} else if (c != '\r') {
			if (p->line_len < sizeof(p->line_buf) - 1)
				p->line_buf[p->line_len++] = c;
		}
	}
	return total;
}

/* State for processing OpenAI pass-through SSE (Kilo & Copilot) */
struct openai_stream_parser {
	int stream;
	int job_id;
	void *ud;
	char line_buf[65536];
	size_t line_len;
	struct mem_buf full_body;
	int emitted_any;
};

static void
openai_parse_sse_line(struct openai_stream_parser *p, const char *line)
{
	if (strncmp(line, "data: ", 6) != 0)
		return;

	const char *payload = line + 6;
	if (strcmp(payload, "[DONE]") == 0)
		return;

	struct auren_engine_result r = {
		.job_id = p->job_id,
		.kind = AUREN_JOB_CHAT_ONESHOT,
		.err = 0,
		.more = 1,
		.payload = strdup(payload),
		.ud = p->ud,
	};
	auren_engine_push_external_result(&r);
	p->emitted_any = 1;
}

static size_t
curl_write_openai_stream(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t total = size * nmemb;
	struct openai_stream_parser *p = userdata;

	if (!p->stream) {
		curl_write_mem(ptr, size, nmemb, &p->full_body);
		return total;
	}

	const char *src = ptr;
	for (size_t i = 0; i < total; ++i) {
		char c = src[i];
		if (c == '\n') {
			p->line_buf[p->line_len] = '\0';
			if (p->line_len > 0)
				openai_parse_sse_line(p, p->line_buf);
			p->line_len = 0;
		} else if (c != '\r') {
			if (p->line_len < sizeof(p->line_buf) - 1)
				p->line_buf[p->line_len++] = c;
		}
	}
	return total;
}

static int
execute_antigravity_request(struct upstream_worker_job *job)
{
	pthread_mutex_lock(&g_auth.lock);
	if (antigravity_refresh_token_locked() != 0) {
		pthread_mutex_unlock(&g_auth.lock);
		return -1;
	}
	char access_token[1024];
	char project_id[128];
	snprintf(access_token, sizeof(access_token), "%s", g_auth.agy_access);
	snprintf(project_id, sizeof(project_id), "%s", g_auth.agy_project_id);
	pthread_mutex_unlock(&g_auth.lock);

	struct json_object *openai_req = json_tokener_parse(job->request_json);
	if (!openai_req)
		return -1;

	struct json_object *agy_req = auren_upstream_build_antigravity_request(openai_req, &job->route, project_id);
	json_object_put(openai_req);
	if (!agy_req)
		return -1;

	const char *body_str = json_object_to_json_string(agy_req);

	CURL *curl = curl_easy_init();
	if (!curl) {
		json_object_put(agy_req);
		return -1;
	}

	struct curl_slist *headers = NULL;
	char auth_hdr[1100];
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", access_token);
	headers = curl_slist_append(headers, auth_hdr);
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "User-Agent: " AGY_USER_AGENT);

	struct agy_stream_parser parser = {
		.stream = job->stream,
		.job_id = job->job_id,
		.ud = job->ud,
		.model_id = job->route.requested_model,
	};

	char endpoint_url[256];
	snprintf(endpoint_url, sizeof(endpoint_url), "%s/v1internal:streamGenerateContent?alt=sse", AGY_DEFAULT_ENDPOINT);

	curl_easy_setopt(curl, CURLOPT_URL, endpoint_url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_agy_stream);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &parser);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

	CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	/* If daily endpoint failed with 404 or connection error, try fallback endpoint */
	if ((res != CURLE_OK || http_code != 200) && !parser.emitted_any_chunk) {
		snprintf(endpoint_url, sizeof(endpoint_url), "%s/v1internal:streamGenerateContent?alt=sse", AGY_FALLBACK_ENDPOINT);
		curl_easy_setopt(curl, CURLOPT_URL, endpoint_url);
		res = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	json_object_put(agy_req);

	if (res != CURLE_OK || http_code != 200)
		return -1;

	/* Finish response */
	if (job->stream) {
		if (parser.tool_calls_arr)
			json_object_put(parser.tool_calls_arr);
		struct auren_engine_result fin = {
			.job_id = job->job_id,
			.kind = AUREN_JOB_CHAT_ONESHOT,
			.err = 0,
			.more = 0,
			.payload = NULL,
			.ud = job->ud,
		};
		auren_engine_push_external_result(&fin);
	} else {
		/* Build non-stream completion JSON */
		struct json_object *resp = json_object_new_object();
		char id[64];
		snprintf(id, sizeof(id), "chatcmpl-%d", job->job_id);
		json_object_object_add(resp, "id", json_object_new_string(id));
		json_object_object_add(resp, "object", json_object_new_string("chat.completion"));
		json_object_object_add(resp, "created", json_object_new_int64((int64_t)time(NULL)));
		json_object_object_add(resp, "model", json_object_new_string(job->route.requested_model));

		struct json_object *choices = json_object_new_array();
		struct json_object *choice = json_object_new_object();
		struct json_object *message = json_object_new_object();

		json_object_object_add(message, "role", json_object_new_string("assistant"));
		if (parser.full_content[0] || !parser.has_tool_calls)
			json_object_object_add(message, "content", json_object_new_string(parser.full_content));
		else
			json_object_object_add(message, "content", NULL);

		if (parser.tool_calls_arr && json_object_array_length(parser.tool_calls_arr) > 0)
			json_object_object_add(message, "tool_calls", parser.tool_calls_arr);
		else if (parser.tool_calls_arr)
			json_object_put(parser.tool_calls_arr);

		json_object_object_add(choice, "message", message);
		json_object_object_add(choice, "index", json_object_new_int(0));
		json_object_object_add(choice, "finish_reason", json_object_new_string(parser.has_tool_calls ? "tool_calls" : "stop"));
		json_object_array_add(choices, choice);
		json_object_object_add(resp, "choices", choices);

		struct auren_engine_result fin = {
			.job_id = job->job_id,
			.kind = AUREN_JOB_CHAT_ONESHOT,
			.err = 0,
			.more = 0,
			.payload = strdup(json_object_to_json_string(resp)),
			.ud = job->ud,
		};
		json_object_put(resp);
		auren_engine_push_external_result(&fin);
	}
	return 0;
}

static int
execute_kilo_request(struct upstream_worker_job *job)
{
	pthread_mutex_lock(&g_auth.lock);
	char access_token[1024];
	snprintf(access_token, sizeof(access_token), "%s", g_auth.kilo_access);
	pthread_mutex_unlock(&g_auth.lock);

	if (!access_token[0])
		return -1;

	/* Rewrite model in body if bare is kilo-auto/free */
	struct json_object *req_obj = json_tokener_parse(job->request_json);
	if (!req_obj) return -1;
	json_object_object_add(req_obj, "model", json_object_new_string(job->route.canonical_id));
	const char *body_str = json_object_to_json_string(req_obj);

	CURL *curl = curl_easy_init();
	if (!curl) {
		json_object_put(req_obj);
		return -1;
	}

	struct curl_slist *headers = NULL;
	char auth_hdr[1100];
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", access_token);
	headers = curl_slist_append(headers, auth_hdr);
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "X-KILOCODE-EDITORNAME: Pi");
	headers = curl_slist_append(headers, "User-Agent: " KILO_USER_AGENT);

	struct openai_stream_parser parser = {
		.stream = job->stream,
		.job_id = job->job_id,
		.ud = job->ud,
	};

	curl_easy_setopt(curl, CURLOPT_URL, KILO_DEFAULT_GATEWAY);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_openai_stream);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &parser);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

	CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	if ((res != CURLE_OK || http_code != 200) && !parser.emitted_any) {
		curl_easy_setopt(curl, CURLOPT_URL, KILO_FALLBACK_GATEWAY);
		res = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	json_object_put(req_obj);

	if (res != CURLE_OK || http_code != 200) {
		free(parser.full_body.data);
		return -1;
	}

	if (job->stream) {
		struct auren_engine_result fin = {
			.job_id = job->job_id,
			.kind = AUREN_JOB_CHAT_ONESHOT,
			.err = 0,
			.more = 0,
			.payload = NULL,
			.ud = job->ud,
		};
		auren_engine_push_external_result(&fin);
	} else {
		struct auren_engine_result fin = {
			.job_id = job->job_id,
			.kind = AUREN_JOB_CHAT_ONESHOT,
			.err = 0,
			.more = 0,
			.payload = parser.full_body.data,
			.ud = job->ud,
		};
		auren_engine_push_external_result(&fin);
	}
	return 0;
}

static int
execute_copilot_request(struct upstream_worker_job *job)
{
	pthread_mutex_lock(&g_auth.lock);
	if (copilot_refresh_token_locked() != 0) {
		pthread_mutex_unlock(&g_auth.lock);
		return -1;
	}
	char access_token[2048];
	char api_url[256];
	snprintf(access_token, sizeof(access_token), "%s", g_auth.copilot_access);
	snprintf(api_url, sizeof(api_url), "%s", g_auth.copilot_api_url[0] ? g_auth.copilot_api_url : COPILOT_DEFAULT_API);
	pthread_mutex_unlock(&g_auth.lock);

	struct json_object *req_obj = json_tokener_parse(job->request_json);
	if (!req_obj) return -1;
	json_object_object_add(req_obj, "model", json_object_new_string(job->route.canonical_id));
	const char *body_str = json_object_to_json_string(req_obj);

	CURL *curl = curl_easy_init();
	if (!curl) {
		json_object_put(req_obj);
		return -1;
	}

	struct curl_slist *headers = NULL;
	char auth_hdr[2100];
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", access_token);
	headers = curl_slist_append(headers, auth_hdr);
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "User-Agent: " COPILOT_USER_AGENT);
	headers = curl_slist_append(headers, "Editor-Version: vscode/1.107.0");
	headers = curl_slist_append(headers, "Editor-Plugin-Version: copilot-chat/0.35.0");
	headers = curl_slist_append(headers, "Copilot-Integration-Id: vscode-chat");
	headers = curl_slist_append(headers, "Openai-Intent: conversation-edits");
	headers = curl_slist_append(headers, "X-Initiator: user");

	struct openai_stream_parser parser = {
		.stream = job->stream,
		.job_id = job->job_id,
		.ud = job->ud,
	};

	curl_easy_setopt(curl, CURLOPT_URL, api_url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_openai_stream);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &parser);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

	CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	json_object_put(req_obj);

	if (res != CURLE_OK || http_code != 200) {
		free(parser.full_body.data);
		return -1;
	}

	if (job->stream) {
		struct auren_engine_result fin = {
			.job_id = job->job_id,
			.kind = AUREN_JOB_CHAT_ONESHOT,
			.err = 0,
			.more = 0,
			.payload = NULL,
			.ud = job->ud,
		};
		auren_engine_push_external_result(&fin);
	} else {
		struct auren_engine_result fin = {
			.job_id = job->job_id,
			.kind = AUREN_JOB_CHAT_ONESHOT,
			.err = 0,
			.more = 0,
			.payload = parser.full_body.data,
			.ud = job->ud,
		};
		auren_engine_push_external_result(&fin);
	}
	return 0;
}

static void *
upstream_worker_thread(void *arg)
{
	struct upstream_worker_job *job = arg;
	int ok = -1;

	if (job->route.provider == AUREN_UPSTREAM_ANTIGRAVITY) {
		ok = execute_antigravity_request(job);
	} else if (job->route.provider == AUREN_UPSTREAM_KILO) {
		ok = execute_kilo_request(job);
	} else if (job->route.provider == AUREN_UPSTREAM_COPILOT) {
		ok = execute_copilot_request(job);
	} else if (job->route.provider == AUREN_UPSTREAM_ALIAS_FAST || job->route.provider == AUREN_UPSTREAM_ALIAS_SMART) {
		/* Try Antigravity first */
		job->route.provider = AUREN_UPSTREAM_ANTIGRAVITY;
		ok = execute_antigravity_request(job);
		if (ok != 0) {
			/* Fallback to Kilo free */
			job->route.provider = AUREN_UPSTREAM_KILO;
			snprintf(job->route.canonical_id, sizeof(job->route.canonical_id), "kilo-auto/free");
			ok = execute_kilo_request(job);
		}
	}

	if (ok != 0) {
		/* Emit error completion */
		struct auren_engine_result err_res = {
			.job_id = job->job_id,
			.kind = AUREN_JOB_CHAT_ONESHOT,
			.err = -502,
			.more = 0,
			.payload = NULL,
			.ud = job->ud,
		};
		auren_engine_push_external_result(&err_res);
	}

	free(job->request_json);
	free(job);
	return NULL;
}

int
auren_upstream_submit_chat(
	const struct auren_upstream_route *route,
	const char *request_json,
	int stream,
	void *ud)
{
	if (!route || !request_json)
		return -1;

	int job_id = auren_engine_next_job_id();
	if (job_id < 0)
		return -1;

	struct upstream_worker_job *job = calloc(1, sizeof(*job));
	if (!job)
		return -1;

	job->route = *route;
	job->request_json = strdup(request_json);
	if (!job->request_json) {
		free(job);
		return -1;
	}
	job->stream = stream;
	job->ud = ud;
	job->job_id = job_id;

	pthread_t tid;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	if (pthread_create(&tid, &attr, upstream_worker_thread, job) != 0) {
		pthread_attr_destroy(&attr);
		free(job->request_json);
		free(job);
		return -1;
	}
	pthread_attr_destroy(&attr);
	return 0;
}
