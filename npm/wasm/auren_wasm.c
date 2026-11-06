#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ttypt/iso.h>

#define AUREN_WASM_MAX_BUF 65536

static char *g_out_buf = NULL;
static size_t g_out_cap = 0;
static size_t g_out_len = 0;
static int g_inited = 0;

ISO_EXPORT(int, auren_wasm_init, size_t max_buf) {
    if (max_buf == 0 || max_buf > 1048576) {
        max_buf = AUREN_WASM_MAX_BUF;
    }
    if (g_out_buf) {
        free(g_out_buf);
    }
    g_out_buf = (char *)malloc(max_buf);
    if (!g_out_buf) return -1;
    g_out_cap = max_buf;
    g_out_len = 0;
    g_out_buf[0] = '\0';
    g_inited = 1;
    return 0;
}

ISO_EXPORT(void, auren_wasm_reset, void) {
    if (g_out_buf) {
        g_out_len = 0;
        g_out_buf[0] = '\0';
    }
}

ISO_EXPORT(int, auren_wasm_accumulate_token, const char *token, size_t len) {
    if (!g_inited || !g_out_buf || !token || len == 0) return -1;
    if (g_out_len + len + 1 > g_out_cap) {
        return -2; /* Overflow protection */
    }
    memcpy(g_out_buf + g_out_len, token, len);
    g_out_len += len;
    g_out_buf[g_out_len] = '\0';
    return (int)g_out_len;
}

ISO_EXPORT(size_t, auren_wasm_get_len, void) {
    return g_out_len;
}

ISO_EXPORT(int, auren_wasm_get_output, char *out, size_t max_len) {
    if (!g_inited || !g_out_buf || !out || max_len == 0) return -1;
    size_t copy_len = g_out_len;
    if (copy_len >= max_len) {
        copy_len = max_len - 1;
    }
    memcpy(out, g_out_buf, copy_len);
    out[copy_len] = '\0';
    return (int)copy_len;
}

ISO_EXPORT(float, auren_wasm_cosine, const float *a, const float *b, size_t dim) {
    if (!a || !b || dim == 0) return 0.0f;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < dim; i++) {
        dot += (double)a[i] * (double)b[i];
        na += (double)a[i] * (double)a[i];
        nb += (double)b[i] * (double)b[i];
    }
    if (na <= 0.0 || nb <= 0.0) return 0.0f;
    return (float)(dot / sqrt(na * nb));
}

ISO_EXPORT(int, auren_wasm_choice, const float *logits, size_t n_vocab, int token_pos, int token_neg, float *prob_out) {
    if (!logits || token_pos < 0 || (size_t)token_pos >= n_vocab ||
        token_neg < 0 || (size_t)token_neg >= n_vocab || !prob_out) {
        return -1;
    }
    float l_pos = logits[token_pos];
    float l_neg = logits[token_neg];
    float diff = l_pos - l_neg;
    *prob_out = 1.0f / (1.0f + expf(-diff));
    return 0;
}

ISO_EXPORT(void, auren_wasm_free, void) {
    if (g_out_buf) {
        free(g_out_buf);
        g_out_buf = NULL;
    }
    g_out_cap = 0;
    g_out_len = 0;
    g_inited = 0;
}
