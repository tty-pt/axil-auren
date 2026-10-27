<p align="center">
  <img src="logo.png" alt="axil-auren logo" width="220" />
</p>

# axil-auren

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![Vulkan/Metal/CPU](https://img.shields.io/badge/backends-Vulkan%20%7C%20Metal%20%7C%20CPU-4B8BBE)](#)
[![Inference & Embeddings](https://img.shields.io/badge/engine-inference%20%2B%20embeddings-16A34A)](#)
[![System One](https://img.shields.io/badge/decisions-Jev%20System%20One-blueviolet)](#)

> Ultra-portable local LLM inference, streaming chat, and dense vector embeddings engine powered by llama.cpp and Vulkan/Metal, served as an axil module or embedded C library. Canonical header is `<ttypt/auren.h>`.

No CUDA toolchains to manage, no gigabytes of Python dependencies. `axil-auren` compiles to a single, compact C library (under 5 MB) that loads standard open GGUF models directly on Linux (Vulkan/CPU), macOS (Metal), and Windows.

---

## 1. Quickstart: Multi-Model Daemon

Start the local inference server on port 4242:

```bash
# Start server in foreground using auto-discovered ~/.config/auren/config.json or ~/auren.json:
scripts/auren-server
```

*(Alternatively, run as an axil module: `axil -A -d -p 4242 -m libaxil-auren`).*

### Verified `curl` Queries

```bash
# 1. Streaming Chat Generation (OpenAI-compatible)
curl -s http://127.0.0.1:4242/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"chat","messages":[{"role":"user","content":"Explain pure C in 10 words."}]}'

# 2. Dense Vector Embeddings (for libsepal semantic search)
curl -s http://127.0.0.1:4242/v1/embeddings \
  -H "Content-Type: application/json" \
  -d '{"model":"bge-base","input":"Zero-copy in-memory associative search"}'

# 3. System Zero: Instant Binary Decision (0 decode steps)
curl -s http://127.0.0.1:4242/api/v1/systemzero/ \
  -H "Content-Type: application/json" \
  -d '{"prompt":"Is this statement about programming? Statement: I wrote a C struct.\nAnswer [y/n]:"}'
```

---

## 2. Recommended Models & Hardware Tiers

Models reside in `~/.models/` and are automatically discovered:

| Tier | Target Hardware | Recommended Chat Model | Recommended Embed Model | RAM Footprint |
| :--- | :--- | :--- | :--- | :--- |
| **Tier 1: Lightweight** | <4 GB RAM / Low-spec VPS | [Qwen2.5-0.5B-Instruct](https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf) (491 MB) | [bge-small-en-v1.5](https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf/resolve/main/bge-small-en-v1.5-q4_k_m.gguf) (37 MB, 384-d) | ~500 MB total |
| **Tier 2: Balanced MoE (Default)** | 8–16 GB RAM / Standard Laptop | [OLMoE-1B-7B-Instruct](https://huggingface.co/allenai/OLMoE-1B-7B-0924-Instruct-GGUF/resolve/main/olmoe-1b-7b-0924-instruct-q3_k_m.gguf) (3.3 GB) | [bge-base-en-v1.5](https://huggingface.co/CompendiumLabs/bge-base-en-v1.5-gguf/resolve/main/bge-base-en-v1.5-q4_k_m.gguf) (118 MB, 768-d) | ~3.8 GB total |
| **Tier 3: High-Fidelity** | 16–32 GB+ RAM / Discrete GPU | Qwen2.5-7B-Instruct (~4.7 GB) | bge-large-en-v1.5 (330 MB, 1024-d) | ~6–10 GB total |

👉 *For model configuration options, resident limits, and Jinja chat templates, see [docs/USAGE.md](docs/USAGE.md).*

---

## 3. HTTP Endpoints

| Endpoint | Method | Input Model / Role | Description |
| :--- | :--- | :--- | :--- |
| `/v1/chat/completions` | `POST` | `model: "chat"` (or explicit ID) | OpenAI-compatible streaming and non-streaming text generation |
| `/v1/embeddings` | `POST` | `model: "bge-base"` / `"bge-small"` | Generate dense vector embeddings for semantic search |
| `/api/v1/systemzero/` | `POST` | `model: "judge"` | Zero-decode binary logit-probe decision evaluation |
| `/api/v1/systemone/` | `POST` | `model: "judge"` | Jev-compatible structured choice question classifier |
| `/v1/models` | `GET` | *none* | List all registered models and active capabilities |

---

## 4. Embedded C API Reference (`<ttypt/auren.h>`)

Embed local inference directly into your C application:

```c
#include <stdio.h>
#include <ttypt/auren.h>

int main(void) {
    struct auren_config cfg = {
        .model_path = "/home/user/.models/qwen2.5-0.5b-instruct.gguf",
        .n_ctx = 2048,
        .n_threads = 4,
    };
    struct auren_ctx *ctx = auren_create(&cfg);
    if (!ctx) return 1;

    // Prime prompt into KV cache
    auren_prime(ctx, "Explain C pointers in 5 words:");

    // Stream generated tokens
    char token[64];
    while (auren_next(ctx, token, sizeof(token)) > 0) {
        printf("%s", token);
        fflush(stdout);
    }
    printf("\n");

    auren_free(ctx);
    return 0;
}
```

Compile and link:
```sh
cc inference_demo.c $(pkg-config --cflags --libs axil-auren) -o inference_demo
```

### Core C API Functions

| Function | Signature | Description |
| :--- | :--- | :--- |
| `auren_create` | `struct auren_ctx *auren_create(cfg)` | Initialize model context and compute engine |
| `auren_prime` | `int auren_prime(ctx, prompt)` | Tokenize and evaluate prompt into KV cache |
| `auren_next` | `int auren_next(ctx, buf, bufsz)` | Decode next token from context |
| `auren_embed` | `int auren_embed(ctx, text, out, out_dim)` | Generate normalized float vector embedding |
| `auren_free` | `void auren_free(ctx)` | Release model resources and memory |

👉 *For the complete C function reference, VRAM calculators, and session KV prefix caching, see [docs/API.md](docs/API.md).*

---

## 5. Installation & Build

### Prebuilt Packages
Prebuilt packages for Linux (APT / Alpine / Arch / Fedora-RHEL), macOS (Homebrew), Windows (winget / MSYS2), and OpenBSD are distributed via [tty.pt packages](https://github.com/tty-pt/ci/blob/main/docs/install.md). Use **axil-auren** as the package name.

### Build from Source
```sh
make                  # builds lib/libaxil-auren.so and bin/auren-chat
make test             # run comprehensive unit test suite
sudo make install     # install headers, library, and launchers to system
```

## License

BSD 2-Clause License. Copyright (c) 2026, tty-pt. See `LICENSE`.
