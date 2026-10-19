## 1.2.0

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

## 1.1.0

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