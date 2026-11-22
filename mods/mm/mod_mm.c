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
    mm_recall_req_t req;
    memset(&req, 0, sizeof(req));
    req.scope = MM_SCOPE_PERSISTENT;
    req.query = args;
    req.limit = 5;

    char *buf = NULL;
    size_t len = 0;
    int rc = mm_recall(g_mm_ctx, &req, &buf, &len);
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

    ren_register_tool("recall", "recall <query>", do_recall);
    ren_register_tool("record", "record <problem>::<findings>", do_record);
    ren_register_prompt_hint(
        "Memory tools are available: use 'recall' to look up prior findings and 'record' to save important facts.");
}
