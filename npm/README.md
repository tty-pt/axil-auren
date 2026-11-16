<p align="center">
  <img src="https://raw.githubusercontent.com/tty-pt/axil-auren/main/assets/logo.png" alt="@tty-pt/auren logo" width="220" />
</p>

# @tty-pt/auren

[![npm version](https://img.shields.io/npm/v/@tty-pt/auren.svg)](https://www.npmjs.com/package/@tty-pt/auren)
[![License: BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](https://github.com/tty-pt/axil-auren/blob/main/LICENSE)
[![TypeScript Ready](https://img.shields.io/badge/TypeScript-Ready-3178C6?logo=typescript)](#)
[![Node.js Native](https://img.shields.io/badge/Node.js-N--API-339933?logo=node.js)](#)
[![WebAssembly](https://img.shields.io/badge/WebAssembly-Wllama-654FF0?logo=webassembly)](#)
[![Zero Dependencies](https://img.shields.io/badge/dependencies-0-success)](#)

> Local GGUF LLM inference, OpenAI-compatible streaming chat, dense vector embeddings, Jev System 1 reflexive classification, and multi-tenant persistent collections for Node.js and the Browser.

📖 **Full C & Systems Documentation**: See the main [**axil-auren GitHub README**](https://github.com/tty-pt/axil-auren#readme) for the complete C API (`<ttypt/auren.h>`), HTTP daemon architecture, and model configuration guide.

---

## 1. Overview & Domain Concepts

`axil-auren` (`libaxil-auren.so` / `@tty-pt/auren`) is a zero-cloud local inference engine built on `llama.cpp` (with Vulkan, Metal, and CPU acceleration), [`libcorm`](https://github.com/tty-pt/libcorm), and [`libsepal`](https://github.com/tty-pt/libsepal):

1. **Three Hardware & RAM Model Tiers (`~/.models/`)**:
   - **Tier 1 (Lightweight, ~500 MB RAM)**: `Qwen2.5-0.5B-Instruct` (chat) + `bge-small-en-v1.5` (384-d embeddings) for low-spec VPS and edge nodes.
   - **Tier 2 (Balanced MoE Default, ~3.8 GB RAM)**: `OLMoE-1B-7B-Instruct` (chat) + `bge-base-en-v1.5` (768-d embeddings) for standard laptops and workstations.
   - **Tier 3 (High-Fidelity, 6–10 GB+ RAM)**: `Qwen2.5-7B-Instruct` + `bge-large-en-v1.5` (1024-d embeddings) for discrete GPUs.
2. **Jev System 1 Reflexive Engine (`/v1/systemone`)**:
   - Sub-15ms zero-decode decision engine supporting **Choice Gating** (`labels: ["yes", "no"]`), **Exemplar Matching** (`exemplars: { label: ["phrase", ...] }`), and **Question Evaluation** (`question: "..."`) without autoregressive token generation overhead.
3. **Multi-Tenant Persistent Vector Collections (`/v1/collections/:name`)**:
   - Built-in document ingestion, automatic embedding generation, and two-stage ANN vector search backed by `libsepal` and `libcorm`.
4. **Isomorphic Browser Wllama Fallback (`AurenBrowserEngine`)**:
   - Runs `llama.cpp` directly in browser Web Workers via WebAssembly (`SmolLM2` / GGUF) with OPFS/CacheStorage model caching when no local `axil-auren` daemon is reachable.

---

## 2. Installation

```bash
npm install @tty-pt/auren
```

**Native OS Packages (`tty.pt`)**: Prebuilt native C packages (`axil-auren`) for Linux (`apt`, `apk`, `pacman`, `dnf`), macOS (`brew`), Windows (`winget`, MSYS2), and OpenBSD (`pkg_add`) are available via the [**tty.pt package repositories**](https://github.com/tty-pt/ci/blob/main/docs/install.md).

---

## 3. Quickstart

### Launch the Local `axil-auren` Server via `@tty-pt/axil`

`@tty-pt/auren` bundles prebuilt `libaxil-auren` shared libraries so `npx axil` can host the inference server on port `4242` directly from `node_modules`:

```bash
npx axil -A -p 4242 -m axil-auren
```

### Streaming Chat, Embeddings, System 1 & Collections

```typescript
import { createAurenClient, cosineSimilarity } from "@tty-pt/auren";

const auren = createAurenClient({ baseUrl: "http://localhost:4242" });

// 1. OpenAI-Compatible Streaming Chat (/v1/chat/completions)
for await (const chunk of auren.chatStream({
  model: "chat",
  messages: [{ role: "user", content: "Explain C pointers in one sentence." }]
})) {
  process.stdout.write(chunk.choices?.[0]?.delta?.content || "");
}

// 2. Dense Vector Embeddings (/v1/embeddings) & Native Cosine Similarity
const { data } = await auren.embeddings({
  model: "bge-base",
  input: ["Zero-copy memory-mapped storage", "Lexical full-text indexing"]
});
console.log("Cosine similarity:", cosineSimilarity(data[0].embedding, data[1].embedding));

// 3. Jev System 1 Reflexive Classification (~5-15ms)
const decision = await auren.systemOne({
  text: "Fix typo in README.md",
  labels: ["trivial", "complex", "architectural"]
});
console.log("System 1 choice:", decision);

// 4. Persistent Vector Collections (/v1/collections/:name)
const docs = auren.collection("engineering-notes");
await docs.insert({ id: "note-1", text: "Use libsepal for two-stage ANN cosine search." });
const matches = await docs.search("vector similarity search", 5);
console.log("Collection hits:", matches);
```

### Browser WebAssembly Inference (`AurenBrowserEngine`)

```typescript
import { AurenBrowserEngine } from "@tty-pt/auren";

const engine = new AurenBrowserEngine();
await engine.init(({ status, percent }) => {
  console.log(`${status} (${percent}%)`);
});

for await (const token of engine.chatStream({
  messages: [{ role: "user", content: "Hello from WebAssembly!" }],
  maxTokens: 128
})) {
  process.stdout.write(token);
}
```

---

## 4. JavaScript / TypeScript API Reference

### `createAurenClient(options?: AurenClientOptions): AurenClient`
Creates an isomorphic HTTP/SSE client for an `axil-auren` daemon:
- `options.baseUrl`: Server URL (default: `"http://127.0.0.1:4242"`).
- `options.defaultModel`: Default model alias (default: `"chat"`).
- `options.timeoutMs`: Request timeout in milliseconds (default: `30000`).
- `.chat(request: ChatRequest): Promise<any>`: Non-streaming chat completion (`POST /v1/chat/completions`).
- `.chatStream(request: ChatRequest): AsyncGenerator<any>`: Streaming SSE chat completion.
- `.embeddings(request: EmbedRequest): Promise<any>`: Generates normalized dense vector embeddings (`POST /v1/embeddings`).
- `.systemOne(request: SystemOneRequest): Promise<any>`: Executes System 1 reflexive classification (`POST /v1/systemone`).
- `.collection(name: string): AurenCollectionClient`: Returns a scoped handle with `.insert({ id?, text })`, `.search(query, limit?)`, and `.delete(id)`.

### `AurenBrowserEngine`
In-browser WebAssembly GGUF inference engine powered by Wllama:
- `constructor(options?: { modelName?: string; modelUrl?: string; workerPath?: string | URL })`
- `.init(onProgress?: (progress: { status: string; percent: number }) => void): Promise<this>`
- `.chatStream(request: ChatRequest): AsyncGenerator<string>`
- `.terminate(): void`

### Vector & Module Utilities
- `cosineSimilarity(vecA: number[] | Float32Array, vecB: number[] | Float32Array): number`: Computes normalized cosine similarity via the native `auren.node` N-API addon in Node.js with automatic pure-JS fallback in browsers.
- `getCachedModelFile(modelName: string, modelUrl: string, onProgress?): Promise<ArrayBuffer>`: Downloads and caches GGUF weights in browser CacheStorage / OPFS.
- `getModulePath(): string | null`: Resolves the bundled `libaxil-auren` shared library path for `axil -m`.

---

## 5. C Development Headers & Native Integration

```makefile
CFLAGS += -I./node_modules/@tty-pt/auren/include
LDLIBS += -laxil-auren
```

---

## License

BSD-2-Clause — Copyright (c) 2026, Paulo Quirino <quirinpa@gmail.com>. See [LICENSE](https://github.com/tty-pt/axil-auren/blob/main/LICENSE).
