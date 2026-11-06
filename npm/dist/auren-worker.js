/**
 * @file auren-worker.js
 * @brief Dedicated Web Worker for 100% in-browser LLM neural inference using WebAssembly llama.cpp (Wllama)
 *        with full auren-chat conversation standards, persistent sessions, and GGUF models.
 */

import { Wllama } from "./wllama/wllama.js";

let wllama = null;
let currentModelId = "";
let isReady = false;
let initError = null;
let activeSession = null;

/**
 * Conversation session manager implementing axil-auren / auren-chat standards:
 * - Persistent session lifecycle keyed by sessionId
 * - Dynamic sliding-window context trimming (system prompt pinned, FIFO turn eviction)
 * - Model-native ChatML formatting and prefix KV persistence
 */
class AurenConversationSession {
  constructor(sessionId = "default-session", systemPrompt = "You are a concise, helpful assistant. Answer questions directly and accurately.") {
    this.sessionId = sessionId;
    this.systemPrompt = systemPrompt;
    this.messages = [{ role: "system", content: systemPrompt }];
    this.maxContextTokens = 2048;
  }

  addUserMessage(content) {
    this.messages.push({ role: "user", content });
  }

  addAssistantMessage(content) {
    this.messages.push({ role: "assistant", content });
  }

  reset() {
    this.messages = [{ role: "system", content: this.systemPrompt }];
  }

  /**
   * Sliding-window dialogue trimming matching qllm_chat() in libauren.c:
   * Retain root system prompt (messages[0]), evict oldest completed (user, assistant) pair.
   */
  getTrimmedMessages(wllamaInstance) {
    const msgs = [...this.messages];
    if (msgs.length <= 2) return msgs;

    while (msgs.length > 2) {
      let tokenCount = 0;
      if (wllamaInstance && typeof wllamaInstance.tokenize === "function") {
        try {
          const text = msgs.map((m) => `${m.role}: ${m.content}`).join("\n");
          tokenCount = wllamaInstance.tokenize(text).length;
        } catch {
          tokenCount = msgs.reduce((acc, m) => acc + Math.ceil(m.content.length / 3.5), 0);
        }
      } else {
        tokenCount = msgs.reduce((acc, m) => acc + Math.ceil(m.content.length / 3.5), 0);
      }

      if (tokenCount <= this.maxContextTokens) break;

      /* Pin root system prompt at index 0, drop oldest dialogue pair */
      msgs.splice(1, 2);
    }
    return msgs;
  }
}

activeSession = new AurenConversationSession();

/**
 * Initialize WebAssembly llama.cpp engine with GGUF model entirely inside the browser
 */
async function loadWllamaModel(modelKey, customUrl, onProgress) {
  const modelUrls = {
    smollm2: "/models/smollm2-135m-instruct-q4_k_m.gguf",
    qwen: "/models/qwen2.5-0.5b-instruct.gguf",
  };

  const targetUrl = customUrl || modelUrls[modelKey] || modelUrls.smollm2;

  if (wllama && currentModelId === targetUrl && isReady) {
    return "wasm-llamacpp";
  }

  try {
    const wasmPath = new URL("./wllama/wllama.wasm", import.meta.url).href;
    const configPaths = {
      "single-thread/wllama.wasm": wasmPath,
      "multi-thread/wllama.wasm": wasmPath,
    };

    wllama = new Wllama(configPaths, {
      suppressNativeLog: false,
      parallelDownloads: 2,
      allowOffline: true,
    });

    onProgress({ status: "initializing_wasm", percent: 15 });

    /* Load GGUF model via HTTP Range requests with OPFS caching */
    await wllama.loadModelFromUrl(targetUrl, {
      useCache: true,
      onProgress: (prog) => {
        if (prog && prog.total > 0) {
          const percent = Math.min(99, Math.round((prog.loaded / prog.total) * 100));
          onProgress({
            status: prog.loaded === prog.total ? "cached" : "downloading",
            percent,
            loaded: prog.loaded,
            total: prog.total,
          });
        }
      },
    });

    currentModelId = targetUrl;
    isReady = true;
    initError = null;
    return "wasm-llamacpp";
  } catch (err) {
    console.warn("[auren-worker] In-browser Wllama initialization error:", err);
    initError = err;
    return null;
  }
}

self.onmessage = async (e) => {
  const { type, modelKey, modelName, modelUrl, streamId, messages, prompt, maxTokens, temperature, sessionId } = e.data;

  if (type === "init") {
    self.postMessage({ type: "progress", data: { status: "downloading", percent: 10 } });

    const resolvedKey = modelKey || (modelName && modelName.includes("qwen") ? "qwen" : "smollm2");
    const targetUrl = modelUrl || (modelName ? `/models/${modelName}` : null);

    const backend = await loadWllamaModel(resolvedKey, targetUrl, (prog) => {
      self.postMessage({ type: "progress", data: prog });
    });

    if (backend) {
      self.postMessage({ type: "ready", backend, modelId: currentModelId });
    } else {
      self.postMessage({
        type: "error",
        error: initError ? initError.message : "Failed to initialize WebAssembly llama.cpp engine",
      });
    }
  } else if (type === "reset") {
    if (activeSession) {
      activeSession.reset();
    }
    self.postMessage({ type: "reset_done" });
  } else if (type === "generate") {
    if (!isReady || !wllama) {
      self.postMessage({
        type: "error",
        streamId,
        error: initError ? initError.message : "In-browser WebAssembly llama.cpp engine is not ready",
      });
      return;
    }

    if (sessionId && activeSession.sessionId !== sessionId) {
      activeSession = new AurenConversationSession(sessionId);
    }

    /* Record user message */
    const userText = prompt || (messages && messages[messages.length - 1]?.content) || "";
    if (userText) {
      activeSession.addUserMessage(userText);
    }

    const trimmedConv = activeSession.getTrimmedMessages(wllama);
    let fullReply = "";

    try {
      /* Pure in-browser neural generation using llama.cpp WebAssembly */
      yieldToken: for await (const chunk of await wllama.createChatCompletion({
        messages: trimmedConv,
        stream: true,
        nPredict: maxTokens || 128,
        sampling: {
          temp: temperature || 0.2,
          top_p: 0.9,
          penalty_repeat: 1.1,
        },
      })) {
        const piece = chunk.choices?.[0]?.delta?.content;
        if (piece) {
          fullReply += piece;
          self.postMessage({ type: "token", streamId, token: piece });
        }
      }

      if (fullReply) {
        activeSession.addAssistantMessage(fullReply);
      }
      self.postMessage({ type: "done", streamId });
    } catch (err) {
      console.warn("[auren-worker] In-browser generation exception:", err);
      self.postMessage({ type: "error", streamId, error: err.message });
    }
  }
};
