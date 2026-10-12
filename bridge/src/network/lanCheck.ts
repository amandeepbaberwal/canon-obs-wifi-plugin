export interface LanSubnetCheck {
  cameraIp: string;
  localAddress?: string;
  sameSubnet24: boolean | null;
  warning: string | null;
}

export function isValidIpv4(value: string): boolean {
  const parts = value.split(".");
  if (parts.length !== 4) {
    return false;
  }
  return parts.every((part) => {
    if (part.length === 0 || !/^\d+$/.test(part)) {
      return false;
    }
    const n = Number(part);
    return Number.isInteger(n) && n >= 0 && n <= 255;
  });
}

export function sameSubnet24(a: string, b: string): boolean {
  if (!isValidIpv4(a) || !isValidIpv4(b)) {
    return false;
  }
  const a3 = a.split(".").slice(0, 3).join(".");
  const b3 = b.split(".").slice(0, 3).join(".");
  return a3 === b3;
}

export function describeLanMismatch(cameraIp: string, localAddress?: string): LanSubnetCheck {
  if (localAddress === undefined) {
    return { cameraIp, sameSubnet24: null, warning: "No --local-address given; bind interface is ambiguous on multi-homed PCs." };
  }
  if (!isValidIpv4(cameraIp) || !isValidIpv4(localAddress)) {
    return { cameraIp, localAddress, sameSubnet24: null, warning: "Camera IP or local address is not valid IPv4." };
  }
  if (sameSubnet24(cameraIp, localAddress)) {
    return { cameraIp, localAddress, sameSubnet24: true, warning: null };
  }
  return {
    cameraIp,
    localAddress,
    sameSubnet24: false,
    warning: `Camera ${cameraIp} and PC ${localAddress} are on different /24 subnets; LAN discovery and PTP/IP usually require the same router subnet.`
  };
}

export function firewallHintForProbe(errorMessage?: string): string {
  const base =
    "Windows Firewall: allow outbound TCP to <camera-ip>:15740 (PTP/IP) and inbound/outbound UDP 239.255.255.250:1900 (SSDP discovery). " +
    "Rivet never changes firewall rules automatically. " +
    "To add a scoped outbound rule, run in elevated PowerShell: " +
    "New-NetFirewallRule -DisplayName 'Rivet Canon PTP/IP' -Direction Outbound -Protocol TCP -RemotePort 15740 -RemoteAddress <camera-ip> -Action Allow -Profile Private. " +
    "Then re-run the LAN check.";
  if (errorMessage === undefined || errorMessage.length === 0) {
    return base;
  }
  return `${base} Last error: ${errorMessage}`;
}
