import WebSocket from 'ws';
import {
  MESSAGE_TYPE,
  encodeAudioFrame,
  encodeFullRequest,
  extractError,
  extractTranscript,
  messageBytes,
  parseServerMessage,
} from '../voice/frames';
import type {TranscriptResult, VoiceSettings} from '../voice/volc';

const ASR_URL = 'wss://openspeech.bytedance.com/api/v3/plan/sauc/bigmodel_async';

/*
 * Node 版的语音识别。
 *
 * Worker 里可以用 "fetch + Upgrade 头" 拿到 WebSocket，Node 没有这套语义，
 * 所以这里用 ws 库直连——帧格式复用 server/voice/frames.ts（与 Worker 版同一份协议实现），
 * 逻辑对照 scripts/volc-voice-smoke.mjs（那份在本机真机验证过）。
 */
export async function transcribeViaWs(
  settings: VoiceSettings,
  pcm: Uint8Array,
  options: {timeoutMs?: number} = {},
): Promise<TranscriptResult> {
  const timeoutMs = options.timeoutMs ?? 20000;
  const started = Date.now();
  const requestId = crypto.randomUUID();

  return await new Promise<TranscriptResult>((resolve, reject) => {
    let settled = false;
    let text = '';
    let utterances: unknown[] = [];
    const finish = (error?: Error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      try {
        socket.close();
      } catch {
        /* 连接可能已经断了，忽略 */
      }
      if (error) reject(error);
      else resolve({text, utterances, elapsedMs: Date.now() - started});
    };
    const timer = setTimeout(() => finish(new Error('语音识别超时')), timeoutMs);

    const socket = new WebSocket(ASR_URL, {
      headers: {
        'X-Api-Key': settings.apiKey,
        'X-Api-Resource-Id': settings.asrResourceId,
        'X-Api-Request-Id': requestId,
        'X-Api-Connect-Id': requestId,
        'X-Api-Sequence': '-1',
      },
    });

    socket.on('open', () => {
      void (async () => {
        const bytesPerChunk = Math.max(2, Math.floor(settings.sampleRate * 0.2) * 2); // 200 ms
        socket.send(
          Buffer.from(
            await encodeFullRequest(1, {
              user: {uid: 'aidesk-device'},
              audio: {format: 'pcm', codec: 'raw', rate: settings.sampleRate, bits: 16, channel: 1},
              request: {
                model_name: 'bigmodel',
                enable_itn: true,
                enable_punc: true,
                enable_ddc: true,
                show_utterances: true,
                enable_nonstream: false,
              },
            }),
          ),
        );
        let seq = 2;
        for (let offset = 0; offset < pcm.length; offset += bytesPerChunk) {
          const chunk = pcm.subarray(offset, Math.min(offset + bytesPerChunk, pcm.length));
          const isLast = offset + bytesPerChunk >= pcm.length;
          socket.send(Buffer.from(await encodeAudioFrame(seq, chunk, isLast)));
          seq += 1;
        }
        if (!pcm.length) {
          socket.send(Buffer.from(await encodeAudioFrame(seq, new Uint8Array(0), true)));
        }
      })().catch((error: unknown) => finish(error instanceof Error ? error : new Error(String(error))));
    });

    socket.on('message', (data: unknown) => {
      void (async () => {
        const bytes = await messageBytes(data);
        if (bytes.length < 4) return;
        const parsed = await parseServerMessage(bytes);
        if (parsed.messageType === MESSAGE_TYPE.SERVER_ERROR_RESPONSE || parsed.code) {
          finish(new Error(extractError(parsed.payload) || `语音识别错误码 ${parsed.code}`));
          return;
        }
        const next = extractTranscript(parsed.payload);
        if (next) text = next; // 服务端返回累积文本
        const found = parsed.payload?.result as Record<string, unknown> | undefined;
        if (Array.isArray(found?.utterances)) utterances = found.utterances as unknown[];
        if (parsed.isLast) finish();
      })();
    });
    socket.on('close', () => finish());
    socket.on('error', (error: Error) => finish(new Error(`语音识别连接失败：${error.message}`)));
  });
}
