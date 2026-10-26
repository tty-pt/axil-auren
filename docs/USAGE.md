# axil-auren Usage Guide

`axil-auren` (`libauren`) is an ultra-portable local LLM inference engine, OpenAI-compatible streaming chat server, and dense vector embeddings library powered by `llama.cpp` and Vulkan/CPU.

It runs either as a **resident Axil network module** serving HTTP on port `4242`, or as an **embedded C library** linked directly into your application via `<ttypt/auren.h>`.

---

## Table of Contents
1. [Starting the Server](#1-starting-the-server)
2. [Model Configuration (`auren.json`)](#2-model-configuration-aurenjson)
3. [First-Class Session Continuity & Prefix KV Caching](#3-first-class-session-continuity--prefix-kv-caching)
4. [Pi Coding Agent Integration](#4-pi-coding-agent-integration)
5. [OpenAI-Compatible HTTP API](#5-openai-compatible-http-api)
   - [Chat Completions (`POST /v1/chat/completions`)](#a-chat-completions-post-v1chatcompletions)
   - [Vector Embeddings (`POST /v1/embeddings`)](#b-vector-embeddings-post-v1embeddings)
   - [System One Decisions (`POST /api/v1/systemone/`)](#c-system-one-decisions-post-apiv1systemone)
   - [Model Discovery (`GET /v1/models`)](#d-model-discovery-get-v1models)
6. [Command-Line CLI Utilities](#6-command-line-cli-utilities)
7. [Embedded C Library API (`<ttypt/auren.h>`)](#7-embedded-c-library-api-ttyptaurenh)
8. [Automated Verification & Diagnostics](#8-automated-verification--diagnostics)

---

## 1. Starting the Server

### Turnkey Runner Script
The repository provides a turnkey server launcher that automatically configures library paths and discovers configuration files:

```bash
# Start server on default port 4242:
./scripts/auren-server

# Or specify a custom port:
AUREN_PORT=8000 ./scripts/auren-server
```

### Running Directly via Axil
If `axil` is installed system-wide:
```bash
LD_LIBRARY_PATH=/home/quirinpa/axil-auren/lib \
axil -A -d -p 4242 -m /home/quirinpa/axil-auren/lib/libaxil-auren
```

### Background Daemon Execution
To run in the background as a daemon:
```bash
nohup /home/quirinpa/axil-auren/scripts/auren-server > /tmp/auren.log 2>&1 &
```

To stop a running daemon:
```bash
pkill -f "axil.*4242"
```

---

## 2. Model Configuration (`auren.json`)

`axil-auren` uses a declarative JSON configuration file (`auren.json`) to define model paths, capabilities, context limits, and hardware acceleration:

```json
{
  "models": [
    {
      "id": "qwen",
      "path": "~/.models/qwen2.5-0.5b-instruct.gguf",
      "capabilities": ["chat", "judge"],
      "n_ctx": 131072,
      "n_gpu_layers": 0,
      "temperature": 0.7
    },
    {
      "id": "bge-small",
      "path": "~/.models/bge-small-en-v1.5.gguf",
      "capabilities": ["embed"],
      "n_ctx": 512,
      "n_gpu_layers": 0
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

### Key Configuration Parameters
- **`id`**: Unique string identifier referenced in API calls (e.g. `"model": "qwen"`).
- **`path`**: Filesystem path to the `.gguf` file (supports `~/` tilde expansion).
- **`capabilities`**: Bitmask roles:
  - `"chat"`: Conversational instruction following and OpenAI `/v1/chat/completions`.
  - `"judge"`: Jev-compatible System One calibrated evaluations (`/api/v1/systemone/`).
  - `"embed"`: Dense vector embeddings (`/v1/embeddings`).
- **`n_ctx`**: Maximum context window in tokens. `axil-auren` uses **$Q4\_0$ KV cache quantization**, allowing large contexts (32K to 131K tokens) with minimal RAM overhead (~1.5 GB for 131K tokens).
- **`n_gpu_layers`**: Number of transformer layers to offload to Vulkan/Metal GPU (`0` = pure CPU, `99` = full GPU offload).
- **`max_resident`**: Maximum number of model contexts held simultaneously in RAM before LRU eviction.

### Configuration Search Order
When starting up, `axil-auren` searches for configuration in the following order:
1. `AUREN_CONFIG_PATH` environment variable (if explicitly set).
2. `./auren.json` (current working directory).
3. `~/.config/auren/config.json` (user configuration directory).
4. `~/.auren.json` (user home directory).
5. `~/auren.json` (fallback user root).

---

## 3. First-Class Session Continuity & Prefix KV Caching

### The Problem in Autonomous Agent Loops
Autonomous agents (such as the Pi coding agent) typically send large system instructions, tool definitions, and project documentation (~10,000–15,000 tokens) on **every conversational turn**.

On standard stateless servers, the server wipes its KV cache on every turn. Processing 15,000 tokens from scratch on CPU can take 30–60 seconds per prompt, even when generating just a 5-token response.

### How axil-auren Solves This
`axil-auren` introduces **native session continuity**:
1. When a client includes an `x-session-id` HTTP header or `"session_id"` JSON field:
   - `axil-auren` assigns the connection to a persistent session slot in `g_engine.sessions`.
   - The session owns an independent `qllm_context` that is kept resident across turns.
2. When Turn 2 arrives with the accumulated conversation:
   - `axil-auren` renders and tokenizes the full prompt.
   - It performs **longest common prefix matching** against `cached_tokens`.
   - It evicts only divergent downstream cells via `llama_memory_seq_rm()` and evaluates **only the new turn suffix**.
3. **Turn 2+ latency drops from ~30 seconds to milliseconds**:

| Turn | Total Context | Reused from KV | Evaluated Delta | Measured Latency |
| :--- | :--- | :--- | :--- | :--- |
| **Turn 1 (Cold Ingestion)** | 3,552 tokens | 0 tokens | 3,552 tokens | **30.49s** |
| **Turn 2 (Prefix Cache Hit)** | 3,569 tokens | **3,552 tokens (99.5%)** | **17 tokens** | **2.01s (15.18x speedup)** |
| **Turn 3 (Continued Session)**| 3,584 tokens | **3,569 tokens (99.6%)** | **15 tokens** | **1.98s (15.40x speedup)** |

---

## 4. Pi Coding Agent Integration

`axil-auren` is designed to be the primary local inference backend for the [Pi coding agent](https://github.com/earendil-works/pi).

### Step 1: Configure `~/.pi/agent/models.json`
Add the `auren` provider to `/home/quirinpa/.pi/agent/models.json`:

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
          "maxTokens": 32768,
          "cost": {
            "input": 0,
            "output": 0,
            "cacheRead": 0,
            "cacheWrite": 0
          },
          "compat": {
            "supportsDeveloperRole": false,
            "supportsUsageInStreaming": false,
            "maxTokensField": "max_tokens"
          }
        }
      ]
    }
  }
}
```

### Step 2: Install the Session Continuity Extension
Ensure the provider hook is active at `~/.pi/agent/extensions/auren-prefix-cache.ts`:

```ts
export default function aurenPrefixCache(pi: any) {
  function getActiveSessionId(ctx: any): string {
    return ctx?.sessionManager?.getSessionId?.() || "default-session";
  }

  pi.on("before_provider_headers", (event: any, ctx: any) => {
    if (ctx?.model?.provider === "auren") {
      event.headers["x-session-id"] = getActiveSessionId(ctx);
    }
  });

  pi.on("before_provider_request", (event: any, ctx: any) => {
    if (ctx?.model?.provider === "auren") {
      if (event?.payload && typeof event.payload === "object") {
        event.payload.session_id = getActiveSessionId(ctx);
      }
    }
  });
}
```

### Step 3: Run Pi with Local Inference
```bash
# Launch interactive coding session:
pi --model auren/qwen

# Or run a single instruction:
pi --model auren/qwen -p "Review git status and tell me what is uncommitted."
```

---

## 5. OpenAI-Compatible HTTP API

### A. Chat Completions (`POST /v1/chat/completions`)

Supports both traditional string messages and modern array-of-blocks content (`[{"type":"text","text":"..."}]`).

#### Non-Streaming Example:
```bash
curl -s http://localhost:4242/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "x-session-id: dev-session-1" \
  -d '{
    "model": "qwen",
    "messages": [
      { "role": "system", "content": "You are a concise engineering assistant." },
      { "role": "user", "content": "What is the primary benefit of zero-copy in-memory maps?" }
    ],
    "temperature": 0.7,
    "stream": false
  }'
```

#### Streaming SSE Example:
```bash
curl -N -s http://localhost:4242/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "x-session-id: dev-session-1" \
  -d '{
    "model": "qwen",
    "messages": [
      { "role": "system", "content": "You are a concise engineering assistant." },
      { "role": "user", "content": "Write a C function to reverse a string." }
    ],
    "stream": true
  }'
```

### B. Vector Embeddings (`POST /v1/embeddings`)

Generates dense 384-dimensional float embeddings using `bge-small-en-v1.5` in under 1 millisecond.

#### Single Text Embedding:
```bash
curl -s http://localhost:4242/v1/embeddings \
  -H "Content-Type: application/json" \
  -d '{
    "model": "bge-small",
    "input": "in-memory zero-copy vectors"
  }'
```

#### Batch Array Embeddings:
```bash
curl -s http://localhost:4242/v1/embeddings \
  -H "Content-Type: application/json" \
  -d '{
    "model": "bge-small",
    "input": [
      "zero-copy memory mapping",
      "spatial morton z-order curve",
      "accent-preserving lemma tokenizer"
    ]
  }'
```

### C. System One Decisions (`POST /api/v1/systemone/`)

Evaluates structured questions against conversational state, returning calibrated mathematical probabilities rather than free-form text. Ideal for memory classifiers, autonomous agents, and safety filters.

Question `criteria` definitions and `choices` arrays are automatically formatted into the evaluation prompt with allowed option constraints. Semantic aliases (e.g. `"instruction"`, `"task"`, `"directive"` mapping to `"quest"`) are supported out of the box. Automatic CPU batch threads are clamped to 4 cores to ensure sub-second latency and prevent spinlock thrashing on multi-core host systems.

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

### D. Model Discovery (`GET /v1/models`)

```bash
curl -s http://localhost:4242/v1/models
```

---

## 6. Command-Line CLI Utilities

The repository provides several native CLI utilities located in `./scripts/` and `./bin/`:

- **`auren-chat`**: Interactive REPL connecting directly to port 4242:
  ```bash
  ./bin/auren-chat
  # Or with custom port:
  AUREN_PORT=4242 ./bin/auren-chat
  ```
- **`scripts/auren-list`**: List all GGUF models cached in `~/.cache/huggingface/hub/` with file sizes and hashes:
  ```bash
  ./scripts/auren-list
  ./scripts/auren-list qwen
  ```
- **`scripts/auren-path <pattern>`**: Print the exact resolved absolute path for a cached GGUF model:
  ```bash
  ./scripts/auren-path qwen2.5-0.5b
  ```

---

## 7. Embedded C Library API (`<ttypt/auren.h>`)

Link `libaxil-auren.so` directly into C applications without network overhead.

### Text Generation Example
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

### Dense Vector Embeddings Example
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
        printf("Generated %ld-dim vector: [%.4f, %.4f, ...]\n", dim, vec[0], vec[1]);
    }

    auren_free(ctx);
    return 0;
}
```

### Compiling and Linking
```bash
cc app.c $(pkg-config --cflags --libs axil-auren)
```

---

## 8. Automated Verification & Diagnostics

Run the standalone native driver to verify inference, embedding dimensions, and multi-turn session persistence without HTTP or external daemons:

```bash
LD_LIBRARY_PATH=/home/quirinpa/axil-auren/lib \
/home/quirinpa/axil-auren/bin/engine-test ~/.models/qwen2.5-0.5b-instruct.gguf
```

Expected output:
```text
puddle: dim=896 max|v|=77.6 finite=1 ok
turn1: 3 chunks, seq ok, full reply (7 bytes)
turn2: recalled history ('cherry')
after-reset: ok (7 bytes)
oneshot(non-stream): object=chat.completion finish=stop (312 bytes)
oneshot(stream): 33 delta chunks + stop
oneshot(array-content): success (319 bytes)
session-continuity: 500+ token context verified, turn1=13269.6ms, turn2=5308.0ms, recalled secret token ok
PASS: engine embed/chat/oneshot (61 results, 14 final)
```
