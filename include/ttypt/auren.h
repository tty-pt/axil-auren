/**
 * @file auren.h
 * @brief Local LLM inference and embeddings (llama.cpp), served as an axil module.
 *
 * Wraps llama.cpp behind a single C API: generation, streaming, embeddings,
 * conversation history, sliding-window compression and GBNF constrained
 * decoding. All functions are extern "C"-safe.
 * Formerly known as libqllm (ttypt/qllm.h).
 */

#ifndef AUREN_H
#define AUREN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle for the model + context. */
struct auren_context;

/** Opaque llama sampler (forward-declared; see llama.h for the real type). */
struct llama_sampler;

/**
 * Configuration structure for creating an Auren context.
 * All fields optional except model_path.
 */
struct auren_config {
	const char   *model_path; /**< Required */
	int32_t       n_ctx;      /**< Context size (0 = read from model GGUF, else 2048) */
	int32_t       n_threads;  /**< Number of CPU threads (default: half of CPUs) */
	uint32_t      max_offload_bytes; /**< Max byte offload */
	int32_t       n_contexts; /**< How many contexts to account for */
	int32_t       n_gpu_layers; /**< >0 force this many GPU layers (-g), 0 = auto */
	float         temperature;    /**< Sampling temperature (default 0.7, 0.0 = greedy) */
	int32_t       top_k;         /**< Top-k sampling (default 40, 0 = disabled) */
	float         top_p;         /**< Nucleus sampling (default 0.95, 1.0 = disabled) */
	float         repeat_penalty; /**< Repetition penalty (default 1.1, 1.0 = disabled) */
	int32_t       repeat_last_n;  /**< Penalty lookback window (default 64) */
	int           enable_embeddings; /**< Enable embeddings + mean pooling for auren_embed() */
};

/**
 * Create a new Auren context.
 * Returns NULL on failure.
 */
struct auren_context *
auren_create(const struct auren_config *cfg);

/**
 * Free an Auren context.
 */
void
auren_free(struct auren_context *ctx);

/**
 * Non-streaming generation.
 * Writes into `out` (user allocated).
 * Returns number of bytes written, or -1 on error.
 */
long
auren_generate(struct auren_context *ctx,
	       const char *prompt,
	       char *out,
	       size_t out_size);

/**
 * Streaming callback type.
 * `chunk` is a piece of text from generation.
 * `len` is the chunk size.
 */
typedef void (*auren_token_cb)(void *user,
			       const char *chunk,
			       size_t len);

/**
 * Full streaming callback receiving token ID and UTF-8 chunk.
 */
typedef void (*auren_token_full_cb)(void *user,
				   int32_t token,
				   const char *chunk,
				   size_t len);

/**
 * Streaming generation.
 * Calls cb() for each generated text chunk.
 * Returns 0 on success, < 0 on error.
 */
int
auren_generate_stream(struct auren_context *ctx,
		      const char *prompt,
		      auren_token_cb cb,
		      void *user);

/**
 * Streaming generation with full token metadata.
 * Calls cb() for each generated token ID and text chunk.
 * Returns 0 on success, < 0 on error.
 */
int
auren_generate_stream_full(struct auren_context *ctx,
			   const char *prompt,
			   auren_token_full_cb cb,
			   void *user);

/**
 * Compute embeddings for the entire input text.
 * Writes a vector of size >= model embedding dimension.
 *
 * Returns:
 *   >0  = embedding dimension (success)
 *   <0  = error
 */
int
auren_embed(struct auren_context *ctx,
	    const char *text,
	    float *out,
	    size_t out_dim);

/**
 * Get the model embedding dimension (returns 0 if ctx invalid).
 */
int
auren_n_embd(struct auren_context *ctx);

/**
 * Prime the context with a prompt.
 *
 * Returns:
 *   0  on success
 *  <0  on error
 */
int
auren_prime(struct auren_context *ctx,
	    const char *prompt);

/**
 * Prime the context with a content-addressed prompt block.
 * If block_hash matches a pre-evaluated cached block at the current
 * KV context position, prompt evaluation is bypassed (zero decode cost).
 *
 * Returns:
 *   0  on success
 *  <0  on error
 */
int
auren_prime_block(struct auren_context *ctx,
		  uint64_t block_hash,
		  const char *text);

/**
 * Get the actual context size used (returns 0 if ctx invalid).
 */
int
auren_n_ctx(struct auren_context *ctx);

/**
 * Set the current sequence ID for generation (multi-sequence support).
 */
void
auren_set_seq(struct auren_context *ctx, uint32_t seq_id);

/**
 * Set a GBNF grammar for constrained decoding.
 * Pass NULL to disable grammar. Returns 0 on success, -1 on error.
 */
int
auren_set_grammar(struct auren_context *ctx, const char *grammar_str);

/**
 * Create a new sampler chain (eos_bias + penalties + top_k/top_p + temp + dist)
 * based on config. Returns NULL on failure.
 */
struct llama_sampler *
auren_sampler_create(struct auren_context *ctx,
		     const struct auren_config *cfg);

/**
 * Append a GBNF grammar sampler to an existing sampler chain.
 * Returns 0 on success, -1 on error.
 */
int
auren_sampler_add_grammar(struct auren_context *ctx,
			  struct llama_sampler *sampler,
			  const char *grammar_str);

/**
 * Free a sampler chain returned by auren_sampler_create()/
 * auren_sampler_add_grammar(). Safe to call with NULL.
 */
void
auren_sampler_free(struct llama_sampler *smpl);

/**
 * Generate the next token as text.
 *
 * `sampler` may be NULL, in which case the context's internal sampler chain
 * is used.
 *
 * Returns:
 *   >0  number of bytes written to 'out' (UTF-8, NUL-terminated)
 *    0  end of generation (EOS/EOG)
 *   <0  error
 */
int
auren_next(struct auren_context *ctx,
	   struct llama_sampler *sampler,
	   char *out,
	   size_t out_size);

/** Chat message — mirrors llama_chat_message. */
struct auren_message {
	const char *role;    /**< "system" | "user" | "assistant" | ... */
	const char *content;
};

/**
 * Streaming chat completion.
 * Renders the full conversation via the model's chat template, primes the
 * context with only the newly-added increment, then streams via cb().
 * prev_prompt is the previously rendered prompt (or NULL for the first turn).
 * Returns 0 on success, < 0 on error.
 */
int
auren_chat(struct auren_context *ctx,
	   const struct auren_message *msgs, size_t n_msgs,
	   const char *prev_prompt,
	   auren_token_cb cb, void *user);

/**
 * Render a conversation with the model's chat template.
 * add_ass=true appends the assistant prefix (for priming generation);
 * add_ass=false renders the exact conversation state.
 * On success *out is malloc'd (caller frees) and *out_len is set.
 * Returns 0 on success, < 0 on error.
 */
int
auren_render(struct auren_context *ctx,
	     const struct auren_message *msgs, size_t n_msgs,
	     bool add_ass,
	     char **out, size_t *out_len);

/**
 * Reset the context's generation state: clears the KV cache, keeps the
 * loaded model and context alive.
 */
void
auren_reset(struct auren_context *ctx);

/**
 * Mark the start of the protected region of the KV cache. Call before
 * priming a prompt so that auren_compress() keeps this region (the current
 * prompt + response) while evicting older tokens.
 */
void
auren_anchor_start(struct auren_context *ctx);

/**
 * Mark the end of the protected region of the KV cache. Call after
 * generation completes.
 */
void
auren_anchor_end(struct auren_context *ctx);

/**
 * Sliding-window compression: shrink the KV sequence to `limit` tokens,
 * preferring to evict the oldest prefix and the tail beyond the anchored
 * region. limit == 0 clears the whole sequence.
 */
void
auren_compress(struct auren_context *ctx,
	       uint32_t limit);

/**
 * Configure the EOS-bias sampler. After `start_tokens` have been generated
 * the sampler ramps the end-of-turn token's logit up to `max_bias`, so long
 * generations terminate on their own.
 */
void
auren_set_eos_bias(struct auren_context *ctx,
		   int32_t start_tokens,
		   float max_bias);

/**
 * Binary decision evaluation using raw next-token logits (zero decode passes).
 * Evaluates the prompt forward pass, extracts logits of the last token,
 * compares affirmative tokens (choice_pos or default "y"/"yes") vs
 * negative tokens (choice_neg or default "n"/"no"), and computes
 * probability = sigmoid(L_pos - L_neg).
 *
 * Returns 0 on success, < 0 on error.
 */
int
auren_systemzero_eval(struct auren_context *ctx,
		      const char *prompt,
		      const char *choice_pos,
		      const char *choice_neg,
		      double *probability_out,
		      double *margin_out,
		      bool *decision_out,
		      double threshold);

#ifdef __cplusplus
}
#endif

#endif /* AUREN_H */
