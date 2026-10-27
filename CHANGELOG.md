## [1.3.1]

- **System Zero High-Efficiency Binary Decision Endpoint (`POST /api/v1/systemzero/`)**:
  - Implemented 0-decode-pass next-token logit probing: evaluates prompt forward pass once and retrieves raw output logits from `llama_get_logits_ith(ctx, -1)` without autoregressive generation or string JSON decoding.
  - Computes exact calibrated sigmoid probability ($P = \sigma(L_{pos} - L_{neg})$) between affirmative (`y`, `yes`, `proceed`, `remember`) and negative (`n`, `no`, `abort`, `skip`) token families.
  - Added support for custom binary candidate choices via `"choices": ["remember", "skip"]` or `"choice_pos"` / `"choice_neg"`.
  - Added automatic chat template wrapping (`qllm_render`) for raw user prompts.
  - Added `auren_systemzero_eval()` to `<ttypt/auren.h>` with backward-compatibility alias `qllm_systemzero_eval()` in `<ttypt/qllm.h>`.
- **Sparse Mixture of Experts (MoE) & High-Accuracy Embeddings Support**:
  - Configured `OLMoE-1B-7B-Instruct` (sparse MoE, 64 experts with 8 active, ~1.3B active compute) as default chat/judge model.
  - Configured `BGE-Base-en-v1.5` (768-d) as default vector embeddings model.
  - Added Hardware Sizing & Scaling Tiers guide across documentation (Tier 1 Ultra-Light, Tier 2 Balanced MoE Default, Tier 3 High-Fidelity).
- **Session Continuation & Multi-Turn Context Fixes**:
  - Activated compact $Q4\_0$ KV cache quantization for large context windows (`ctx_params.n_ctx >= 32768`), reducing KV cache RAM footprint by 75% and enabling stable 131,072 context execution.
  - Aligned default Qwen context length (`n_ctx`) in `auren.json` to 131,072 tokens to match Pi coding agent specifications, preventing `Engine error -4` context exhaustion on multi-turn dialogue.
  - Dynamic sliding-window dialogue trimming: when incoming conversation turns exceed the context window, `qllm_chat()` automatically drops the oldest non-system dialogue turns while preserving system instructions rather than aborting with an unhandled tokenization error.
  - Prefix cache consistency: synchronized token cache state on sliding-window KV sequence eviction (`qllm_compress()`) to prevent stale offset re-evaluations.
- **Engine Test Suite Enhancements**:
  - Updated multi-turn session persistence tests in `engine-test` with dynamic assistant turn propagation and case-insensitive keyword verification.

## [1.3.0]

- **First-Class Session Continuity & Prefix KV Cache Persistence**:
  - Native session lifecycle management via `x-session-id` HTTP request headers and JSON payload `"session_id"` field.
  - Dedicated persistent session slots in `qllm-engine` with independent context allocations (`qllm_registry_create_session_context`) sharing underlying model weights without KV cache interference.
  - Strict compliance with Axil deferred response lifecycle: `axil_header_set(cfd, "x-session-id", ...)` is called before `axil_respond_defer(cfd, 200)` to preserve HTTP response framing.
  - Token-level prefix KV reuse: `qllm_chat()` compares incoming conversation tokens against `cached_tokens`, evicting only divergent downstream cells via `llama_memory_seq_rm()` and evaluating only the incremental suffix. Cuts multi-turn prompt latency by 15x+ (e.g. 30.5s -> 2.0s on 3,500+ token prompts).
  - Preserved conversational assistant tokens in cache across generation turns for seamless delta evaluations.
- **Pi Coding Agent & Modern OpenAI Payload Compatibility**:
  - Support for structured content block arrays (`[{"type":"text","text":"..."}]`) alongside traditional strings in `POST /v1/chat/completions`.
  - Full OpenAI SSE streaming spec compliance: each delta chunk delivers `id`, `object: "chat.completion.chunk"`, `created`, `model`, and explicit `finish_reason: null`, followed by the terminal `finish_reason: "stop"`.
  - Dynamic message allocation in `worker_oneshot` replacing fixed limits.
  - Structured error propagation in SSE streams instead of silent stream drops.
- **System One Decision Calibration & Choice Prompt Formatting**:
  - Formatted question `criteria` descriptions and `choices` arrays directly into the evaluation prompt string passed to the LLM, ensuring the model is explicitly informed of allowed options rather than generating unconstrained choices.
  - Decoupled memory-specific fallback logic in `build_single_typed_answer()`: selection falls back dynamically to the question's declared criteria or choices rather than hardcoding memory-engine verbs.
  - Added semantic alias mapping for quest and conversational directives (`"instruction"`, `"task"`, `"directive"`, `"code"` -> `"quest"`, and `"dialogue"`, `"chat"` -> `"conversation"`).
- **CPU Batch Threading Optimization & Spinlock Prevention**:
  - Clamped automatic CPU thread pools (`n_threads` and `n_threads_batch`) to a maximum of 4 threads (while honoring explicit user-configured `cfg->n_threads` overrides), eliminating severe OpenMP spinlock contention and CPU thrashing in virtualized WSL environments.
  - Reduced default Qwen context length (`n_ctx`) in `auren.json` from 131,072 to 8,192, slashing unnecessary KV cache memory footprint on CPU.
  - Reduced System One decision latency by over 60x (from ~84 seconds to ~1.3 seconds).
- **CPU Batch Matrix-Multiplication Optimization**:
  - Configured `n_threads_batch = sysconf(_SC_NPROCESSORS_ONLN)` to scale prompt processing across all online CPU cores while maintaining balanced single-token generation threads.
  - Enabled $Q4\_0$ KV cache quantization for ultra-large context windows.
- **Vulkan Software Device Filter & Config Discovery**:
  - Filtered out `VK_PHYSICAL_DEVICE_TYPE_CPU` (e.g. llvmpipe) in `qllm_backend_mem_check()` to prevent pathological slowdowns when running under CPU-emulated Vulkan rasterizers.
  - Expanded configuration discovery to inspect `~/auren.json` and `~/.auren.json` in addition to `./auren.json`.
- **Automated Verification & Benchmarks**:
  - Expanded `engine-test` suite with 500+ token multi-turn session persistence tests verifying prefix reuse, delta decoding, and conversational recall.

## [1.2.0]

- **Rebranded `libqllm` → `libauren`**: The inference library and Axil dynamic module are officially rebranded to `libauren` (`<ttypt/auren.h>`, `libaxil-auren.so`, `auren.pc`). A backwards-compatibility forwarding header `<ttypt/qllm.h>` is retained.
- **Block-Aware KV Cache Reuse**: Added `auren_prime_block(ctx, block_hash, text)` (`qllm_prime_block`) allowing 0 ms bypass of prompt tokenization and decoding when a content-addressed block hash matches pre-evaluated KV slots at the active sequence pointer. Downstream cached block entries are cleanly truncated on cache divergence or reset via `auren_reset()`.
- **Full Token Streaming API**: Added `auren_generate_stream_full(ctx, prompt, cb, user)` and `auren_token_full_cb` (`qllm_generate_stream_full`) emitting both integer token IDs and decoded UTF-8 chunks during generation.
- **Packaging & CPU portability**: Auto-detect toolchain assembler support for `vpdpbusd` on x86_64, defaulting `GGML_NATIVE=ON` but gracefully falling back to `GGML_NATIVE=OFF` with `-DGGML_AVX_VNNI=OFF -DGGML_AVX512_VNNI=OFF` on systems with older binutils (such as Rocky Linux 9 / RHEL 9 binutils 2.35) where `vpdpbusd` would otherwise fail the build. Overrideable via `make GGML_NATIVE=ON/OFF`.

- **Multi-model registry & configuration discovery**: new model registry subsystem (`qllm-registry`)
  supporting up to 16 named models with explicit capability tracking
  (`chat`, `judge`, `embed`), independent role defaults, on-demand loading,
  and LRU context eviction under `max_resident`. Supports flexible configuration discovery
  checking candidate paths in priority order (`QLLM_CONFIG_PATH` if set, otherwise `./qllm.json`,
  `~/.config/qllm/config.json`, `~/.qllm.json`) with diagnostic error logging via `qsyslog`,
  bare-array `QLLM_MODELS` JSON support (allowing one-liner container setups without an object wrapper),
  automatic capability merging and deduplication for shared model paths defined via environment variables,
  or legacy single-model fallback (`QLLM_MODEL_PATH`).
- **Jev-compatible System One API**: new `POST /api/v1/systemone/` and
  `POST /v1/systemone` endpoints for fast, typed, calibrated classification
  evaluations (`choice`, `noul`, `score`) against conversational state strings,
  evaluating statelessly with automatic KV-cache reset.
- **Multi-model HTTP routing & discovery**: `POST /v1/chat/completions` and
  `POST /v1/embeddings` parse the requested `"model"` field and route to the
  matching registered model, validating capabilities and returning standard 404
  / 400 status codes on missing models or capability mismatches. Added
  `GET /v1/models` and `GET /v1/models/` returning OpenAI-compatible JSON model
  lists for automatic client discovery.
- **Batch embeddings support**: `POST /v1/embeddings` supports batch input
  via an array of strings in `"input"`, computing embeddings concurrently per
  batch item with standard index sequencing.
- **C API addition (`qllm_n_embd`)**: new `qllm_n_embd(ctx)` function in
  `<ttypt/qllm.h>` allowing applications to query a model's vector dimension
  directly.
- **Inference & memory safety**: capped batch limits (`n_batch` clamped to
  `min(n_ctx, 2048)`, `n_ubatch` to `min(n_batch, 512)`) and guarded context
  fallbacks (capped at 2048 when GGUF metadata reports `nc > 4096`). Clean
  stateless KV cache reset (`qllm_reset`) between requests for oneshot
  completions, System One judgments, and new chat sessions to prevent
  cross-request pollution.
- **`qllm-chat` client fixes**: configurable connection port via `QLLM_PORT`
  (default 4242), removed obsolete connection handshake, and proper `\r\n\r\n`
  line termination for telnet `ask` commands.
- **Unit test suite expansion**: added `test_model_registry` covering registry
  lifecycle, capability merging, role defaults, capability validation,
  model discovery JSON listing, bare-array JSON parsing, config error diagnostics,
  role model deduplication, refcounted acquire/release, and legacy fallback,
  plus `qllm_n_embd` coverage (94 tests total, all passing).
- **Documentation & developer UX**: refreshed README with recommended compact
  GGUF models (< 600 MB total disk footprint for chat, judge, and embeddings),
  1-click curl recipes, `qllm.json` schema documentation, and verified curl
  examples for all endpoints.

## [1.1.0]

- **axil module**: `libaxil-qllm` is now an axil module rather than a
  standalone daemon; `xy_install()` registers the `ask`/`chat` telnet commands
  and the OpenAI-compatible HTTP endpoints.
- **OpenAI-compatible embeddings**: new `POST /v1/embeddings` endpoint —
  single-string input, standard OpenAI JSON shape (`data[0].embedding`).
- **Worker-thread inference**: LLM inference runs on the module's worker
  thread, so it never blocks axil's single-threaded event loop.
- **Sliding-window context rework**: ported the sliding-window branch — model
  cache with refcounting, multi-context accounting (`n_contexts`),
  `qllm_compress`/`qllm_anchor_*`, `qllm_n_ctx`, history management
  (`qllm_prime`, `qllm_chat`, `qllm_render`, `qllm_reset`), and the
  sampler/grammar API (`qllm_set_grammar`, `qllm_sampler_*`).
- **Mock-based test suite**: unit (`tests/unit/core`), edge (`tests/edge`)
  and stress (`tests/stress`) tests compiled against llama/gguf/corm/vulkan
  mocks — `make test`.
- **Tools**: the `qllm-chat` client plus the `qllm-list` and `qllm-path`
  helpers.
- Dependencies updated; README shows how to download a GGUF model with `curl`
  (no `huggingface-cli` needed).

## [1.0.0] - 2026-05-02

- GPU offload improvements (`max_offload_bytes`) and better GGUF layer
  estimation.
- `qllm-chat`: tools support, first-char bug fix, larger system-prompt
  truncation.
- OpenAI-compatible streaming: final completion chunk with usage before
  `data: [DONE]`.
- `make test` target; llama/gguf sampler mocks aligned; integration
  orchestration scripts (`scripts/run-integration.sh`).
- Build / CI: winget target replacing pacman/mingw, brew fixes.

## [0.0.3] - 2026-02-01

- Add `pacman_mingw` build target; test target touched.

## [0.0.2] - 2025-12-22

- EOS bias (`qllm_set_eos_bias`).

## [0.0.1] - 2025-12-17

- Basic sliding-window context.