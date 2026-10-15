/*
 * 语音出流模拟：用真实 TTS + 假模型跑 runVoiceTurn，并把 emit 接上一个"设备播放模型"，
 * 用来量化"机器人一卡一卡"到底是服务端出流有空洞，还是网络/设备侧的问题。
 *
 * 设备模型：收到的音频按真实时间播放（16 kHz×2 字节 = 32 字节/ms），队列有上限；
 * 队列满则 emit 阻塞（等价于固件的背压），队列空而音频还没出完就是一次"饿死"（听感上的卡顿）。
 */
import {readFileSync} from 'node:fs';
import {DatabaseSync} from 'node:sqlite';
import {runVoiceTurn} from './server/voice/pipeline';
import {synthesize} from './server/voice/volc';
import {decrypt} from './server/security';

const CAPACITY_MS = Number(process.argv[2] || 800);
const db = new DatabaseSync(process.env.SEED_DB || 'data/voice-sim/aidesk.sqlite', {readOnly: true});
const voice = db.prepare('SELECT * FROM voice_settings').get() as any;
db.close();
const settings = {
  apiKey: await decrypt(voice.api_key_cipher, process.env.ENCRYPTION_KEY || ''),
  asrResourceId: voice.asr_resource,
  ttsResourceId: voice.tts_resource,
  speaker: voice.speaker,
  sampleRate: 16000,
};

let queueMs = 0;
const starvations: Array<{startMs: number; durationMs: number}> = [];
let starvingSince: number | null = null;
let producing = true;
const started = Date.now();
let drainTimer: NodeJS.Timeout | null = null;
let now = () => Date.now() - started;

/* 真实时间 drain：每 10 ms 放掉 10 ms 音频。队列见底而音频还没出完 = 一次"饿死"（听感上的卡顿）。 */
function startDrain() {
  drainTimer = setInterval(() => {
    if (queueMs > 0) {
      queueMs = Math.max(0, queueMs - 10);
      if (starvingSince !== null) {
        starvations.push({startMs: starvingSince, durationMs: now() - starvingSince});
        starvingSince = null;
      }
    } else if (producing && starvingSince === null) {
      starvingSince = now();
    }
  }, 10);
}

const chunks: number[] = [];
const emit = async (event: any) => {
  if (!(event instanceof Uint8Array)) {
    console.log(`  [event @${Date.now() - started}ms] ${JSON.stringify(event).slice(0, 100)}`);
    return;
  }
  const ms = event.length / 32;
  chunks.push(ms);
  queueMs += ms;
  if (starvingSince !== null) {
    starvations.push({startMs: starvingSince, durationMs: now() - starvingSince});
    starvingSince = null;
  }
  /* 队列满 = 固件的背压：设备按真实时间消费，服务端只能等。 */
  while (queueMs > CAPACITY_MS) {
    await new Promise(resolve => setTimeout(resolve, 10));
    queueMs = Math.max(0, queueMs - 10);
  }
};

const REPLY = '好嘞，我在的。你说啥子嘛？我这边听着呢，慢慢说就是了。';
const deps = {
  transcribe: async () => '你好呀',
  synthesize: (text: string, onChunk: (c: Uint8Array) => void | Promise<void>) => synthesize(settings, text, onChunk).then(() => undefined),
  complete: async (_messages: any[], onDelta: (d: string) => void) => {
    /* 假模型：按句子节奏吐字，模拟真实模型"边说边生成"。 */
    for (const piece of ['好嘞，我在的。', '你说啥子嘛？', '我这边听着呢，慢慢说就是了。']) {
      onDelta(piece);
      await new Promise(resolve => setTimeout(resolve, 250));
    }
    return REPLY;
  },
};

startDrain();
const result = await runVoiceTurn(deps as any, [], new Uint8Array(3200), emit);
producing = false;
if (drainTimer) clearInterval(drainTimer);
if (starvingSince !== null) {
  starvations.push({startMs: starvingSince, durationMs: now() - starvingSince});
  starvingSince = null;
}

const audioMs = chunks.reduce((s, v) => s + v, 0);
console.log(`\n=== 设备队列上限 ${CAPACITY_MS} ms ===`);
console.log(`音频总长 ${Math.round(audioMs)} ms，TTS 片数 ${chunks.length}，出流墙钟 ${result.timings.totalMs} ms`);
const totalStarved = starvations.reduce((s, v) => s + v.durationMs, 0);
console.log(`饿死（听得见的卡顿）次数 ${starvations.length}，合计 ${Math.round(totalStarved)} ms`);
for (const s of starvations) console.log(`   @${Math.round(s.startMs)}ms 持续 ${Math.round(s.durationMs)} ms`);
console.log('timings:', JSON.stringify(result.timings));
