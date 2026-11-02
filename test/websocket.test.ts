import { afterAll, beforeAll, describe, expect, test } from "bun:test";

type JsonObject = Record<string, unknown>;
type Deferred<T> = {
  promise: Promise<T>;
  resolve: (value: T) => void;
  reject: (error: Error) => void;
};

const endpoint = process.env.OBS_WEBSOCKET_URL;
const password = process.env.OBS_WEBSOCKET_PASSWORD;
const sourceName = process.env.OBS_SRTLA_SOURCE_NAME ?? "SRTLA Source";
const timeoutMs = 5_000;

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  let reject!: (error: Error) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

function isObject(value: unknown): value is JsonObject {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function withTimeout<T>(promise: Promise<T>, label: string): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  return Promise.race([
    promise,
    new Promise<T>((_, reject) => {
      timer = setTimeout(() => reject(new Error(`Timed out waiting for ${label}`)), timeoutMs);
    }),
  ]).finally(() => {
    if (timer !== undefined) clearTimeout(timer);
  });
}

async function sha256Base64(value: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(value));
  return btoa(String.fromCharCode(...new Uint8Array(digest)));
}

class ObsWebSocket {
  private readonly socket: WebSocket;
  private readonly hello = deferred<JsonObject>();
  private readonly identified = deferred<void>();
  private readonly pendingRequests = new Map<string, Deferred<JsonObject>>();

  constructor(url: string) {
    this.socket = new WebSocket(url);
    this.socket.addEventListener("message", (event) => this.handleMessage(event));
    this.socket.addEventListener("error", () => this.fail(new Error("OBS WebSocket connection failed")));
    this.socket.addEventListener("close", () => this.fail(new Error("OBS WebSocket connection closed")));
  }

  async identify(passwordValue?: string): Promise<void> {
    const hello = await withTimeout(this.hello.promise, "OBS WebSocket Hello");
    const identifyData: JsonObject = { rpcVersion: 1, eventSubscriptions: 0 };

    if (passwordValue !== undefined) {
      const authentication = hello.authentication;
      if (!isObject(authentication) ||
          typeof authentication.salt !== "string" ||
          typeof authentication.challenge !== "string") {
        throw new Error("OBS WebSocket did not provide authentication details");
      }
      const secret = await sha256Base64(passwordValue + authentication.salt);
      identifyData.authentication = await sha256Base64(secret + authentication.challenge);
    }

    this.socket.send(JSON.stringify({ op: 1, d: identifyData }));
    await withTimeout(this.identified.promise, "OBS WebSocket Identified");
  }

  async callVendor(requestData: JsonObject): Promise<JsonObject> {
    const requestId = crypto.randomUUID();
    const pending = deferred<JsonObject>();
    this.pendingRequests.set(requestId, pending);

    try {
      this.socket.send(
        JSON.stringify({
          op: 6,
          d: {
            requestType: "CallVendorRequest",
            requestId,
            requestData,
          },
        }),
      );
      const response = await withTimeout(pending.promise, "OBS WebSocket vendor response");
      const status = response.requestStatus;
      if (!isObject(status) || status.result !== true) {
        const detail = isObject(status) && typeof status.comment === "string" ? `: ${status.comment}` : "";
        throw new Error(`CallVendorRequest failed${detail}`);
      }

      const responseData = response.responseData;
      if (!isObject(responseData) || !isObject(responseData.responseData)) {
        throw new Error("CallVendorRequest did not return vendor response data");
      }
      return responseData.responseData;
    } finally {
      this.pendingRequests.delete(requestId);
    }
  }

  close(): void {
    if (this.socket.readyState < WebSocket.CLOSING) {
      this.socket.close();
    }
  }

  private handleMessage(event: MessageEvent): void {
    try {
      const message: unknown = JSON.parse(String(event.data));
      console.log("Received message:", event.data);
      if (!isObject(message) || typeof message.op !== "number" || !isObject(message.d)) {
        throw new Error("OBS WebSocket sent an invalid message");
      }

      if (message.op === 0) {
        this.hello.resolve(message.d);
      } else if (message.op === 2) {
        this.identified.resolve();
      } else if (message.op === 7 && typeof message.d.requestId === "string") {
        this.pendingRequests.get(message.d.requestId)?.resolve(message.d);
      }
    } catch (error) {
      this.fail(error instanceof Error ? error : new Error(String(error)));
    }
  }

  private fail(error: Error): void {
    this.hello.reject(error);
    this.identified.reject(error);
    for (const request of this.pendingRequests.values()) {
      request.reject(error);
    }
  }
}

test("sends authenticated obs-websocket requests over WebSocket", async () => {
  let identifyData: JsonObject | undefined;
  let vendorRequest: JsonObject | undefined;
  const server = Bun.serve({
    port: 0,
    fetch(request, server) {
      if (server.upgrade(request)) return;
      return new Response("WebSocket upgrade required", { status: 400 });
    },
    websocket: {
      open(socket) {
        socket.send(
          JSON.stringify({
            op: 0,
            d: {
              rpcVersion: 1,
              authentication: { salt: "test-salt", challenge: "test-challenge" },
            },
          }),
        );
      },
      message(socket, data) {
        const message: unknown = JSON.parse(String(data));
        if (!isObject(message) || !isObject(message.d)) return;

        if (message.op === 1) {
          identifyData = message.d;
          socket.send(JSON.stringify({ op: 2, d: { negotiatedRpcVersion: 1 } }));
        } else if (message.op === 6) {
          vendorRequest = message.d;
          socket.send(
            JSON.stringify({
              op: 7,
              d: {
                requestId: message.d.requestId,
                requestStatus: { result: true, code: 100 },
                responseData: {
                  vendorName: "obs-srtla-source",
                  requestType: "GetSrtStats",
                  responseData: { success: true, sourceName: "Local Test", connected: false },
                },
              },
            }),
          );
        }
      },
    },
  });
  const client = new ObsWebSocket(`ws://127.0.0.1:${server.port}`);

  try {
    await client.identify("bun-test-password");
    const response = await client.callVendor({
      vendorName: "obs-srtla-source",
      requestType: "GetSrtStats",
      requestData: { sourceName: "Local Test" },
    });

    expect(typeof identifyData?.authentication).toBe("string");
    expect(vendorRequest?.requestType).toBe("CallVendorRequest");
    expect(response).toEqual({ success: true, sourceName: "Local Test", connected: false });
  } finally {
    client.close();
    server.stop(true);
  }
});

describe.skipIf(!endpoint)("obs-websocket GetSrtStats integration", () => {
  let client: ObsWebSocket;

  beforeAll(async () => {
    if (!endpoint) throw new Error("OBS_WEBSOCKET_URL is required");
    client = new ObsWebSocket(endpoint);
    await client.identify(password);
  });

  afterAll(() => {
    client?.close();
  });

  test("returns stats for the configured SRTLA source without secrets", async () => {
    const response = await client.callVendor({
      vendorName: "obs-srtla-source",
      requestType: "GetSrtStats",
      requestData: { sourceName },
    });
console.log("Response:", response);
    expect(response.success).toBe(true);
    expect(response.sourceName).toBe(sourceName);
    expect(typeof response.connected).toBe("boolean");
    expect(response).not.toHaveProperty("srt_passphrase");
    expect(response).not.toHaveProperty("srt_streamid");

    if (response.connected === true) {
      expect(isObject(response.stats)).toBe(true);
      for (const key of [
        "pktRecvTotal",
        "byteRecvTotal",
        "mbpsRecvRate",
        "msRTT",
        "pktRcvLossTotal",
        "pktRcvRetrans",
        "msRcvBuf",
      ]) {
        expect(response.stats).toHaveProperty(key);
      }
    } else {
      expect(response).not.toHaveProperty("stats");
    }
  });

  test("returns a vendor error for a missing source", async () => {
    const response = await client.callVendor({
      vendorName: "obs-srtla-source",
      requestType: "GetSrtStats",
      requestData: { sourceName: `${sourceName} (missing)` },
    });
    expect(response.success).toBe(false);
    expect(response.success).toBe(false);
    expect(typeof response.error === "string" && response.error.length > 0).toBe(true);
  });
});
