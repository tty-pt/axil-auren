# libauren C API Reference

The canonical C interface for `libauren` (formerly `libqllm`) is exposed in `<ttypt/auren.h>` (with backwards-compatibility aliases in `<ttypt/qllm.h>`).

All functions are `extern "C"`-safe and link against `libaxil-auren.so` (or `libauren.so`).

```bash
# Link using pkg-config:
cc app.c $(pkg-config --cflags --libs axil-auren)
```

---

## Data Structures

### `struct auren_config`
Configuration parameters for initializing a model and context:

```c
struct auren_config {
    const char   *model_path;        /**< Required: path to GGUF model file */
    int32_t       n_ctx;             /**< Context size (0 = read from GGUF, default: 2048) */
    int32_t       n_threads;         /**< Number of CPU threads (default: half of CPUs) */
    uint32_t      max_offload_bytes; /**< Max byte offload limit */
    int32_t       n_contexts;        /**< How many contexts to account for in VRAM */
    int32_t       n_gpu_layers;      /**< Force this many GPU layers (0 = auto / CPU) */
    float         temperature;       /**< Sampling temperature (default 0.7, 0.0 = greedy) */
    int32_t       top_k;             /**< Top-k sampling (default 40, 0 = disabled) */
    float         top_p;             /**< Nucleus sampling (default 0.95, 1.0 = disabled) */
    float         repeat_penalty;    /**< Repetition penalty (default 1.1, 1.0 = disabled) */
    int32_t       repeat_last_n;     /**< Lookback penalty window (default 64) */
    int           enable_embeddings; /**< Enable embeddings + mean pooling */
};
```

### `struct auren_message`
Chat message representing one conversational turn:

```c
struct auren_message {
    const char *role;    /**< "system" | "user" | "assistant" */
    const char *content; /**< Message text content */
};
```

### Callback Types
```c
/* Streaming text chunk callback */
typedef void (*auren_token_cb)(void *user, const char *chunk, size_t len);

/* Streaming full token metadata callback */
typedef void (*auren_token_full_cb)(void *user, int32_t token, const char *chunk, size_t len);
```

---

## Lifecycle Management

### `auren_create`
```c
struct auren_context *auren_create(const struct auren_config *cfg);
```
Creates a new inference context and loads the specified model. Model weights are shared across contexts via reference counting. Uses $Q4\_0$ quantized KV caching for low memory footprint. Returns `NULL` on failure.

### `auren_free`
```c
void auren_free(struct auren_context *ctx);
```
Frees an inference context and its dedicated KV cache. Decrements the shared model's reference count. Safe to call with `NULL`.

### `auren_reset`
```c
void auren_reset(struct auren_context *ctx);
```
Clears the KV cache memory and resets generation state while keeping the context and loaded model resident in RAM.

---

## Inference & Generation

### `auren_generate`
```c
long auren_generate(struct auren_context *ctx, const char *prompt, char *out, size_t out_size);
```
Synchronous non-streaming generation. Generates text into the caller-allocated buffer `out` (up to `out_size` bytes, NUL-terminated). Returns number of bytes written, or `-1` on error.

### `auren_generate_stream`
```c
int auren_generate_stream(struct auren_context *ctx, const char *prompt, auren_token_cb cb, void *user);
```
Streaming generation. Decodes prompt and streams generated UTF-8 text chunks to `cb` as they are sampled. Returns `0` on success, `< 0` on error.

### `auren_generate_stream_full`
```c
int auren_generate_stream_full(struct auren_context *ctx, const char *prompt, auren_token_full_cb cb, void *user);
```
Streaming generation with full token metadata. Invokes `cb` with both integer token ID and decoded UTF-8 string chunk.

### `auren_next`
```c
int auren_next(struct auren_context *ctx, struct llama_sampler *sampler, char *out, size_t out_size);
```
Samples and decodes the next single token as text into `out`. Returns:
- `> 0`: number of bytes written.
- `0`: end of generation (EOG/EOS reached).
- `< 0`: error.

---

## Conversational Chat & KV Prefix Persistence

### `auren_chat`
```c
int auren_chat(struct auren_context *ctx, const struct auren_message *msgs, size_t n_msgs,
               const char *prev_prompt, auren_token_cb cb, void *user);
```
Executes a multi-turn chat interaction:
1. Renders the conversation using the model's native chat template.
2. Performs token-level prefix matching against `cached_tokens`.
3. Truncates divergent downstream KV cache cells via `llama_memory_seq_rm()`.
4. Evaluates only the new turn delta starting explicitly at the matched position.
5. Streams sampled tokens to `cb`.

### `auren_render`
```c
int auren_render(struct auren_context *ctx, const struct auren_message *msgs, size_t n_msgs,
                 bool add_ass, char **out, size_t *out_len);
```
Renders a conversation transcript using the model's embedded Jinja/ChatML template. If `add_ass=true`, appends the trailing assistant generation prompt. Allocates `*out` (caller frees with `free()`).

---

## Vector Embeddings

### `auren_embed`
```c
int auren_embed(struct auren_context *ctx, const char *text, float *out, size_t out_dim);
```
Computes dense vector embeddings for `text` using mean pooling. Writes float vector into `out` (must have `out_dim >= auren_n_embd(ctx)`). Returns embedding dimension on success, `< 0` on error.

### `auren_n_embd`
```c
int auren_n_embd(struct auren_context *ctx);
```
Returns the embedding vector dimension of the loaded model (e.g. 384 for `bge-small`, 896 for `qwen2.5-0.5b`).

---

## Context Geometry & Block Caching

### `auren_prime`
```c
int auren_prime(struct auren_context *ctx, const char *prompt);
```
Evaluates `prompt` into the KV cache without sampling tokens. Useful for priming system contexts ahead of time.

### `auren_prime_block`
```c
int auren_prime_block(struct auren_context *ctx, uint64_t block_hash, const char *text);
```
Content-addressed prompt priming. If `block_hash` matches a pre-evaluated cached block at the current KV position, evaluation is bypassed in **0 ms**. If there is a cache miss, the block is evaluated and recorded.

### `auren_n_ctx`
```c
int auren_n_ctx(struct auren_context *ctx);
```
Returns the active context capacity in tokens.

### `auren_set_seq`
```c
void auren_set_seq(struct auren_context *ctx, uint32_t seq_id);
```
Selects the active sequence ID (`llama_seq_id`) within the context's KV memory, allowing multi-sequence state caching.

---

## Sliding-Window Context Compression

### `auren_anchor_start` / `auren_anchor_end`
```c
void auren_anchor_start(struct auren_context *ctx);
void auren_anchor_end(struct auren_context *ctx);
```
Marks the start and end of the protected region of the KV cache (typically the current turn prompt + response).

### `auren_compress`
```c
void auren_compress(struct auren_context *ctx, uint32_t limit);
```
Shrinks the KV cache sequence down to `limit` tokens by evicting older prefix tokens outside the anchored region. If `limit == 0`, clears the sequence.

### `auren_set_eos_bias`
```c
void auren_set_eos_bias(struct auren_context *ctx, int32_t start_tokens, float max_bias);
```
Configures logit bias on end-of-turn (EOS) tokens. Once `start_tokens` have been generated, EOS logit ramps cubically to `max_bias` to prevent runaway generations.

---

## Constrained Decoding & Samplers

### `auren_set_grammar`
```c
int auren_set_grammar(struct auren_context *ctx, const char *grammar_str);
```
Constrains subsequent generation to a GBNF grammar string. Pass `NULL` to disable.

### `auren_sampler_create` / `auren_sampler_add_grammar` / `auren_sampler_free`
```c
struct llama_sampler *auren_sampler_create(struct auren_context *ctx, const struct auren_config *cfg);
int auren_sampler_add_grammar(struct auren_context *ctx, struct llama_sampler *sampler, const char *grammar_str);
void auren_sampler_free(struct llama_sampler *smpl);
```
Low-level control over the sampling chain (penalties, top-k, top-p, temperature, and GBNF grammars).
