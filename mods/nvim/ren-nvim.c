#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <json-c/json.h>
#include "ttypt/libren.h"
#include <ttypt/xy.h>

static int run_nvim_expr(const char *expr, char **out_result) {
    const char *sock = getenv("NVIM");
    if (!sock || !*sock) {
        *out_result = strdup("Error: $NVIM is not set (run ren inside a Neovim terminal)");
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        *out_result = strdup("Error: pipe failed");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        *out_result = strdup("Error: fork failed");
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execlp("nvim", "nvim", "--server", sock, "--remote-expr", expr, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        *out_result = strdup("Error: out of memory");
        return -1;
    }

    ssize_t n;
    while ((n = read(pipefd[0], buf + len, cap - len - 1)) > 0) {
        len += (size_t)n;
        if (cap - len - 1 < 1024) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                close(pipefd[0]);
                waitpid(pid, NULL, 0);
                *out_result = strdup("Error: out of memory");
                return -1;
            }
            buf = nb;
        }
    }
    close(pipefd[0]);
    buf[len] = '\0';

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        *out_result = buf[0] ? buf : strdup("Error: nvim --remote-expr failed");
        if (buf[0] == '\0') free(buf);
        return -1;
    }

    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    *out_result = buf;
    return 0;
}

static int tool_nvim_context(const char *args, char **out_result) {
    (void)args;
    const char *expr =
        "luaeval('(function() "
        "local win = vim.api.nvim_get_current_win(); "
        "local buf = vim.api.nvim_win_get_buf(win); "
        "if vim.bo[buf].buftype ~= \"\" then "
        "  for _, w in ipairs(vim.api.nvim_list_wins()) do "
        "    local b = vim.api.nvim_win_get_buf(w); "
        "    if vim.bo[b].buftype == \"\" then win = w; buf = b; break end "
        "  end "
        "end; "
        "local pos = vim.api.nvim_win_get_cursor(win); "
        "local bufs = {}; "
        "for _, b in ipairs(vim.api.nvim_list_bufs()) do "
        "  if vim.api.nvim_buf_is_loaded(b) and vim.bo[b].buftype == \"\" then "
        "    table.insert(bufs, vim.api.nvim_buf_get_name(b)) "
        "  end "
        "end; "
        "return vim.json.encode({ "
        "  file = vim.api.nvim_buf_get_name(buf), "
        "  filetype = vim.bo[buf].filetype, "
        "  line = pos[1], "
        "  col = pos[2], "
        "  modified = vim.bo[buf].modified, "
        "  buffers = bufs "
        "}) end)()')";
    return run_nvim_expr(expr, out_result);
}

static int tool_nvim_diagnostics(const char *args, char **out_result) {
    (void)args;
    const char *expr =
        "luaeval('(function() "
        "local diags = vim.diagnostic.get(nil); "
        "local out = {}; "
        "for i, d in ipairs(diags) do "
        "  if i > 50 then break end; "
        "  table.insert(out, { "
        "    file = vim.api.nvim_buf_get_name(d.bufnr), "
        "    line = d.lnum + 1, "
        "    col = d.col + 1, "
        "    severity = d.severity, "
        "    message = d.message "
        "  }) "
        "end; "
        "return vim.json.encode(out) end)()')";
    return run_nvim_expr(expr, out_result);
}

static int tool_nvim_jump(const char *args, char **out_result) {
    if (!args || !*args) {
        *out_result = strdup("Error: expected JSON with path/file and optional line, col");
        return -1;
    }

    char path[4096] = {0};
    int line = 1;
    int col = 0;

    struct json_object *obj = json_tokener_parse(args);
    if (obj && json_object_is_type(obj, json_type_object)) {
        struct json_object *p = json_object_object_get(obj, "path");
        if (!p) p = json_object_object_get(obj, "file");
        if (p && json_object_is_type(p, json_type_string)) {
            snprintf(path, sizeof(path), "%s", json_object_get_string(p));
        }
        struct json_object *l = json_object_object_get(obj, "line");
        if (l && json_object_is_type(l, json_type_int)) {
            line = json_object_get_int(l);
            if (line < 1) line = 1;
        }
        struct json_object *c = json_object_object_get(obj, "col");
        if (c && json_object_is_type(c, json_type_int)) {
            col = json_object_get_int(c);
            if (col < 0) col = 0;
        }
    }
    if (obj) json_object_put(obj);

    if (!path[0]) {
        *out_result = strdup("Error: missing path");
        return -1;
    }

    /* Escape single quotes for Vim/Lua string literal */
    char escaped[8192];
    size_t j = 0;
    for (size_t i = 0; path[i] && j + 2 < sizeof(escaped); i++) {
        if (path[i] == '\'' || path[i] == '\\') {
            escaped[j++] = '\\';
        }
        escaped[j++] = path[i];
    }
    escaped[j] = '\0';

    char expr[9216];
    snprintf(expr, sizeof(expr),
        "luaeval('(function() "
        "vim.cmd(\"edit \" .. vim.fn.fnameescape(\"%s\")); "
        "pcall(vim.api.nvim_win_set_cursor, 0, {%d, %d}); "
        "return \"ok\" end)()')",
        escaped, line, col);
    return run_nvim_expr(expr, out_result);
}

static int tool_nvim_reload(const char *args, char **out_result) {
    (void)args;
    return run_nvim_expr("execute('checktime')", out_result);
}

int xy_install(void) {
    ren_register_tool_with_schema(
        "nvim_context",
        "Get active Neovim buffer, cursor position, and open buffers",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_nvim_context
    );
    ren_register_tool_with_schema(
        "nvim_diagnostics",
        "Get Neovim LSP diagnostics across open buffers",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_nvim_diagnostics
    );
    ren_register_tool_with_schema(
        "nvim_jump",
        "Open a file and jump Neovim cursor to line/col",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"line\":{\"type\":\"integer\"},\"col\":{\"type\":\"integer\"}},\"required\":[\"path\"]}",
        tool_nvim_jump
    );
    ren_register_tool_with_schema(
        "nvim_reload",
        "Reload modified buffers in Neovim (:checktime)",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_nvim_reload
    );
    return 0;
}
