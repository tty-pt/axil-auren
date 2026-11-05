export interface ChatMessage {
  role: 'system' | 'user' | 'assistant';
  content: string;
}

export interface ChatRequest {
  model?: string;
  messages: ChatMessage[];
  temperature?: number;
  max_tokens?: number;
  stream?: boolean;
}

export interface ChatChoice {
  index: number;
  message: ChatMessage;
  finish_reason?: string;
}

export interface ChatResponse {
  id: string;
  model: string;
  choices: ChatChoice[];
  usage?: {
    prompt_tokens: number;
    completion_tokens: number;
    total_tokens: number;
  };
}

export interface ChatChunkChoice {
  index: number;
  delta: Partial<ChatMessage>;
  finish_reason?: string | null;
}

export interface ChatChunk {
  id: string;
  model: string;
  choices: ChatChunkChoice[];
}

export interface EmbeddingRequest {
  model?: string;
  input: string | string[];
}

export interface EmbeddingData {
  index: number;
  embedding: number[];
  object: 'embedding';
}

export interface EmbeddingResponse {
  model: string;
  data: EmbeddingData[];
  usage?: {
    prompt_tokens: number;
    total_tokens: number;
  };
}

export interface SystemOneRequest {
  model?: string;
  text: string;
  labels?: Record<string, string[]> | string[];
  question?: string;
}

export interface SystemOneResponse {
  decision: string;
  confidence: number;
  scores?: Record<string, number>;
  margin?: number;
}

export interface CollectionDocument {
  id: string;
  text: string;
  metadata?: Record<string, any>;
}

export interface CollectionSearchResult {
  id: string;
  score: number;
  text?: string;
  metadata?: Record<string, any>;
}

export interface AurenClientOptions {
  baseUrl?: string;
  defaultModel?: string;
  timeoutMs?: number;
  headers?: Record<string, string>;
}

export class AurenCollectionClient {
  constructor(client: AurenClient, name: string);
  insert(doc: CollectionDocument): Promise<{ status: string; id: string }>;
  search(query: string, limit?: number): Promise<CollectionSearchResult[]>;
  delete(id: string): Promise<{ status: string; id: string }>;
}

export class AurenClient {
  baseUrl: string;
  defaultModel: string;
  timeoutMs: number;
  headers: Record<string, string>;

  constructor(options?: AurenClientOptions);

  models(): Promise<string[]>;
  chat(request: ChatRequest): Promise<ChatResponse>;
  chatStream(request: ChatRequest): AsyncIterable<ChatChunk>;
  embeddings(request: EmbeddingRequest): Promise<EmbeddingResponse>;
  systemOne(request: SystemOneRequest): Promise<SystemOneResponse>;
  collection(name: string): AurenCollectionClient;
}

export function createAurenClient(options?: AurenClientOptions): AurenClient;
export function cosineSimilarity(a: number[], b: number[]): number;
