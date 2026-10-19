# libqllm

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![Vulkan/Metal](https://img.shields.io/badge/Vulkan%2FMetal-backends-4B8BBE)](#)
[![Multi-Model](https://img.shields.io/badge/runtime-multi--model-16A34A)](#)
[![System One](https://img.shields.io/badge/API-Jev%20System%20One-blueviolet)](#)

> Ultra-portable local LLM inference and vector embeddings engine powered by llama.cpp and Vulkan/Metal, served as an axil module with multi-model routing and Jev-compatible System One decisions.

No CUDA toolchains to manage, no gigabytes of python dependencies. Install it through your package manager or compile it from source, point it at standard GGUF quantized models, and run high-throughput text generation, typed decision evaluations, and dense vector embeddings on Linux (Vulkan) or macOS (Metal).

`axil-qllm` is the recommended local inference engine for [pi-mm](https://github.com/tty-pt/site/tree/main/.pi/extensions/pi-mm) (persistent memory) and [pi-quest](https://github.com/tty-pt/pi-quest) (quest journal and task gating), providing zero-cost, private decision judging and semantic vector search.

---

## Get Cheap & Fast Models in 60 Seconds

You don't need a 70B model or expensive cloud GPUs. You can run blazing-fast local generation, structured memory judgement, and semantic embeddings with less than 600 MB of total disk footprint:

| Role | Recommended Model | Size | Quantization | Context | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Generative & Judge** | `Qwen2.5-0.5B-Instruct` | **398 MB** | Q4_K_M | 4096 | Conversational chat & Jev System One decision evaluations |
| **Alternative Judge** | `Qwen2.5-1.5B-Instruct` | **986 MB** | Q4_K_M | 4096 | Higher reasoning fidelity for complex decision rubrics |
| **Embeddings** | `bge-small-en-v1.5` | **133 MB** | Q8_0 | 512 | Dense 384-dimensional vector embeddings for semantic search |

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

`libqllm` ships as the native axil module `libaxil-qllm.so`. Start the daemon on port `4242` with dedicated models for judging and vector embeddings:

```bash
QLLM_JUDGE_MODEL="qwen" \
QLLM_JUDGE_PATH=~/.models/qwen2.5-0.5b-instruct.gguf \
QLLM_CHAT_MODEL="qwen" \
QLLM_CHAT_PATH=~/.models/qwen2.5-0.5b-instruct.gguf \
QLLM_EMBED_MODEL="bge-small" \
QLLM_EMBED_PATH=~/.models/bge-small-en-v1.5.gguf \
axil -A -d -p 4242 -m libaxil-qllm
```

*(If running outside of an axil site deployment, pass the library path: `-m /home/quirinpa/libqllm/lib/libaxil-qllm`).*

> **Tip — Automatic Model Deduplication**: If `QLLM_CHAT_PATH` and `QLLM_JUDGE_PATH` point to the identical canonical model file and explicit IDs (`QLLM_CHAT_MODEL`, `QLLM_JUDGE_MODEL`) are omitted, `libqllm` automatically merges capabilities into a single shared resident model instance (`<stem>-shared`, e.g. `qwen-shared`), preventing redundant VRAM allocation.

### Configuration via `qllm.json`

For multi-model deployments with custom GPU offloading or context sizing, create a `qllm.json` file (paths support `~/` tilde expansion):

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

Point to it with `QLLM_CONFIG_PATH=/path/to/qllm.json axil -A -d -p 4242 -m libaxil-qllm`.

#### Configuration Discovery & Search Order

When `QLLM_CONFIG_PATH` is specified, `libqllm` attempts to read that exact path (with tilde `~` expansion). If unset, `libqllm` automatically discovers configuration files in the following priority order:
1. `./qllm.json` (current working directory)
2. `~/.config/qllm/config.json` (standard user configuration directory)
3. `~/.qllm.json` (user home directory)

The first readable configuration file is loaded. If `QLLM_CONFIG_PATH` is explicitly set but unreadable, an error is logged via `qsyslog(QLOG_ERR, ...)`.

#### Inline `QLLM_MODELS` (Bare Array or Object)

For containerized environments or one-liner launches, `QLLM_MODELS` accepts either a full configuration object or a bare JSON array of models:

```bash
QLLM_MODELS='[{"id":"qwen","path":"~/.models/qwen2.5-0.5b-instruct.gguf","capabilities":["chat","judge"]},{"id":"bge-small","path":"~/.models/bge-small-en-v1.5.gguf","capabilities":["embed"]}]' \
axil -A -d -p 4242 -m libaxil-qllm
```

---

## Verified Copy-Paste Testing

Once started, verify each capability with instant `curl` commands:

### 1. Jev-Compatible System One Decision (`POST /api/v1/systemone/`)
Evaluate conversational exchanges or tool events and return typed, calibrated classifications:

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

### 2. Semantic Vector Embeddings (`POST /v1/embeddings`)
Generate dense float vectors for semantic search with `libsepal`. Supports single strings or batch arrays:

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

### 3. OpenAI-Compatible Chat Completions (`POST /v1/chat/completions`)

```bash
curl -s http://localhost:4242/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "qwen",
    "messages": [
      { "role": "system", "content": "You are a concise engineering assistant." },
      { "role": "user", "content": "What is the primary benefit of zero-copy in-memory maps?" }
    ]
  }'
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

### 5. Interactive Terminal Chat (`qllm-chat`)

Talk directly to the running server from the command line:

```bash
# Connect to localhost:4242 (or configure port via QLLM_PORT)
qllm-chat

# Or specify a custom port:
QLLM_PORT=4242 qllm-chat
```

Additional CLI utilities:
- `qllm-list [filter]` — List local GGUF models cached in `~/.cache/huggingface/hub/` with sizes and hashes.
- `qllm-path <pattern>` — Print the resolved filesystem path for a cached GGUF model.

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
* **Single-Model Compatibility**: If only `QLLM_MODEL_PATH` is supplied, `libqllm` automatically registers `"default"` supporting all three roles (`chat`, `judge`, `embed`) with zero breaking changes.
* **Worker-Thread Concurrency**: Inference executes on a dedicated background worker thread communicating over a non-blocking self-pipe with axil's main event loop.

---

## C API Quickstart (`ttypt/qllm.h`)

Embed local inference directly into your C application in under 20 lines:

```c
#include <stdio.h>
#include <ttypt/qllm.h>

int main(int argc, char **argv) {
    struct qllm_config cfg = {
        .model_path = argc > 1 ? argv[1] : "model.gguf",
        .temperature = 0.7f,
        .n_ctx = 2048
    };

    struct qllm_context *ctx = qllm_create(&cfg);
    if (!ctx) {
        fprintf(stderr, "Failed to load model\n");
        return 1;
    }

    char out[1024];
    long bytes = qllm_generate(ctx, "Explain in-memory key-value maps briefly.", out, sizeof(out));
    if (bytes > 0) {
        printf("Response:\n%s\n", out);
    }

    qllm_free(ctx);
    return 0;
}
```

Compile and link:
```bash
cc app.c $(pkg-config --cflags --libs axil-qllm)
```

---

## Build & Test

```bash
make            # Builds lib/libaxil-qllm.so and bin/engine-test
make test       # Runs the comprehensive 91-test native C test suite
sudo make install
```

## License

BSD 2-Clause License. Copyright (c) 2025, tty-pt. See `LICENSE`.
