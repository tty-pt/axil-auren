/**
 * @file index.js
 * @brief Universal isomorphic SDK for axil-auren local LLM and semantic search service.
 */

export function cosineSimilarity(a, b) {
  if (!a || !b || a.length !== b.length || a.length === 0) return 0;
  let dot = 0;
  let normA = 0;
  let normB = 0;
  for (let i = 0; i < a.length; i++) {
    dot += a[i] * b[i];
    normA += a[i] * a[i];
    normB += b[i] * b[i];
  }
  if (normA <= 0 || normB <= 0) return 0;
  return dot / (Math.sqrt(normA) * Math.sqrt(normB));
}

export class AurenCollectionClient {
  constructor(client, name) {
    this.client = client;
    this.name = name;
  }

  async insert(doc) {
    return this.client._fetch(`/v1/collections/${encodeURIComponent(this.name)}/documents`, {
      method: "POST",
      body: JSON.stringify(doc),
    });
  }

  async search(query, limit = 10) {
    const q = encodeURIComponent(query);
    return this.client._fetch(`/v1/collections/${encodeURIComponent(this.name)}/search?q=${q}&limit=${limit}`, {
      method: "GET",
    });
  }

  async delete(id) {
    return this.client._fetch(`/v1/collections/${encodeURIComponent(this.name)}/documents/${encodeURIComponent(id)}`, {
      method: "DELETE",
    });
  }
}

export class AurenClient {
  constructor(options = {}) {
    this.baseUrl = (options.baseUrl || "http://localhost:4242").replace(/\/+$/, "");
    this.defaultModel = options.defaultModel || "bge-base";
    this.timeoutMs = options.timeoutMs || 30000;
    this.headers = options.headers || {};
  }

  async _fetch(path, init = {}) {
    const url = `${this.baseUrl}${path}`;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);

    const headers = {
      "Content-Type": "application/json",
      ...this.headers,
      ...init.headers,
    };

    try {
      const res = await fetch(url, {
        ...init,
        headers,
        signal: controller.signal,
      });

      if (!res.ok) {
        const errText = await res.text().catch(() => "");
        throw new Error(`Auren error ${res.status} (${res.statusText}): ${errText}`);
      }

      return await res.json();
    } finally {
      clearTimeout(timer);
    }
  }

  async models() {
    const res = await this._fetch("/v1/models");
    if (Array.isArray(res)) return res;
    if (res && Array.isArray(res.data)) return res.data.map((m) => (typeof m === "string" ? m : m.id));
    return [];
  }

  async chat(request) {
    const payload = {
      model: request.model || this.defaultModel,
      ...request,
      stream: false,
    };
    return this._fetch("/v1/chat/completions", {
      method: "POST",
      body: JSON.stringify(payload),
    });
  }

  async *chatStream(request) {
    const url = `${this.baseUrl}/v1/chat/completions`;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);

    const payload = {
      model: request.model || this.defaultModel,
      ...request,
      stream: true,
    };

    const headers = {
      "Content-Type": "application/json",
      ...this.headers,
    };

    let res;
    try {
      res = await fetch(url, {
        method: "POST",
        headers,
        body: JSON.stringify(payload),
        signal: controller.signal,
      });
    } finally {
      clearTimeout(timer);
    }

    if (!res.ok) {
      const errText = await res.text().catch(() => "");
      throw new Error(`Auren stream error ${res.status}: ${errText}`);
    }

    const reader = res.body.getReader();
    const decoder = new TextDecoder();
    let buffer = "";

    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;

        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split("\n");
        buffer = lines.pop(); // keep uncompleted line

        for (const line of lines) {
          const trimmed = line.trim();
          if (!trimmed || !trimmed.startsWith("data:")) continue;
          const dataStr = trimmed.slice(5).trim();
          if (dataStr === "[DONE]") return;

          try {
            const parsed = JSON.parse(dataStr);
            yield parsed;
          } catch {
            // ignore non-json SSE lines
          }
        }
      }
    } finally {
      reader.releaseLock();
    }
  }

  async embeddings(request) {
    const payload = {
      model: request.model || this.defaultModel,
      input: request.input,
    };
    return this._fetch("/v1/embeddings", {
      method: "POST",
      body: JSON.stringify(payload),
    });
  }

  async systemOne(request) {
    const payload = {
      model: request.model || this.defaultModel,
      text: request.text,
      ...(request.labels ? { labels: request.labels } : {}),
      ...(request.question ? { question: request.question } : {}),
    };
    return this._fetch("/v1/systemone", {
      method: "POST",
      body: JSON.stringify(payload),
    });
  }

  collection(name) {
    return new AurenCollectionClient(this, name);
  }
}

export function createAurenClient(options) {
  return new AurenClient(options);
}
