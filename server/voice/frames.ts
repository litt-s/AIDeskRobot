/*
 * 火山引擎（豆包）语音 WebSocket 帧协议。
 *
 * 内容依据官方《接入语音模型》文档，并经 2026-10-14 的烟测验证通过
 * （scripts/volc-voice-smoke.mjs）：发送剥头裸 PCM 时必须声明 format="pcm"。
 *
 * 这里只做纯协议编解码，不涉及网络，便于单元测试；传输层在 volc.ts。
 * 压缩用 CompressionStream（Workers 与 Node 18+ 都原生支持），
 * 因此同一份代码在 Worker 与本地测试中行为一致。
 */

export const PROTOCOL_VERSION = 0b0001;

export const MESSAGE_TYPE = {
  CLIENT_FULL_REQUEST: 0b0001,
  CLIENT_AUDIO_ONLY_REQUEST: 0b0010,
  SERVER_FULL_RESPONSE: 0b1001,
  SERVER_ERROR_RESPONSE: 0b1111,
} as const;

export const FLAGS = {
  NO_SEQUENCE: 0b0000,
  POS_SEQUENCE: 0b0001,
  NEG_SEQUENCE: 0b0010,
  NEG_WITH_SEQUENCE: 0b0011,
} as const;

export const SERIALIZATION = { NONE: 0b0000, JSON: 0b0001 } as const;
export const COMPRESSION = { GZIP: 0b0001 } as const;

export async function gzipBytes(data: Uint8Array): Promise<Uint8Array> {
  const stream = new Blob([data as BlobPart]).stream().pipeThrough(new CompressionStream('gzip'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

export async function gunzipBytes(data: Uint8Array): Promise<Uint8Array> {
  const stream = new Blob([data as BlobPart]).stream().pipeThrough(new DecompressionStream('gzip'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

export function frameHeader(messageType: number, flags: number): Uint8Array {
  return new Uint8Array([
    (PROTOCOL_VERSION << 4) | 1,
    (messageType << 4) | flags,
    (SERIALIZATION.JSON << 4) | COMPRESSION.GZIP,
    0x00,
  ]);
}

function concat(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((sum, part) => sum + part.length, 0);
  const out = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }
  return out;
}

/** 建连后的第一帧：携带 user/audio/request 配置。 */
export async function encodeFullRequest(seq: number, payload: unknown): Promise<Uint8Array> {
  const body = await gzipBytes(new TextEncoder().encode(JSON.stringify(payload)));
  const prefix = new Uint8Array(8);
  new DataView(prefix.buffer).setInt32(0, seq);
  new DataView(prefix.buffer).setUint32(4, body.length);
  return concat([frameHeader(MESSAGE_TYPE.CLIENT_FULL_REQUEST, FLAGS.POS_SEQUENCE), prefix, body]);
}

/** 音频帧：`isLast` 时序号取负并置 NEG_WITH_SEQUENCE，表示音频发送结束。 */
export async function encodeAudioFrame(seq: number, pcm: Uint8Array, isLast: boolean): Promise<Uint8Array> {
  const body = await gzipBytes(pcm);
  const prefix = new Uint8Array(8);
  const view = new DataView(prefix.buffer);
  view.setInt32(0, isLast ? -seq : seq);
  view.setUint32(4, body.length);
  const flags = isLast ? FLAGS.NEG_WITH_SEQUENCE : FLAGS.POS_SEQUENCE;
  return concat([frameHeader(MESSAGE_TYPE.CLIENT_AUDIO_ONLY_REQUEST, flags), prefix, body]);
}

export interface ServerMessage {
  messageType: number;
  code: number;
  sequence: number;
  event: number;
  isLast: boolean;
  payload: Record<string, unknown> | null;
  /** 解压失败等情况下保留原始 payload，便于诊断。 */
  raw?: Uint8Array;
}

/** 解析服务端返回的帧；顺序与官方 ResponseParser 一致。 */
export async function parseServerMessage(message: Uint8Array): Promise<ServerMessage> {
  const headerSize = message[0] & 0x0f;
  const messageType = message[1] >> 4;
  const flags = message[1] & 0x0f;
  const serialization = message[2] >> 4;
  const compression = message[2] & 0x0f;
  let payload = message.subarray(headerSize * 4);
  const view = () => new DataView(payload.buffer, payload.byteOffset, payload.byteLength);

  const result: ServerMessage = { messageType, code: 0, sequence: 0, event: 0, isLast: false, payload: null };

  if (flags & 0x01) {
    result.sequence = view().getInt32(0);
    payload = payload.subarray(4);
  }
  if (flags & 0x02) result.isLast = true;
  if (flags & 0x04) {
    result.event = view().getInt32(0);
    payload = payload.subarray(4);
  }
  if (messageType === MESSAGE_TYPE.SERVER_FULL_RESPONSE) {
    payload = payload.subarray(4); // 跳过 payload_size
  } else if (messageType === MESSAGE_TYPE.SERVER_ERROR_RESPONSE) {
    result.code = view().getInt32(0);
    payload = payload.subarray(8); // code + payload_size
  }
  if (!payload.length) return result;

  if (compression === COMPRESSION.GZIP) {
    try {
      payload = await gunzipBytes(payload);
    } catch {
      result.raw = payload;
      return result;
    }
  }
  if (serialization === SERIALIZATION.JSON) {
    try {
      const text = new TextDecoder().decode(payload);
      result.payload = JSON.parse(text) as Record<string, unknown>;
    } catch {
      result.raw = payload;
    }
  }
  return result;
}

/** 从响应 payload 中取出识别文本（服务端返回累积结果），并兼容错误结构。 */
export function extractTranscript(payload: Record<string, unknown> | null): string {
  if (!payload) return '';
  const result = payload.result as Record<string, unknown> | undefined;
  const text = result?.text;
  return typeof text === 'string' ? text : '';
}

/**
 * 把 WebSocket 收到的消息统一成字节。
 *
 * 消息是什么类型由运行时决定，不是由我们决定：Workers 主动发起的 WebSocket
 * 既可能给 ArrayBuffer，也可能按 WebSocket 规范默认的 `binaryType = "blob"`
 * 给 Blob。原先的实现只认 ArrayBuffer 和字符串，**其他一切都会被静默编码成空数组**——
 * 而空数组解析出来是一个"没有任何内容的帧"：不报错、不结束、也没有文本，
 * 于是整轮看起来就是"识别说没听到人说话"。这类错误完全无声，所以这里把每种
 * 可能的形态都显式处理掉。
 */
export async function messageBytes(data: unknown): Promise<Uint8Array> {
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  /* 视图要按 byteOffset/byteLength 取，不能直接用 .buffer（可能带别的数据）。 */
  if (ArrayBuffer.isView(data)) {
    return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  }
  if (typeof data === 'string') return new TextEncoder().encode(data);
  const blob = data as Blob | null;
  if (blob && typeof blob.arrayBuffer === 'function') {
    return new Uint8Array(await blob.arrayBuffer());
  }
  return new Uint8Array(0);
}

export function extractError(payload: Record<string, unknown> | null): string {
  if (!payload) return '';
  const error = payload.error;
  if (typeof error === 'string') return error;
  const message = payload.message;
  return typeof message === 'string' ? message : '';
}
