# @tty-pt/auren

Isomorphic typed client SDK, WebAssembly browser inference engine (`AurenBrowserEngine`), N-API vector utilities, and prebuilt **`libaxil-auren`** Xylem server module for `axil-auren`—the pure-C local LLM inference, OpenAI-compatible streaming chat, dense vector embeddings, and System 1 classification engine.

## Features
- **Prebuilt `libaxil-auren` Daemon Module**: Ships `libaxil-auren.{so,dylib,dll}` and its native dependencies inside `prebuilds/<platform>-<arch>/` so `@tty-pt/axil` (`npx axil -A -p 4242 -m axil-auren`) can launch the native C server without system-installed binaries.
- **N-API Accelerated Vector Math**: Exposes `cosineSimilarity(vecA, vecB)` backed by `auren.node` in Node.js with automatic pure-JS fallback in browsers.
- **Isomorphic Client & Browser Wllama Runtime**: Runs identically in Node.js, Bun, Deno, and browsers, with built-in WebAssembly `llama.cpp` (`AurenBrowserEngine`) fallback when no local server is reachable.
- **Full Capabilities**: Streaming chat (`chatStream`), non-streaming chat (`chat`), embeddings (`embeddings`), System 1 zero-decode fast classification (`systemOne`), and persistent collections (`collection`).

## Usage

### 1. Launch the Bundled Native `axil-auren` Server
```bash
npx axil -A -p 4242 -m axil-auren
```

Or resolve the bundled module path programmatically in Node.js:
```typescript
import { getModulePath } from "@tty-pt/auren";
import { runCli } from "@tty-pt/axil";

console.log("Bundled module:", getModulePath());
// Start in foreground on port 4242:
// runCli(["-A", "-d", "-p", "4242", "-m", getModulePath()]);
```

### 2. Client SDK (`createAurenClient`)
```typescript
import { createAurenClient, cosineSimilarity } from "@tty-pt/auren";

const auren = createAurenClient({ baseUrl: "http://localhost:4242" });

// 1. Streaming chat
for await (const chunk of auren.chatStream({
  messages: [{ role: "user", content: "Explain WebAssembly in C" }]
})) {
  process.stdout.write(chunk.choices[0]?.delta?.content || "");
}

// 2. Vector Embeddings
const { data } = await auren.embeddings({ input: "Semantic search with C" });
console.log("Vector dimension:", data[0].embedding.length);

// 3. System 1 Fast Decision (~15ms)
const res = await auren.systemOne({
  text: "make test",
  labels: { build: ["make", "gcc"], test: ["test", "assert"] }
});
console.log("Decision:", res.decision);
```

## License
BSD-2-Clause — Author: Paulo Quirino <q@qnixsoft.com>
