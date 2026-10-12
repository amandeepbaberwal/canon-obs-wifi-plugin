import { describe, expect, test } from "bun:test";
import {
  describeLanMismatch,
  firewallHintForProbe,
  isValidIpv4,
  sameSubnet24
} from "../src/network/lanCheck";

describe("lanCheck", () => {
  test("validates IPv4", () => {
    expect(isValidIpv4("192.168.1.43")).toBe(true);
    expect(isValidIpv4("999.1.1.1")).toBe(false);
    expect(isValidIpv4("not-an-ip")).toBe(false);
  });

  test("detects same /24 subnet", () => {
    expect(sameSubnet24("192.168.1.60", "192.168.1.43")).toBe(true);
    expect(sameSubnet24("192.168.1.60", "192.168.2.10")).toBe(false);
  });

  test("warns on subnet mismatch", () => {
    const match = describeLanMismatch("192.168.1.60", "192.168.1.43");
    expect(match.sameSubnet24).toBe(true);
    expect(match.warning).toBeNull();

    const mismatch = describeLanMismatch("192.168.1.60", "10.0.0.5");
    expect(mismatch.sameSubnet24).toBe(false);
    expect(mismatch.warning).toContain("different /24");
  });

  test("firewall hint explains the rule without changing it", () => {
    const hint = firewallHintForProbe("Timed out after 2000ms");
    expect(hint).toContain("15740");
    expect(hint).toContain("New-NetFirewallRule");
    expect(hint).toContain("never changes firewall");
  });
});
