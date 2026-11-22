#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include "ttypt/libren.h"
#include <ttypt/jarvis.h>
#include <ttypt/xy.h>

static jarvis_engine_t *g_eng = NULL;

static void load_wm_mod(const char *home, const char *mod) {
    if (!mod || !*mod) return;
    char clean[512];
    if (mod[0] == '/' || mod[0] == '.') {
        snprintf(clean, sizeof(clean), "%s", mod);
    } else {
        snprintf(clean, sizeof(clean), "%s/.local/lib/%s", home, mod);
    }
    size_t l = strlen(clean);
    if (l > 3 && strcmp(clean + l - 3, ".so") == 0)
        clean[l - 3] = '\0';
    if (xy_load(clean) != XY_OK && mod[0] != '/') {
        snprintf(clean, sizeof(clean), "%s", mod);
        l = strlen(clean);
        if (l > 3 && strcmp(clean + l - 3, ".so") == 0)
            clean[l - 3] = '\0';
        xy_load(clean);
    }
}

static void ensure_jarvis_engine(void) {
    if (g_eng) return;
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/home/quirinpa";

    char wm_buf[64] = {0};
    jarvis_rc_get("wm", "dwm", wm_buf, sizeof(wm_buf));

    char wm_mod[128];
    snprintf(wm_mod, sizeof(wm_mod), "mod_%s", wm_buf[0] ? wm_buf : "dwm");
    load_wm_mod(home, wm_mod);

    static char ont_path[512], ctx_path[512], sess_path[512], mem_path[512];
    char def_ont[512], def_mem[512], def_sess[512];
    snprintf(def_ont, sizeof(def_ont), "%s/.config/jarvis/ontology.json", home);
    snprintf(def_mem, sizeof(def_mem), "%s/.config/jarvis/memories.json", home);
    snprintf(def_sess, sizeof(def_sess), "%s/.cache/jarvis/session.json", home);

    const char *u = getenv("USER");
    if (!u || !*u) u = "quirinpa";

    jarvis_rc_get("ontology_path", def_ont, ont_path, sizeof(ont_path));
    snprintf(ctx_path, sizeof(ctx_path), "/tmp/jarvis-%s.context.json", u);
    jarvis_rc_get("session_path", def_sess, sess_path, sizeof(sess_path));
    jarvis_rc_get("memory_path", def_mem, mem_path, sizeof(mem_path));

    jarvis_config_t cfg;
    jarvis_config_init(&cfg);
    cfg.wm = wm_buf[0] ? wm_buf : "dwm";
    cfg.ontology_path = ont_path;
    cfg.context_path = ctx_path;
    cfg.session_path = sess_path;
    cfg.memory_path = mem_path;
    /* Tier 0 ONLY inside jarvis_process: never run libjarvis's legacy LLM loop */
    cfg.infer_mode = JARVIS_INFER_NONE;
    g_eng = jarvis_engine_create(&cfg);
}

static bool prompt_needs_tier1(const jarvis_ontology_t *ont, const char *prompt) {
    if (!prompt) return true;
    while (*prompt && isspace((unsigned char)*prompt)) prompt++;
    size_t len = strlen(prompt);
    while (len > 0 && isspace((unsigned char)prompt[len - 1])) len--;
    if (len == 0) return true;

    char clean[512];
    jarvis_clean_phrase(prompt, clean, sizeof(clean));

    if (prompt[len - 1] == '?') {
        jarvis_ir_t q_ir;
        if (!ont || !jarvis_solve_clause(ont, prompt, "any", NULL, &q_ir) || q_ir.score < 0.90) {
            return true;
        }
    }
    if (ont && jarvis_ontology_matches_tier1_trigger(ont, clean)) {
        return true;
    }
    return false;
}

static int jarvis_tier0_hook(const char *prompt, char **out_op, char **out_output, char **out_spoken) {
    ensure_jarvis_engine();
    if (!g_eng) return 0;

    const jarvis_ontology_t *ont = jarvis_engine_get_ontology(g_eng);
    if (prompt_needs_tier1(ont, prompt)) {
        return 0;
    }

    /* Check single/multi-clause high-confidence Tier-0 resolution */
    jarvis_world_t world;
    memset(&world, 0, sizeof(world));
    jarvis_xy_observe(&world);

    const char *surf = "any";
    if (world.focused.wm_class[0]) {
        char cls_low[128];
        jarvis_clean_phrase(world.focused.wm_class, cls_low, sizeof(cls_low));
        if (strstr(cls_low, "st") || strstr(cls_low, "xterm") || strstr(cls_low, "alacritty") ||
            strstr(cls_low, "kitty") || strstr(cls_low, "foot") || strstr(cls_low, "wezterm") ||
            strstr(cls_low, "urxvt") || strstr(cls_low, "ghostty")) {
            surf = "terminal";
        } else if (strstr(cls_low, "firefox") || strstr(cls_low, "chromium") || strstr(cls_low, "chrome") ||
                   strstr(cls_low, "brave") || strstr(cls_low, "qutebrowser")) {
            surf = "browser";
        }
    }

    char clauses[JARVIS_MAX_STEPS][512];
    size_t n_clauses = jarvis_split_clauses(ont, prompt, clauses, JARVIS_MAX_STEPS);
    if (n_clauses == 0) return 0;

    for (size_t i = 0; i < n_clauses; i++) {
        jarvis_ir_t ir;
        if (!jarvis_solve_clause(ont, clauses[i], surf, &world, &ir) || ir.score < 0.90) {
            return 0;
        }
    }

    jarvis_process_result_t res;
    memset(&res, 0, sizeof(res));
    if (!jarvis_process(g_eng, prompt, surf, &res) || !res.ok) {
        return 0;
    }

    const char *op = res.last_dsl.op[0] ? res.last_dsl.op : "tier0";
    const char *out_msg = res.combined_result.output[0]
                        ? res.combined_result.output
                        : (res.combined_result.error[0] ? res.combined_result.error : op);
    char spoken[512] = {0};
    jarvis_format_voice_reply(ont, &res, spoken, sizeof(spoken));

    *out_op = strdup(op);
    *out_output = strdup(out_msg);
    *out_spoken = strdup(spoken[0] ? spoken : out_msg);
    return 1;
}

static char *jarvis_context_hook(void) {
    ensure_jarvis_engine();
    jarvis_world_t w;
    memset(&w, 0, sizeof(w));
    jarvis_xy_observe(&w);

    char wins[512] = {0};
    size_t woff = 0;
    for (size_t i = 0; i < w.client_count && woff + 64 < sizeof(wins); i++) {
        woff += (size_t)snprintf(wins + woff, sizeof(wins) - woff,
                                 "%stag%d:%s:'%.48s'",
                                 i > 0 ? ", " : "",
                                 w.clients[i].tag,
                                 w.clients[i].wm_class,
                                 w.clients[i].title);
    }

    char buf[1536];
    snprintf(buf, sizeof(buf),
             "wm=%s focused_class='%s' focused_title='%.80s' cwd='%s' fg_proc='%s' windows=[%s] open_tabs=%zu",
             w.wm[0] ? w.wm : "dwm",
             w.focused.wm_class,
             w.focused.title,
             w.focused_cwd,
             w.focused_process,
             wins,
             w.tab_count);
    return strdup(buf);
}

static int exec_dsl(const jarvis_dsl_t *dsl, char **out_result) {
    jarvis_adapter_result_t res;
    memset(&res, 0, sizeof(res));
    jarvis_xy_execute(dsl, &res);
    if (!res.handled) {
        *out_result = strdup("Error: operation not handled by active desktop adapter");
        return -1;
    }
    if (!res.success) {
        *out_result = strdup(res.error[0] ? res.error : "Error: desktop operation failed");
        return -1;
    }
    *out_result = strdup(res.output[0] ? res.output : "OK");
    return 0;
}

static int tool_workspace(const char *args, char **out_result) {
    ensure_jarvis_engine();
    jarvis_dsl_t dsl;
    memset(&dsl, 0, sizeof(dsl));
    if (strncasecmp(args, "move ", 5) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "workspace_move_window");
        dsl.args.workspace = atoi(args + 5);
    } else {
        snprintf(dsl.op, sizeof(dsl.op), "workspace_switch");
        dsl.args.workspace = atoi(args);
    }
    return exec_dsl(&dsl, out_result);
}

static int tool_window(const char *args, char **out_result) {
    ensure_jarvis_engine();
    jarvis_dsl_t dsl;
    memset(&dsl, 0, sizeof(dsl));
    if (strcasecmp(args, "close") == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "window_close");
        snprintf(dsl.target.kind, sizeof(dsl.target.kind), "window");
    } else if (strcasecmp(args, "fullscreen") == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "window_fullscreen");
    } else if (strcasecmp(args, "float") == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "window_float");
    } else if (strncasecmp(args, "focus ", 6) == 0) {
        const char *target = args + 6;
        while (*target == ' ') target++;
        if (strcasecmp(target, "other") == 0 || strcasecmp(target, "next") == 0 ||
            strcasecmp(target, "previous") == 0 || strcasecmp(target, "master") == 0) {
            snprintf(dsl.op, sizeof(dsl.op), "focus_relative_window");
            snprintf(dsl.target.kind, sizeof(dsl.target.kind), "relative");
            snprintf(dsl.target.relation, sizeof(dsl.target.relation), "%s", target);
        } else {
            snprintf(dsl.op, sizeof(dsl.op), "focus_window");
            snprintf(dsl.target.identifier, sizeof(dsl.target.identifier), "%s", target);
        }
    } else {
        *out_result = strdup("Error: usage: window <close|fullscreen|float|focus <target>>");
        return -1;
    }
    return exec_dsl(&dsl, out_result);
}

static int tool_open_terminal(const char *args, char **out_result) {
    ensure_jarvis_engine();
    jarvis_dsl_t dsl;
    memset(&dsl, 0, sizeof(dsl));
    while (args && *args == ' ') args++;
    if (args && *args) {
        snprintf(dsl.op, sizeof(dsl.op), "spawn_shell_command");
        snprintf(dsl.target.command, sizeof(dsl.target.command), "%s", args);
        dsl.target.new_terminal = true;
    } else {
        snprintf(dsl.op, sizeof(dsl.op), "open_terminal");
    }
    return exec_dsl(&dsl, out_result);
}

static int tool_open_app(const char *args, char **out_result) {
    ensure_jarvis_engine();
    jarvis_dsl_t dsl;
    memset(&dsl, 0, sizeof(dsl));
    snprintf(dsl.op, sizeof(dsl.op), "open_application");
    snprintf(dsl.target.identifier, sizeof(dsl.target.identifier), "%s", args ? args : "");
    return exec_dsl(&dsl, out_result);
}

static int tool_browser(const char *args, char **out_result) {
    ensure_jarvis_engine();
    jarvis_dsl_t dsl;
    memset(&dsl, 0, sizeof(dsl));
    while (args && *args == ' ') args++;
    if (!args || !*args) {
        snprintf(dsl.op, sizeof(dsl.op), "inspect_browser");
        return exec_dsl(&dsl, out_result);
    }
    if (strncasecmp(args, "tab ", 4) == 0) {
        const char *q = args + 4;
        while (*q == ' ') q++;
        snprintf(dsl.op, sizeof(dsl.op), "focus_tab");
        snprintf(dsl.target.query, sizeof(dsl.target.query), "%s", q);
    } else if (strncasecmp(args, "url ", 4) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "open_url");
        snprintf(dsl.target.url, sizeof(dsl.target.url), "%s", args + 4);
    } else if (strncasecmp(args, "search ", 7) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "search_web");
        snprintf(dsl.target.query, sizeof(dsl.target.query), "%s", args + 7);
    } else if (strncmp(args, "http://", 7) == 0 || strncmp(args, "https://", 8) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "open_url");
        snprintf(dsl.target.url, sizeof(dsl.target.url), "%s", args);
    } else {
        snprintf(dsl.op, sizeof(dsl.op), "search_web");
        snprintf(dsl.target.query, sizeof(dsl.target.query), "%s", args);
    }
    return exec_dsl(&dsl, out_result);
}

static int tool_media(const char *args, char **out_result) {
    ensure_jarvis_engine();
    jarvis_dsl_t dsl;
    memset(&dsl, 0, sizeof(dsl));
    if (strncasecmp(args, "play", 4) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "media_play");
        const char *q = args + 4;
        while (*q == ' ') q++;
        snprintf(dsl.target.query, sizeof(dsl.target.query), "%s", q);
    } else if (strcasecmp(args, "pause") == 0 || strcasecmp(args, "stop") == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "media_pause");
    } else if (strcasecmp(args, "next") == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "media_next");
    } else if (strcasecmp(args, "prev") == 0 || strcasecmp(args, "previous") == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "media_prev");
    } else if (strcasecmp(args, "volume_up") == 0 || strcasecmp(args, "up") == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "media_volume");
        snprintf(dsl.args.action, sizeof(dsl.args.action), "volume_up");
    } else if (strcasecmp(args, "volume_down") == 0 || strcasecmp(args, "down") == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "media_volume");
        snprintf(dsl.args.action, sizeof(dsl.args.action), "volume_down");
    } else if (strcasecmp(args, "mute") == 0 || strcasecmp(args, "volume_mute") == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "media_volume");
        snprintf(dsl.args.action, sizeof(dsl.args.action), "volume_mute");
    } else {
        *out_result = strdup("Error: usage: media <play [query]|pause|next|prev|volume_up|volume_down|mute>");
        return -1;
    }
    return exec_dsl(&dsl, out_result);
}

static int tool_inspect(const char *args, char **out_result) {
    ensure_jarvis_engine();
    jarvis_dsl_t dsl;
    memset(&dsl, 0, sizeof(dsl));
    while (args && *args == ' ') args++;
    if (!args || !*args || strncasecmp(args, "terminal", 8) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "inspect_terminal");
        const char *rel = (args && strlen(args) > 9) ? (args + 9) : "focused";
        while (*rel == ' ') rel++;
        snprintf(dsl.target.relation, sizeof(dsl.target.relation), "%s", *rel ? rel : "focused");
    } else if (strncasecmp(args, "browser", 7) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "inspect_browser");
        const char *q = args + 7;
        while (*q == ' ') q++;
        if (*q) {
            snprintf(dsl.target.query, sizeof(dsl.target.query), "%s", q);
        }
    } else if (strncasecmp(args, "windows", 7) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "inspect_windows");
    } else if (strncasecmp(args, "clipboard", 9) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "inspect_clipboard");
    } else if (strncasecmp(args, "system", 6) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "inspect_system");
        const char *asp = args + 6;
        while (*asp == ' ') asp++;
        snprintf(dsl.args.aspect, sizeof(dsl.args.aspect), "%s", *asp ? asp : "all");
    } else {
        *out_result = strdup("Error: usage: inspect <terminal [focused|other]|browser [filter]|windows|clipboard|system [aspect]>");
        return -1;
    }
    return exec_dsl(&dsl, out_result);
}

static void parse_target_pipe_text(const char *raw, jarvis_dsl_t *dsl) {
    const char *pipe = strchr(raw, '|');
    if (pipe) {
        size_t tlen = (size_t)(pipe - raw);
        while (tlen > 0 && isspace((unsigned char)raw[tlen - 1])) tlen--;
        if (tlen >= sizeof(dsl->target.query)) tlen = sizeof(dsl->target.query) - 1;
        memcpy(dsl->target.query, raw, tlen);
        dsl->target.query[tlen] = '\0';
        const char *msg = pipe + 1;
        while (*msg == ' ') msg++;
        snprintf(dsl->args.text, sizeof(dsl->args.text), "%s", msg);
    } else {
        snprintf(dsl->args.text, sizeof(dsl->args.text), "%s", raw);
    }
}

static int tool_input(const char *args, char **out_result) {
    ensure_jarvis_engine();
    jarvis_dsl_t dsl;
    memset(&dsl, 0, sizeof(dsl));
    while (args && *args == ' ') args++;
    if (!args || !*args) {
        *out_result = strdup("Error: usage: input <send <target> | <text> | submit <text> | type <text> | key <combo> | click <x y | element_label> | scroll <up|down|left|right> [amount]>");
        return -1;
    }

    if (strncasecmp(args, "send ", 5) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "type_and_enter");
        parse_target_pipe_text(args + 5, &dsl);
    } else if (strncasecmp(args, "fill ", 5) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "type_text");
        parse_target_pipe_text(args + 5, &dsl);
    } else if (strncasecmp(args, "type ", 5) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "type_text");
        parse_target_pipe_text(args + 5, &dsl);
    } else if (strncasecmp(args, "submit ", 7) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "type_and_enter");
        parse_target_pipe_text(args + 7, &dsl);
    } else if (strncasecmp(args, "enter ", 6) == 0) {
        snprintf(dsl.op, sizeof(dsl.op), "type_and_enter");
        parse_target_pipe_text(args + 6, &dsl);
    } else if (strncasecmp(args, "key ", 4) == 0) {
        const char *k = args + 4;
        while (*k == ' ') k++;
        snprintf(dsl.op, sizeof(dsl.op), "press_key");
        snprintf(dsl.args.key_combo, sizeof(dsl.args.key_combo), "%s", k);
    } else if (strncasecmp(args, "click", 5) == 0) {
        const char * rest = args + 5;
        while (*rest == ' ') rest++;
        snprintf(dsl.op, sizeof(dsl.op), "click");
        int x = 0, y = 0;
        char extra = '\0';
        if (*rest && sscanf(rest, "%d %d %c", &x, &y, &extra) == 2) {
            dsl.target.x = x;
            dsl.target.y = y;
        } else if (*rest && sscanf(rest, "%d,%d %c", &x, &y, &extra) == 2) {
            dsl.target.x = x;
            dsl.target.y = y;
        } else if (*rest) {
            snprintf(dsl.target.query, sizeof(dsl.target.query), "%s", rest);
        }
    } else if (strncasecmp(args, "scroll", 6) == 0) {
        const char *rest = args + 6;
        while (*rest == ' ') rest++;
        snprintf(dsl.op, sizeof(dsl.op), "scroll");
        char dir[32] = "down";
        int amt = 5;
        if (*rest) {
            sscanf(rest, "%31s %d", dir, &amt);
        }
        snprintf(dsl.args.direction, sizeof(dsl.args.direction), "%s", dir);
        dsl.args.amount = amt > 0 ? amt : 5;
    } else {
        *out_result = strdup("Error: usage: input <send <target> | <text> | submit <text> | type <text> | key <combo> | click <x y | element_label> | scroll <up|down|left|right> [amount]>");
        return -1;
    }
    return exec_dsl(&dsl, out_result);
}

static void set_jarvis_identity_prompt(void) {
    char custom_prompt[4096] = {0};
    if (jarvis_rc_get("initial_prompt", NULL, custom_prompt, sizeof(custom_prompt)) && custom_prompt[0]) {
        ren_set_system_prompt(custom_prompt);
        return;
    }

    const jarvis_ontology_t *ont = jarvis_engine_get_ontology(g_eng);
    const char *ont_wake = ont ? jarvis_ontology_get_wake_word(ont) : NULL;
    if (!ont_wake || !*ont_wake) ont_wake = "assistant";
    char wake_buf[64] = {0};
    jarvis_rc_get("wake_word", ont_wake, wake_buf, sizeof(wake_buf));
    if (!wake_buf[0]) snprintf(wake_buf, sizeof(wake_buf), "%s", ont_wake);

    char wake_title[64] = {0};
    snprintf(wake_title, sizeof(wake_title), "%s", wake_buf);
    if (wake_title[0])
        wake_title[0] = (char)toupper((unsigned char)wake_title[0]);

    char identity[1024];
    snprintf(identity, sizeof(identity),
             "You are %s, the user's personal voice and desktop AI assistant. "
             "Never identify yourself as ren. Help the user with desktop automation, "
             "browser control, file operations, and memory using the available tools.",
             wake_title[0] ? wake_title : "Assistant");
    ren_set_system_prompt(identity);
}

static void set_jarvis_model(void) {
    char model_buf[128] = {0};
    jarvis_rc_get("model", "fast", model_buf, sizeof(model_buf));
    if (model_buf[0]) {
        ren_set_model(model_buf);
    }
}

XY_MODULE_API void xy_install(void) {
    ensure_jarvis_engine();
    set_jarvis_identity_prompt();
    set_jarvis_model();
    ren_register_tier0(jarvis_tier0_hook);
    ren_register_context_provider(jarvis_context_hook);

    ren_register_tool("workspace", "workspace <1..9 | move 1..9>", tool_workspace);
    ren_register_tool("window", "window <close | fullscreen | float | focus <class_or_title|other|next|previous>>", tool_window);
    ren_register_tool("open_terminal", "open_terminal [command]", tool_open_terminal);
    ren_register_tool("open_app", "open_app <application_name>", tool_open_app);
    ren_register_tool("browser", "browser <tab <title_or_url_substring> | url <https://...> | search <query>>", tool_browser);
    ren_register_tool("media", "media <play [query] | pause | next | prev | volume_up | volume_down | mute>", tool_media);
    ren_register_tool("inspect", "inspect <terminal [focused|other] | browser [filter_query] | windows | clipboard | system [aspect]>", tool_inspect);
    ren_register_tool("input", "input <send <contact_or_entry> | <text> | submit <text> | type <text> | key <combo> | click <x y | element_label> | scroll <up|down|left|right> [amount]>", tool_input);

    ren_register_prompt_hint(
        "You have live GUI and browser control via 'inspect', 'browser', 'window', and 'input'. Never claim you cannot access or interact with browser tabs, WhatsApp Web, or desktop apps.\n"
        "To interact with an open browser tab or send a message (e.g. on WhatsApp Web):\n"
        "1. Run 'browser' with 'tab whatsapp' (a title/URL substring, NEVER a numeric index) or 'inspect' with 'browser' to focus the tab and inspect UI elements.\n"
        "2. Prefer a single 'input' call with 'send <contact_or_entry> | <message>' (or 'click <contact>' then 'submit <message>') to select the conversation, type, and press Enter without extra sleeps.\n"
        "After receiving tool results that complete the user's request, always output a brief 1-sentence confirmation to the user (never an empty reply).\n"
        "When answering the user directly, keep spoken replies concise, natural, and direct.");
}
