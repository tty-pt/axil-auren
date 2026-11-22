/**
 * @file ren-reflex.c
 * @brief Lightweight Tier-0 deterministic reflex router for ren.
 *
 * Pure-C pattern matcher (<1ms) over registered ren tools (`media`,
 * `workspace`, `window`, `open_terminal`, `open_app`) plus optional user
 * overrides from ~/.config/ren/reflex.tsv. Fails open (returns 0) on
 * questions, multi-clause utterances, or failed/missing tools so the Tier-1
 * LLM handles anything ambiguous.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "ttypt/libren.h"
#include <ttypt/xy.h>

typedef struct {
	const char *pattern;
	const char *tool;
	const char *args_fmt;
	const char *spoken_fmt;
	const char *op_override;
} reflex_rule_t;

typedef struct {
	char *pattern;
	char *tool;
	char *args_fmt;
	char *spoken_fmt;
} reflex_user_rule_t;

#define MAX_USER_RULES 128

static reflex_user_rule_t g_user_rules[MAX_USER_RULES];
static int g_num_user_rules = 0;
static char g_loaded_path[1024] = {0};
static time_t g_loaded_mtime = 0;
static int g_loaded_once = 0;
static pthread_mutex_t g_reflex_mtx = PTHREAD_MUTEX_INITIALIZER;

static const reflex_rule_t g_builtin_rules[] = {
	/* Conversation control (handled natively by jarvisd via TIER0 op) */
	{ "exit conversation",      "_builtin", "Conversation ended.", "Okay.", "conversation_control" },
	{ "end conversation",       "_builtin", "Conversation ended.", "Okay.", "conversation_control" },
	{ "stop conversation",      "_builtin", "Conversation ended.", "Okay.", "conversation_control" },
	{ "never mind",             "_builtin", "Conversation ended.", "Okay.", "conversation_control" },
	{ "nevermind",              "_builtin", "Conversation ended.", "Okay.", "conversation_control" },
	{ "that's all",             "_builtin", "Conversation ended.", "Okay.", "conversation_control" },
	{ "thats all",              "_builtin", "Conversation ended.", "Okay.", "conversation_control" },
	{ "fim de conversa",        "_builtin", "Conversa terminada.", "Certo.", "conversation_control" },
	{ "terminar conversa",      "_builtin", "Conversa terminada.", "Certo.", "conversation_control" },
	{ "esquece",                "_builtin", "Conversa terminada.", "Certo.", "conversation_control" },
	{ "por agora \xc3\xa9 tudo","_builtin", "Conversa terminada.", "Certo.", "conversation_control" },
	{ "por agora e tudo",       "_builtin", "Conversa terminada.", "Certo.", "conversation_control" },

	/* Media playback & volume */
	{ "pause",                 "media", "pause",       "Paused.",                NULL },
	{ "pause media",           "media", "pause",       "Paused.",                NULL },
	{ "pause music",           "media", "pause",       "Paused.",                NULL },
	{ "pausa",                 "media", "pause",       "Pausado.",               NULL },
	{ "pausar",                "media", "pause",       "Pausado.",               NULL },
	{ "resume",                "media", "play",        "Resuming.",              "media_play" },
	{ "resume media",          "media", "play",        "Resuming.",              "media_play" },
	{ "resume music",          "media", "play",        "Resuming.",              "media_play" },
	{ "play",                  "media", "play",        "Playing.",               "media_play" },
	{ "play music",            "media", "play",        "Playing.",               "media_play" },
	{ "continua",              "media", "play",        "A retomar.",             "media_play" },
	{ "next track",            "media", "next",        "Next track.",            NULL },
	{ "next song",             "media", "next",        "Next track.",            NULL },
	{ "skip track",            "media", "next",        "Next track.",            NULL },
	{ "pr\xc3\xb3xima m\xc3\xbasica", "media", "next", "Pr\xc3\xb3xima m\xc3\xbasica.", NULL },
	{ "proxima musica",        "media", "next",        "Pr\xc3\xb3xima m\xc3\xbasica.", NULL },
	{ "previous track",        "media", "prev",        "Previous track.",        NULL },
	{ "previous song",         "media", "prev",        "Previous track.",        NULL },
	{ "m\xc3\xbasica anterior", "media", "prev",       "M\xc3\xbasica anterior.", NULL },
	{ "musica anterior",       "media", "prev",        "M\xc3\xbasica anterior.", NULL },
	{ "volume up",             "media", "volume_up",   "Volume up.",             NULL },
	{ "turn up the volume",    "media", "volume_up",   "Volume up.",             NULL },
	{ "louder",                "media", "volume_up",   "Volume up.",             NULL },
	{ "sobe o volume",         "media", "volume_up",   "Volume aumentado.",      NULL },
	{ "aumenta o volume",      "media", "volume_up",   "Volume aumentado.",      NULL },
	{ "volume down",           "media", "volume_down", "Volume down.",           NULL },
	{ "turn down the volume",  "media", "volume_down", "Volume down.",           NULL },
	{ "quieter",               "media", "volume_down", "Volume down.",           NULL },
	{ "baixa o volume",        "media", "volume_down", "Volume reduzido.",       NULL },
	{ "diminui o volume",      "media", "volume_down", "Volume reduzido.",       NULL },
	{ "mute",                  "media", "mute",        "Toggled mute.",          NULL },
	{ "unmute",                "media", "mute",        "Toggled mute.",          NULL },
	{ "mute audio",            "media", "mute",        "Toggled mute.",          NULL },
	{ "silencia",              "media", "mute",        "\xc3\x81udio silenciado.", NULL },

	/* Workspaces */
	{ "workspace {n}",                    "workspace", "{n}",      "Workspace {n}.",           NULL },
	{ "go to workspace {n}",              "workspace", "{n}",      "Workspace {n}.",           NULL },
	{ "switch to workspace {n}",          "workspace", "{n}",      "Workspace {n}.",           NULL },
	{ "tag {n}",                          "workspace", "{n}",      "Workspace {n}.",           NULL },
	{ "desktop {n}",                      "workspace", "{n}",      "Workspace {n}.",           NULL },
	{ "\xc3\xa1rea de trabalho {n}",      "workspace", "{n}",      "\xc3\x81rea de trabalho {n}.", NULL },
	{ "area de trabalho {n}",             "workspace", "{n}",      "\xc3\x81rea de trabalho {n}.", NULL },
	{ "vai para a \xc3\xa1rea de trabalho {n}", "workspace", "{n}", "\xc3\x81rea de trabalho {n}.", NULL },
	{ "vai para a area de trabalho {n}",  "workspace", "{n}",      "\xc3\x81rea de trabalho {n}.", NULL },
	{ "move to workspace {n}",            "workspace", "move {n}", "Moved to workspace {n}.",  NULL },
	{ "move window to workspace {n}",     "workspace", "move {n}", "Moved to workspace {n}.",  NULL },
	{ "send to workspace {n}",            "workspace", "move {n}", "Moved to workspace {n}.",  NULL },
	{ "manda para a \xc3\xa1rea de trabalho {n}", "workspace", "move {n}", "Movido para a \xc3\xa1rea {n}.", NULL },
	{ "move para a \xc3\xa1rea de trabalho {n}",  "workspace", "move {n}", "Movido para a \xc3\xa1rea {n}.", NULL },

	/* Window management */
	{ "close window",          "window", "close",          "Closed window.",     NULL },
	{ "close this window",     "window", "close",          "Closed window.",     NULL },
	{ "fecha a janela",        "window", "close",          "Janela fechada.",    NULL },
	{ "fullscreen",            "window", "fullscreen",     "Toggled fullscreen.", NULL },
	{ "toggle fullscreen",     "window", "fullscreen",     "Toggled fullscreen.", NULL },
	{ "ecr\xc3\xa3 inteiro",   "window", "fullscreen",     "Ecr\xc3\xa3 inteiro.", NULL },
	{ "ecra inteiro",          "window", "fullscreen",     "Ecr\xc3\xa3 inteiro.", NULL },
	{ "float window",          "window", "float",          "Toggled floating.",  NULL },
	{ "toggle floating",       "window", "float",          "Toggled floating.",  NULL },
	{ "other window",          "window", "focus other",    "Switched window.",   NULL },
	{ "switch window",         "window", "focus other",    "Switched window.",   NULL },
	{ "outra janela",          "window", "focus other",    "Janela alternada.",  NULL },
	{ "next window",           "window", "focus next",     "Next window.",       NULL },
	{ "pr\xc3\xb3xima janela", "window", "focus next",     "Pr\xc3\xb3xima janela.", NULL },
	{ "previous window",       "window", "focus previous", "Previous window.",   NULL },
	{ "focus master",          "window", "focus master",   "Focused master.",    NULL },
	{ "focus other",           "window", "focus other",    "Switched window.",   NULL },
	{ "focus next",            "window", "focus next",     "Next window.",       NULL },
	{ "focus previous",        "window", "focus previous", "Previous window.",   NULL },

	/* Terminal & explicit app launch */
	{ "open terminal",         "open_terminal", "",        "Opening terminal.",  NULL },
	{ "launch terminal",       "open_terminal", "",        "Opening terminal.",  NULL },
	{ "abre um terminal",      "open_terminal", "",        "A abrir terminal.",  NULL },
	{ "abre o terminal",       "open_terminal", "",        "A abrir terminal.",  NULL },
	{ "open firefox",          "open_app",      "firefox", "Opening Firefox.",   NULL },
	{ "open browser",          "open_app",      "firefox", "Opening browser.",   NULL },
	{ "abre o firefox",        "open_app",      "firefox", "A abrir o Firefox.", NULL },
	{ "open app {arg}",        "open_app",      "{arg}",   "Opening {arg}.",     NULL },
	{ "launch {arg}",          "open_app",      "{arg}",   "Launching {arg}.",   NULL },
	{ "abre app {arg}",        "open_app",      "{arg}",   "A abrir {arg}.",     NULL },
};

static void free_user_rules(void)
{
	for (int i = 0; i < g_num_user_rules; i++) {
		free(g_user_rules[i].pattern);
		free(g_user_rules[i].tool);
		free(g_user_rules[i].args_fmt);
		free(g_user_rules[i].spoken_fmt);
	}
	g_num_user_rules = 0;
}

static char *trim_dup(const char *s)
{
	if (!s) return strdup("");
	while (*s && isspace((unsigned char)*s)) s++;
	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
	char *out = malloc(len + 1);
	if (!out) return NULL;
	memcpy(out, s, len);
	out[len] = '\0';
	return out;
}

static void ensure_user_rules_loaded(void)
{
	const char *home = getenv("HOME");
	char path[1024] = {0};
	if (home && *home) {
		snprintf(path, sizeof(path), "%s/.config/ren/reflex.tsv", home);
	}

	struct stat st;
	int have_file = (path[0] && stat(path, &st) == 0);
	time_t mtime = have_file ? st.st_mtime : 0;

	if (g_loaded_once && strcmp(g_loaded_path, path) == 0 && g_loaded_mtime == mtime) {
		return;
	}

	free_user_rules();
	snprintf(g_loaded_path, sizeof(g_loaded_path), "%s", path);
	g_loaded_mtime = mtime;
	g_loaded_once = 1;

	if (!have_file) return;

	FILE *f = fopen(path, "r");
	if (!f) return;

	char line[1024];
	while (fgets(line, sizeof(line), f) && g_num_user_rules < MAX_USER_RULES) {
		size_t l = strlen(line);
		while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
		const char *p = line;
		while (*p && isspace((unsigned char)*p)) p++;
		if (!*p || *p == '#') continue;

		char *copy = strdup(p);
		if (!copy) break;

		char *t1 = strchr(copy, '\t');
		if (!t1) { free(copy); continue; }
		*t1 = '\0';
		char *f_pat = copy;
		char *f_tool = t1 + 1;
		char *t2 = strchr(f_tool, '\t');
		if (!t2) { free(copy); continue; }
		*t2 = '\0';
		char *f_args = t2 + 1;
		char *t3 = strchr(f_args, '\t');
		char *f_spoken = "";
		if (t3) {
			*t3 = '\0';
			f_spoken = t3 + 1;
		}

		char *pat = trim_dup(f_pat);
		char *tool = trim_dup(f_tool);
		char *args = trim_dup(f_args);
		char *spkn = trim_dup(f_spoken);
		free(copy);

		if (pat && *pat && tool && *tool) {
			for (char *c = pat; *c; c++) {
				if ((unsigned char)*c < 0x80) *c = (char)tolower((unsigned char)*c);
			}
			g_user_rules[g_num_user_rules].pattern = pat;
			g_user_rules[g_num_user_rules].tool = tool;
			g_user_rules[g_num_user_rules].args_fmt = args ? args : strdup("");
			g_user_rules[g_num_user_rules].spoken_fmt = spkn ? spkn : strdup("");
			g_num_user_rules++;
		} else {
			free(pat);
			free(tool);
			free(args);
			free(spkn);
		}
	}
	fclose(f);
}

static int normalize_prompt(const char *raw, char *out, size_t out_sz)
{
	if (!raw || !*raw || out_sz < 4) return 0;

	size_t rlen = strlen(raw);
	while (rlen > 0 && isspace((unsigned char)raw[rlen - 1])) rlen--;
	if (rlen == 0 || raw[rlen - 1] == '?') return 0;

	while (rlen > 0 && (raw[rlen - 1] == '.' || raw[rlen - 1] == '!' || isspace((unsigned char)raw[rlen - 1]))) {
		rlen--;
	}
	if (rlen == 0) return 0;

	size_t o = 0;
	int prev_space = 1;
	for (size_t i = 0; i < rlen && o + 1 < out_sz; i++) {
		unsigned char c = (unsigned char)raw[i];
		if (isspace(c)) {
			if (!prev_space) {
				out[o++] = ' ';
				prev_space = 1;
			}
		} else {
			out[o++] = (c < 0x80) ? (char)tolower(c) : (char)c;
			prev_space = 0;
		}
	}
	while (o > 0 && out[o - 1] == ' ') o--;
	out[o] = '\0';
	return o > 0;
}

static int has_multi_clause(const char *norm)
{
	if (strchr(norm, ',') || strchr(norm, ';') || strstr(norm, "&&")) return 1;
	static const char *const conns[] = {
		" and ", " then ", " after ", " before ", " e ", " depois ", NULL
	};
	for (int i = 0; conns[i]; i++) {
		if (strstr(norm, conns[i])) return 1;
	}
	return 0;
}

static int parse_number_token(const char *tok, char *out_num, size_t out_sz)
{
	if (!tok || !*tok) return 0;

	char *endp = NULL;
	if (isdigit((unsigned char)tok[0])) {
		long v = strtol(tok, &endp, 10);
		if (*endp == '\0' && v >= 1 && v <= 20) {
			snprintf(out_num, out_sz, "%ld", v);
			return 1;
		}
		return 0;
	}

	static const struct { const char *word; int val; } words[] = {
		{ "one",    1 }, { "first",  1 }, { "um",     1 }, { "uma",     1 },
		{ "two",    2 }, { "second", 2 }, { "dois",   2 }, { "duas",    2 },
		{ "three",  3 }, { "third",  3 }, { "tres",   3 }, { "tr\xc3\xaas", 3 },
		{ "four",   4 }, { "fourth", 4 }, { "quatro", 4 },
		{ "five",   5 }, { "fifth",  5 }, { "cinco",  5 },
		{ "six",    6 }, { "sixth",  6 }, { "seis",   6 },
		{ "seven",  7 }, { "seventh",7 }, { "sete",   7 },
		{ "eight",  8 }, { "eighth", 8 }, { "oito",   8 },
		{ "nine",   9 }, { "ninth",  9 }, { "nove",   9 },
		{ "ten",   10 }, { "tenth", 10 }, { "dez",   10 },
	};
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		if (strcmp(tok, words[i].word) == 0) {
			snprintf(out_num, out_sz, "%d", words[i].val);
			return 1;
		}
	}
	return 0;
}

static int match_pattern(const char *pat, const char *norm, char *cap_out, size_t cap_sz)
{
	if (cap_out && cap_sz > 0) cap_out[0] = '\0';

	const char *ph_n = strstr(pat, "{n}");
	const char *ph_arg = strstr(pat, "{arg}");

	if (!ph_n && !ph_arg) {
		return strcmp(pat, norm) == 0;
	}

	const char *ph = ph_n ? ph_n : ph_arg;
	size_t prefix_len = (size_t)(ph - pat);
	size_t ph_len = ph_n ? 3 : 5;
	const char *suffix = ph + ph_len;

	if (strncmp(norm, pat, prefix_len) != 0) return 0;
	const char *rest = norm + prefix_len;
	while (*rest == ' ') rest++;
	if (!*rest) return 0;

	if (ph_n) {
		size_t suf_len = strlen(suffix);
		size_t rest_len = strlen(rest);
		if (suf_len > 0) {
			if (rest_len <= suf_len) return 0;
			if (strcmp(rest + rest_len - suf_len, suffix) != 0) return 0;
			rest_len -= suf_len;
		}
		char tok[64];
		if (rest_len >= sizeof(tok)) return 0;
		memcpy(tok, rest, rest_len);
		tok[rest_len] = '\0';
		while (rest_len > 0 && tok[rest_len - 1] == ' ') tok[--rest_len] = '\0';
		return parse_number_token(tok, cap_out, cap_sz);
	}

	/* {arg} */
	size_t suf_len = strlen(suffix);
	size_t rest_len = strlen(rest);
	if (suf_len > 0) {
		if (rest_len <= suf_len) return 0;
		if (strcmp(rest + rest_len - suf_len, suffix) != 0) return 0;
		rest_len -= suf_len;
	}
	while (rest_len > 0 && rest[rest_len - 1] == ' ') rest_len--;
	if (rest_len == 0 || rest_len >= cap_sz) return 0;
	memcpy(cap_out, rest, rest_len);
	cap_out[rest_len] = '\0';
	return 1;
}

static void format_with_cap(const char *fmt, const char *cap, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	if (!fmt || !*fmt) {
		out[0] = '\0';
		return;
	}
	if (!cap) {
		snprintf(out, out_sz, "%s", fmt);
		return;
	}
	size_t o = 0;
	const char *p = fmt;
	size_t cap_len = strlen(cap);
	while (*p && o + 1 < out_sz) {
		size_t ph_len = 0;
		if (strncmp(p, "{n}", 3) == 0) ph_len = 3;
		else if (strncmp(p, "{arg}", 5) == 0) ph_len = 5;
		else if (strncmp(p, "%s", 2) == 0) ph_len = 2;

		if (ph_len > 0) {
			size_t avail = (out_sz - 1) - o;
			size_t copy_len = cap_len < avail ? cap_len : avail;
			if (copy_len > 0) {
				memcpy(out + o, cap, copy_len);
				o += copy_len;
			}
			p += ph_len;
		} else {
			out[o++] = *p++;
		}
	}
	out[o] = '\0';
}

static int run_matched_rule(const char *tool, const char *args_fmt,
                            const char *spoken_fmt, const char *op_override,
                            const char *cap,
                            char **out_op, char **out_output, char **out_spoken)
{
	char args_buf[512];
	format_with_cap(args_fmt, cap, args_buf, sizeof(args_buf));

	char *tool_out = NULL;
	if (strcmp(tool, "_builtin") == 0) {
		tool_out = strdup(args_buf[0] ? args_buf : "ok");
	} else {
		int rc = ren_call_tool(tool, args_buf, &tool_out);
		if (rc != 0) {
			free(tool_out);
			return 0; /* fail-open to Tier-1 LLM */
		}
	}

	char spoken_buf[512];
	if (spoken_fmt && *spoken_fmt) {
		format_with_cap(spoken_fmt, cap, spoken_buf, sizeof(spoken_buf));
	} else {
		snprintf(spoken_buf, sizeof(spoken_buf), "%s", tool_out ? tool_out : "ok");
	}

	if (out_op) *out_op = strdup((op_override && *op_override) ? op_override : tool);
	if (out_output) *out_output = tool_out ? tool_out : strdup("ok");
	else free(tool_out);
	if (out_spoken) *out_spoken = strdup(spoken_buf);
	return 1;
}

static int reflex_tier0_solve(const char *prompt, char **out_op, char **out_output, char **out_spoken)
{
	if (out_op) *out_op = NULL;
	if (out_output) *out_output = NULL;
	if (out_spoken) *out_spoken = NULL;

	char norm[512];
	if (!normalize_prompt(prompt, norm, sizeof(norm))) return 0;
	if (has_multi_clause(norm)) return 0;

	char cap[256];
	char tool_buf[128] = {0};
	char args_buf[512] = {0};
	char spoken_buf[512] = {0};
	int matched = 0;

	pthread_mutex_lock(&g_reflex_mtx);
	ensure_user_rules_loaded();
	for (int i = 0; i < g_num_user_rules; i++) {
		if (match_pattern(g_user_rules[i].pattern, norm, cap, sizeof(cap))) {
			snprintf(tool_buf, sizeof(tool_buf), "%s", g_user_rules[i].tool ? g_user_rules[i].tool : "");
			snprintf(args_buf, sizeof(args_buf), "%s", g_user_rules[i].args_fmt ? g_user_rules[i].args_fmt : "");
			snprintf(spoken_buf, sizeof(spoken_buf), "%s", g_user_rules[i].spoken_fmt ? g_user_rules[i].spoken_fmt : "");
			matched = 1;
			break;
		}
	}
	pthread_mutex_unlock(&g_reflex_mtx);

	if (matched) {
		return run_matched_rule(tool_buf, args_buf, spoken_buf, NULL, cap,
		                        out_op, out_output, out_spoken);
	}

	size_t n_builtin = sizeof(g_builtin_rules) / sizeof(g_builtin_rules[0]);
	for (size_t i = 0; i < n_builtin; i++) {
		if (match_pattern(g_builtin_rules[i].pattern, norm, cap, sizeof(cap))) {
			return run_matched_rule(g_builtin_rules[i].tool, g_builtin_rules[i].args_fmt,
			                        g_builtin_rules[i].spoken_fmt, g_builtin_rules[i].op_override,
			                        cap, out_op, out_output, out_spoken);
		}
	}

	return 0;
}

XY_MODULE_API void xy_install(void)
{
	pthread_mutex_lock(&g_reflex_mtx);
	ensure_user_rules_loaded();
	pthread_mutex_unlock(&g_reflex_mtx);
	ren_register_tier0(reflex_tier0_solve);
}
