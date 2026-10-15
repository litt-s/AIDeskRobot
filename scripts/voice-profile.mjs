/*
 * 语音一轮的"出流剖析"：把一次 /api/device/voice 的响应按帧拆开，记录每帧到达时刻，
 * 用来回答"机器人为什么一卡一卡"——是服务端出流有空洞，还是别的原因。
 */
import {readFileSync, writeFileSync} from 'node:fs';

const BASE = process.env.VOICE_BASE || 'http://127.0.0.1:8896';
const wavPath = process.argv[2] || 'Hardware/esp32/assets/provisioning.wav';
const sdkconfig = readFileSync('Hardware/esp32/sdkconfig', 'utf8');
const key = /^CONFIG_ROBOT_CLOUD_KEY="?([^"]*)"?$/m.exec(sdkconfig)[1].replace(/[^0-9A-Za-z]/g, '');

const wav = readFileSync(wavPath);
/* 标准 44 字节 WAV 头之后就是 16 kHz / 16 bit / 单声道裸 PCM。 */
const pcm = new Uint8Array(wav.subarray(44));
console.log(`input: ${wavPath} pcm=${pcm.length} bytes = ${(pcm.length / 32000).toFixed(2)} s`);

const started = Date.now();
const response = await fetch(`${BASE}/api/device/voice`, {
  method: 'POST',
  headers: {
    Authorization: 'Device ' + key,
    'Content-Type': 'application/octet-stream',
    'X-Sample-Rate': '16000',
  },
  body: pcm,
});
console.log(`HTTP ${response.status} ${response.headers.get('content-type')}`);

const frames = [];
let pending = new Uint8Array(0);
let audioBytes = 0;
const reader = response.body.getReader();
for (;;) {
  const {done, value} = await reader.read();
  if (done) break;
  const merged = new Uint8Array(pending.length + value.length);
  merged.set(pending, 0); merged.set(value, pending.length);
  const view = new DataView(merged.buffer, merged.byteOffset, merged.byteLength);
  let offset = 0;
  while (merged.length - offset >= 5) {
    const type = merged[offset];
    const length = view.getUint32(offset + 1, true);
    if (merged.length - offset - 5 < length) break;
    const payload = merged.subarray(offset + 5, offset + 5 + length);
    const at = Date.now() - started;
    if (type === 2) {
      audioBytes += length;
      frames.push({kind: 'pcm', at, bytes: length, audioMs: audioBytes / 32});
    } else {
      let text = '';
      try { text = new TextDecoder().decode(payload); } catch {}
      frames.push({kind: 'json', at, text});
    }
    offset += 5 + length;
  }
  pending = merged.subarray(offset).slice();
}

let lastPcmAt = 0;
let lastAudioMs = 0;
const gaps = [];
for (const f of frames) {
  if (f.kind !== 'pcm') continue;
  const producedMs = f.audioMs - lastAudioMs;          // 这一帧带来多少秒音频
  const wallMs = f.at - lastPcmAt;                     // 与上一帧的墙钟间隔
  if (lastPcmAt) gaps.push({atMs: f.at, wallMs, producedMs: Math.round(producedMs), deficitMs: Math.round(wallMs - producedMs)});
  lastPcmAt = f.at; lastAudioMs = f.audioMs;
}
const big = gaps.filter(g => g.deficitMs > 120);
console.log(`\n总音频 ${(audioBytes / 32000).toFixed(2)} s，墙钟 ${(Date.now() - started) / 1000} s`);
console.log(`PCM 帧数 ${frames.filter(f => f.kind === 'pcm').length}，JSON 事件 ${frames.filter(f => f.kind === 'json').length}`);
console.log(`帧间"缺口"（墙钟 > 音频产出，单位 ms）大于 120 的共 ${big.length} 处：`);
for (const g of big.slice(0, 20)) console.log(`   @${g.atMs}ms  缺口 ${g.deficitMs}ms（墙钟 ${g.wallMs} / 音频 ${g.producedMs}）`);
const worst = big.reduce((m, g) => Math.max(m, g.deficitMs), 0);
console.log(`最大缺口 ${worst} ms；所有缺口合计 ${big.reduce((s, g) => s + g.deficitMs, 0)} ms`);
console.log('\n事件顺序：');
for (const f of frames.filter(f => f.kind === 'json')) console.log(`   @${f.at}ms ${f.text.slice(0, 120)}`);
writeFileSync('data/voice-profile.json', JSON.stringify({frames, gaps, audioBytes, wallMs: Date.now() - started}, null, 1));
console.log('\n明细已写入 data/voice-profile.json');
