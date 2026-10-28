#ifndef OPENAI_CLASSIFY_H
#define OPENAI_CLASSIFY_H

/*
 * OpenAI-compatible /v1/classify endpoint for axil-auren.
 *
 * Registers POST /v1/classify and POST /api/v1/classify routes.
 * Takes a probe text and a map of label -> example sentences.
 * Runs in-memory embedding and SIMD dot products inside the engine worker thread,
 * returning the winning choice, cosine similarity, runner-up margin, and per-label scores.
 */

int openai_classify_init(void);
void openai_classify_shutdown(void);

#endif /* OPENAI_CLASSIFY_H */
