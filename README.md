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

`libauren` (and its axil module `libaxil-auren.so`) provides the local neural backbone for the tty-pt ecosystem:
- **High-Throughput Local Inference**: Multi-turn chat, GGUF quantization, OpenAI-compatible `/v1/chat/completions`, streaming tokens, and sampling controls.
- **Dense Vector Embeddings**: OpenAI-compatible `/v1/embeddings`, batch vector extraction, sub-millisecond latencies, and direct integration with [libsepal](https://github.com/tty-pt/libsepal) for semantic search.
- **Autonomous System One Decision Engine**: Calibrated classification, choice / noul / score rubrics for autonomous agent loops (`/api/v1/systemone/`), used by [pi-mm](https://github.com/tty-pt/pi-mm) (persistent memory) and [pi-quest](https://github.com/tty-pt/pi-quest) (quest gating).

---

## Get Cheap & Fast Models in 60 Seconds

You don't need a 70B model or expensive cloud GPUs. You can run blazing-fast local generation, structured memory judgement, and semantic embeddings with less than 600 MB of total disk footprint:

| Role | Recommended Model | Size | Quantization | Context | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Chat & Generation** | `Qwen2.5-0.5B-Instruct` | **398 MB** | Q4_K_M | 4096 | Conversational chat, instruction following & System One decisions |
| **High-Fidelity Reasoning** | `Qwen2.5-1.5B-Instruct` | **986 MB** | Q4_K_M | 4096 | Higher reasoning fidelity for multi-step tasks and complex rubrics |
| **Vector Embeddings** | `bge-small-en-v1.5` | **133 MB** | Q8_0 | 512 | Dense 384-dimensional vector embeddings for semantic vector search |

### 1-Click Download Recipe

Download both models directly to `~/.models/` using `curl`:

```bash
mkdir -p ~/.models

# 1. Download Qwen 2.5 0.5B Instruct (Generative Chat + System One Judge)
curl -L -o ~/.models/qwen2.5-0.5b-instruct.gguf \
  https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf

# 2. Download BGE-Small-en-v1.5 (Semantic Vector Embeddings)
curl -L -o ~/.models/bge-small-en-v1.5.gguf \
  https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf/resolve/main/bge-small-en-v1.5-q8_0.gguf
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
      "id": "qwen",
      "path": "~/.models/qwen2.5-0.5b-instruct.gguf",
      "capabilities": ["chat", "judge"],
      "n_ctx": 4096,
      "n_gpu_layers": 32,
      "temperature": 0.7
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
    "chat": "qwen",
    "judge": "qwen",
    "embed": "bge-small"
  },
  "max_resident": 2
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

### 2. Semantic Vector Embeddings (`POST /v1/embeddings`)

Generate dense float vectors for semantic search and retrieval with `libsepal`. Supports single strings or batch arrays:

```bash
# Single string:
curl -s http://localhost:4242/v1/embeddings \
  -H "Content-Type: application/json" \
  -d '{
    "model": "bge-small",
    "input": "lossless FLAC acoustic recording format"
  }'

# Or batch array:
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
  "model": "bge-small",
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
    { "id": "bge-small", "object": "model", "owned_by": "local" }
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
| **`choice`** | `{"type": "choice", "instructions": "...", "criteria": {"optA": "...", "optB": "..."}}` | `{choice, probabilities, confidence}` | Categorical routing & memory action selection (`remember` / `update` / `skip`). |
| **`noul`** | `{"type": "noul", "instructions": "Is condition X true?"}` | `{noul: float [0.0 - 1.0]}` | Boolean probability verification (e.g. destructive action detection, leak checks). |
| **`score`** | `{"type": "score", "instructions": "...", "criteria": ["low", "med", "high"]}` | `{score: float, confidence}` | Ordered rubric evaluation (e.g. relevance or impact scoring). |

---

## Design Philosophy: Multi-Model Safety

* **Role Isolation**: Generative chat, memory decision judging, and vector embeddings are independently configurable. An embedding request will never accidentally run against the generative model simply because it is the current default.
* **Safe Model Lifecycle**: The runtime supports multiple resident models up to `max_resident`. When GPU/RAM capacity is reached, idle models (`in_flight == 0`) are safely evicted using an LRU policy. Models currently processing in-flight requests are **never** evicted.
* **Single-Model Compatibility**: If only `AUREN_MODEL_PATH` is supplied, `libauren` automatically registers `"default"` supporting all three roles (`chat`, `judge`, `embed`) with zero breaking changes.
* **Worker-Thread Concurrency**: Inference executes on a dedicated background worker thread communicating over a non-blocking self-pipe with axil's main event loop.

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

## Build & Test

```bash
make            # Builds lib/libaxil-auren.so, bin/auren-chat, and bin/engine-test
make test       # Runs the comprehensive native test suite
sudo make install
```

## License

BSD 2-Clause License. Copyright (c) 2025, tty-pt. See `LICENSE`.
