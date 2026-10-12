import { describe, expect, test } from "bun:test";
import { CANON_KNOWN_SERVICES, parseSsdpCameraResponse } from "../src/canon/ssdpDiscovery";

describe("LAN SSDP services", () => {
  test("known services include smartphone, remote-control and observed WFT", () => {
    expect(CANON_KNOWN_SERVICES).toContain(
      "urn:schemas-canon-com:service:ICPO-SmartPhoneEOSSystemService:1"
    );
    expect(CANON_KNOWN_SERVICES).toContain(
      "urn:schemas-canon-com:service:ICPO-RemoteControlEOSSystemService:1"
    );
    expect(CANON_KNOWN_SERVICES).toContain(
      "urn:schemas-canon-com:service:ICPO-WFTEOSSystemService:1"
    );
  });

  test("parses remote-control service response for LAN diagnostics", () => {
    const response = [
      "HTTP/1.1 200 OK",
      "Location: http://192.168.1.60:49152/upnp/CameraDevDesc.xml",
      "Server: Camera OS/1.0 UPnP/1.0 Canon Device Discovery/1.0",
      "ST: urn:schemas-canon-com:service:ICPO-RemoteControlEOSSystemService:1",
      "USN: uuid:camera::urn:schemas-canon-com:service:ICPO-RemoteControlEOSSystemService:1",
      "",
      ""
    ].join("\r\n");

    expect(parseSsdpCameraResponse(response, "192.168.1.60", 54911)).toMatchObject({
      address: "192.168.1.60",
      serviceType: "urn:schemas-canon-com:service:ICPO-RemoteControlEOSSystemService:1"
    });
  });

  test("still ignores non-Canon traffic on a busy router LAN", () => {
    const response = ["HTTP/1.1 200 OK", "ST: upnp:rootdevice", "USN: uuid:router", "", ""].join(
      "\r\n"
    );
    expect(parseSsdpCameraResponse(response, "192.168.1.1", 1900)).toBeNull();
  });
});
