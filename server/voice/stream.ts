/*
 * 设备语音通道的应答分帧。
 *
 * 机器人在一条 HTTP 响应里同时要拿到"识别出的文字""回复文字"和"要播放的 PCM"，
 * 而固件侧只有很薄的解析能力，所以格式按最小可解析设计：
 *
 *   [类型 1 字节][长度 4 字节 小端][负载]
 *
 *   0x01 JSON 事件（UTF-8，形如 {"type":"asr","text":"..."}）
 *   0x02 PCM 音频片（16 kHz / 16 位 / 单声道，小端）
 *
 * 不套 chunked 之外的容器、不做压缩：设备要边收边播，解码越少延迟越低。
 * 这里只做纯编解码，便于单元测试；同样格式的解析在固件 robot_cloud 中实现。
 */

export const STREAM_TYPE = { JSON: 0x01, PCM: 0x02 } as const;

const HEADER_BYTES = 5;

export function encodeStreamFrame(type: number, payload: Uint8Array): Uint8Array {
  const frame = new Uint8Array(HEADER_BYTES + payload.length);
  frame[0] = type;
  new DataView(frame.buffer).setUint32(1, payload.length, true);
  frame.set(payload, HEADER_BYTES);
  return frame;
}

export function encodeJsonEvent(value: unknown): Uint8Array {
  return encodeStreamFrame(STREAM_TYPE.JSON, new TextEncoder().encode(JSON.stringify(value)));
}

export function encodePcmChunk(pcm: Uint8Array): Uint8Array {
  return encodeStreamFrame(STREAM_TYPE.PCM, pcm);
}

export interface StreamFrame {
  type: number;
  payload: Uint8Array;
}

/**
 * 增量拆帧器：字节流可能在任何位置被切断，因此保留半帧等待后续数据。
 * 服务端测试用它验证分帧，固件侧按同样的状态机解析。
 */
export class StreamDemuxer {
  private pending: Uint8Array = new Uint8Array(0);

  push(chunk: Uint8Array): StreamFrame[] {
    const merged = new Uint8Array(this.pending.length + chunk.length);
    merged.set(this.pending, 0);
    merged.set(chunk, this.pending.length);
    const view = new DataView(merged.buffer, merged.byteOffset, merged.byteLength);
    const frames: StreamFrame[] = [];
    let offset = 0;
    while (merged.length - offset >= HEADER_BYTES) {
      const length = view.getUint32(offset + 1, true);
      if (merged.length - offset - HEADER_BYTES < length) break;
      frames.push({
        type: merged[offset],
        payload: merged.subarray(offset + HEADER_BYTES, offset + HEADER_BYTES + length),
      });
      offset += HEADER_BYTES + length;
    }
    this.pending = merged.subarray(offset).slice();
    return frames;
  }
}
