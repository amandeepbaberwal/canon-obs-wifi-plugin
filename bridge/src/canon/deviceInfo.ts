export interface PtpDeviceInfo {
  standardVersion: number;
  vendorExtensionId: number;
  vendorExtensionVersion: number;
  vendorExtensionDesc: string;
  functionalMode: number;
  operationsSupported: number[];
  eventsSupported: number[];
  devicePropertiesSupported: number[];
  captureFormats: number[];
  imageFormats: number[];
  manufacturer: string;
  model: string;
  deviceVersion: string;
  serialNumber: string;
}

export interface OperationSupport {
  name: string;
  code: string;
  supported: boolean;
}

// Operations used by LiveViewBootstrap / CanonLiveViewFrameSource.
const LIVE_VIEW_OPERATIONS: Array<[string, number]> = [
  ["SetRemoteMode", 0x9114],
  ["SetEventMode", 0x9115],
  ["GetEvent", 0x9116],
  ["SetDevicePropValueEx", 0x9110],
  ["SetCapacity", 0x913d],
  ["SetPcConnectMode", 0x911a],
  ["GetViewFinderData", 0x9153]
];

export function parsePtpDeviceInfo(bytes: Uint8Array): PtpDeviceInfo {
  const reader = new DeviceInfoReader(bytes);
  return {
    standardVersion: reader.u16(),
    vendorExtensionId: reader.u32(),
    vendorExtensionVersion: reader.u16(),
    vendorExtensionDesc: reader.string(),
    functionalMode: reader.u16(),
    operationsSupported: reader.u16Array(),
    eventsSupported: reader.u16Array(),
    devicePropertiesSupported: reader.u16Array(),
    captureFormats: reader.u16Array(),
    imageFormats: reader.u16Array(),
    manufacturer: reader.string(),
    model: reader.string(),
    deviceVersion: reader.string(),
    serialNumber: reader.string()
  };
}

export function describeLiveViewSupport(info: PtpDeviceInfo): OperationSupport[] {
  const supported = new Set(info.operationsSupported);
  return LIVE_VIEW_OPERATIONS.map(([name, code]) => ({
    name,
    code: `0x${code.toString(16)}`,
    supported: supported.has(code)
  }));
}

class DeviceInfoReader {
  private offset = 0;
  private readonly view: DataView;

  constructor(private readonly bytes: Uint8Array) {
    this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  u8(): number {
    this.require(1);
    const value = this.view.getUint8(this.offset);
    this.offset += 1;
    return value;
  }

  u16(): number {
    this.require(2);
    const value = this.view.getUint16(this.offset, true);
    this.offset += 2;
    return value;
  }

  u32(): number {
    this.require(4);
    const value = this.view.getUint32(this.offset, true);
    this.offset += 4;
    return value;
  }

  u16Array(): number[] {
    const count = this.u32();
    this.require(count * 2);
    const values: number[] = [];
    for (let index = 0; index < count; index += 1) {
      values.push(this.u16());
    }
    return values;
  }

  string(): string {
    const charCount = this.u8();
    if (charCount === 0) {
      return "";
    }
    this.require(charCount * 2);
    const chars: number[] = [];
    for (let index = 0; index < charCount; index += 1) {
      const code = this.u16();
      if (code !== 0) {
        chars.push(code);
      }
    }
    return String.fromCharCode(...chars);
  }

  private require(byteLength: number): void {
    if (this.offset + byteLength > this.bytes.byteLength) {
      throw new Error("Truncated PTP DeviceInfo");
    }
  }
}
