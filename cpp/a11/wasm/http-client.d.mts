export interface HttpCapabilities {
  http1: boolean;
  http2: boolean;
  streamingUpload: boolean;
  streamingDownload: boolean;
  concurrentDuplex: boolean;
  trailers: boolean;
  pushes: boolean;
  redirectChain: boolean;
  connectionDetails: boolean;
}

export interface HttpRequestOptions {
  url: string;
  method?: string;
  headers?: HeadersInit;
  body?: string | Uint8Array | ArrayBuffer | Iterable<Uint8Array> |
    AsyncIterable<Uint8Array>;
  httpVersion?: 'auto' | '1.1' | '2';
  acceptPushes?: boolean;
  requireTrailers?: boolean;
  requireConnectionDetails?: boolean;
  requireConcurrentDuplex?: boolean;
  maxRedirects?: number;
  maxRequestBytes?: number;
  maxResponseBytes?: number;
  deadline?: number | Date;
  signal?: AbortSignal;
}

export interface HttpConnectionDetails {
  httpVersion: string | null;
  secure: boolean | null;
  reused: boolean | null;
  remoteAddress: string | null;
  alpnProtocol: string | null;
}

export interface HttpResponse {
  status: number;
  headers: Record<string, string>;
  fields: Array<[string, string]> | null;
  url: string;
  connection: HttpConnectionDetails;
  redirects: Array<{url: string; status: number; location: string}> | null;
  pushes: Array<{requestHeaders: Record<string, string>;
    request: {method: string; scheme: string; authority: string; path: string};
    status: Promise<number>;
    headers: Promise<Record<string, string>>; body: AsyncIterable<Uint8Array>;
    cancel(): void}> | null;
  body: AsyncIterable<Uint8Array>;
  trailers(): Promise<Record<string, string> | null>;
  cancel(): void;
}

export interface HttpTransport {
  capabilities: HttpCapabilities;
  request(options: HttpRequestOptions): Promise<HttpResponse>;
}

export class A11HttpClient {
  readonly capabilities: HttpCapabilities;
  constructor(options?: {transport?: HttpTransport});
  request(options: HttpRequestOptions): Promise<HttpResponse>;
}

export function createHttpClient(options?: {transport?: HttpTransport}): A11HttpClient;
