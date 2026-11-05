import test from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import {
  AurenClient,
  createAurenClient,
  cosineSimilarity
} from "../dist/index.js";

test("cosineSimilarity: mathematical properties", () => {
  assert.equal(cosineSimilarity([1, 0, 0], [1, 0, 0]), 1);
  assert.equal(cosineSimilarity([1, 0, 0], [0, 1, 0]), 0);
  assert.equal(cosineSimilarity([1, 0], [-1, 0]), -1);
  assert.equal(cosineSimilarity([], []), 0);
  assert.equal(cosineSimilarity([1, 2], [1]), 0);
});

test("AurenClient: end-to-end mocked HTTP protocol", async (t) => {
  // Spin up a mock HTTP server mimicking axil-auren endpoints
  const server = http.createServer((req, res) => {
    let body = "";
    req.on("data", (chunk) => { body += chunk; });
    req.on("end", () => {
      const url = new URL(req.url, `http://${req.headers.host}`);

      if (req.method === "GET" && url.pathname === "/v1/models") {
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ data: [{ id: "bge-base" }, { id: "qwen2.5-0.5b" }] }));
        return;
      }

      if (req.method === "POST" && url.pathname === "/v1/chat/completions") {
        const payload = JSON.parse(body || "{}");
        if (payload.stream) {
          res.writeHead(200, {
            "Content-Type": "text/event-stream",
            "Cache-Control": "no-cache",
            "Connection": "keep-alive"
          });
          res.write('data: {"id":"chatcmpl-1","choices":[{"index":0,"delta":{"content":"Hello"}}]}\n\n');
          res.write('data: {"id":"chatcmpl-1","choices":[{"index":0,"delta":{"content":" World"}}]}\n\n');
          res.write("data: [DONE]\n\n");
          res.end();
          return;
        }

        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({
          id: "chatcmpl-1",
          model: payload.model,
          choices: [{ index: 0, message: { role: "assistant", content: "Hello World" } }]
        }));
        return;
      }

      if (req.method === "POST" && url.pathname === "/v1/embeddings") {
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({
          model: "bge-base",
          data: [{ index: 0, embedding: [0.1, 0.2, 0.3], object: "embedding" }]
        }));
        return;
      }

      if (req.method === "POST" && url.pathname === "/v1/systemone") {
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({
          decision: "build",
          confidence: 0.95,
          scores: { build: 0.95, test: 0.12 }
        }));
        return;
      }

      if (url.pathname.startsWith("/v1/collections/")) {
        res.writeHead(200, { "Content-Type": "application/json" });
        if (req.method === "POST") {
          res.end(JSON.stringify({ status: "ok", id: "doc-123" }));
        } else if (req.method === "GET") {
          res.end(JSON.stringify([{ id: "doc-123", score: 0.88 }]));
        } else if (req.method === "DELETE") {
          res.end(JSON.stringify({ status: "deleted", id: "doc-123" }));
        }
        return;
      }

      res.writeHead(404);
      res.end("Not Found");
    });
  });

  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const port = server.address().port;
  const client = createAurenClient({ baseUrl: `http://127.0.0.1:${port}` });

  try {
    // 1. Models
    const models = await client.models();
    assert.deepEqual(models, ["bge-base", "qwen2.5-0.5b"]);

    // 2. Chat non-streaming
    const chatRes = await client.chat({
      messages: [{ role: "user", content: "Hi" }]
    });
    assert.equal(chatRes.choices[0].message.content, "Hello World");

    // 3. Chat streaming
    let streamedText = "";
    for await (const chunk of client.chatStream({
      messages: [{ role: "user", content: "Hi" }]
    })) {
      streamedText += chunk.choices[0]?.delta?.content || "";
    }
    assert.equal(streamedText, "Hello World");

    // 4. Embeddings
    const embRes = await client.embeddings({ input: "test" });
    assert.equal(embRes.data[0].embedding.length, 3);

    // 5. SystemOne
    const sys1 = await client.systemOne({
      text: "make",
      labels: { build: ["make"], test: ["test"] }
    });
    assert.equal(sys1.decision, "build");
    assert.equal(sys1.confidence, 0.95);

    // 6. Collections
    const col = client.collection("resumes");
    const ins = await col.insert({ id: "doc-123", text: "Software Engineer" });
    assert.equal(ins.status, "ok");

    const search = await col.search("Software", 5);
    assert.equal(search[0].id, "doc-123");

    const del = await col.delete("doc-123");
    assert.equal(del.status, "deleted");
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});
