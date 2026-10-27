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
      "n_ctx": 131072,
      "n_gpu_layers": 0,
      "temperature": 0.7
    },
    {
      "id": "bge-base",
      "path": "~/.models/bge-base-en-v1.5.gguf",
      "capabilities": ["embed"],
      "n_ctx": 512,
      "n_gpu_layers": 0
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
    "chat": "olmoe",
    "judge": "olmoe",
    "embed": "bge-base"
  },
  "max_resident": 3
}
```

### Mixture of Experts (MoE) Support
`axil-auren` natively supports sparse MoE models like `OLMoE-1B-7B-Instruct`. With 64 experts and 8 active experts per token, `OLMoE` evaluates only **~1.3B parameters of compute per token**, delivering generation latency close to a 1B model while maintaining the knowledge, context handling, and reasoning depth of a 7B model. Thanks to `mmap`, weights are dynamically paged without requiring all expert layers to be actively resident in compute registers.

### Aiming Higher: Hardware Sizing & Scaling Guide
Different environments require different trade-offs between memory footprint, latency, and reasoning capability. `axil-auren` supports 3 clear hardware tiers:

#### Tier 1: Ultra-Lightweight (<4 GB RAM or Low-Spec VPS)
- **Generative Model**: `Qwen2.5-0.5B-Instruct` (398 MB Q4_K_M, 4096 to 131K context).
- **Embedding Model**: `bge-small-en-v1.5` (37 MB Q8_0, 384-dimensional).
- **RAM Footprint**: Under 500 MB total resident memory.
- **Use Case**: Embedded boards, shared VPS, sub-second single-core CPU execution.

#### Tier 2: Balanced MoE (8–16 GB RAM — Recommended Default)
- **Generative Model**: `OLMoE-1B-7B-Instruct` (3.34 GB Q3_K_M, 64 experts, 8 active per token).
- **Embedding Model**: `bge-base-en-v1.5` (118 MB Q8_0, 768-dimensional).
- **RAM Footprint**: ~3.8 GB total resident memory.
- **Use Case**: Standard developer laptops and workstations without discrete GPUs. Delivers full 7B-class reasoning and instruction adherence while sustaining ~15–25 tokens/sec CPU throughput due to only ~1.3B active parameters per token.

#### Tier 3: Aiming Higher (16–32 GB+ RAM or Dedicated GPU Offload)
For users on high-spec workstations or systems with Vulkan/CUDA GPUs who want maximum reasoning depth:
- **Generative Model**: `Qwen2.5-7B-Instruct` (~4.7 GB Q4_K_M) or `Qwen2.5-14B-Instruct` (~9.0 GB Q4_K_M).
- **Embedding Model**: `bge-large-en-v1.5` (330 MB Q8_0, 1024-dimensional).
- **RAM / VRAM Footprint**: ~6 GB to 12 GB.
- **Configuration**:
  ```json
  {
    "models": [
      {
        "id": "qwen-7b",
        "path": "~/.models/qwen2.5-7b-instruct-q4_k_m.gguf",
        "capabilities": ["chat", "judge"],
        "n_ctx": 8192,
        "n_gpu_layers": 33
      },
      {
        "id": "bge-large",
        "path": "~/.models/bge-large-en-v1.5.gguf",
        "capabilities": ["embed"],
        "enable_embeddings": 1,
        "n_ctx": 512
      }
    ],
    "defaults": {
      "chat": "qwen-7b",
      "judge": "qwen-7b",
      "embed": "bge-large"
    }
  }
  ```
- **Note on CPU vs. GPU**: Dense 7B models compute all 7B parameters for every token. On CPU-only systems without GPUs, dense 7B models will generate at ~3–6 tokens/second, whereas Tier 2's `OLMoE` MoE generates at ~15–25 tokens/second with comparable reasoning capabilities.

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

Generates dense float embeddings using `bge-small-en-v1.5` (384-dimensional, 37 MB, ultra-lightweight and fast) or `bge-base-en-v1.5` (768-dimensional, 118 MB, higher retrieval accuracy) in under a few milliseconds.

#### Single Text Embedding (bge-base, 768 dimensions):
```bash
curl -s http://localhost:4242/v1/embeddings \
  -H "Content-Type: application/json" \
  -d '{
    "model": "bge-base",
    "input": "in-memory zero-copy vectors"
  }'
```

#### Batch Array Embeddings (bge-small, 384 dimensions):
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
