# @tty-pt/auren-client

Isomorphic typed client SDK for `axil-auren`, the pure-C local LLM inference, OpenAI-compatible streaming chat, dense vector embeddings, and System 1 classification engine.

## Features
- **Zero Dependencies**: Powered by native `fetch`, `ReadableStream`, and `TextDecoder`.
- **Isomorphic**: Runs identically in Node.js, Bun, Deno, and browser frontends.
- **Full Capabilities**: Streaming chat (`chatStream`), non-streaming chat (`chat`), embeddings (`embeddings`), System 1 zero-decode fast classification (`systemOne`), and persistent collections (`collection`).

## Usage
```typescript
import { createAurenClient, cosineSimilarity } from "@tty-pt/auren-client";

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
CC BY-NC-SA 4.0 — Author: Paulo Quirino <q@qnixsoft.com>
