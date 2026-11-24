#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#define isatty _isatty
#else
#include <unistd.h>
#include <sys/ioctl.h>
#endif
#include <sys/stat.h>
#include <ttypt/stele.h>
#include "ttypt/libren.h"

static char *session_path = NULL;
static char *listen_sock_path = NULL;
static stl_ipc_listener_t g_listener = { STL_INVALID_SOCKET, 0, {0}, 0 };
static volatile sig_atomic_t g_interrupted = 0;
static const char *initial_prompts[16];
static int n_initial_prompts = 0;

static pthread_mutex_t g_turn_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_turn_active = 0;
static int g_turn_finished_pending = 0;
static pthread_t g_turn_tid;
static stl_wake_t g_turn_done_wake = { STL_INVALID_SOCKET, STL_INVALID_SOCKET };
static int g_turn_done_ready = 0;
static char g_turn_prompt[4096];
static stl_socket_t g_turn_cli = STL_INVALID_SOCKET;
static ren_context_t *g_turn_ctx = NULL;
static const char *g_turn_host = NULL;
static int g_turn_port = 0;

static void on_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static void print_usage(const char *prog) {
    printf(
        "Usage: %s [OPTIONS] [PROMPT]\n"
        "Options:\n"
        "  -H <host>                  auren host (default: 127.0.0.1)\n"
        "  -P <port>                  auren port (default: 4242)\n"
        "  -p <prompt>                run a single prompt\n"
        "  -m <mods>                  colon-separated modules to load (e.g. ren-mm:ren-jarvis), repeatable\n"
        "  --session <name>           session filename under ~/.ren/sessions/\n"
        "  --listen <socket>          listen on Unix socket, tcp://host:port, or service name\n"
        "  --max-turns <N>            max tool turns before wrap-up (0 = unlimited, default: 0)\n"
        "  --max-history <bytes>      proactive history byte cap (0 = overflow-driven, default: 0)\n"
        "  --max-tool-output <bytes>  per-tool output byte cap (0 = unlimited, default: %d)\n"
        "  --initial-prompt <text>    initial system identity prompt (repeatable)\n"
        "  --model <name>             upstream model/tier (default: fast -> gemini-4-hydrogen-low)\n"
        "  -h, --help                 show this help\n",
        prog, REN_DEFAULT_TOOL_OUTPUT_CHARS);
}

static void load_mod_spec(const char *spec) {
    if (!spec || !*spec) return;
    char *copy = strdup(spec);
    if (!copy) return;
    char *p = copy;
    while (p && *p) {
        char *sep = strchr(p, ':');
        if (sep) *sep = '\0';
        if (*p && ren_load_module(p) != 0) {
            fprintf(stderr, "[ren] Warning: failed to load module %s\n", p);
        }
        p = sep ? (sep + 1) : NULL;
    }
    free(copy);
}

static volatile sig_atomic_t g_winch_received = 0;
static int g_term_rows = 24;
static int g_term_cols = 80;
static int g_scroll_region_active = 0;

static void on_sigwinch(int sig) {
    (void)sig;
    g_winch_received = 1;
}

static void update_term_size(void) {
#ifndef _WIN32
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2 && ws.ws_col > 10) {
        g_term_rows = ws.ws_row;
        g_term_cols = ws.ws_col;
    }
#endif
}

static void term_enable_scroll_region(void) {
    update_term_size();
    if (g_term_rows > 3) {
        /* Set scrolling region from line 1 to line (rows - 1) */
        printf("\033[1;%dr", g_term_rows - 1);
        /* Move cursor to line (rows - 1), col 1 */
        printf("\033[%d;1H", g_term_rows - 1);
        fflush(stdout);
        g_scroll_region_active = 1;
    }
}

static void term_restore_scroll_region(void) {
    if (g_scroll_region_active) {
        /* Reset scrolling region to full screen */
        printf("\033[r");
        /* Move cursor to bottom row */
        printf("\033[%d;1H\n", g_term_rows);
        fflush(stdout);
        g_scroll_region_active = 0;
    }
}

static const char *g_active_model = NULL;
static const char *resolve_active_model(void) {
    if (g_active_model && *g_active_model) return g_active_model;
    const char *env_m = getenv("REN_MODEL");
    if (env_m && *env_m) return env_m;
    return "fast";
}

static size_t resolve_max_context_tokens(void) {
    const char *env_mc = getenv("REN_MAX_CONTEXT_TOKENS");
    if (env_mc && *env_mc) {
        long long val = atoll(env_mc);
        if (val > 0) return (size_t)val;
    }
    const char *m = resolve_active_model();
    if (strstr(m, "hydrogen") || strcmp(m, "fast") == 0) return 1000000;
    if (strstr(m, "gemini") || strstr(m, "flash")) return 1000000;
    if (strstr(m, "gpt-4") || strstr(m, "claude")) return 200000;
    return 32768; /* Default for local models */
}

static void render_status_bar(const ren_context_t *ctx, int turn_busy) {
    if (!g_scroll_region_active) {
        printf("%s> ", turn_busy ? "[steer] " : "");
        fflush(stdout);
        return;
    }

    size_t bytes = ctx ? ctx->history_len : 0;
    size_t tokens = bytes / 4;
    size_t max_tokens = resolve_max_context_tokens();
    const char *model_name = resolve_active_model();

    char k_buf[32];
    if (bytes >= 1024 * 1024) {
        snprintf(k_buf, sizeof(k_buf), "%.1fMB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(k_buf, sizeof(k_buf), "%.1fKB", (double)bytes / 1024.0);
    } else {
        snprintf(k_buf, sizeof(k_buf), "%zuB", bytes);
    }

    char tok_buf[64];
    if (max_tokens >= 1000000) {
        snprintf(tok_buf, sizeof(tok_buf), "~%.1fk/%.1fM tok", (double)tokens / 1000.0, (double)max_tokens / 1000000.0);
    } else if (max_tokens >= 1000) {
        snprintf(tok_buf, sizeof(tok_buf), "~%.1fk/%.0fk tok", (double)tokens / 1000.0, (double)max_tokens / 1000.0);
    } else {
        snprintf(tok_buf, sizeof(tok_buf), "~%zu/%zu tok", tokens, max_tokens);
    }

    /* Save cursor, move to row rows col 1, clear line, draw status, restore cursor */
    printf("\033[s");
    printf("\033[%d;1H\033[2K", g_term_rows);
    if (turn_busy) {
        printf("\033[7;33m[%s | %s | %s | busy]\033[0m \033[1m[steer]\033[0m > ", model_name, tok_buf, k_buf);
    } else {
        printf("\033[7;36m[%s | %s | %s]\033[0m > ", model_name, tok_buf, k_buf);
    }
    fflush(stdout);
}

static char *default_session_name(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char *name = malloc(64);
    snprintf(name, 64, "session-%04d%02d%02d-%02d%02d%02d",
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec);
    return name;
}

static void load_session(ren_context_t *ctx, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return; }
    free(ctx->history);
    ctx->history = malloc((size_t)size + 1);
    (void)fread(ctx->history, 1, (size_t)size, f);
    ctx->history[size] = '\0';
    ctx->history_len = (size_t)size;
    ctx->history_cap = (size_t)size + 1;
    fclose(f);
}

static void save_session(ren_context_t *ctx, const char *path) {
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[ren] Error: failed to save session %s: %s\n", path, strerror(errno));
        return;
    }
    if (ctx->history_len > 0 && fwrite(ctx->history, 1, ctx->history_len, f) != ctx->history_len) {
        fprintf(stderr, "[ren] Error: short write saving session %s: %s\n", path, strerror(errno));
    }
    fclose(f);
}

static void sanitize_line(const char *in, char *out, size_t cap) {
    size_t j = 0;
    if (!in) { out[0] = '\0'; return; }
    for (size_t i = 0; in[i] && j < cap - 1; i++) {
        char c = in[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            out[j++] = ' ';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

static void sock_event_cb(const char *event_type, const char *arg1, const char *arg2, void *userdata) {
    stl_socket_t fd = *(stl_socket_t *)userdata;
    if (fd == STL_INVALID_SOCKET) return;
    char a1[1024], a2[2048], line[3200];
    sanitize_line(arg1, a1, sizeof(a1));
    sanitize_line(arg2, a2, sizeof(a2));
    int n = snprintf(line, sizeof(line), "%s\t%s\t%s\n", event_type, a1, a2);
    if (n > 0) {
        (void)stl_sock_send(fd, line, (size_t)n, 0);
    }
}

static void setup_done_pipe(void) {
    if (g_turn_done_ready) return;
    if (stl_wake_init(&g_turn_done_wake) == 0)
        g_turn_done_ready = 1;
}

static void drain_done_pipe(void) {
    if (g_turn_done_ready) stl_wake_drain(&g_turn_done_wake);
}

static int join_finished_worker(int abort_if_running) {
    pthread_mutex_lock(&g_turn_mtx);
    int active = g_turn_active;
    int pending = g_turn_finished_pending;
    pthread_mutex_unlock(&g_turn_mtx);

    if (active && abort_if_running) {
        ren_abort();
    }
    if (pending || (active && abort_if_running)) {
        pthread_join(g_turn_tid, NULL);
        pthread_mutex_lock(&g_turn_mtx);
        g_turn_active = 0;
        g_turn_finished_pending = 0;
        pthread_mutex_unlock(&g_turn_mtx);
        drain_done_pipe();
        return 1;
    }
    return 0;
}

static void *ren_chat_worker(void *arg) {
    (void)arg;
    stl_socket_t cli = g_turn_cli;
    g_interrupted = 0;
    if (cli != STL_INVALID_SOCKET) {
        ren_chat_ex(g_turn_ctx, g_turn_prompt, g_turn_host, g_turn_port, sock_event_cb, &cli);
        stl_sock_close(cli);
    } else {
        ren_chat(g_turn_ctx, g_turn_prompt, g_turn_host, g_turn_port);
    }
    g_interrupted = 0;
    save_session(g_turn_ctx, session_path);

    pthread_mutex_lock(&g_turn_mtx);
    g_turn_cli = STL_INVALID_SOCKET;
    g_turn_active = 0;
    g_turn_finished_pending = 1;
    if (g_turn_done_ready) {
        stl_wake_signal(&g_turn_done_wake);
    }
    pthread_mutex_unlock(&g_turn_mtx);
    return NULL;
}

static ssize_t ren_read_line(char **buf, size_t *cap, FILE *fp) {
    if (!*buf || *cap == 0) {
        *cap = 256;
        *buf = malloc(*cap);
        if (!*buf) return -1;
    }
    size_t len = 0;
    for (;;) {
        int c = fgetc(fp);
        if (c == EOF) {
            if (len == 0) return -1;
            break;
        }
        if (len + 2 > *cap) {
            size_t ncap = (*cap) * 2;
            char *nb = realloc(*buf, ncap);
            if (!nb) return -1;
            *buf = nb;
            *cap = ncap;
        }
        (*buf)[len++] = (char)c;
        if (c == '\n') break;
    }
    (*buf)[len] = '\0';
    return (ssize_t)len;
}

int main(int argc, char **argv) {
    stl_ignore_pipe();
    const char *host = "127.0.0.1";
    int port = 4242;
    const char *prompt = NULL;
    const char *session_name = NULL;
    const char *listen_arg = NULL;
    int max_turns = 0;
    size_t max_history = 0;
    size_t max_tool_output = REN_DEFAULT_TOOL_OUTPUT_CHARS;
    const char *mod_specs[16];
    int n_mod_specs = 0;
    const char *cli_model = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            prompt = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            if (n_mod_specs < 16) mod_specs[n_mod_specs++] = argv[++i];
            else i++;
        } else if (strncmp(argv[i], "-m", 2) == 0 && argv[i][2] != '\0') {
            if (n_mod_specs < 16) mod_specs[n_mod_specs++] = argv[i] + 2;
        } else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            session_name = argv[++i];
        } else if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_arg = argv[++i];
        } else if (strcmp(argv[i], "--max-turns") == 0 && i + 1 < argc) {
            max_turns = atoi(argv[++i]);
            if (max_turns < 0) max_turns = 0;
        } else if (strcmp(argv[i], "--max-history") == 0 && i + 1 < argc) {
            long long v = atoll(argv[++i]);
            max_history = v > 0 ? (size_t)v : 0;
        } else if (strcmp(argv[i], "--max-tool-output") == 0 && i + 1 < argc) {
            long long v = atoll(argv[++i]);
            max_tool_output = v > 0 ? (size_t)v : 0;
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            cli_model = argv[++i];
        } else if (strcmp(argv[i], "--initial-prompt") == 0 && i + 1 < argc) {
            if (n_initial_prompts < 16) {
                initial_prompts[n_initial_prompts++] = argv[++i];
            } else {
                i++;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "[ren] Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else if (!prompt) {
            prompt = argv[i];
        }
    }

    char home[512];
    stl_home_dir(home, sizeof(home));
    char dir[560];
    snprintf(dir, sizeof(dir), "%s/.ren/sessions", home);
    stl_mkdir_p(dir);

    if (!session_name) {
        session_name = default_session_name();
    }
    session_path = malloc(1024);
    snprintf(session_path, 1024, "%s/%s", dir, session_name);

    ren_context_t ctx;
    ren_init(&ctx);

    for (int i = 0; i < n_mod_specs; i++) {
        load_mod_spec(mod_specs[i]);
    }
    if (cli_model && *cli_model) {
        g_active_model = cli_model;
        ren_set_model(cli_model);
    }

    /* CLI --initial-prompt explicitly overrides module/default identity */
    if (n_initial_prompts > 0) {
        size_t cap = 1024, len = 0;
        char *joined = malloc(cap);
        if (joined) {
            joined[0] = '\0';
            for (int i = 0; i < n_initial_prompts; i++) {
                if (!initial_prompts[i] || !*initial_prompts[i]) continue;
                size_t sl = strlen(initial_prompts[i]);
                size_t need = len + (len > 0 ? 1 : 0) + sl + 1;
                if (need > cap) {
                    while (cap < need) cap *= 2;
                    char *nj = realloc(joined, cap);
                    if (!nj) break;
                    joined = nj;
                }
                if (len > 0) joined[len++] = '\n';
                memcpy(joined + len, initial_prompts[i], sl);
                len += sl;
                joined[len] = '\0';
            }
            if (len > 0) ren_set_system_prompt(joined);
            free(joined);
        }
    }

    ctx.max_turns = max_turns;
    ctx.max_history_chars = max_history;
    ctx.max_tool_output = max_tool_output;
    load_session(&ctx, session_path);

    int ret = 0;
    if (prompt && !listen_arg) {
        ret = ren_chat(&ctx, prompt, host, port) < 0 ? 1 : 0;
        save_session(&ctx, session_path);
    } else {
        int interactive = isatty(STDIN_FILENO);
        if (interactive) {
            stl_stop_on_signal(on_sigint);
#ifndef _WIN32
            stl_stop_on_signal(on_sigwinch);
#endif
            ren_set_abort_flag(&g_interrupted);
            term_enable_scroll_region();
        }

        int have_listener = 0;
        if (listen_arg && *listen_arg) {
            listen_sock_path = strdup(listen_arg);
            if (stl_ipc_listen(listen_sock_path, 8, &g_listener) == 0) {
                have_listener = 1;
                setup_done_pipe();
            } else {
                fprintf(stderr, "[ren] Error: listen(%s) failed: %s\n",
                        listen_sock_path, stl_sock_strerror());
            }
        }

        if (interactive) {
            printf("ren (auren %s:%d) session: %s%s%s\n",
                   host, port, session_path,
                   have_listener ? " socket: " : "",
                   have_listener ? (g_listener.path[0] ? g_listener.path : listen_sock_path) : "");
        }

        if (prompt && *prompt) {
            printf("> %s\n", prompt);
            fflush(stdout);
            g_interrupted = 0;
            if (ren_chat(&ctx, prompt, host, port) < 0)
                ret = 1;
            g_interrupted = 0;
            save_session(&ctx, session_path);
        }

        char *line = NULL;
        size_t cap = 0;
        int stdin_closed = 0;
#ifdef _WIN32
        if (have_listener && !interactive) {
            stdin_closed = 1;
        }
#endif
        int need_prompt = (interactive && !stdin_closed);

        while (1) {
            if (join_finished_worker(0)) {
                need_prompt = (interactive && !stdin_closed);
            }

            if (g_winch_received) {
                g_winch_received = 0;
                term_enable_scroll_region();
                need_prompt = (interactive && !stdin_closed);
            }

            pthread_mutex_lock(&g_turn_mtx);
            int turn_busy = g_turn_active;
            pthread_mutex_unlock(&g_turn_mtx);

            if (need_prompt) {
                render_status_bar(&ctx, turn_busy);
                need_prompt = 0;
            }

            int handle_stdin = 0;
            int handle_listener = 0;
            int handle_done = 0;

#ifdef _WIN32
            if (!have_listener) {
                if (stdin_closed) break;
                handle_stdin = 1;
            } else
#endif
            {
                struct pollfd pfds[3];
                int nfds = 0;
                int stdin_idx = -1;
                int listen_idx = -1;
                int done_idx = -1;

#ifndef _WIN32
                if (!stdin_closed) {
                    stdin_idx = nfds;
                    pfds[nfds].fd = STDIN_FILENO;
                    pfds[nfds].events = POLLIN;
                    pfds[nfds].revents = 0;
                    nfds++;
                }
#endif

                if (have_listener) {
                    listen_idx = nfds;
                    pfds[nfds].fd = g_listener.fd;
                    pfds[nfds].events = POLLIN;
                    pfds[nfds].revents = 0;
                    nfds++;
                }

                if (turn_busy && g_turn_done_ready) {
                    done_idx = nfds;
                    pfds[nfds].fd = stl_wake_fd(&g_turn_done_wake);
                    pfds[nfds].events = POLLIN;
                    pfds[nfds].revents = 0;
                    nfds++;
                }

                if (nfds == 0) break;

                int prc = stl_poll(pfds, (nfds_t)nfds, -1);
                if (prc < 0) {
                    if (errno == EINTR) {
                        if (g_interrupted) {
                            g_interrupted = 0;
                            if (turn_busy) {
                                ren_abort();
                            } else if (interactive) {
                                printf("\n");
                                need_prompt = !stdin_closed;
                            }
                        }
                        continue;
                    }
                    break;
                }

                if (done_idx >= 0 && (pfds[done_idx].revents & POLLIN))
                    handle_done = 1;
                if (listen_idx >= 0 && (pfds[listen_idx].revents & POLLIN))
                    handle_listener = 1;
                if (stdin_idx >= 0 && (pfds[stdin_idx].revents & (POLLIN | POLLHUP | POLLERR)))
                    handle_stdin = 1;
            }

            if (handle_done) {
                join_finished_worker(0);
                need_prompt = (interactive && !stdin_closed);
            }

            /* 1. Handle IPC socket connection from jarvisd / CLI */
            if (handle_listener) {
                stl_socket_t cli = stl_ipc_accept(&g_listener);
                if (cli != STL_INVALID_SOCKET) {
                    char req[4096];
                    ssize_t n = stl_sock_recv(cli, req, sizeof(req) - 1, 0);
                    if (n > 0) {
                        req[n] = '\0';
                        while (n > 0 && (req[n - 1] == '\n' || req[n - 1] == '\r')) {
                            req[--n] = '\0';
                        }
                        if (n > 0) {
                            int is_steer = (strncmp(req, "STEER\t", 6) == 0 || strncmp(req, "/steer ", 7) == 0);
                            int is_stop  = (strcmp(req, "STOP") == 0 || strcmp(req, "/stop") == 0);
                            int is_reset = (strcmp(req, "/new") == 0 || strcmp(req, "/reset") == 0);

                            if (is_steer) {
                                const char *stext = (strncmp(req, "STEER\t", 6) == 0) ? (req + 6) : (req + 7);
                                while (*stext == ' ' || *stext == '\t') stext++;

                                join_finished_worker(0);
                                pthread_mutex_lock(&g_turn_mtx);
                                int active = g_turn_active;
                                pthread_mutex_unlock(&g_turn_mtx);

                                if (active && *stext) {
                                    if (interactive && !stdin_closed) {
                                        printf("\n[ipc-steer] > %s\n", stext);
                                        fflush(stdout);
                                    }
                                    ren_steer(stext);
                                    (void)stl_sock_send(cli, "OK\n", 3, 0);
                                } else if (!active) {
                                    (void)stl_sock_send(cli, "IDLE\n", 5, 0);
                                } else {
                                    (void)stl_sock_send(cli, "ERR\n", 4, 0);
                                }
                                stl_sock_close(cli);
                            } else if (is_stop) {
                                join_finished_worker(0);
                                pthread_mutex_lock(&g_turn_mtx);
                                int active = g_turn_active;
                                pthread_mutex_unlock(&g_turn_mtx);

                                if (active) {
                                    if (interactive && !stdin_closed) {
                                        printf("\n[ipc-stop]\n");
                                        fflush(stdout);
                                    }
                                    ren_abort();
                                }
                                (void)stl_sock_send(cli, "OK\n", 3, 0);
                                stl_sock_close(cli);
                            } else if (is_reset) {
                                join_finished_worker(1);
                                ren_reset(&ctx);
                                save_session(&ctx, session_path);
                                if (interactive && !stdin_closed) {
                                    printf("\n[voice] /reset (session cleared)\n");
                                    fflush(stdout);
                                }
                                sock_event_cb("DONE", "1", "Session reset", &cli);
                                stl_sock_close(cli);
                                need_prompt = (interactive && !stdin_closed);
                            } else {
                                join_finished_worker(0);
                                pthread_mutex_lock(&g_turn_mtx);
                                int active = g_turn_active;
                                pthread_mutex_unlock(&g_turn_mtx);

                                if (active) {
                                    sock_event_cb("DONE", "0", "Busy", &cli);
                                    stl_sock_close(cli);
                                } else {
                                    if (interactive && !stdin_closed) {
                                        printf("\n[ipc] > %s\n", req);
                                        fflush(stdout);
                                    }
                                    g_turn_ctx = &ctx;
                                    g_turn_host = host;
                                    g_turn_port = port;
                                    g_turn_cli = cli;
                                    snprintf(g_turn_prompt, sizeof(g_turn_prompt), "%s", req);
                                    pthread_mutex_lock(&g_turn_mtx);
                                    g_turn_active = 1;
                                    g_turn_finished_pending = 0;
                                    pthread_mutex_unlock(&g_turn_mtx);

                                    if (pthread_create(&g_turn_tid, NULL, ren_chat_worker, NULL) != 0) {
                                        pthread_mutex_lock(&g_turn_mtx);
                                        g_turn_active = 0;
                                        g_turn_cli = STL_INVALID_SOCKET;
                                        pthread_mutex_unlock(&g_turn_mtx);
                                        sock_event_cb("DONE", "0", "Failed to start worker thread", &cli);
                                        stl_sock_close(cli);
                                    }
                                }
                            }
                        } else {
                            stl_sock_close(cli);
                        }
                    } else {
                        stl_sock_close(cli);
                    }
                }
                continue;
            }

            /* 2. Handle interactive STDIN line in terminal */
            if (handle_stdin) {
                ssize_t nread = ren_read_line(&line, &cap, stdin);
                if (nread <= 0) {
                    if (errno == EINTR && g_interrupted) {
                        clearerr(stdin);
                        g_interrupted = 0;
                        if (interactive) printf("\n");
                        need_prompt = !stdin_closed;
                        continue;
                    }
                    if (have_listener) {
                        stdin_closed = 1;
                        continue;
                    }
                    break;
                }

                while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r')) {
                    line[--nread] = '\0';
                }
                need_prompt = (interactive && !stdin_closed);
                if (nread == 0) continue;
                if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) break;
                if (strcmp(line, "/new") == 0 || strcmp(line, "/reset") == 0) {
                    ren_reset(&ctx);
                    save_session(&ctx, session_path);
                    printf("[session cleared]\n");
                    continue;
                }

                pthread_mutex_lock(&g_turn_mtx);
                int busy = g_turn_active;
                pthread_mutex_unlock(&g_turn_mtx);

                if (busy) {
                    /* Steering mid-turn */
                    if (g_scroll_region_active) {
                        printf("\033[%d;1H\n", g_term_rows - 1);
                    }
                    printf("\033[1;33m[steer] > %s\033[0m\n", line);
                    fflush(stdout);
                    ren_steer(line);
                    need_prompt = (interactive && !stdin_closed);
                    continue;
                }

                /* Print prompt in session scrollback */
                if (g_scroll_region_active) {
                    printf("\033[%d;1H\n", g_term_rows - 1);
                }
                printf("\033[1m> %s\033[0m\n", line);
                fflush(stdout);

                g_turn_ctx = &ctx;
                g_turn_host = host;
                g_turn_port = port;
                g_turn_cli = STL_INVALID_SOCKET;
                snprintf(g_turn_prompt, sizeof(g_turn_prompt), "%s", line);
                setup_done_pipe();

                pthread_mutex_lock(&g_turn_mtx);
                g_turn_active = 1;
                g_turn_finished_pending = 0;
                pthread_mutex_unlock(&g_turn_mtx);

                if (pthread_create(&g_turn_tid, NULL, ren_chat_worker, NULL) != 0) {
                    pthread_mutex_lock(&g_turn_mtx);
                    g_turn_active = 0;
                    pthread_mutex_unlock(&g_turn_mtx);
                    fprintf(stderr, "[ren] Error: Failed to start chat worker thread\n");
                }
                need_prompt = (interactive && !stdin_closed);
            }
        }
        join_finished_worker(1);
        if (g_turn_done_ready) {
            stl_wake_close(&g_turn_done_wake);
            g_turn_done_ready = 0;
        }
        free(line);
        if (have_listener) {
            stl_ipc_close_listener(&g_listener);
        }
        free(listen_sock_path);
        term_restore_scroll_region();
        if (interactive) printf("\n");
        save_session(&ctx, session_path);
    }

    term_restore_scroll_region();
    ren_set_abort_flag(NULL);
    ren_free(&ctx);
    free(session_path);
    return ret;
}
