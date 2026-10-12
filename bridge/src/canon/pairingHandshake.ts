import { CANON_EOS_OPERATION } from "./canonEos";
import { describeLiveViewSupport, parsePtpDeviceInfo, type OperationSupport } from "./deviceInfo";
import { MtpSession, type MtpPayloadTransport } from "./mtpSession";
import { MtpTransport } from "./mtpTransport";
import { PtpIpEventChannel } from "./ptpIpEventChannel";
import { initializePtpIpTransport } from "./ptpIpSession";

const CLOSE_SESSION = 0x1003;
const ACCEPTED_EVENT_CODES = [0x2001, 0x2019, 0xa102];

export interface PairingStep {
  step: string;
  ok: boolean;
  ms: number;
  detail?: string;
}

export interface PairingReport {
  ok: boolean;
  cameraName?: string;
  connectionNumber?: number;
  cameraGuid?: string;
  deviceInfo?: {
    manufacturer: string;
    model: string;
    deviceVersion: string;
    serialNumber: string;
    vendorExtensionId: string;
    operationCount: number;
  };
  liveViewOperations?: OperationSupport[];
  eventPollResponses?: Record<string, number>;
  steps: PairingStep[];
  failedStep?: string;
  hint?: string;
}

export interface PairingHandshakeOptions {
  host: string;
  port: number;
  localAddress?: string;
  // How long to wait for the camera to answer Init. The camera holds the
  // Init ACK while its on-screen pairing prompt is open, so this must cover
  // the time a person needs to accept it.
  handshakeTimeoutMs: number;
  stepTimeoutMs?: number;
  holdMs: number;
  pollIntervalMs?: number;
  log?: (message: string) => void;
  createTransport?: () => MtpPayloadTransport;
}

export async function runPairingHandshake(options: PairingHandshakeOptions): Promise<PairingReport> {
  const log = options.log ?? (() => {});
  const stepTimeoutMs = options.stepTimeoutMs ?? 10_000;
  const pollIntervalMs = options.pollIntervalMs ?? 500;
  const createTransport =
    options.createTransport ??
    (() =>
      new MtpTransport({
        host: options.host,
        port: options.port,
        timeoutMs: 10_000,
        ...(options.localAddress === undefined ? {} : { localAddress: options.localAddress })
      }));

  const steps: PairingStep[] = [];
  const report: PairingReport = { ok: false, steps };
  const commandTransport = createTransport();
  const eventTransport = createTransport();
  let session: MtpSession | null = null;
  let sessionOpen = false;

  const run = async <T>(step: string, timeoutMs: number, action: () => Promise<T>): Promise<T> => {
    const startedAt = Date.now();
    try {
      const result = await withTimeout(action(), timeoutMs, `${step} timed out after ${timeoutMs}ms`);
      steps.push({ step, ok: true, ms: Date.now() - startedAt });
      return result;
    } catch (error) {
      steps.push({ step, ok: false, ms: Date.now() - startedAt, detail: describeError(error) });
      report.failedStep = step;
      throw error;
    }
  };

  try {
    log(
      `Pairing: sending PTP/IP Init. If the camera asks to connect or to allow image viewing, accept it within ${Math.round(options.handshakeTimeoutMs / 1000)}s.`
    );
    const ack = await run("init-command", options.handshakeTimeoutMs, () => initializePtpIpTransport(commandTransport));
    report.cameraName = ack.cameraName.trim();
    report.connectionNumber = ack.connectionNumber;
    report.cameraGuid = ack.cameraGuid;
    log(`Pairing: camera accepted as ${report.cameraName} (connection ${ack.connectionNumber})`);

    const eventChannel = new PtpIpEventChannel(eventTransport);
    await run("init-event", stepTimeoutMs, () => eventChannel.initialize(ack.connectionNumber));
    log("Pairing: event channel accepted");

    session = new MtpSession(commandTransport, { sessionId: ack.connectionNumber });
    const activeSession = session;
    await run("open-session", stepTimeoutMs, () => activeSession.openSession());
    sessionOpen = true;
    log("Pairing: MTP session open");

    const deviceInfoBytes = await run("get-device-info", stepTimeoutMs, () => activeSession.getDeviceInfo());
    try {
      const info = parsePtpDeviceInfo(deviceInfoBytes);
      report.deviceInfo = {
        manufacturer: info.manufacturer,
        model: info.model,
        deviceVersion: info.deviceVersion,
        serialNumber: info.serialNumber,
        vendorExtensionId: `0x${info.vendorExtensionId.toString(16)}`,
        operationCount: info.operationsSupported.length
      };
      report.liveViewOperations = describeLiveViewSupport(info);
      log(`Pairing: device info ${info.model} firmware ${info.deviceVersion}`);
    } catch (error) {
      steps.push({ step: "parse-device-info", ok: false, ms: 0, detail: describeError(error) });
    }

    const responses: Record<string, number> = {};
    const holdStartedAt = Date.now();
    await run("hold-session", options.holdMs + stepTimeoutMs, async () => {
      while (Date.now() - holdStartedAt < options.holdMs) {
        const response = await activeSession.executeCameraCommand({
          operationCode: CANON_EOS_OPERATION.GET_EVENT,
          parameters: [],
          acceptedResponseCodes: ACCEPTED_EVENT_CODES
        });
        const key = `0x${response.responseCode.toString(16)}`;
        responses[key] = (responses[key] ?? 0) + 1;
        await delay(pollIntervalMs);
      }
    });
    report.eventPollResponses = responses;
    report.ok = true;
  } catch (error) {
    report.hint = hintFor(report.failedStep, error, options.handshakeTimeoutMs);
  } finally {
    if (session !== null && sessionOpen) {
      try {
        await withTimeout(
          session.executeCameraCommand({
            operationCode: CLOSE_SESSION,
            parameters: [],
            acceptedResponseCodes: [0x2001, 0x2019, 0x2003]
          }),
          2_000,
          "close-session timed out"
        );
      } catch {
        // Best effort: the camera may already have dropped the session.
      }
    }
    eventTransport.close();
    commandTransport.close();
  }

  return report;
}

function hintFor(failedStep: string | undefined, error: unknown, handshakeTimeoutMs: number): string {
  const message = describeError(error);
  if (failedStep === "init-command") {
    if (message.includes("ECONNRESET")) {
      return "Camera reset the connection on Init. Clear any Err screen, put the camera back on the smartphone waiting screen (Wi-Fi function > Connect to smartphone), then run --pair again.";
    }
    if (message.includes("ECONNREFUSED") || message.includes("ETIMEDOUT") || message.startsWith("Timed out after ")) {
      return "Camera is not accepting TCP 15740. Wake the camera, confirm the IP in Connection Information, and keep it on the smartphone waiting screen.";
    }
    if (message.includes("timed out after")) {
      return `No Init answer within ${Math.round(handshakeTimeoutMs / 1000)}s. Accept the prompt on the camera sooner, or raise --handshake-timeout-ms.`;
    }
    return "Camera rejected PTP/IP Init. Record this message; it identifies a protocol difference from the T100 path.";
  }
  if (failedStep === "init-event") {
    return "Command channel accepted but event channel failed. Record this message; the M50 may need a different event-channel sequence.";
  }
  if (failedStep === "open-session" || failedStep === "get-device-info") {
    return "PTP/IP channels opened but MTP session failed. Record the response code; it is the missing protocol step.";
  }
  if (failedStep === "hold-session") {
    return "Session opened but GetEvent polling failed. Record the response code before trying Live View.";
  }
  return message;
}

function describeError(error: unknown): string {
  if (error instanceof Error) {
    const code = "code" in error && error.code !== undefined ? `${String(error.code)}: ` : "";
    return error.message.startsWith(code) ? error.message : `${code}${error.message}`;
  }
  return String(error);
}

async function withTimeout<T>(promise: Promise<T>, timeoutMs: number, message: string): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  const timeout = new Promise<never>((_, reject) => {
    timer = setTimeout(() => reject(new Error(message)), timeoutMs);
  });
  try {
    return await Promise.race([promise, timeout]);
  } finally {
    if (timer !== undefined) {
      clearTimeout(timer);
    }
  }
}

async function delay(milliseconds: number): Promise<void> {
  await new Promise<void>((resolve) => {
    setTimeout(resolve, milliseconds);
  });
}
