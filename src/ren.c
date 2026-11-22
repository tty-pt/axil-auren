#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
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

        while (1) {
            if (interactive && !stdin_closed) {
                printf("> ");
                fflush(stdout);
            }

            struct pollfd pfds[2];
            int nfds = 0;
            int stdin_idx = -1;
            int listen_idx = -1;

            if (!stdin_closed) {
                stdin_idx = nfds;
                pfds[nfds].fd = STDIN_FILENO;
                pfds[nfds].events = POLLIN;
                nfds++;
            }

            if (listen_fd >= 0) {
                listen_idx = nfds;
                pfds[nfds].fd = listen_fd;
                pfds[nfds].events = POLLIN;
                nfds++;
            }

            if (nfds == 0) break;

            int prc = poll(pfds, nfds, -1);
            if (prc < 0) {
                if (errno == EINTR) {
                    if (g_interrupted) {
                        g_interrupted = 0;
                        if (interactive) printf("\n");
                    }
                    continue;
                }
                break;
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
                            if (strcmp(req, "/new") == 0 || strcmp(req, "/reset") == 0) {
                                ren_reset(&ctx);
                                save_session(&ctx, session_path);
                                if (interactive && !stdin_closed) {
                                    printf("\n[voice] /reset (session cleared)\n");
                                    fflush(stdout);
                                }
                                sock_event_cb("DONE", "1", "Session reset", &cli);
                            } else {
                                if (interactive && !stdin_closed) {
                                    printf("\n[ipc] > %s\n", req);
                                    fflush(stdout);
                                }
                                g_interrupted = 0;
                                ren_chat_ex(&ctx, req, host, port, sock_event_cb, &cli);
                                g_interrupted = 0;
                                save_session(&ctx, session_path);
                            }
                        }
                    }
                    close(cli);
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
