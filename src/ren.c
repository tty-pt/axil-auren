#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <errno.h>
#include "ttypt/libren.h"

static char *session_path = NULL;
static char *listen_sock_path = NULL;
static volatile sig_atomic_t g_interrupted = 0;
static const char *initial_prompts[16];
static int n_initial_prompts = 0;

static pthread_mutex_t g_turn_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_turn_active = 0;
static int g_turn_finished_pending = 0;
static pthread_t g_turn_tid;
static int g_turn_done_pipe[2] = {-1, -1};
static char g_turn_prompt[4096];
static int g_turn_cli = -1;
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
        "  -m <mods>                  colon-separated modules to load (e.g. mod_mm:mod_jarvis), repeatable\n"
        "  --session <name>           session filename under ~/.ren/sessions/\n"
        "  --listen <socket>          listen on Unix domain socket\n"
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

static void ensure_dir(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
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
    ctx->history = malloc(size + 1);
    (void)fread(ctx->history, 1, size, f);
    ctx->history[size] = '\0';
    ctx->history_len = size;
    ctx->history_cap = size + 1;
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
    int fd = *(int *)userdata;
    if (fd < 0) return;
    char a1[1024], a2[2048], line[3200];
    sanitize_line(arg1, a1, sizeof(a1));
    sanitize_line(arg2, a2, sizeof(a2));
    int n = snprintf(line, sizeof(line), "%s\t%s\t%s\n", event_type, a1, a2);
    if (n > 0) {
        (void)write(fd, line, (size_t)n);
    }
}

static int setup_unix_listener(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "[ren] Error: socket(%s) failed: %s\n", path, strerror(errno));
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[ren] Error: bind(%s) failed: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0600);
    if (listen(fd, 8) < 0) {
        fprintf(stderr, "[ren] Error: listen(%s) failed: %s\n", path, strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

static void setup_done_pipe(void) {
    if (g_turn_done_pipe[0] >= 0) return;
    int fds[2];
    if (pipe(fds) < 0) return;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(fds[i], F_GETFL, 0);
        if (fl >= 0) fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    g_turn_done_pipe[0] = fds[0];
    g_turn_done_pipe[1] = fds[1];
}

static void drain_done_pipe(void) {
    if (g_turn_done_pipe[0] < 0) return;
    char tmp[64];
    while (read(g_turn_done_pipe[0], tmp, sizeof(tmp)) > 0) {}
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

static void *ren_ipc_worker(void *arg) {
    (void)arg;
    int cli = g_turn_cli;
    g_interrupted = 0;
    ren_chat_ex(g_turn_ctx, g_turn_prompt, g_turn_host, g_turn_port, sock_event_cb, &cli);
    g_interrupted = 0;
    save_session(g_turn_ctx, session_path);
    close(cli);

    pthread_mutex_lock(&g_turn_mtx);
    g_turn_cli = -1;
    g_turn_active = 0;
    g_turn_finished_pending = 1;
    if (g_turn_done_pipe[1] >= 0) {
        char b = 1;
        ssize_t wr = write(g_turn_done_pipe[1], &b, 1);
        (void)wr;
    }
    pthread_mutex_unlock(&g_turn_mtx);
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
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

    const char *home = getenv("HOME");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.ren/sessions", home ? home : ".");
    ensure_dir(dir);

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
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = on_sigint;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            sigaction(SIGINT, &sa, NULL);
            ren_set_abort_flag(&g_interrupted);
        }

        int listen_fd = -1;
        if (listen_arg && *listen_arg) {
            listen_sock_path = strdup(listen_arg);
            listen_fd = setup_unix_listener(listen_sock_path);
            if (listen_fd >= 0) {
                setup_done_pipe();
            }
        }

        if (interactive) {
            printf("ren (auren %s:%d) session: %s%s%s\n",
                   host, port, session_path,
                   listen_sock_path ? " socket: " : "",
                   listen_sock_path ? listen_sock_path : "");
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
        int need_prompt = (interactive && !stdin_closed);

        while (1) {
            if (join_finished_worker(0)) {
                need_prompt = (interactive && !stdin_closed);
            }

            pthread_mutex_lock(&g_turn_mtx);
            int turn_busy = g_turn_active;
            pthread_mutex_unlock(&g_turn_mtx);

            if (need_prompt && !turn_busy) {
                printf("> ");
                fflush(stdout);
                need_prompt = 0;
            }

            struct pollfd pfds[3];
            int nfds = 0;
            int stdin_idx = -1;
            int listen_idx = -1;
            int done_idx = -1;

            if (!stdin_closed && !turn_busy) {
                stdin_idx = nfds;
                pfds[nfds].fd = STDIN_FILENO;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                nfds++;
            }

            if (listen_fd >= 0) {
                listen_idx = nfds;
                pfds[nfds].fd = listen_fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                nfds++;
            }

            if (turn_busy && g_turn_done_pipe[0] >= 0) {
                done_idx = nfds;
                pfds[nfds].fd = g_turn_done_pipe[0];
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                nfds++;
            }

            if (nfds == 0) break;

            int prc = poll(pfds, nfds, -1);
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

            if (done_idx >= 0 && (pfds[done_idx].revents & POLLIN)) {
                join_finished_worker(0);
                need_prompt = (interactive && !stdin_closed);
            }

            /* 1. Handle Unix Domain Socket connection from jarvisd / CLI */
            if (listen_idx >= 0 && (pfds[listen_idx].revents & POLLIN)) {
                int cli = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
                if (cli >= 0) {
                    char req[4096];
                    ssize_t n = read(cli, req, sizeof(req) - 1);
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
                                    ssize_t wr = write(cli, "OK\n", 3);
                                    (void)wr;
                                } else if (!active) {
                                    ssize_t wr = write(cli, "IDLE\n", 5);
                                    (void)wr;
                                } else {
                                    ssize_t wr = write(cli, "ERR\n", 4);
                                    (void)wr;
                                }
                                close(cli);
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
                                ssize_t wr = write(cli, "OK\n", 3);
                                (void)wr;
                                close(cli);
                            } else if (is_reset) {
                                join_finished_worker(1);
                                ren_reset(&ctx);
                                save_session(&ctx, session_path);
                                if (interactive && !stdin_closed) {
                                    printf("\n[voice] /reset (session cleared)\n");
                                    fflush(stdout);
                                }
                                sock_event_cb("DONE", "1", "Session reset", &cli);
                                close(cli);
                                need_prompt = (interactive && !stdin_closed);
                            } else {
                                join_finished_worker(0);
                                pthread_mutex_lock(&g_turn_mtx);
                                int active = g_turn_active;
                                pthread_mutex_unlock(&g_turn_mtx);

                                if (active) {
                                    sock_event_cb("DONE", "0", "Busy", &cli);
                                    close(cli);
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

                                    if (pthread_create(&g_turn_tid, NULL, ren_ipc_worker, NULL) != 0) {
                                        pthread_mutex_lock(&g_turn_mtx);
                                        g_turn_active = 0;
                                        g_turn_cli = -1;
                                        pthread_mutex_unlock(&g_turn_mtx);
                                        sock_event_cb("DONE", "0", "Failed to start worker thread", &cli);
                                        close(cli);
                                    }
                                }
                            }
                        } else {
                            close(cli);
                        }
                    } else {
                        close(cli);
                    }
                }
                continue;
            }

            /* 2. Handle interactive STDIN line in tmux / terminal */
            if (stdin_idx >= 0 && (pfds[stdin_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t nread = getline(&line, &cap, stdin);
                if (nread <= 0) {
                    if (errno == EINTR && g_interrupted) {
                        clearerr(stdin);
                        g_interrupted = 0;
                        if (interactive) printf("\n");
                        need_prompt = !stdin_closed;
                        continue;
                    }
                    if (listen_fd >= 0) {
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

                g_interrupted = 0;
                if (ren_chat(&ctx, line, host, port) < 0) {
                    ret = 1;
                }
                g_interrupted = 0;
                save_session(&ctx, session_path);
            }
        }
        join_finished_worker(1);
        if (g_turn_done_pipe[0] >= 0) {
            close(g_turn_done_pipe[0]);
            g_turn_done_pipe[0] = -1;
        }
        if (g_turn_done_pipe[1] >= 0) {
            close(g_turn_done_pipe[1]);
            g_turn_done_pipe[1] = -1;
        }
        free(line);
        if (listen_fd >= 0) {
            close(listen_fd);
            if (listen_sock_path) unlink(listen_sock_path);
        }
        free(listen_sock_path);
        if (interactive) printf("\n");
        save_session(&ctx, session_path);
    }

    ren_set_abort_flag(NULL);
    ren_free(&ctx);
    free(session_path);
    return ret;
}
