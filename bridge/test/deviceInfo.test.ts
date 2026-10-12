import { describe, expect, test } from "bun:test";
import { describeLiveViewSupport, parsePtpDeviceInfo } from "../src/canon/deviceInfo";
import { buildDeviceInfo } from "./helpers/deviceInfoFixture";

describe("parsePtpDeviceInfo", () => {
  test("parses model, firmware, and operations", () => {
    const info = parsePtpDeviceInfo(
      buildDeviceInfo({ model: "Canon EOS M50", version: "3-1.1.0", operations: [0x1001, 0x9116, 0x9153] })
    );

    expect(info.manufacturer).toBe("Canon Inc.");
    expect(info.model).toBe("Canon EOS M50");
    expect(info.deviceVersion).toBe("3-1.1.0");
    expect(info.serialNumber).toBe("682045004792");
    expect(info.vendorExtensionId).toBe(0x0b);
    expect(info.operationsSupported).toEqual([0x1001, 0x9116, 0x9153]);
  });

  test("reports which Live View operations the camera advertises", () => {
    const info = parsePtpDeviceInfo(
      buildDeviceInfo({ model: "Canon EOS M50", version: "1", operations: [0x9116, 0x9153] })
    );
    const support = describeLiveViewSupport(info);

    expect(support.find((entry) => entry.name === "GetViewFinderData")).toEqual({
      name: "GetViewFinderData",
      code: "0x9153",
      supported: true
    });
    expect(support.find((entry) => entry.name === "SetRemoteMode")?.supported).toBe(false);
  });

  test("rejects truncated payloads", () => {
    const bytes = buildDeviceInfo({ model: "Canon EOS M50", version: "1", operations: [0x9153] });
    expect(() => parsePtpDeviceInfo(bytes.slice(0, 20))).toThrow("Truncated PTP DeviceInfo");
  });
});
