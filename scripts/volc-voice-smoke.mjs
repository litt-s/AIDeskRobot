#!/usr/bin/env node
/*
 * 火山引擎（豆包）语音链路烟测。
 *
 * 目的：在我们投入实现之前，用真实 Key 验证三件事——
 *   1. 语音合成（TTS）能否调通、返回的音频格式是什么、延迟多少；
 *   2. 语音识别（ASR）能否调通、识别结果是否准确、延迟多少；
 *   3. 音频格式是否与机器人的 16 kHz 单声道采集对得上。
 *
 * 协议依据官方《接入语音模型》文档：
 *   ASR 双流：wss://openspeech.bytedance.com/api/v3/plan/sauc/bigmodel_async
 *             （自定义二进制帧：4 字节头 + int32 序号 + uint32 长度 + gzip(JSON)）
 *   TTS HTTP：https://openspeech.bytedance.com/api/v3/plan/tts/unidirectional
 *             （分块返回 JSON 行，音频为 base64）
 *
 * 用法（Key 只放在环境变量里，不要写进命令行历史或提交到仓库）：
 *   VOLC_API_KEY=你的key npm run volc:smoke
 *   VOLC_API_KEY=你的key VOLC_ASR_WAV=D:\some\16k-mono.wav npm run volc:smoke
 *
 * 不提供 WAV 时，脚本会先用 TTS 合成一句话、包装成 WAV 再送去识别，
 * 这样一次运行就能把两条链路都跑通。
 */

import WebSocket from 'ws';
import { gzipSync, gunzipSync } from 'node:zlib';
import { readFileSync, writeFileSync } from 'node:fs';
import { randomUUID } from 'node:crypto';

const ASR_URL = 'wss://openspeech.bytedance.com/api/v3/plan/sauc/bigmodel_async';
const TTS_URL = 'https://openspeech.bytedance.com/api/v3/plan/tts/unidirectional';

const apiKey = process.env.VOLC_API_KEY;
const asrResource = process.env.VOLC_ASR_RESOURCE || 'volc.seedasr.sauc.duration';
const ttsResource = process.env.VOLC_TTS_RESOURCE || 'seed-tts-2.0';
const speaker = process.env.VOLC_TTS_SPEAKER || 'zh_female_vv_uranus_bigtts';
const speechRate = Number(process.env.VOLC_SAMPLE_RATE || 16000);
const phrase = process.env.VOLC_TTS_TEXT || '你好，这是语音链路自检，请把这句话完整识别出来。';
const wavPath = process.env.VOLC_ASR_WAV;

/* TTS 没拿到音频时，把原因记在这里，供主流程打印，避免只看到"0 字节"而不知为何。 */
let lastTtsDiagnostic = '';

if (!apiKey) {
  console.error('缺少 VOLC_API_KEY。请用环境变量传入，不要把 Key 写进命令行或提交到仓库。');
  process.exit(2);
}

/* ---------- 协议常量（与官方文档一致） ---------- */

const PROTOCOL_VERSION = 0b0001;
const MSG = { FULL_REQUEST: 0b0001, AUDIO_ONLY: 0b0010, SERVER_FULL: 0b1001, SERVER_ERROR: 0b1111 };
const FLAG = { NONE: 0b0000, POS_SEQUENCE: 0b0001, NEG_SEQUENCE: 0b0010, NEG_WITH_SEQUENCE: 0b0011 };
const SERIALIZATION = { NONE: 0b0000, JSON: 0b0001 };
const COMPRESSION = { GZIP: 0b0001 };

function frameHeader(messageType, flags) {
  return Buffer.from([
    (PROTOCOL_VERSION << 4) | 1,
    (messageType << 4) | flags,
    (SERIALIZATION.JSON << 4) | COMPRESSION.GZIP,
    0x00,
  ]);
}

function fullRequestFrame(seq, payloadObject) {
  const compressed = gzipSync(Buffer.from(JSON.stringify(payloadObject), 'utf8'));
  const prefix = Buffer.alloc(8);
  prefix.writeInt32BE(seq, 0);
  prefix.writeUInt32BE(compressed.length, 4);
  return Buffer.concat([frameHeader(MSG.FULL_REQUEST, FLAG.POS_SEQUENCE), prefix, compressed]);
}

function audioFrame(seq, pcm, isLast) {
  const compressed = gzipSync(pcm);
  const prefix = Buffer.alloc(8);
  prefix.writeInt32BE(isLast ? -seq : seq, 0);
  prefix.writeUInt32BE(compressed.length, 4);
  const flags = isLast ? FLAG.NEG_WITH_SEQUENCE : FLAG.POS_SEQUENCE;
  return Buffer.concat([frameHeader(MSG.AUDIO_ONLY, flags), prefix, compressed]);
}

function parseResponse(message) {
  const headerSize = message[0] & 0x0f;
  const messageType = message[1] >> 4;
  const flags = message[1] & 0x0f;
  const serialization = message[2] >> 4;
  const compression = message[2] & 0x0f;
  let payload = message.subarray(headerSize * 4);

  const result = { messageType, code: 0, sequence: 0, event: 0, isLast: false, payload: null };
  if (flags & 0x01) {
    result.sequence = payload.readInt32BE(0);
    payload = payload.subarray(4);
  }
  if (flags & 0x02) result.isLast = true;
  if (flags & 0x04) {
    result.event = payload.readInt32BE(0);
    payload = payload.subarray(4);
  }
  if (messageType === MSG.SERVER_FULL) {
    result.size = payload.readUInt32BE(0);
    payload = payload.subarray(4);
  } else if (messageType === MSG.SERVER_ERROR) {
    result.code = payload.readInt32BE(0);
    result.size = payload.readUInt32BE(4);
    payload = payload.subarray(8);
  }
  if (!payload.length) return result;
  if (compression === COMPRESSION.GZIP) {
    try {
      payload = gunzipSync(payload);
    } catch {
      return result;
    }
  }
  if (serialization === SERIALIZATION.JSON) {
    try {
      result.payload = JSON.parse(payload.toString('utf8'));
    } catch {
      /* 非 JSON 的 payload 不解析，交由调用方判断 */
    }
  }
  return result;
}

/* ---------- WAV 读写（只处理 16 位 PCM） ---------- */

function pcmToWav(pcm, rate, channels = 1) {
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36 + pcm.length, 4);
  header.write('WAVE', 8);
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);            // PCM
  header.writeUInt16LE(channels, 22);
  header.writeUInt32LE(rate, 24);
  header.writeUInt32LE(rate * channels * 2, 28);
  header.writeUInt16LE(channels * 2, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36);
  header.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([header, pcm]);
}

/* 判断一段字节到底是 WAV / MP3 / OGG 容器还是裸 PCM——TTS 未必真按请求的格式返回。 */
function detectContainer(buffer) {
  if (buffer.length >= 12 && buffer.toString('ascii', 0, 4) === 'RIFF') return 'wav';
  if (buffer.length >= 4 && buffer.toString('ascii', 0, 4) === 'OggS') return 'ogg';
  if (buffer.length >= 3 && buffer.toString('ascii', 0, 3) === 'ID3') return 'mp3';
  if (buffer.length >= 2 && buffer[0] === 0xff && (buffer[1] & 0xe0) === 0xe0) return 'mp3';
  return 'pcm';
}

function readWav(buffer) {  if (buffer.length < 44 || buffer.toString('ascii', 0, 4) !== 'RIFF') {
    throw new Error('不是合法的 WAV 文件（缺少 RIFF 头）');
  }
  let offset = 12;
  let format = null;
  while (offset + 8 <= buffer.length) {
    const id = buffer.toString('ascii', offset, offset + 4);
    const size = buffer.readUInt32LE(offset + 4);
    const body = buffer.subarray(offset + 8, offset + 8 + size);
    if (id === 'fmt ') {
      format = {
        channels: body.readUInt16LE(2),
        rate: body.readUInt32LE(4),
        bits: body.readUInt16LE(14),
      };
    } else if (id === 'data') {
      if (!format) throw new Error('WAV 缺少 fmt 块');
      return { ...format, pcm: body };
    }
    offset += 8 + size + (size % 2);
  }
  throw new Error('WAV 缺少 data 块');
}

/* ---------- TTS：HTTP 分块返回 JSON 行，音频为 base64 ---------- */

async function synthesize(text, format = 'mp3') {
  const started = Date.now();
  const response = await fetch(TTS_URL, {
    method: 'POST',
    headers: {
      'X-Api-Key': apiKey,
      'X-Api-Resource-Id': ttsResource,
      'Content-Type': 'application/json',
      'Connection': 'keep-alive',
    },
    body: JSON.stringify({
      req_params: {
        text,
        speaker,
        audio_params: { format, sample_rate: format === 'pcm' ? speechRate : 24000 },
      },
    }),
  });
  if (!response.ok) {
    throw new Error(`TTS 返回 HTTP ${response.status}：${(await response.text()).slice(0, 300)}`);
  }

  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  const chunks = [];
  const rawParts = [];
  let pendingLine = '';
  let firstChunkMs = 0;
  let finished = false;

  while (!finished) {
    const { value, done } = await reader.read();
    if (done) break;
    rawParts.push(Buffer.from(value));
    pendingLine += decoder.decode(value, { stream: true });
    const lines = pendingLine.split('\n');
    pendingLine = lines.pop() ?? '';
    for (const rawLine of lines) {
      const line = rawLine.replace(/^data:\s*/, '').trim();
      if (!line) continue;
      let parsed;
      try {
        parsed = JSON.parse(line);
      } catch {
        continue;
      }
      if (parsed.code === 20000000) {
        finished = true;
        break;
      }
      if (parsed.code && parsed.code !== 0) {
        throw new Error(`TTS 返回错误码 ${parsed.code}：${JSON.stringify(parsed).slice(0, 300)}`);
      }
      if (parsed.data) {
        if (!firstChunkMs) firstChunkMs = Date.now() - started;
        chunks.push(Buffer.from(parsed.data, 'base64'));
      }
    }
  }

  const raw = Buffer.concat(rawParts);
  /*
   * 服务端不一定按"每行一个 JSON"返回：整段可能是一个不带换行的 JSON 对象，
   * 也可能直接就是音频二进制。上面按行解析没拿到音频时，这里兜底一次。
   */
  if (!chunks.length && raw.length) {
    const text = raw.toString('utf8').trim().replace(/^data:\s*/, '');
    if (text.startsWith('{')) {
      let parsed = null;
      try {
        parsed = JSON.parse(text);
      } catch {
        /* 下面按未识别处理 */
      }
      if (parsed) {
        if (parsed.code && parsed.code !== 0 && parsed.code !== 20000000) {
          throw new Error(`TTS 返回错误码 ${parsed.code}：${text.slice(0, 300)}`);
        }
        const base64Audio = parsed.data || parsed.audio || parsed.payload;
        if (typeof base64Audio === 'string') {
          chunks.push(Buffer.from(base64Audio, 'base64'));
          firstChunkMs = firstChunkMs || Date.now() - started;
        } else {
          lastTtsDiagnostic = `JSON 顶层字段：${Object.keys(parsed).join(', ')}`;
        }
      } else {
        lastTtsDiagnostic = `响应既不是行分隔 JSON 也不是完整 JSON，前 200 字符：${text.slice(0, 200)}`;
      }
    } else if (raw.length > 64) {
      chunks.push(raw); // 直接就是音频
      firstChunkMs = firstChunkMs || Date.now() - started;
    }
  } else if (!chunks.length) {
    lastTtsDiagnostic = `响应体为空（HTTP ${response.status}，Content-Type: ${response.headers.get('content-type') ?? '未知'}）`;
  }

  return {
    audio: Buffer.concat(chunks),
    firstChunkMs,
    totalMs: Date.now() - started,
    contentType: response.headers.get('content-type') ?? '未知',
    bytes: raw.length,
  };
}

/* ---------- ASR：WebSocket 双向流式 ---------- */

function transcribe(wav, { format, timeoutMs = 20000 } = {}) {
  const { rate, pcm } = readWav(wav);
  /* 默认按官方示例：剥掉 WAV 头只发裸 PCM，但声明 format=wav + codec=raw。
   * 若服务端要求连头一起发，或要求声明 pcm，可用环境变量切换，避免改代码。 */
  const declaredFormat = format || process.env.VOLC_ASR_FORMAT || 'wav';
  const declaredCodec = process.env.VOLC_ASR_CODEC || 'raw';
  const stripHeader = process.env.VOLC_ASR_STRIP_HEADER !== '0';
  const audioBytes = stripHeader ? pcm : wav;
  console.log(`  · ASR 输入：${stripHeader ? '已剥离 WAV 头' : '保留完整 WAV'}，发送 ${audioBytes.length} 字节；`
    + `声明 format=${declaredFormat} codec=${declaredCodec} rate=${rate} bits=16 channel=1`);
  const bytesPerChunk = Math.max(2, Math.floor(rate * 0.2) * 2);  // 200ms
  const started = Date.now();

  return new Promise((resolve, reject) => {
    const socket = new WebSocket(ASR_URL, {
      headers: {
        'X-Api-Key': apiKey,
        'X-Api-Resource-Id': asrResource,
        'X-Api-Request-Id': randomUUID(),
        'X-Api-Connect-Id': randomUUID(),
        'X-Api-Sequence': '-1',
      },
    });

    const transcript = { text: '', utterances: [], finalMs: 0, firstTextMs: 0 };
    let seq = 1;
    let settled = false;

    const timer = setTimeout(() => {
      if (!settled) {
        settled = true;
        try { socket.close(); } catch { /* 已关闭 */ }
        reject(new Error(`ASR 超时（${timeoutMs}ms），已收到文本：${transcript.text || '(空)'}`));
      }
    }, timeoutMs);

    const finish = (error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      try { socket.close(); } catch { /* 已关闭 */ }
      error ? reject(error) : resolve({ ...transcript, totalMs: Date.now() - started, rate });
    };

    socket.on('open', () => {
      socket.send(fullRequestFrame(seq++, {
        user: { uid: 'aidesk-smoke' },
        audio: { format: declaredFormat, codec: declaredCodec, rate, bits: 16, channel: 1 },
        request: {
          model_name: 'bigmodel',
          enable_itn: true,
          enable_punc: true,
          enable_ddc: true,
          show_utterances: true,
          enable_nonstream: false,
        },
      }));

      const chunks = [];
      for (let offset = 0; offset < audioBytes.length; offset += bytesPerChunk) {
        chunks.push(audioBytes.subarray(offset, Math.min(offset + bytesPerChunk, audioBytes.length)));
      }
      chunks.forEach((chunk, index) => {
        socket.send(audioFrame(seq++, chunk, index === chunks.length - 1));
      });
    });

    socket.on('message', (data) => {
      const parsed = parseResponse(Buffer.from(data));
      if (parsed.messageType === MSG.SERVER_ERROR || parsed.code) {
        finish(new Error(`ASR 返回错误：${JSON.stringify(parsed.payload ?? parsed).slice(0, 400)}`));
        return;
      }
      const payload = parsed.payload;
      if (payload && payload.result) {
        const text = payload.result.text;
        if (typeof text === 'string' && text) {
          if (!transcript.firstTextMs) transcript.firstTextMs = Date.now() - started;
          transcript.text = text;                        // 服务端返回的是累积结果
          transcript.finalMs = Date.now() - started;
        }
        if (Array.isArray(payload.result.utterances)) {
          transcript.utterances = payload.result.utterances;
        }
      }
      if (parsed.isLast) finish(null);
    });

    socket.on('error', (error) => finish(error));
    socket.on('close', () => finish(null));
  });
}

/* ---------- 主流程 ---------- */

async function main() {
  console.log('火山语音烟测开始（音频格式：16 kHz / 16 bit / 单声道）\n');

  console.log('[1/3] 语音合成（TTS，HTTP 分块）');
  lastTtsDiagnostic = '';
  const tts = await synthesize(phrase, 'mp3');
  writeFileSync('volc-tts-smoke.mp3', tts.audio);
  console.log(`  ✓ 首块音频 ${tts.firstChunkMs} ms，全部完成 ${tts.totalMs} ms`);
  console.log(`  ✓ 收到 ${tts.bytes} 字节响应、解析出 ${tts.audio.length} 字节音频，已写入 volc-tts-smoke.mp3`);
  console.log(`  ✓ 响应 Content-Type：${tts.contentType}`);
  if (!tts.audio.length && lastTtsDiagnostic) {
    console.log(`  ! 未解析出音频：${lastTtsDiagnostic}`);
  }
  console.log(`  ✓ 合成文本：「${phrase}」\n`);

  console.log('[2/3] 语音识别（ASR，WebSocket 双向流式）');
  let wavBuffer;
  let source;
  if (wavPath) {
    wavBuffer = readFileSync(wavPath);
    source = `本地文件 ${wavPath}`;
  } else {
    console.log('  未提供 VOLC_ASR_WAV，改用 TTS 合成一段音频作为识别输入');
    /* TTS 未必按请求的 format 返回（实测请求 pcm 时可能仍给 mp3 等容器），
     * 所以这里逐个格式试，拿到裸 PCM 或 WAV 为止。 */
    const candidates = ['pcm', 'wav', 'mp3'];
    let picked = null;
    for (const format of candidates) {
      const attempt = await synthesize(phrase, format);
      const container = detectContainer(attempt.audio);
      console.log(`  · 请求 format=${format} → 实际容器 ${container}，${attempt.audio.length} 字节，`
        + `前 8 字节 ${attempt.audio.subarray(0, 8).toString('hex')}`);
      if (container === 'pcm' && !picked) {
        picked = { kind: 'pcm', bytes: attempt.audio, label: `请求 ${format} 得到裸 PCM，按 ${speechRate} Hz 包装` };
      } else if (container === 'wav' && !picked) {
        const info = readWav(attempt.audio);
        picked = {
          kind: 'wav',
          bytes: attempt.audio,
          label: `请求 ${format} 得到 WAV（${info.rate} Hz / ${info.channels} 声道 / ${info.bits} bit）`,
        };
      }
    }
    if (!picked) {
      console.error('\n三种格式都没能拿到可用的 PCM/WAV，无法回环自检。');
      console.error('请自己录一段 16 kHz 单声道 WAV，然后用环境变量指定：');
      console.error("   $env:VOLC_ASR_WAV='D:\\某处\\voice.wav'; npm run volc:smoke");
      process.exit(3);
    }
    wavBuffer = picked.kind === 'pcm' ? pcmToWav(picked.bytes, speechRate) : picked.bytes;
    source = picked.label;
  }
  /* 发送的是裸 PCM（默认剥头），但该声明成 wav 还是 pcm 有歧义：
   * 官方示例声明 wav，而部分服务端要求与数据一致。这里依次试，一次运行定论。 */
  const formatsToTry = process.env.VOLC_ASR_FORMAT ? [process.env.VOLC_ASR_FORMAT] : ['wav', 'pcm'];
  let asr = null;
  let lastError = null;
  for (const format of formatsToTry) {
    try {
      asr = await transcribe(wavBuffer, { format });
      break;
    } catch (error) {
      lastError = error;
      console.log(`  · 声明 format=${format} 失败：${error.message.slice(0, 200)}`);
    }
  }
  if (!asr) throw lastError ?? new Error('ASR 未返回结果');
  console.log(`  ✓ 输入来源：${source}`);
  console.log(`  ✓ 首个识别结果 ${asr.firstTextMs} ms，最终结果 ${asr.finalMs} ms`);
  console.log(`  ✓ 识别文本：「${asr.text}」`);
  if (asr.utterances.length) {
    console.log(`  ✓ 分句结果 ${asr.utterances.length} 条，首条：${JSON.stringify(asr.utterances[0]).slice(0, 200)}`);
  }
  console.log('');

  console.log('[3/3] 结论');
  const keywords = ['语音', '自检', '识别'];
  const hit = keywords.filter((word) => asr.text.includes(word));
  console.log(`  识别结果命中关键词 ${hit.length}/${keywords.length}${hit.length ? `（${hit.join('、')}）` : ''}`);
  console.log('  链路可用。请据此判断：');
  console.log(`    · 机器人开口延迟 ≈ TTS 首块时间（${tts.firstChunkMs} ms，可流式播放）`);
  console.log(`    · 每轮识别耗时 ≈ ASR 最终结果时间（${asr.finalMs} ms）`);
  console.log('    · 若识别文本明显偏离合成内容，说明音频格式或采样率需要调整');
}

main().catch((error) => {
  console.error('\n烟测失败：' + error.message);
  if (error.stack) console.error(error.stack.split('\n').slice(1, 3).join('\n'));
  process.exit(1);
});
