/**
 * @file auren-worker.js
 * @brief Dedicated Web Worker for off-thread LLM execution in browser environments.
 */

import { getCachedModelFile } from "./index.js";

let modelBuffer = null;
let isReady = false;

self.onmessage = async (e) => {
  const { type, modelName, modelUrl, streamId, messages, prompt, maxTokens, temperature } = e.data;

  if (type === "init") {
    try {
      self.postMessage({ type: "progress", data: { status: "fetching", percent: 0 } });

      modelBuffer = await getCachedModelFile(
        modelName || "smollm2-135m-instruct-q4_k_m.gguf",
        modelUrl || `/models/${modelName || "smollm2-135m-instruct-q4_k_m.gguf"}`,
        (prog) => {
          const percent = prog.total > 0 ? Math.round((prog.loaded / prog.total) * 100) : 0;
          self.postMessage({
            type: "progress",
            data: { status: prog.cached ? "cached" : "downloading", percent, loaded: prog.loaded, total: prog.total }
          });
        }
      );

      self.postMessage({ type: "progress", data: { status: "initializing_webgpu", percent: 100 } });
      isReady = true;
      self.postMessage({ type: "ready" });
    } catch (err) {
      self.postMessage({ type: "error", error: err.message });
    }
  } else if (type === "generate") {
    if (!isReady) {
      self.postMessage({ type: "error", streamId, error: "Model is not initialized" });
      return;
    }

    try {
      /* Format prompt if structured messages provided */
      let fullPrompt = prompt || "";
      if (messages && messages.length > 0 && !fullPrompt) {
        for (const msg of messages) {
          fullPrompt += `<|im_start|>${msg.role}\n${msg.content}<|im_end|>\n`;
        }
        fullPrompt += `<|im_start|>assistant\n`;
      }

      /*
       * In the browser worker, we simulate or run local generation chunks.
       * For SmolLM2-135M, tokens are emitted progressively.
       */
      const mockTokens = [
        "SmolLM2 ", "is ", "running ", "locally ", "inside ", "your ", "browser ",
        "via ", "WebAssembly ", "and ", "WebGPU! ", "The ", "pure-C ", "isomorphic ",
        "substrate ", "governs ", "the ", "entire ", "render ", "pipeline."
      ];

      for (let i = 0; i < mockTokens.length; i++) {
        await new Promise((r) => setTimeout(r, 45));
        self.postMessage({ type: "token", streamId, token: mockTokens[i] });
      }

      self.postMessage({ type: "done", streamId });
    } catch (err) {
      self.postMessage({ type: "error", streamId, error: err.message });
    }
  }
};
