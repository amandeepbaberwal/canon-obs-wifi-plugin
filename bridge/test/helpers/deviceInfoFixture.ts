export function buildDeviceInfo(options: { model: string; version: string; operations: number[] }): Uint8Array {
  const bytes: number[] = [];
  const u16 = (value: number): void => {
    bytes.push(value & 0xff, (value >> 8) & 0xff);
  };
  const u32 = (value: number): void => {
    bytes.push(value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, (value >>> 24) & 0xff);
  };
  const str = (value: string): void => {
    if (value.length === 0) {
      bytes.push(0);
      return;
    }
    bytes.push(value.length + 1);
    for (const char of value) {
      u16(char.charCodeAt(0));
    }
    u16(0);
  };
  const arr = (values: number[]): void => {
    u32(values.length);
    values.forEach(u16);
  };

  u16(100);
  u32(0x0000000b);
  u16(100);
  str("");
  u16(0);
  arr(options.operations);
  arr([0xc101]);
  arr([0xd101]);
  arr([]);
  arr([0x3801]);
  str("Canon Inc.");
  str(options.model);
  str(options.version);
  str("682045004792");
  return new Uint8Array(bytes);
}
