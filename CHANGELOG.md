## [Unreleased]

- **`ren` Line Editor, Pluggable UI Driver, Structured Tools & `ren-nvim` (`src/ren.c`, `src/libren.c`, `src/ren-complete.{c,h}`, `include/ttypt/libren.h`, `mods/`)**:
  - Added `ren_register_tool_with_schema()`, `edit_file` tool with structured JSON schemas (`READ_SCHEMA`/`WRITE_SCHEMA`/`EDIT_SCHEMA` in `mods/fs/ren-fs.c`, bash schema in `mods/bash/ren-bash.c`), and automatic project context / `AGENTS.md` & skills discovery (`16bf7b0`, `dfd33c2`).
  - Built pure-C interactive line editor with history ring, arrow/editing keys, ANSI escape constants, and Tab completion (`src/ren-complete.{c,h}`), plus IPC `STATUS` query (`f9b8666`, `d431410`, `c91df99`).
  - Added pluggable `ren_ui_driver_t` API with `TOKEN`/`TOOL_RESULT` events, fixed bottom powerline prompt with live context usage, and async interactive turns for live mid-turn steering (`173edd1`, `c505a7c`, `4c00e77`).
  - Added Neovim RPC plugin module `mods/nvim/ren-nvim.c` (`nvim_context`, `nvim_diagnostics`, `nvim_jump`, `nvim_reload`) (`ec3f9ef`).
  - Ported `libren`, `ren`, `ren-fs`, and `ren-bash` to `libstele` IPC and process APIs (`b017e11`).
  - Fixed streamed TTS clause splitting on comma boundaries before bare spaces (`bca83b1`), applied `[steer]` prefix via `append_steer_message()` across all steer paths (`9c98e55`, `ce523fe`), and fixed bottom prompt cursor placement and scroll-region restoration (`5ff83be`, `b264814`, `d4d181b`, `2d04cf2`).
- **Upstream Thought-Signature Persistence (`src/auren-upstream.c`, `tests/unit/core/test_upstream.c`)**:
  - Persisted Antigravity/Gemini thought signatures to disk across daemon restarts and prevented omission poisoning on cached tool calls (`79e04fd`).
- **Build & Service Hardening (`Makefile`, `src/libauren.c`, `services/systemd/axil-auren.service`)**:
  - Auto-enabled `ggml` Vulkan backend and Vulkan VRAM probe on Win32 builds when `VULKAN_SDK` is set (`137bf32`, `ddcaf04`).
  - Emitted `%h` paths in user systemd units, symlinked config units with `DESTDIR` guards (`68fd526`), and raised `axil` max request body buffer (`-B 33554432`, 32 MiB) in `services/systemd/axil-auren.service` (`1dbdfc9`).

## [1.6.0] - 2026-11-23

- **Zero-Config `~/.models/*.gguf` Auto-Discovery (`src/auren-registry.c`, `src/libaxil-auren.c`)**:
  - Automatically scans `$AUREN_MODELS_DIR` or `~/.models/` for `.gguf` files when no JSON config or `AUREN_MODEL_PATH` is set, deduplicates symlinks via `realpath()`, classifies embedding filenames (`bge`, `embed`, `nomic`, `minilm`, `e5-`) as `embed|judge` and generative models as `chat|judge`, and assigns default roles automatically.
  - Updated `auren_init()` to allow daemon startup without an explicit config file whenever `~/.models/*.gguf` contains valid models (`572fd4f`).
- **Unified Upstream Router, Protocol Translator & Deferred Hardening (`src/auren-upstream.{c,h}`, `src/libaxil-auren.c`, `src/auren-engine.c`, `src/openai_chat.c`)**:
  - Added unified OpenAI upstream router and protocol translator with 4-tier routing (`very-fast`, `fast`, `smart`, `genius`), HTTP/2 `libcurl` connection pooling, circuit breakers, session affinity, `reasoning_effort` forwarding, SSE `[DONE]` length fix, and `$AUREN_AUTH_PATH` / `~/.config/auren/auth.json` lookup ahead of `~/.pi/agent/auth.json` (`7bd4fa4`, `00ac113`, `e076e1b`, `572fd4f`).
  - Hardened deferred upstream response drains with socket fd epoch guards (`8998822`, `ec34fac`), tracked worker PID to avoid `pthread_join` across `fork()` (`868f9b4`), and added upstream unit tests (`tests/unit/core/test_upstream.c`).
- **`ren` Agent CLI, IPC Socket Server & `libren` Runtime (`src/libren.c`, `src/ren.c`, `include/ttypt/libren.h`, `docs/MOD_MM.md`)**:
  - Built pure-C streaming agent runtime (`libren.so`) and `ren --listen` Unix-socket IPC daemon with extensible `libxylem` modules (`mods/fs/ren-fs.c`, `mods/bash/ren-bash.c`, `mods/reflex/ren-reflex.c`), multi-context providers, `ren_call_tool()`, cooperative `ren_is_interrupted()` cancellation, mid-stream steer/abort, stall detection, and overflow compaction (`d14e8c3`, `4be5787`, `2c7644d`, `0591fbf`, `1de338f`).
  - Prevented JSON history corruption on trim and socket FD leaks to child processes (`3485b08`), added SSE chunk validation and `tests/unit/core/test_ren.c` (`2ddb8b5`), replaced per-turn `gethostbyname()` with pre-loop `getaddrinfo()` + `TCP_NODELAY` (`572fd4f`), and removed obsolete `npm/dist/axil-auren.wasm` (`d14e8c3`).
  - Decoupled core `ren` from `mod_mm` and `mod_jarvis` (moved to their owner repositories with `REN_MOD_DIR` discovery) and added `docs/MOD_MM.md` (`d628886`, `ea6e159`, `94ce95e`, `f463707`).
- **Tier-0 Deterministic `ren-reflex` Router (`mods/reflex/ren-reflex.c`)**:
  - Added sub-millisecond `<1 ms` deterministic EN/PT command matching for workspaces, windows, media/volume, browser tab open/close, mute, terminal launch, and conversation control, plus custom TSV overrides via `~/.config/ren/reflex.tsv` and fail-open to Tier-1 LLM on questions or multi-clause input (`0591fbf`, `33c61f5`).
  - Added UTF-8 Latin diacritic folding (`área` -> `area`, `próxima` -> `proxima`), polite filler prefix stripping (`please`, `can you`, `por favor`, `podes`, etc.), and expanded natural Portuguese shortcuts (`572fd4f`).
- **Cross-Platform Service Definitions & Build (`Makefile`, `services/`)**:
  - Added `systemd`, OpenBSD `rc.d`, FreeBSD `rc.d`, OpenRC, macOS `launchd`, XDG desktop autostart, and Windows service files with `install-services` target (`96c0ca2`, `ae32706`), and sanitized shared library `$ORIGIN` RPATH (`039614b`).
- **Documentation (`README.md`, `docs/USAGE.md`, `docs/API.md`, `docs/MOD_MM.md`)**:
  - Documented zero-config `~/.models/*.gguf` auto-discovery, `~/.config/auren/auth.json` credential resolution, `ren-reflex`, and replaced user-specific paths with `$HOME` / `$PREFIX` (`572fd4f`).

## [1.5.3] - 2026-11-16

- **7-OS Native Packaging & 4-Platform Node-API Prebuilds (`Makefile`, `.github/workflows/ci.yml`, `scripts/vkgen-repair.sh`, `src/*.c`)**:
  - Added Alpine Linux (`apk`), OpenBSD (`openbsd`), and Windows (`winget`) to `publish_to` (`deb,apk,rpm,pacman,brew,openbsd,winget`), enabling full 4-platform Node-API prebuild bundling (`linux-x64`, `darwin-arm64`, `darwin-x64`, `win32-x64`).
  - Made `Makefile` compatible with BSD `make` (OpenBSD) and MSYS2/MinGW-w64 (`MinGW64`), using system `glslc`/`shaderc` on Alpine Linux (`musl`) instead of the glibc LunarG SDK tarball.
  - Added CPU-only backend fallbacks (`qllm_backend_mem_check`, `qllm_backend_get_vram`) and Windows (`_WIN32`) portability across `libauren.c`, `auren-engine.c` (condition-variable wake when `pipe`/`poll` are absent), `libaxil-auren.c` (`socket_t` callback signatures and `on_axil_update` completion drain), `auren-collections.c`, `auren-registry.c`, and `auren-chat.c`.

## [1.5.2] - 2026-11-16

- **Arch Linux (`pacman`) Packaging & Toolchain Fallbacks (`.github/workflows/ci.yml`, `Makefile`)**:
  - Added `pacman` to `publish_to` (`deb,rpm,pacman,brew`) and removed `wget` from `deps_pacman` to avoid partial-upgrading `nettle` inside the Arch builder container.
  - Added `curl -fsSL` primary download with `wget` fallback when fetching the LunarG Vulkan SDK tarball in `Makefile`.
  - Checked `clang --version` / `clang++ --version` instead of `command -v clang` in `Makefile` so containers where `clang` has a broken `GLIBC_2.44` linkage automatically fall back to GCC (`${CC}` / `${CXX}`).

## [1.5.1] - 2026-11-15

- **Prebuilt `libaxil-auren` Packaging & Node-API Addon (`npm/native/auren_napi.c`, `npm/native/LIBS`, `Makefile`)**:
  - Added Node-API native addon (`auren.node`) exposing `cosine(vecA, vecB)` and anchoring `DT_NEEDED` on `libaxil-auren` and `libsepal` so `build-node-addon.sh` automatically packages `libaxil-auren.{so,dylib,dll}` and its transitive shared libraries (`libsepal`, `libjson-c`, `libvulkan`, etc.) into `npm/prebuilds/<platform>-<arch>/`.
  - Added `-Wl,--disable-new-dtags -Wl,-rpath,'$ORIGIN'` to `LDFLAGS-libaxil-auren-Linux` in `Makefile` so `libaxil-auren.so` resolves sibling prebuilt libraries directly from `prebuilds/<key>/` when dynamically loaded via `xy_load()`.
- **Xylem Module Discovery & `@tty-pt/axil` Integration (`npm/dist/index.js`, `npm/dist/index.d.ts`, `npm/package.json`)**:
  - Added `getModulePath()` to resolve the bundled `libaxil-auren` module path in `prebuilds/<platform>-<arch>/` (or local `lib/`).
  - Added `"@tty-pt/axil": "^1.8.0"` dependency so `npx axil -A -p 4242 -m axil-auren` launches the native `axil-auren` daemon out of the box without system-installed C packages.
- **Embedding Context Reuse (`src/libauren.c`)**:
  - Cleared KV memory (`llama_memory_clear`) on the existing `llama_context` instead of freeing and reallocating the context on every `/v1/embeddings` and `/v1/systemone` evaluation.

## [1.5.0] - 2026-11-13

- **Wllama WebAssembly llama.cpp Browser Inference & SmolLM2 Runtime**:
  - Unified browser execution with WebAssembly llama.cpp (Wllama) and GGUF models.
  - Added default wasm config and targetUrl resolution to wllama worker.
  - Updated worker with SmolLM2 default and concise sampling.
- **Libstele v1.6.0 Substrate Alignment**:
  - Upgraded substrate baseline to @tty-pt/stele@^1.6.0 (>=1.6.0).
  - The standalone @tty-pt/auren npm package now carries the repository version (1.5.0); it had not been published before.

## [1.4.0] - 2026-10-30

- **Unified Non-Generative System One Engine (`POST /v1/systemone`)**:
  - Unified all System 1 reflexive decision making under `POST /v1/systemone` (and `/api/v1/systemone/`).
  - 100% non-generative: backed strictly by in-memory unit-vector embeddings and SIMD cosine similarity (`QLLM_CAP_EMBED` via `bge-base` / `bge-small`), evaluating decisions with **0 generated decode tokens** in ~5–10 ms.
  - Supports three unified request shapes:
    1. **Choice Gating**: `{ prompt, choices: [...] }` evaluating candidate options via probe-to-choice vector similarity.
    2. **Nearest-Exemplar Matching**: `{ text, exemplars: {...} }` or `{ text, labels: {...} }` with in-memory vector caching.
    3. **Jev Question Evaluation**: `{ state, questions: {...} }` for conversational memory classifiers and autonomous agents.
  - All JSON responses are constructed deterministically in C via `cJSON`, eliminating generative formatting drift and regex failures (0% parse failure rate).
- **Purged Obsolete Endpoints & Legacy Modules**:
  - Completely removed `src/openai_classify.c`, `src/openai_classify.h`, `src/systemzero.c`, and `src/systemzero.h`.
  - Unregistered `/v1/classify` and `/v1/systemzero` HTTP routes (both return HTTP 404).
  - Excised `QE_JOB_SYSTEMZERO` and `QE_JOB_CLASSIFY` from the engine job queue.
  - Removed all deprecation warnings on System One; restored it as the premier cognitive decision endpoint.
- **Lightweight Default Startup Configuration (`auren.json`)**:
  - Configured `bge-base` as the default model for both `embed` and `judge`.
  - Removed heavy generative LLMs from default startup autoload; inference models (`olmoe`, `qwen`) load strictly on-demand when `/v1/chat/completions` is requested.
- **Multi-Tenant Vector Collections (`POST /v1/collections`)**:
  - Dynamic in-memory vector collection management for grouped embeddings and semantic recall using `libsepal` and `libcorm`.
- **Refactored Internal Architecture (`qllm` → `auren`)**:
  - Reorganized internal engine headers and translation units to `src/auren-engine.*`, `src/auren-registry.*`, `src/libauren.c`, and `src/auren-pending.h`.
  - Standardized all internal symbols, structs, and capability flags to `auren_*` and `AUREN_*`.
  - Excised deprecated legacy files `src/libaxil-qllm.c` and `src/qllm-chat.c`.

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