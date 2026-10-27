<p align="center">
  <img src="logo.png" alt="axil-auren logo" width="220" />
</p>

# libauren

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![Vulkan/Metal/CPU](https://img.shields.io/badge/backends-Vulkan%20%7C%20Metal%20%7C%20CPU-4B8BBE)](#)
[![Inference & Embeddings](https://img.shields.io/badge/engine-inference%20%2B%20embeddings-16A34A)](#)
[![System One](https://img.shields.io/badge/decisions-Jev%20System%20One-blueviolet)](#)

> Ultra-portable local LLM inference, streaming chat, and dense vector embeddings engine powered by llama.cpp and Vulkan/Metal, served as an axil module or embedded C library with multi-model routing and Jev-compatible System One decisions. Canonical header is `<ttypt/auren.h>`.

No CUDA toolchains to manage, no gigabytes of python dependencies. Install it through your package manager or compile it from source, point it at standard GGUF quantized models, and run high-throughput text generation, dense vector embeddings, and typed decision evaluations on Linux (Vulkan/CPU) or macOS (Metal).

---

## Why libauren? (Local AI Without the Python & CUDA Bloat)

Running local AI models shouldn't require a 4 GB Python runtime, fragile Conda environments, or gigabytes of CUDA packages that break on system updates.

`libauren` is a single, compact C library (under 5 MB compiled) powered by `llama.cpp` that runs standard open GGUF models directly on your hardware using Vulkan, Apple Metal, or raw CPU.

### The Three Jobs It Does
1. **The Conversational Chatbot**: High-throughput text generation and streaming chat via standard OpenAI-compatible endpoints (`POST /v1/chat/completions`) or command-line CLI (`auren-chat`).
2. **The Semantic Memory Indexer**: Convert any text into dense 384-dimensional vector embeddings (`POST /v1/embeddings`) in under 1 millisecond, feeding directly into [libsepal](https://github.com/tty-pt/libsepal) for instant semantic vector search.
3. **The Autonomous Decision Guard**: Evaluate yes/no or multiple-choice questions with calibrated mathematical confidence scores (`POST /api/v1/systemone/`), perfect for automated agents, memory filters, and safety checks without prompt parsing fragility.

---

---

## Hardware Sizing & Scaling Tiers

Choose the tier that matches your hardware and workload requirements. `axil-auren` runs seamlessly across all tiers:

| Tier | Target Hardware | Recommended Chat Model | Recommended Embed Model | RAM Footprint | Compute Characteristics |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Tier 1: Ultra-Lightweight** | <4 GB RAM / Low-spec VPS | `Qwen2.5-0.5B-Instruct` (398 MB) | `bge-small-en-v1.5` (37 MB, 384-d) | ~500 MB total | Sub-second pure CPU latency on single-core / low-power chips. |
| **Tier 2: Balanced MoE (Default)** | 8–16 GB RAM / Standard Dev Laptop | `OLMoE-1B-7B-Instruct` (3.34 GB) | `bge-base-en-v1.5` (118 MB, 768-d) | ~3.8 GB total | **Recommended Default**: 7B-class reasoning with ~1.3B active compute speed. |
| **Tier 3: Aiming Higher / High-Fidelity** | 16–32 GB+ RAM or Discrete GPU | `Qwen2.5-7B-Instruct` (~4.7 GB) or `Qwen2.5-14B` (~9 GB) | `bge-large-en-v1.5` (330 MB, 1024-d) | ~6–12 GB total | Maximum instruction adherence, complex coding, deep semantic indexing. |

### 1-Click Download Recipes

#### Tier 2: Default Recommended Setup (OLMoE MoE + BGE-Base)
```bash
mkdir -p ~/.models

# 1. Download OLMoE 1B-7B Instruct (Sparse MoE: 1.3B active compute, 7B intelligence)
curl -L -o ~/.models/olmoe-1b-7b-instruct-q3_k_m.gguf \
  https://huggingface.co/allenai/OLMoE-1B-7B-0924-Instruct-GGUF/resolve/main/olmoe-1b-7b-0924-instruct-q3_k_m.gguf

# 2. Download BGE-Base-en-v1.5 (High-Accuracy 768-d Vector Embeddings)
curl -L -o ~/.models/bge-base-en-v1.5.gguf \
  https://huggingface.co/CompendiumLabs/bge-base-en-v1.5-gguf/resolve/main/bge-base-en-v1.5-q8_0.gguf
```

#### Tier 1: Ultra-Lightweight Alternative (<500 MB RAM)
```bash
# 1. Download Qwen 2.5 0.5B Instruct (Dense Ultra-Lightweight Chat & Judge)
curl -L -o ~/.models/qwen2.5-0.5b-instruct.gguf \
  https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf

# 2. Download BGE-Small-en-v1.5 (Fast & Lightweight 384-d Embeddings)
curl -L -o ~/.models/bge-small-en-v1.5.gguf \
  https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf/resolve/main/bge-small-en-v1.5-q8_0.gguf
```

#### Tier 3: Aiming Higher (High-Fidelity for 16-32 GB+ Systems or GPUs)
```bash
# 1. Download Qwen 2.5 7B Instruct (Dense 7B Reasoning, ~4.7 GB)
curl -L -o ~/.models/qwen2.5-7b-instruct-q4_k_m.gguf \
  https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/qwen2.5-7b-instruct-q4_k_m.gguf

# 2. Download BGE-Large-en-v1.5 (Dense 1024-d Vector Embeddings, ~330 MB)
curl -L -o ~/.models/bge-large-en-v1.5.gguf \
  https://huggingface.co/CompendiumLabs/bge-large-en-v1.5-gguf/resolve/main/bge-large-en-v1.5-q8_0.gguf
```

---

## Quickstart: Run the Multi-Model Server

`libauren` ships as the native axil module `libaxil-auren.so` (with a backward-compatible `libaxil-qllm.so` alias). Start the daemon on port `4242` with dedicated models for judging and vector embeddings:

```bash
AUREN_JUDGE_MODEL="qwen" \
AUREN_JUDGE_PATH=~/.models/qwen2.5-0.5b-instruct.gguf \
AUREN_CHAT_MODEL="qwen" \
AUREN_CHAT_PATH=~/.models/qwen2.5-0.5b-instruct.gguf \
AUREN_EMBED_MODEL="bge-small" \
AUREN_EMBED_PATH=~/.models/bge-small-en-v1.5.gguf \
axil -A -d -p 4242 -m libaxil-auren
```

*(If running outside of an axil site deployment, pass the library path: `-m /home/quirinpa/auren/lib/libaxil-auren`).*

> **Tip — Automatic Model Deduplication**: If `AUREN_CHAT_PATH` and `AUREN_JUDGE_PATH` point to the identical canonical model file and explicit IDs (`AUREN_CHAT_MODEL`, `AUREN_JUDGE_MODEL`) are omitted, `libauren` automatically merges capabilities into a single shared resident model instance (`<stem>-shared`, e.g. `qwen-shared`), preventing redundant VRAM allocation.

### Configuration via `auren.json`

For multi-model deployments with custom GPU offloading or context sizing, create an `auren.json` file (paths support `~/` tilde expansion):

```json
{
  "models": [
    {
      "id": "olmoe",
      "path": "~/.models/olmoe-1b-7b-instruct-q3_k_m.gguf",
      "capabilities": ["chat", "judge"],
      "n_ctx": 4096,
      "n_gpu_layers": 0,
      "temperature": 0.7
    },
    {
      "id": "qwen",
      "path": "~/.models/qwen2.5-0.5b-instruct.gguf",
      "capabilities": ["chat", "judge"],
      "n_ctx": 4096,
      "n_gpu_layers": 0,
      "temperature": 0.7
    },
    {
      "id": "bge-base",
      "path": "~/.models/bge-base-en-v1.5.gguf",
      "capabilities": ["embed"],
      "enable_embeddings": 1,
      "n_ctx": 512
    },
    {
      "id": "bge-small",
      "path": "~/.models/bge-small-en-v1.5.gguf",
      "capabilities": ["embed"],
      "enable_embeddings": 1,
      "n_ctx": 512
    }
  ],
  "defaults": {
    "chat": "olmoe",
    "judge": "olmoe",
    "embed": "bge-base"
  },
  "max_resident": 3
}
```

Point to it with `AUREN_CONFIG_PATH=/path/to/auren.json axil -A -d -p 4242 -m libaxil-auren`.

#### Configuration Discovery & Search Order

When `AUREN_CONFIG_PATH` is specified, `libauren` attempts to read that exact path (with tilde `~` expansion). If unset, `libauren` automatically discovers configuration files in the following priority order:
1. `./auren.json` (current working directory)
2. `~/.config/auren/config.json` (standard user configuration directory)
3. `~/.auren.json` (user home directory)

The first readable configuration file is loaded. If `AUREN_CONFIG_PATH` is explicitly set but unreadable, an error is logged via `qsyslog(QLOG_ERR, ...)`.

#### Inline `AUREN_MODELS` (Bare Array or Object)

For containerized environments or one-liner launches, `AUREN_MODELS` accepts either a full configuration object or a bare JSON array of models:

```bash
AUREN_MODELS='[{"id":"qwen","path":"~/.models/qwen2.5-0.5b-instruct.gguf","capabilities":["chat","judge"]},{"id":"bge-small","path":"~/.models/bge-small-en-v1.5.gguf","capabilities":["embed"]}]' \
axil -A -d -p 4242 -m libaxil-auren
```

---

## Verified Copy-Paste Testing

Once started, verify each capability with instant `curl` commands:

### 1. OpenAI-Compatible Chat Completions (`POST /v1/chat/completions`)

Run standard conversational chat or instruct inference:

```bash
curl -s http://localhost:4242/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "qwen",
    "messages": [
      { "role": "system", "content": "You are a concise engineering assistant." },
      { "role": "user", "content": "What is the primary benefit of zero-copy in-memory maps?" }
    ],
    "temperature": 0.7
  }'
```

**Response**:
```json
{
  "choices": [
    {
      "finish_reason": "stop",
      "index": 0,
      "message": {
        "content": "Zero-copy in-memory maps eliminate serialization and deserialization overhead by referencing data directly in cache-aligned structures, maximizing L1/L2 memory throughput and sub-microsecond retrieval.",
        "role": "assistant"
      }
    }
  ],
  "model": "qwen",
  "object": "chat.completion",
  "usage": { "completion_tokens": 38, "prompt_tokens": 28, "total_tokens": 66 }
}
```

### 1.1 First-Class Session Continuity & Prefix KV Cache Persistence

Unlike naive LLM servers that treat every chat request as a stateless "oneshot" (wiping the KV cache and re-evaluating 10,000+ system prompt tokens on every turn), `axil-auren` provides **native session continuity**:

1. Pass an `x-session-id: <session_id>` HTTP header or a `"session_id": "..."` field in the JSON body.
2. `axil-auren` binds the request to a persistent session slot with dedicated context and retains the KV cache across conversational turns.
3. On subsequent turns (Turn 2+), `axil-auren` compares the rendered conversation token stream against its cached tokens, evicting only divergent downstream cells via `llama_memory_seq_rm()` and evaluating **only the new turn delta**.
4. Large system prompts and tool schemas (~15,000 tokens) are ingested once on Turn 1; subsequent turns respond in milliseconds with up to **15x+ speedup**.

```bash
# Turn 1: Starts session and ingests system context
curl -s http://localhost:4242/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "x-session-id: my-session-42" \
  -d '{
    "model": "qwen",
    "messages": [
      { "role": "system", "content": "You are a coding assistant. Project rule: C99 standard." },
      { "role": "user", "content": "What is the project standard?" }
    ]
  }'

# Turn 2: Reuses KV cache for system prompt and previous turn — evaluates ONLY the new question!
curl -s http://localhost:4242/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "x-session-id: my-session-42" \
  -d '{
    "model": "qwen",
    "messages": [
      { "role": "system", "content": "You are a coding assistant. Project rule: C99 standard." },
      { "role": "user", "content": "What is the project standard?" },
      { "role": "assistant", "content": "The project standard is C99." },
      { "role": "user", "content": "Confirm standard in two words." }
    ]
  }'
```

### 1.2 Pi Coding Agent Integration

`axil-auren` is fully compatible with modern autonomous agent loops such as the [Pi coding agent](https://github.com/earendil-works/pi):
- Supports OpenAI content block arrays (`[{"type":"text","text":"..."}]`) alongside strings.
- Supports assistant messages with `tool_calls` and null/empty content.
- Full SSE delta streaming spec compliance (`id`, `object: "chat.completion.chunk"`, `created`, `model`, `finish_reason`).
- Scaled CPU matrix-multiplication batch threading utilizing all online cores.

Configure `~/.pi/agent/models.json`:
```json
{
  "providers": {
    "auren": {
      "name": "axil-auren",
      "baseUrl": "http://localhost:4242/v1",
      "api": "openai-completions",
      "apiKey": "local",
      "models": [
        {
          "id": "qwen",
          "name": "Qwen 2.5 0.5B Instruct",
          "contextWindow": 131072,
          "maxTokens": 32768
        }
      ]
    }
  }
}
```

Install the session continuity extension at `~/.pi/agent/extensions/auren-prefix-cache.ts` to automatically inject the workspace session ID on every request:
```ts
export default function aurenPrefixCache(pi: any) {
  pi.on("before_provider_headers", (event: any, ctx: any) => {
    if (ctx?.model?.provider === "auren") {
      event.headers["x-session-id"] = ctx?.sessionManager?.getSessionId?.() || "default";
    }
  });
}
```
Run `pi --model auren/qwen` to start coding with instant multi-turn turnaround.

### 2. Semantic Vector Embeddings (`POST /v1/embeddings`)

Generate dense float vectors for semantic search and retrieval with `libsepal`. Choose `bge-small` (384-dimensional, fastest) or `bge-base` (768-dimensional, higher accuracy). Supports single strings or batch arrays:

```bash
# Single string (bge-base, 768 dimensions):
curl -s http://localhost:4242/v1/embeddings \
  -H "Content-Type: application/json" \
  -d '{
    "model": "bge-base",
    "input": "lossless FLAC acoustic recording format"
  }'

# Or bge-small (384 dimensions) with batch array:
curl -s http://localhost:4242/v1/embeddings \
  -H "Content-Type: application/json" \
  -d '{
    "model": "bge-small",
    "input": ["lossless FLAC acoustic format", "in-memory zero-copy vectors"]
  }'
```

**Response**:
```json
{
  "data": [{ "embedding": [0.0381, -0.0412, 0.0894, ...], "index": 0, "object": "embedding" }],
  "model": "bge-base",
  "object": "list"
}
```

### 3. Jev-Compatible System One Decision (`POST /api/v1/systemone/`)

Evaluate conversational exchanges or tool events and return typed, calibrated classifications without prompt parsing:

```bash
curl -s http://localhost:4242/api/v1/systemone/ \
  -H "Content-Type: application/json" \
  -d '{
    "model": "qwen",
    "state": "User: We configured Redis with 3 shards to scale caching.\nAssistant: Noted, 3 shards configured.",
    "questions": {
      "memory_action": {
        "type": "choice",
        "instructions": "Choose whether to remember a new fact, update an existing memory, or skip.",
        "criteria": { "remember": "New fact", "update": "Enrich existing memory", "skip": "Trivial" }
      },
      "is_technical": {
        "type": "noul",
        "instructions": "Is this a technical decision?"
      }
    }
  }'
```

**Response**:
```json
{
  "model": "qwen",
  "answers": {
    "memory_action": {
      "type": "choice",
      "choice": "update",
      "probabilities": { "update": 0.90, "remember": 0.05, "skip": 0.05 },
      "confidence": 0.90
    },
    "is_technical": {
      "type": "noul",
      "noul": 0.95
    }
  },
  "usage": { "input_tokens": 82, "output_tokens": 25 }
}
```

### 4. Model Discovery (`GET /v1/models`)
OpenAI-compatible model listing for automated client and SDK discovery:

```bash
curl -s http://localhost:4242/v1/models
```

**Response**:
```json
{
  "object": "list",
  "data": [
    { "id": "qwen", "object": "model", "owned_by": "local" },
    { "id": "bge-small", "object": "model", "owned_by": "local" },
    { "id": "bge-base", "object": "model", "owned_by": "local" }
  ]
}
```

### 5. Interactive Terminal Chat (`auren-chat`)

Talk directly to the running server from the command line:

```bash
# Connect to localhost:4242 (or configure port via AUREN_PORT)
auren-chat

# Or specify a custom port:
AUREN_PORT=4242 auren-chat
```

Additional CLI utilities (with legacy `qllm-*` aliases):
- `auren-list [filter]` / `qllm-list` — List local GGUF models cached in `~/.cache/huggingface/hub/` with sizes and hashes.
- `auren-path <pattern>` / `qllm-path` — Print the resolved filesystem path for a cached GGUF model.

---

## System One Question Types (The Essential 20%)

The System One API evaluates structured questions against a provided state and returns calibrated, typed responses rather than unstructured prose:

| Type | Wire Schema | Output Values | Purpose |
| :--- | :--- | :--- | :--- |
| **`choice`** | `{"type": "choice", "instructions": "...", "criteria": {"optA": "...", "optB": "..."}}` | `{choice, probabilities, confidence}` | Categorical routing & decision selection. Criteria descriptions and allowed choices are injected into the evaluation prompt, with semantic alias mapping for quest and conversational directives. |
| **`noul`** | `{"type": "noul", "instructions": "Is condition X true?"}` | `{noul: float [0.0 - 1.0]}` | Boolean probability verification (e.g. destructive action detection, leak checks). |
| **`score`** | `{"type": "score", "instructions": "...", "criteria": ["low", "med", "high"]}` | `{score: float, confidence}` | Ordered rubric evaluation (e.g. relevance or impact scoring). |

---

## Design Philosophy: Multi-Model Safety

* **Role Isolation**: Generative chat, memory decision judging, and vector embeddings are independently configurable. An embedding request will never accidentally run against the generative model simply because it is the current default.
* **Safe Model Lifecycle**: The runtime supports multiple resident models up to `max_resident`. When GPU/RAM capacity is reached, idle models (`in_flight == 0`) are safely evicted using an LRU policy. Models currently processing in-flight requests are **never** evicted.
* **Single-Model Compatibility**: If only `AUREN_MODEL_PATH` is supplied, `libauren` automatically registers `"default"` supporting all three roles (`chat`, `judge`, `embed`) with zero breaking changes.
* **Worker-Thread Concurrency & CPU Clamping**: Inference executes on a dedicated background worker thread communicating over a non-blocking self-pipe with axil's main event loop. Automatic CPU thread pools are clamped to 4 cores to prevent OpenMP spinlock thrashing on multi-core host systems and WSL.

---

## Core API (The Essential 20%)

The canonical C interface is exposed in `<ttypt/auren.h>` (with legacy forwarding in `<ttypt/qllm.h>`):

| Function | Signature | Description |
| :--- | :--- | :--- |
| `auren_create` | `struct auren_context *auren_create(const struct auren_config *cfg)` | Create model context with auto GPU offload and Q4_0 KV |
| `auren_free` | `void auren_free(struct auren_context *ctx)` | Free model context and session memory |
| `auren_generate` | `long auren_generate(ctx, prompt, out, out_sz)` | Synchronous text generation into user-allocated buffer |
| `auren_generate_stream` | `long auren_generate_stream(ctx, prompt, cb, ud)` | Streaming generation delivering UTF-8 text pieces |
| `auren_generate_stream_full` | `long auren_generate_stream_full(ctx, prompt, cb, ud)` | Streaming generation emitting token IDs and text pieces |
| `auren_embed` | `long auren_embed(ctx, text, out, out_dim)` | Generate dense vector embeddings for semantic search |
| `auren_n_embd` | `int auren_n_embd(struct auren_context *ctx)` | Query model embedding dimension (e.g. 384 for bge-small, 768 for bge-base) |
| `auren_chat` | `int auren_chat(ctx, msgs, n, prev_prompt, cb, ud)` | Multi-turn chat with token-level prefix KV cache reuse |
| `auren_prime` | `int auren_prime(struct auren_context *ctx, prompt)` | Pre-decode a prompt into the KV cache |
| `auren_prime_block` | `int auren_prime_block(ctx, hash, text)` | Content-addressed 0 ms bypass of cached prompt blocks |
| `auren_reset` | `void auren_reset(struct auren_context *ctx)` | Clear KV cache and reset generation sequence |
| `auren_set_seq` | `void auren_set_seq(ctx, uint32_t seq_id)` | Switch active sequence index for multi-session KV |

👉 *For complete deployment recipes, session continuity, and Pi agent integration, see [docs/USAGE.md](docs/USAGE.md). For the complete C function reference, see [docs/API.md](docs/API.md).*

---

## C API Quickstart (`ttypt/auren.h`)

Embed local inference and vector embeddings directly into your C application in under 30 lines:

### 1. High-Throughput Text Generation
```c
#include <stdio.h>
#include <ttypt/auren.h>

int main(int argc, char **argv) {
    struct auren_config cfg = {
        .model_path = argc > 1 ? argv[1] : "qwen2.5-0.5b-instruct.gguf",
        .temperature = 0.7f,
        .n_ctx = 2048
    };

    struct auren_context *ctx = auren_create(&cfg);
    if (!ctx) {
        fprintf(stderr, "Failed to load model\n");
        return 1;
    }

    char out[1024];
    long bytes = auren_generate(ctx, "Explain in-memory key-value maps briefly.", out, sizeof(out));
    if (bytes > 0) {
        printf("Response:\n%s\n", out);
    }

    auren_free(ctx);
    return 0;
}
```

### 2. Dense Vector Embeddings
```c
#include <stdio.h>
#include <ttypt/auren.h>

int main(void) {
    struct auren_config cfg = {
        .model_path = "bge-small-en-v1.5.gguf",
        .enable_embeddings = 1,
        .n_ctx = 512
    };

    struct auren_context *ctx = auren_create(&cfg);
    if (!ctx) return 1;

    float vec[384];
    long dim = auren_embed(ctx, "in-memory zero-copy vectors", vec, 384);
    if (dim > 0) {
        printf("Generated %ld-dim embedding vector: [%.4f, %.4f, ...]\n", dim, vec[0], vec[1]);
    }

    auren_free(ctx);
    return 0;
}
```

Compile and link:
```bash
cc app.c $(pkg-config --cflags --libs axil-auren)
```

---

## Installation & Build

### Prebuilt Packages
Prebuilt packages are distributed from tty.pt for Linux (APT / Alpine / Arch / Fedora-RHEL), macOS (Homebrew), Windows (winget / MSYS2), and OpenBSD. Follow the [installation instructions](https://github.com/tty-pt/ci/blob/main/docs/install.md) and use **libauren** (or **axil-auren**) as the package name.

### Build from Source
```sh
make                  # builds lib/libaxil-auren.so, bin/auren-chat, and bin/engine-test
make test             # run unit, engine, and session continuity tests
sudo make install     # install headers, binaries, and module to system
```

## License

BSD 2-Clause License. Copyright (c) 2026, tty-pt. See `LICENSE`.
