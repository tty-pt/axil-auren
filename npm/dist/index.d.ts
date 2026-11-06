/**
 * @file index.d.ts
 * @brief TypeScript definitions for @tty-pt/auren.
 */

export interface AurenMessage {
  role: "system" | "user" | "assistant";
  content: string;
}

export interface ChatRequest {
  model?: string;
  messages?: AurenMessage[];
  prompt?: string;
  maxTokens?: number;
  temperature?: number;
  top_p?: number;
}

export interface EmbedRequest {
  model?: string;
  input: string | string[];
}

export interface SystemOneRequest {
  model?: string;
  text: string;
  labels?: string[];
  question?: string;
}

export interface ModelProgress {
  loaded: number;
  total: number;
  cached: boolean;
  percent?: number;
}

export function cosineSimilarity(a: number[] | Float32Array, b: number[] | Float32Array): number;

export function getCachedModelFile(
  modelName: string,
  modelUrl: string,
  onProgress?: (progress: ModelProgress) => void
): Promise<ArrayBuffer>;

export class AurenBrowserEngine {
  constructor(options?: { modelName?: string; modelUrl?: string; workerPath?: string | URL });
  init(onProgress?: (progress: { status: string; percent: number }) => void): Promise<this>;
  chatStream(request: ChatRequest): AsyncGenerator<string, void, unknown>;
  terminate(): void;
}

export class AurenClient {
  constructor(options?: { baseUrl?: string; defaultModel?: string; timeoutMs?: number; headers?: Record<string, string> });
  chat(request: ChatRequest): Promise<any>;
  chatStream(request: ChatRequest): AsyncGenerator<any, void, unknown>;
  embeddings(request: EmbedRequest): Promise<any>;
  systemOne(request: SystemOneRequest): Promise<any>;
}

export function createAurenClient(options?: any): AurenClient;
