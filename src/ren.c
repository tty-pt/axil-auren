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
    if (!f) return;
    fwrite(ctx->history, 1, ctx->history_len, f);
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
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0600);
    if (listen(fd, 8) < 0) {
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

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            prompt = argv[++i];
        } else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            session_name = argv[++i];
        } else if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_arg = argv[++i];
        } else if (strcmp(argv[i], "--jarvis") == 0) {
            if (!session_name) session_name = "jarvis";
            if (!listen_arg) {
                const char *u = getenv("USER");
                if (!u || !*u) u = "quirinpa";
                static char def_sock[256];
                snprintf(def_sock, sizeof(def_sock), "/tmp/ren-jarvis-%s.sock", u);
                listen_arg = def_sock;
            }
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
    load_session(&ctx, session_path);

    int ret = 0;
    if (prompt && !listen_arg) {
        ret = ren_chat(&ctx, prompt, host, port);
        save_session(&ctx, session_path);
    } else {
        int interactive = isatty(STDIN_FILENO);
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
            ren_chat(&ctx, prompt, host, port);
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
                if (errno == EINTR) continue;
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
                                    printf("\n[jarvis] > %s\n", req);
                                    fflush(stdout);
                                }
                                ren_chat_ex(&ctx, req, host, port, sock_event_cb, &cli);
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

                if (ren_chat(&ctx, line, host, port) < 0) {
                    ret = 1;
                }
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

    ren_free(&ctx);
    free(session_path);
    return ret;
}
