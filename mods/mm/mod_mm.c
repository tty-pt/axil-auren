#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ttypt/libren.h"
#include "ttypt/mm.h"
#include <ttypt/xy.h>

static mm_ctx_t *g_mm_ctx = NULL;

static int do_recall(const char *args, char **out_result) {
    if (!g_mm_ctx) {
        *out_result = strdup("Error: mm context not initialized");
        return -1;
    }
    if (!args) args = "";

    /* Check for "projects" listing */
    if (strcmp(args, "projects") == 0) {
        mm_project_info_t *list = NULL;
        size_t count = 0;
        if (mm_project_list(&list, &count) != 0 || count == 0) {
            free(list);
            *out_result = strdup("No projects registered yet.");
            return 0;
        }
        size_t cap = count * 640 + 64;
        char *buf = malloc(cap);
        size_t off = 0;
        for (size_t i = 0; i < count; i++) {
            int n = snprintf(buf + off, cap - off, "%s: %s\n", list[i].name, list[i].root_path);
            if (n > 0) off += (size_t)n;
        }
        free(list);
        *out_result = buf;
        return 0;
    }

    /* Optional "project:<name> " prefix */
    char proj_buf[128] = {0};
    const char *proj = NULL;
    const char *rest = args;
    if (strncmp(rest, "project:", 8) == 0) {
        rest += 8;
        const char *sp = strchr(rest, ' ');
        if (sp) {
            size_t len = (size_t)(sp - rest);
            if (len >= sizeof(proj_buf)) len = sizeof(proj_buf) - 1;
            memcpy(proj_buf, rest, len);
            proj_buf[len] = '\0';
            proj = proj_buf;
            rest = sp + 1;
            while (*rest == ' ') rest++;
        }
    }

    char *buf = NULL;
    size_t len = 0;
    int rc = -1;

    if (strncmp(rest, "code:", 5) == 0) {
        rc = mm_project_code_search(proj, rest + 5, NULL, NULL, 8, &buf, &len);
        *out_result = (rc == 0 && buf) ? buf : strdup("No code matches found.");
        return 0;
    }
    if (strncmp(rest, "symbol:", 7) == 0) {
        rc = mm_project_code_symbol(proj, rest + 7, NULL, &buf, &len);
        *out_result = (rc == 0 && buf) ? buf : strdup("Symbol not found.");
        return 0;
    }
    if (strncmp(rest, "trace:", 6) == 0) {
        rc = mm_project_code_trace(proj, rest + 6, "both", 2, &buf, &len);
        *out_result = (rc == 0 && buf) ? buf : strdup("No call graph edges found.");
        return 0;
    }
    if (strcmp(rest, "diff") == 0) {
        mm_project_info_t pinfo;
        const char *target = proj ? proj : ".";
        if (mm_project_resolve(target, &pinfo) == 0) {
            mm_code_index_t idx;
            if (mm_project_open_index(&pinfo, &idx) == 0) {
                rc = mm_code_diff_impact(&idx, 2, &buf, &len);
                mm_code_index_free(&idx);
            }
        }
        *out_result = (rc == 0 && buf) ? buf : strdup("No git diff impact found.");
        return 0;
    }
    if (strncmp(rest, "guardrail:", 10) == 0) {
        rc = mm_evaluate_guardrail(g_mm_ctx, rest + 10, NULL, NULL, &buf, &len);
        *out_result = (rc == 0 && buf) ? buf : strdup("No guardrail warnings.");
        return 0;
    }
    if (strncmp(rest, "session:", 8) == 0) {
        rc = mm_session_recall(g_mm_ctx, MM_TIER_BOTH, NULL, rest + 8, 0, 0, 0, 5, &buf, &len);
        *out_result = (rc == 0 && buf) ? buf : strdup("No session history found.");
        return 0;
    }

    mm_recall_req_t req;
    memset(&req, 0, sizeof(req));
    req.scope = MM_SCOPE_PERSISTENT;
    req.query = args;
    req.limit = 5;

    rc = mm_recall(g_mm_ctx, &req, &buf, &len);
    if (rc != 0 || !buf) {
        *out_result = strdup("No memories found.");
        return 0;
    }
    *out_result = buf;
    return 0;
}

static int do_record(const char *args, char **out_result) {
    if (!g_mm_ctx) {
        *out_result = strdup("Error: mm context not initialized");
        return -1;
    }
    /* Check if input is a zero-JSON line-delimited distillation block */
    if (strncmp(args, "PROBLEM:", 8) == 0 || strstr(args, "\nPROBLEM:") != NULL) {
        mm_tier_t tier = MM_TIER_PROJECT;
        uint32_t ref = 0;
        int rc = mm_distill_commit(g_mm_ctx, args, &tier, &ref);
        if (rc == 0 && ref > 0) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Distilled & recorded as [%s Ref %u]",
                     tier == MM_TIER_GLOBAL ? "Global" : "Project", ref);
            *out_result = strdup(msg);
            return 0;
        }
    }

    /* args: problem::findings */
    char *copy = strdup(args);
    char *sep = strstr(copy, "::");
    const char *problem = copy;
    const char *findings = NULL;
    if (sep) {
        *sep = '\0';
        findings = sep + 2;
    }

    mm_record_req_t req;
    memset(&req, 0, sizeof(req));
    req.problem = problem;
    req.type = "episode";
    req.level = MM_LEVEL_L1;
    if (findings) {
        const char *f[1] = { findings };
        req.findings = f;
        req.n_findings = 1;
    }

    mm_tier_t tier;
    uint32_t ref;
    int rc = mm_record(g_mm_ctx, &req, &tier, &ref);
    free(copy);
    if (rc != 0) {
        *out_result = strdup("Error: record failed");
        return -1;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Recorded as ref %u", ref);
    *out_result = strdup(msg);
    return 0;
}

XY_MODULE_API void xy_install(void) {
    mm_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.cwd = ".";
    opts.semantic_search = false;
    g_mm_ctx = mm_open(&opts);

    ren_register_tool("recall", "recall [project:<name>] <query|code:query|symbol:name|trace:name|diff|guardrail:intent|session:query|projects>", do_recall);
    ren_register_tool("record", "record <problem>::<findings> | <line-delimited distillation block>", do_record);
    ren_register_prompt_hint(
        "Memory & code tools are available: use 'recall <query>' for memories, "
        "'recall [project:<name>] code:<query>' to search code, "
        "'recall [project:<name>] symbol:<name>' to read a symbol, "
        "'recall [project:<name>] trace:<name>' for call graphs, "
        "'recall [project:<name>] diff' for git diff blast radius, "
        "'recall guardrail:<intent>' for pre-flight checks, "
        "'recall session:<query>' for past conversations, "
        "'recall projects' to list projects, and 'record <problem>::<findings>' "
        "(or 'record PROBLEM: ...\\nAPPROACH: ...\\nFINDING: ...') to distill and save facts.");
}
