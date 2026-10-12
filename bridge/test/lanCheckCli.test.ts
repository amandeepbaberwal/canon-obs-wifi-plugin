import { describe, expect, test } from "bun:test";
import { parseArgs, usage } from "../src/config";

describe("lan-check CLI", () => {
  test("parses lan-check mode", () => {
    expect(
      parseArgs(["--camera-ip", "192.168.1.60", "--local-address", "192.168.1.43", "--lan-check"])
    ).toMatchObject({
      cameraIp: "192.168.1.60",
      localAddress: "192.168.1.43",
      lanCheck: true,
      probeOnly: false
    });
  });

  test("documents lan-check", () => {
    expect(usage()).toContain("--lan-check");
  });
});
