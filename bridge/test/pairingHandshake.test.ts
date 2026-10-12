import { describe, expect, test } from "bun:test";
import type { MtpPayloadTransport } from "../src/canon/mtpSession";
import { runPairingHandshake } from "../src/canon/pairingHandshake";
import { buildDeviceInfo } from "./helpers/deviceInfoFixture";

type Handler = (payload: Uint8Array, reply: (payload: Uint8Array) => void, fail: (error: Error) => void) => void;

class FakeTransport implements MtpPayloadTransport {
  closed = false;
  private readonly queue: Uint8Array[] = [];
  private failure: Error | null = null;
  private waiter: { resolve: (payload: Uint8Array) => void; reject: (error: Error) => void } | null = null;

  constructor(private readonly handler: Handler) {}

  async writePayload(payload: Uint8Array): Promise<void> {
    this.handler(
      payload,
      (reply) => this.push(reply),
      (error) => this.fail(error)
    );
  }

  async readPayload(): Promise<Uint8Array> {
    const next = this.queue.shift();
    if (next !== undefined) {
      return next;
    }
    if (this.failure !== null) {
      throw this.failure;
    }
    return await new Promise<Uint8Array>((resolve, reject) => {
      this.waiter = { resolve, reject };
    });
  }

  close(): void {
    this.closed = true;
  }

  private push(payload: Uint8Array): void {
    if (this.waiter !== null) {
      const waiter = this.waiter;
      this.waiter = null;
      waiter.resolve(payload);
      return;
    }
    this.queue.push(payload);
  }

  private fail(error: Error): void {
    this.failure = error;
    if (this.waiter !== null) {
      const waiter = this.waiter;
      this.waiter = null;
      waiter.reject(error);
    }
  }
}

function initAck(connectionNumber: number, name: string): Uint8Array {
  const nameBytes = (name + "\0").split("").flatMap((char) => [char.charCodeAt(0), 0]);
  const payload = new Uint8Array(24 + nameBytes.length + 4);
  const view = new DataView(payload.buffer);
  view.setUint32(0, 2, true);
  view.setUint32(4, connectionNumber, true);
  payload.set(nameBytes, 24);
  view.setUint16(24 + nameBytes.length, 1, true);
  return payload;
}

function mtpResponse(transactionId: number, code = 0x2001): Uint8Array {
  const payload = new Uint8Array(10);
  const view = new DataView(payload.buffer);
  view.setUint32(0, 7, true);
  view.setUint16(4, code, true);
  view.setUint32(6, transactionId, true);
  return payload;
}

function dataStart(transactionId: number, byteLength: number): Uint8Array {
  const payload = new Uint8Array(16);
  const view = new DataView(payload.buffer);
  view.setUint32(0, 9, true);
  view.setUint32(4, transactionId, true);
  view.setBigUint64(8, BigInt(byteLength), true);
  return payload;
}

function dataLast(transactionId: number, bytes: Uint8Array): Uint8Array {
  const payload = new Uint8Array(8 + bytes.byteLength);
  const view = new DataView(payload.buffer);
  view.setUint32(0, 0x0c, true);
  view.setUint32(4, transactionId, true);
  payload.set(bytes, 8);
  return payload;
}

function fakeCamera(options: { initDelayMs?: number; initError?: Error; silent?: boolean }): {
  transports: FakeTransport[];
  create: () => MtpPayloadTransport;
} {
  const deviceInfo = buildDeviceInfo({
    model: "Canon EOS M50",
    version: "3-1.1.0",
    operations: [0x1001, 0x1002, 0x9114, 0x9115, 0x9116, 0x9153]
  });
  const command = new FakeTransport((payload, reply, fail) => {
    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    const type = view.getUint32(0, true);
    if (type === 1) {
      if (options.silent === true) {
        return;
      }
      if (options.initError !== undefined) {
        fail(options.initError);
        return;
      }
      setTimeout(() => reply(initAck(5, "EOSM50")), options.initDelayMs ?? 0);
      return;
    }
    if (type === 6) {
      const operation = view.getUint16(8, true);
      const transactionId = view.getUint32(10, true);
      if (operation === 0x1001) {
        reply(dataStart(transactionId, deviceInfo.byteLength));
        reply(dataLast(transactionId, deviceInfo));
      }
      reply(mtpResponse(transactionId));
    }
  });
  const event = new FakeTransport((payload, reply) => {
    const type = new DataView(payload.buffer, payload.byteOffset, payload.byteLength).getUint32(0, true);
    if (type === 3) {
      const ack = new Uint8Array(4);
      new DataView(ack.buffer).setUint32(0, 4, true);
      reply(ack);
    }
  });
  const transports = [command, event];
  let index = 0;
  return {
    transports,
    create: () => {
      const transport = transports[index];
      index += 1;
      if (transport === undefined) {
        throw new Error("Unexpected transport");
      }
      return transport;
    }
  };
}

describe("runPairingHandshake", () => {
  test("waits for a delayed Init answer while the camera prompt is open", async () => {
    const camera = fakeCamera({ initDelayMs: 40 });
    const report = await runPairingHandshake({
      host: "192.168.1.53",
      port: 15740,
      handshakeTimeoutMs: 1_000,
      holdMs: 20,
      pollIntervalMs: 5,
      createTransport: camera.create
    });

    expect(report.ok).toBe(true);
    expect(report.cameraName).toBe("EOSM50");
    expect(report.connectionNumber).toBe(5);
    expect(report.deviceInfo?.model).toBe("Canon EOS M50");
    expect(report.deviceInfo?.deviceVersion).toBe("3-1.1.0");
    expect(report.liveViewOperations?.find((entry) => entry.name === "GetViewFinderData")?.supported).toBe(true);
    expect(report.eventPollResponses?.["0x2001"]).toBeGreaterThan(0);
    expect(report.steps.map((step) => step.step)).toEqual([
      "init-command",
      "init-event",
      "open-session",
      "get-device-info",
      "hold-session"
    ]);
    expect(camera.transports.every((transport) => transport.closed)).toBe(true);
  });

  test("reports a timeout hint when the prompt is not accepted in time", async () => {
    const camera = fakeCamera({ silent: true });
    const report = await runPairingHandshake({
      host: "192.168.1.53",
      port: 15740,
      handshakeTimeoutMs: 30,
      holdMs: 10,
      createTransport: camera.create
    });

    expect(report.ok).toBe(false);
    expect(report.failedStep).toBe("init-command");
    expect(report.hint).toContain("No Init answer");
    expect(camera.transports.every((transport) => transport.closed)).toBe(true);
  });

  test("explains a connection reset on Init", async () => {
    const reset = Object.assign(new Error("read ECONNRESET"), { code: "ECONNRESET" });
    const camera = fakeCamera({ initError: reset });
    const report = await runPairingHandshake({
      host: "192.168.1.53",
      port: 15740,
      handshakeTimeoutMs: 1_000,
      holdMs: 10,
      createTransport: camera.create
    });

    expect(report.ok).toBe(false);
    expect(report.failedStep).toBe("init-command");
    expect(report.steps[0]?.detail).toContain("ECONNRESET");
    expect(report.hint).toContain("smartphone waiting screen");
  });
});
