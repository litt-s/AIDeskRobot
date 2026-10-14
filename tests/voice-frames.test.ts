import {describe,it,expect} from 'vitest';
import {
  FLAGS,
  MESSAGE_TYPE,
  SERIALIZATION,
  COMPRESSION,
  encodeFullRequest,
  encodeAudioFrame,
  gzipBytes,
  gunzipBytes,
  parseServerMessage,
  extractTranscript,
  extractError,
  messageBytes,
} from '../server/voice/frames';

/*
 * 这些断言锁定的是 2026-10-14 烟测验证通过的帧格式。
 * 如果改动破坏了协议，测试应当在这里失败，而不是等到真机联调。
 */

describe('voice frame protocol', () => {
  it('gzip round-trips bytes', async () => {
    const original = new TextEncoder().encode('你好，语音链路自检');
    const restored = await gunzipBytes(await gzipBytes(original));
    expect(new TextDecoder().decode(restored)).toBe('你好，语音链路自检');
  });

  it('encodes a full request frame with the documented header layout', async () => {
    const frame = await encodeFullRequest(1, { audio: { format: 'pcm', rate: 16000 } });
    const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);

    expect(frame[0]).toBe(0x11);                                    // 版本 1 / 头长 1 个字
    expect(frame[1] >> 4).toBe(MESSAGE_TYPE.CLIENT_FULL_REQUEST);
    expect(frame[1] & 0x0f).toBe(FLAGS.POS_SEQUENCE);
    expect(frame[2] >> 4).toBe(SERIALIZATION.JSON);
    expect(frame[2] & 0x0f).toBe(COMPRESSION.GZIP);
    expect(frame[3]).toBe(0x00);

    expect(view.getInt32(4)).toBe(1);                               // 序号
    const payloadSize = view.getUint32(8);
    expect(payloadSize).toBe(frame.length - 12);                    // 头 4 + 序号 4 + 长度 4

    const payload = await gunzipBytes(frame.subarray(12));
    expect(JSON.parse(new TextDecoder().decode(payload))).toEqual({ audio: { format: 'pcm', rate: 16000 } });
  });

  it('negates the sequence and sets NEG_WITH_SEQUENCE on the final audio frame', async () => {
    const middle = await encodeAudioFrame(7, new Uint8Array([1, 2, 3, 4]), false);
    const last = await encodeAudioFrame(7, new Uint8Array([5, 6]), true);

    expect(middle[1] >> 4).toBe(MESSAGE_TYPE.CLIENT_AUDIO_ONLY_REQUEST);
    expect(new DataView(middle.buffer, middle.byteOffset).getInt32(4)).toBe(7);
    expect(middle[1] & 0x0f).toBe(FLAGS.POS_SEQUENCE);

    expect(last[1] & 0x0f).toBe(FLAGS.NEG_WITH_SEQUENCE);
    expect(new DataView(last.buffer, last.byteOffset).getInt32(4)).toBe(-7);
  });

  it('parses a server result frame and reads the cumulative transcript', async () => {
    // 构造服务端返回：版本/头长、SERVER_FULL_RESPONSE + POS_SEQUENCE、JSON + GZIP
    const body = await gzipBytes(new TextEncoder().encode(JSON.stringify({
      result: { text: '你好，这是语音链路自检', utterances: [{ text: '你好，这是语音链路自检' }] },
    })));
    const header = new Uint8Array([0x11, (MESSAGE_TYPE.SERVER_FULL_RESPONSE << 4) | FLAGS.POS_SEQUENCE, 0x11, 0x00]);
    const prefix = new Uint8Array(8);
    new DataView(prefix.buffer).setInt32(0, 1);
    new DataView(prefix.buffer).setUint32(4, body.length);
    const frame = new Uint8Array(header.length + prefix.length + body.length);
    frame.set(header, 0);
    frame.set(prefix, header.length);
    frame.set(body, header.length + prefix.length);

    const parsed = await parseServerMessage(frame);
    expect(parsed.messageType).toBe(MESSAGE_TYPE.SERVER_FULL_RESPONSE);
    expect(extractTranscript(parsed.payload)).toBe('你好，这是语音链路自检');
    expect(Array.isArray((parsed.payload?.result as Record<string, unknown>).utterances)).toBe(true);
    expect(extractError(parsed.payload)).toBe('');
  });

  it('marks the final frame and surfaces server errors', async () => {
    const body = await gzipBytes(new TextEncoder().encode(JSON.stringify({
      error: 'The audio could not be decoded using the declared format.',
    })));
    /*
     * NEG_SEQUENCE 只带"最后一帧"标志、不带序号字段，
     * 因此 payload 直接是 code + size + body（与官方解析器的字段顺序对应）。
     */
    const header = new Uint8Array([0x11, (MESSAGE_TYPE.SERVER_ERROR_RESPONSE << 4) | FLAGS.NEG_SEQUENCE, 0x11, 0x00]);
    const prefix = new Uint8Array(8);
    new DataView(prefix.buffer).setInt32(0, 12345);   // code
    new DataView(prefix.buffer).setUint32(4, body.length);
    const frame = new Uint8Array(header.length + prefix.length + body.length);
    frame.set(header, 0);
    frame.set(prefix, header.length);
    frame.set(body, header.length + prefix.length);

    const parsed = await parseServerMessage(frame);
    expect(parsed.messageType).toBe(MESSAGE_TYPE.SERVER_ERROR_RESPONSE);
    expect(parsed.code).toBe(12345);
    expect(parsed.isLast).toBe(true);
    expect(extractError(parsed.payload)).toContain('could not be decoded');
  });

  it('keeps raw payload when decompression fails instead of throwing', async () => {
    const header = new Uint8Array([0x11, (MESSAGE_TYPE.SERVER_FULL_RESPONSE << 4) | FLAGS.POS_SEQUENCE, 0x11, 0x00]);
    const prefix = new Uint8Array(8);
    new DataView(prefix.buffer).setUint32(4, 5);
    const junk = new Uint8Array([9, 9, 9, 9, 9]);
    const frame = new Uint8Array([...header, ...prefix, ...junk]);

    const parsed = await parseServerMessage(frame);
    expect(parsed.payload).toBeNull();
    expect(parsed.raw?.length).toBeGreaterThan(0);
  });
});

/*
 * WebSocket 消息是什么类型由运行时决定，不是由我们决定。原先的实现只认
 * ArrayBuffer 和字符串，其余一切都被静默变成空数组——而空数组解析出来是
 * 一个"没有内容的帧"：不报错、不结束、也没有文本，整轮看起来就是"识别说
 * 没听到人说话"。这类错误完全无声，所以每种形态都钉一条。
 */
describe('voice websocket message decoding', () => {
  const samples = new Uint8Array([0x11, 0x92, 0x11, 0x00, 0x42]);

  it('decodes an ArrayBuffer as-is', async () => {
    expect(await messageBytes(samples.buffer)).toEqual(samples);
  });

  it('decodes a view by its own offset, not its whole backing buffer', async () => {
    const backing = new Uint8Array([9, 9, 9, ...samples, 7, 7]);
    const view = new Uint8Array(backing.buffer, 3, samples.length);
    /* 用 .buffer 会连带前后各 3 字节，帧头就错位了。 */
    expect(await messageBytes(view)).toEqual(samples);
  });

  it('decodes a Blob, which is what the platform default binaryType gives', async () => {
    expect(await messageBytes(new Blob([samples]))).toEqual(samples);
  });

  it('decodes a string as UTF-8', async () => {
    expect(await messageBytes('ab')).toEqual(new Uint8Array([0x61, 0x62]));
  });

  it('returns nothing for shapes that carry no frame', async () => {
    for (const data of [undefined, null, 42, {}, []]) {
      expect(await messageBytes(data)).toEqual(new Uint8Array(0));
    }
  });
});
