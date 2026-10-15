/*
 * 火山引擎（豆包）语音服务客户端。
 *
 * 实测结论（2026-10-14 烟测）：
 *   · ASR：发送剥头裸 PCM 时必须声明 format="pcm" + codec="raw"；
 *          声明 format="wav"（官方示例写法）会被拒。
 *   · TTS：请求 format="pcm" 直接返回 16 kHz 单声道裸 PCM，
 *          机器人可不经解码直接流式播放；mp3 体积更小，适合存文件。
 *
 * 鉴权只需 X-Api-Key 与 X-Api-Resource-Id（含 X-Api-Request-Id 等请求标识），
 * 无需换取短期 token，也无需签名。
 */

import {
  MESSAGE_TYPE,
  extractError,
  extractTranscript,
  encodeAudioFrame,
  encodeFullRequest,
  messageBytes,
  parseServerMessage,
} from './frames';

/*
 * 走 Workers 的 fetch() + Upgrade 头建 WebSocket，因此这里必须是 https:// 而不是 wss://。
 * Workers 的 Fetch API 会拒绝 wss: 这类 URL（报 "Fetch API cannot load: wss://…"），
 * 升级到 WebSocket 由 Upgrade 头完成；直接用 wss:// 只有 new WebSocket() 才接受，
 * 而构造函数无法自定义鉴权头，所以两者不能混用。
 */
const ASR_URL = 'https://openspeech.bytedance.com/api/v3/plan/sauc/bigmodel_async';
const TTS_URL = 'https://openspeech.bytedance.com/api/v3/plan/tts/unidirectional';

export interface VoiceSettings {
  apiKey: string;
  asrResourceId: string;
  ttsResourceId: string;
  speaker: string;
  sampleRate: number;
}

/** 从 Worker 环境读取平台级语音配置（首版为平台统一配置，后续可改为按用户存储）。 */
export function voiceSettingsFromEnv(env: {
  VOLC_API_KEY?: string;
  VOLC_ASR_RESOURCE?: string;
  VOLC_TTS_RESOURCE?: string;
  VOLC_TTS_SPEAKER?: string;
}): VoiceSettings | null {
  if (!env.VOLC_API_KEY) return null;
  return {
    apiKey: env.VOLC_API_KEY,
    asrResourceId: env.VOLC_ASR_RESOURCE || 'volc.seedasr.sauc.duration',
    ttsResourceId: env.VOLC_TTS_RESOURCE || 'seed-tts-2.0',
    speaker: env.VOLC_TTS_SPEAKER || 'zh_female_vv_uranus_bigtts',
    sampleRate: 16000,
  };
}

/*
 * 识别实现可替换。
 * Workers 里靠 "fetch + Upgrade 头" 拿到 WebSocket（见下面的 transcribe），但 Node 没有这套语义，
 * 所以 Node 容器启动时注入一个基于 ws 的实现（server/node/asr-ws.ts）。Worker 不注入，走原路径。
 */
export type Transcriber = (
  settings: VoiceSettings,
  pcm: Uint8Array,
  options?: { timeoutMs?: number },
) => Promise<TranscriptResult>;

let transcriberOverride: Transcriber | null = null;

export function setTranscriber(next: Transcriber | null): void {
  transcriberOverride = next;
}

/* Workers 的 fetch 在带 Upgrade 头时返回的响应上挂 webSocket；类型不在标准 lib 中。 */
type UpgradeResponse = Response & { webSocket?: WebSocket };

function decodeBase64(value: string): Uint8Array {
  const binary = atob(value);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) bytes[index] = binary.charCodeAt(index);
  return bytes;
}

export interface TranscriptResult {
  text: string;
  utterances: unknown[];
  elapsedMs: number;
}

/**
 * 把一段 16 kHz 单声道 16 位 PCM 转写成文本。
 * 采用双向流式接口：建连 → 发配置 → 按 200 ms 分片发音频 → 收结果。
 */
export async function transcribe(
  settings: VoiceSettings,
  pcm: Uint8Array,
  options: { timeoutMs?: number } = {},
): Promise<TranscriptResult> {
  if (transcriberOverride) return transcriberOverride(settings, pcm, options);
  const timeoutMs = options.timeoutMs ?? 20000;
  const started = Date.now();
  const requestId = crypto.randomUUID();
  /*
   * The connect needs its own timeout, because the socket timer below only
   * starts once the socket exists - a hung handshake would otherwise wait
   * forever. It has to be cleared the moment the socket is ours: an armed
   * signal stays attached to the upgraded connection and would tear down a
   * live session that happens to run past it.
   */
  const connect = new AbortController();
  const connectTimer = setTimeout(() => connect.abort(), timeoutMs);

  let response: UpgradeResponse;
  try {
    response = (await fetch(ASR_URL, {
      signal: connect.signal,
      headers: {
        Upgrade: 'websocket',
        'X-Api-Key': settings.apiKey,
        'X-Api-Resource-Id': settings.asrResourceId,
        'X-Api-Request-Id': requestId,
        'X-Api-Connect-Id': requestId,
        'X-Api-Sequence': '-1',
      },
    })) as UpgradeResponse;
  } catch (error) {
    clearTimeout(connectTimer);
    throw new Error(`语音识别建连失败：${error instanceof Error ? error.message : String(error)}`);
  }

  const socket = response.webSocket;
  clearTimeout(connectTimer);
  if (!socket) {
    throw new Error(`语音识别建连失败：HTTP ${response.status}`);
  }
  socket.accept();

  const bytesPerChunk = Math.max(2, Math.floor(settings.sampleRate * 0.2) * 2); // 200 ms
  let text = '';
  let utterances: unknown[] = [];
  /*
   * Count the response frames the service actually sent. An empty transcript
   * has two very different causes - the service decoded the audio and found no
   * speech in it, or we never understood a single frame it sent - and only this
   * counter tells them apart. Reporting the second as the first is what sends
   * you off tuning a microphone when the fault is in the client, which is
   * exactly where we ended up: the audio was clipped to a square wave and then,
   * once that was fixed, still reported as "no speech" every single time.
   */
  let frames = 0;

  return await new Promise<TranscriptResult>((resolve, reject) => {
    let settled = false;
    const finish = (error?: Error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      try {
        socket.close();
      } catch {
        /* 已关闭 */
      }
      if (error) reject(error);
      else if (!frames) reject(new Error('语音识别没有收到任何结果帧'));
      else resolve({ text, utterances, elapsedMs: Date.now() - started });
    };
    const timer = setTimeout(() => finish(new Error('语音识别超时')), timeoutMs);

    socket.addEventListener('message', (event: MessageEvent) => {
      void (async () => {
        const bytes = await messageBytes(event.data);
        /* 短于 4 字节头的消息不可能是响应帧，不计入。 */
        if (bytes.length >= 4) frames += 1;
        const parsed = await parseServerMessage(bytes);
        if (parsed.messageType === MESSAGE_TYPE.SERVER_ERROR_RESPONSE || parsed.code) {
          finish(new Error(extractError(parsed.payload) || `语音识别错误码 ${parsed.code}`));
          return;
        }
        const next = extractTranscript(parsed.payload);
        if (next) text = next; // 服务端返回的是累积文本
        const found = parsed.payload?.result as Record<string, unknown> | undefined;
        if (Array.isArray(found?.utterances)) utterances = found.utterances as unknown[];
        if (parsed.isLast) finish();
      })();
    });
    socket.addEventListener('close', () => finish());
    socket.addEventListener('error', () => finish(new Error('语音识别连接中断')));

    void (async () => {
      socket.send((await encodeFullRequest(1, {
        user: { uid: 'aidesk-device' },
        audio: { format: 'pcm', codec: 'raw', rate: settings.sampleRate, bits: 16, channel: 1 },
        request: {
          model_name: 'bigmodel',
          enable_itn: true,
          enable_punc: true,
          /* Kept in step with the smoke test that verified this endpoint
           * against the real service; do not drop one without re-testing. */
          enable_ddc: true,
          show_utterances: true,
          enable_nonstream: false,
        },
      })).buffer as ArrayBuffer);

      let seq = 2;
      for (let offset = 0; offset < pcm.length; offset += bytesPerChunk) {
        const chunk = pcm.subarray(offset, Math.min(offset + bytesPerChunk, pcm.length));
        const isLast = offset + bytesPerChunk >= pcm.length;
        socket.send((await encodeAudioFrame(seq, chunk, isLast)).buffer as ArrayBuffer);
        seq += 1;
      }
      if (!pcm.length) {
        socket.send((await encodeAudioFrame(seq, new Uint8Array(0), true)).buffer as ArrayBuffer);
      }
    })().catch((error: unknown) => finish(error instanceof Error ? error : new Error(String(error))));
  });
}

export interface SynthesisResult {
  bytes: number;
  firstChunkMs: number;
  totalMs: number;
  contentType: string;
}

/**
 * 把文本合成为语音。每收到一段音频就立刻交给 onChunk，
 * 因此调用方可以边收边播，不必等整句合成完。
 */
export async function synthesize(
  settings: VoiceSettings,
  text: string,
  onChunk: (chunk: Uint8Array) => void | Promise<void>,
  options: { format?: 'pcm' | 'mp3' | 'wav'; timeoutMs?: number } = {},
): Promise<SynthesisResult> {
  const format = options.format ?? 'pcm';
  const started = Date.now();
  const response = await fetch(TTS_URL, {
    method: 'POST',
    headers: {
      'X-Api-Key': settings.apiKey,
      'X-Api-Resource-Id': settings.ttsResourceId,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({
      req_params: {
        text,
        speaker: settings.speaker,
        audio_params: {
          format,
          sample_rate: format === 'pcm' ? settings.sampleRate : 24000,
        },
      },
    }),
    signal: options.timeoutMs ? AbortSignal.timeout(options.timeoutMs) : undefined,
  });
  if (!response.ok || !response.body) {
    throw new Error(`语音合成失败：HTTP ${response.status}`);
  }

  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let pendingLine = '';
  let firstChunkMs = 0;
  let bytes = 0;
  let finished = false;

  while (!finished) {
    const { value, done } = await reader.read();
    if (done) break;
    pendingLine += decoder.decode(value, { stream: true });
    const lines = pendingLine.split('\n');
    pendingLine = lines.pop() ?? '';
    for (const rawLine of lines) {
      const line = rawLine.replace(/^data:\s*/, '').trim();
      if (!line) continue;
      let parsed: Record<string, unknown>;
      try {
        parsed = JSON.parse(line) as Record<string, unknown>;
      } catch {
        continue;
      }
      const code = typeof parsed.code === 'number' ? parsed.code : 0;
      if (code === 20000000) {
        finished = true;
        break;
      }
      if (code && code !== 0) {
        throw new Error(`语音合成错误码 ${code}`);
      }
      const data = parsed.data;
      if (typeof data === 'string') {
        if (!firstChunkMs) firstChunkMs = Date.now() - started;
        const chunk = decodeBase64(data);
        bytes += chunk.length;
        await onChunk(chunk);
      }
    }
  }

  return {
    bytes,
    firstChunkMs,
    totalMs: Date.now() - started,
    contentType: response.headers.get('content-type') ?? '',
  };
}
