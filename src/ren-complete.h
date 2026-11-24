#ifndef REN_COMPLETE_H
#define REN_COMPLETE_H

#include <stddef.h>

typedef struct ren_completion_result {
    size_t new_len;      /* Updated buffer length */
    size_t new_pos;      /* Updated cursor position */
    int match_count;     /* Number of matching candidates (0, 1, or >1) */
    int changed;         /* 1 if buf was modified (unique match or LCP extension) */
    char matches[512];   /* Space-separated candidate preview when match_count > 1 */
} ren_completion_result_t;

/* Complete token immediately preceding *pos in buf (capacity cap).
 * - First token starting with '/' without a second '/': tries built-in slash
 *   commands ("/exit", "/new", "/quit", "/reset", "/status"), falling through
 *   to filesystem path completion if no slash command matches.
 * - Otherwise: completes filesystem paths (relative, absolute, or ~/...).
 *   Directories get a trailing '/'; unique non-directory matches get a trailing
 *   ' ' (unless already followed by whitespace).
 * - Multiple matches extend the token to their longest common prefix (LCP) and
 *   populate res->matches with sorted candidate names.
 * Returns match_count (0 = no match, 1 = unique match, >1 = ambiguous). */
int ren_complete_line(char *buf, size_t cap, size_t *len, size_t *pos,
                      ren_completion_result_t *res);

/* Format a single-line TSV STATUS response for ren's IPC socket:
 *   STATUS\t<busy|idle>\t<model>\t<tokens>\t<max_tokens>\t<bytes>\n */
int ren_format_status(char *out, size_t cap, int busy, const char *model,
                      size_t history_bytes, size_t max_tokens);

#endif /* REN_COMPLETE_H */
