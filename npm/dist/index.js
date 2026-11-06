/**
 * @file index.js
 * @brief Universal isomorphic SDK for @tty-pt/auren local LLM inference,
 *        embeddings, System 1 classification, and client-side browser execution.
 */

import { loadWasmModule, loadNativeAddon, getSubstrate } from "@tty-pt/stele";

const nativeBinding = loadNativeAddon(import.meta.url, "auren");
let wasmInstance = null;

/**
 * Plug-and-play WebAssembly loader for @tty-pt/auren via Stele substrate
 */
export function getAurenWasm() {
  if (wasmInstance) return wasmInstance;
  try {
    wasmInstance = loadWasmModule(new URL("./auren.wasm", import.meta.url));
  } catch (err) {
    console.warn("[auren] Wasm substrate unavailable, falling back to pure JS/native:", err.message);
  }
  return wasmInstance;
}

/**
 * Calculates normalized cosine similarity between two float vectors
 * strictly via WebAssembly when available.
 */
export function cosineSimilarity(vecA, vecB) {
  if (nativeBinding && nativeBinding.cosine) {
    return nativeBinding.cosine(vecA, vecB);
  }

  if (!vecA || !vecB || vecA.length !== vecB.length || vecA.length === 0) {
    return 0.0;
  }

  const wasm = getAurenWasm();
  const substrate = getSubstrate ? getSubstrate() : null;
  if (wasm && wasm.exports.auren_wasm_cosine && substrate && wasm.exports.malloc) {
    const dim = vecA.length;
    const ptrA = wasm.exports.malloc(dim * 4);
    const ptrB = wasm.exports.malloc(dim * 4);
    const mem = substrate.memory.buffer;
    new Float32Array(mem, ptrA, dim).set(vecA);
    new Float32Array(mem, ptrB, dim).set(vecB);
    const score = wasm.exports.auren_wasm_cosine(ptrA, ptrB, dim);
    wasm.exports.free(ptrA);
    wasm.exports.free(ptrB);
    return score;
  }

  /* Fallback JS computation */
  let dot = 0;
  let normA = 0;
  let normB = 0;
  for (let i = 0; i < vecA.length; i++) {
    dot += vecA[i] * vecB[i];
    normA += vecA[i] * vecA[i];
    normB += vecB[i] * vecB[i];
  }
  if (normA <= 0 || normB <= 0) return 0;
  return dot / (Math.sqrt(normA) * Math.sqrt(normB));
}

/**
 * Browser Model Cache Helper using Origin Private File System (OPFS)
 */
export async function getCachedModelFile(modelName, modelUrl, onProgress) {
  if (typeof navigator === "undefined" || !navigator.storage || !navigator.storage.getDirectory) {
    /* Fallback for environments without OPFS */
    const res = await fetch(modelUrl);
    return await res.arrayBuffer();
  }

  const root = await navigator.storage.getDirectory();
  const modelsDir = await root.getDirectoryHandle("models", { create: true });

  try {
    const fileHandle = await modelsDir.getFileHandle(modelName);
    const file = await fileHandle.getFile();
    if (file.size > 0) {
      if (onProgress) onProgress({ loaded: file.size, total: file.size, cached: true });
      return await file.arrayBuffer();
    }
  } catch {
    /* File not in cache yet */
  }

  /* Stream download and store to OPFS */
  const response = await fetch(modelUrl);
  if (!response.ok) {
    throw new Error(`Failed to fetch model: ${response.status} ${response.statusText}`);
  }

  const total = parseInt(response.headers.get("Content-Length") || "0", 10);
  const reader = response.body.getReader();
  const fileHandle = await modelsDir.getFileHandle(modelName, { create: true });
  const writable = await fileHandle.createWritable();

  let loaded = 0;
  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    await writable.write(value);
    loaded += value.length;
    if (onProgress) {
      onProgress({ loaded, total: total || loaded, cached: false });
    }
  }
  await writable.close();

  const file = await fileHandle.getFile();
  return await file.arrayBuffer();
}

/**
 * Browser In-Worker Execution Client
 */
export class AurenBrowserEngine {
  constructor(options = {}) {
    this.modelName = options.modelName || "smollm2-135m-instruct-q4_k_m.gguf";
    this.modelUrl = options.modelUrl || `/models/${this.modelName}`;
    this.workerPath = options.workerPath || new URL("./auren-worker.js", import.meta.url);
    this.worker = null;
    this.isReady = false;
  }

  async init(onProgress) {
    return new Promise((resolve, reject) => {
      this.worker = new Worker(this.workerPath, { type: "module" });

      this.worker.onmessage = (e) => {
        const { type, data, error } = e.data;
        if (type === "progress" && onProgress) {
          onProgress(data);
        } else if (type === "ready") {
          this.isReady = true;
          resolve(this);
        } else if (type === "error") {
          reject(new Error(error));
        }
      };

      this.worker.onerror = (err) => reject(err);

      this.worker.postMessage({
        type: "init",
        modelName: this.modelName,
        modelUrl: this.modelUrl,
      });
    });
  }

  async *chatStream(request) {
    if (!this.worker || !this.isReady) {
      throw new Error("AurenBrowserEngine is not initialized");
    }

    const streamId = Math.random().toString(36).slice(2);
    const queue = [];
    let resolveNext = null;
    let streamDone = false;
    let streamError = null;

    const handler = (e) => {
      if (e.data.streamId !== streamId) return;
      if (e.data.type === "token") {
        queue.push(e.data.token);
        if (resolveNext) {
          resolveNext();
          resolveNext = null;
        }
      } else if (e.data.type === "done") {
        streamDone = true;
        if (resolveNext) {
          resolveNext();
          resolveNext = null;
        }
      } else if (e.data.type === "error") {
        streamError = new Error(e.data.error);
        if (resolveNext) {
          resolveNext();
          resolveNext = null;
        }
      }
    };

    this.worker.addEventListener("message", handler);
    this.worker.postMessage({
      type: "generate",
      streamId,
      messages: request.messages || [],
      prompt: request.prompt || "",
      maxTokens: request.maxTokens || 256,
      temperature: request.temperature || 0.7,
    });

    try {
      while (!streamDone || queue.length > 0) {
        if (queue.length > 0) {
          yield queue.shift();
        } else if (streamError) {
          throw streamError;
        } else if (!streamDone) {
          await new Promise((resolve) => {
            resolveNext = resolve;
          });
        }
      }
    } finally {
      this.worker.removeEventListener("message", handler);
    }
  }

  terminate() {
    if (this.worker) {
      this.worker.terminate();
      this.worker = null;
      this.isReady = false;
    }
  }
}

/**
 * Universal AurenClient supporting HTTP daemon, native add-on, and browser fallback.
 */
export class AurenClient {
  constructor(options = {}) {
    this.baseUrl = (options.baseUrl || "http://localhost:4242").replace(/\/+$/, "");
    this.defaultModel = options.defaultModel || "smollm2-135m-instruct";
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
        buffer = lines.pop();

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
}

export function createAurenClient(options) {
  return new AurenClient(options);
}
