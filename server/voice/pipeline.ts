/*
 * 一轮语音对话的编排：识别 → 大模型 → 合成，边出边发。
 *
 * 关键取舍是**不等整句回复生成完再合成**：模型每吐够一句就立刻送去 TTS，
 * 于是第 1 句在播的时候第 2 句还在生成，第 3 句还在合成。
 * 首字延迟因此大约是「识别 + 模型首句 + TTS 首块」，而不是三者之和。
 *
 * 依赖以接口注入，测试用假实现即可覆盖分句、流水线与失败分支，不必联网。
 */

export interface ChatMessage {
  role: 'system' | 'user' | 'assistant';
  content: string;
}

export interface VoiceDeps {
  /** 把 16 kHz 单声道 PCM 转写成文字。 */
  transcribe(pcm: Uint8Array): Promise<string>;
  /** 边合成边回调 PCM 片，回调返回前不要继续（保证音频顺序）。 */
  synthesize(text: string, onChunk: (chunk: Uint8Array) => void | Promise<void>): Promise<void>;
  /** 流式请求模型；每个增量同步交给 onDelta，返回完整回复。 */
  complete(messages: ChatMessage[], onDelta: (delta: string) => void): Promise<string>;
}

export type VoiceEvent =
  | { type: 'asr'; text: string }
  | { type: 'reply'; text: string }
  | { type: 'error'; message: string }
  | { type: 'done' };

/** 事件先于音频、错误随时可达，由调用方按顺序写进分帧流。 */
export type Emit = (event: VoiceEvent | Uint8Array) => void | Promise<void>;

const STRONG_END = /[。！？!?；;\n]/;
const WEAK_END = /[，,、：:—…]/;
const HARD_LIMIT = 120;
const SOFT_LIMIT = 48;

/**
 * 增量分句：模型是逐字吐的，所以要能对"半句话"做判断。
 * 强标点即断；没有强标点时，超长句子在弱标点或硬上限处断开，
 * 否则一句很长的话会把首字延迟拖到整段生成完。
 */
export class SentenceSplitter {
  private buffer = '';

  push(delta: string): string[] {
    this.buffer += delta;
    const out: string[] = [];
    for (;;) {
      const sentence = this.take();
      if (sentence === null) break;
      out.push(sentence);
    }
    return out;
  }

  /** 流结束：把剩下的半句也交出去（哪怕没有收尾标点）。 */
  flush(): string[] {
    const rest = this.buffer.trim();
    this.buffer = '';
    return rest ? [rest] : [];
  }

  private take(): string | null {
    if (!this.buffer.trim()) {
      if (/^\s+$/.test(this.buffer)) this.buffer = '';
      return null;
    }
    const strong = this.buffer.search(STRONG_END);
    if (strong >= 0) return this.cut(strong + 1);
    if (this.buffer.length >= SOFT_LIMIT) {
      const weak = this.buffer.slice(0, HARD_LIMIT).search(WEAK_END);
      if (weak >= Math.floor(SOFT_LIMIT / 2)) return this.cut(weak + 1);
      if (this.buffer.length >= HARD_LIMIT) return this.cut(HARD_LIMIT);
    }
    return null;
  }

  private cut(at: number): string | null {
    const sentence = this.buffer.slice(0, at).trim();
    this.buffer = this.buffer.slice(at);
    return sentence || null;
  }
}

/**
 * 送去合成前把不该念出来的符号去掉：模型常回复 markdown，
 * 直接朗读会把星号、井号、代码块原样念出来。
 */
export function plainTextForSpeech(text: string): string {
  return text
    .replace(/```[\s\S]*?```/g, ' 代码省略 ')
    .replace(/`([^`]*)`/g, '$1')
    .replace(/!?\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/^\s{0,3}#{1,6}\s*/gm, '')
    .replace(/^\s{0,3}[-*+]\s+/gm, '')
    .replace(/[*_~#>|]/g, '')
    .replace(/\s+/g, ' ')
    .trim();
}

export interface VoiceTurnResult {
  transcript: string;
  reply: string;
  spoken: number;
  audioBytes: number;
  /*
   * 各段耗时（毫秒）。存在是为了回答"机器人等得久到底久在哪一段"，而不是靠猜：
   * asr=识别（含设备上传与服务端等待）、firstSentence=模型吐出第一句、
   * firstAudio=第一块音频发往设备、model=模型出完、total=整轮。
   */
  timings: { asrMs: number; firstSentenceMs: number; firstAudioMs: number; modelMs: number; totalMs: number };
  /**
   * Set whenever the turn produced no answer. The caller persists it, because
   * otherwise a failed turn is silent everywhere the user can see: the robot
   * simply never speaks and the console shows nothing to act on.
   */
  failure: string | null;
}

export const EMPTY_TRANSCRIPT = '没有听清，请再说一遍。';

/**
 * 给一步操作套上墙钟上限。
 * 上游的 AbortSignal 只能中断"愿意被中断"的请求：实测向外部语音服务发起 WebSocket
 * 握手时，超时信号无法打断那次 fetch。机器人等不起，所以这里用竞速兜住，
 * 宁可让孤立的那次请求自己在后台烂掉，也要让设备拿到一个明确的失败。
 */
export function withDeadline<T>(operation: Promise<T>, ms: number, label: string): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${label}超时（${Math.round(ms / 1000)} 秒）`)), ms);
    operation.then(
      (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      (error) => {
        clearTimeout(timer);
        reject(error);
      },
    );
  });
}

const ASR_DEADLINE_MS = 25000;
export const SPEAK_DEADLINE_MS = 30000;
const MODEL_DEADLINE_MS = 120000;

/**
 * 跑完一轮。任何一步失败都发 error 事件而不是抛出，
 * 因为响应已经开始写了，中断流会让设备只看到连接断开、拿不到原因。
 */
export async function runVoiceTurn(
  deps: VoiceDeps,
  history: ChatMessage[],
  pcm: Uint8Array,
  emit: Emit,
): Promise<VoiceTurnResult> {
  const started = Date.now();
  const result: VoiceTurnResult = { transcript: '', reply: '', spoken: 0, audioBytes: 0, failure: null,
    timings: { asrMs: 0, firstSentenceMs: 0, firstAudioMs: 0, modelMs: 0, totalMs: 0 } };

  let transcript = '';
  try {
    transcript = (await withDeadline(deps.transcribe(pcm), ASR_DEADLINE_MS, '语音识别')).trim();
  } catch (error) {
    result.failure = `语音识别失败：${message(error)}`;
    await emit({ type: 'error', message: result.failure });
    await emit({ type: 'done' });
    return result;
  }
  result.transcript = transcript;
  result.timings.asrMs = Date.now() - started;
  await emit({ type: 'asr', text: transcript });
  if (!transcript) {
    result.failure = EMPTY_TRANSCRIPT;
    await emit({ type: 'error', message: EMPTY_TRANSCRIPT });
    await emit({ type: 'done' });
    return result;
  }

  const splitter = new SentenceSplitter();
  /* 合成分句串成一条链：模型继续生成的同时，TTS 一句接一句跑。
   * 串行保证音频片按句子顺序发出，不会两段交错。 */
  let chain: Promise<void> = Promise.resolve();
  const speak = (sentence: string) => {
    const spoken = plainTextForSpeech(sentence);
    if (!spoken) return;
    result.spoken += 1;
    if (!result.timings.firstSentenceMs) {
      result.timings.firstSentenceMs = Date.now() - started;
    }
    chain = chain.then(async () => {
      await withDeadline(deps.synthesize(spoken, async (chunk) => {
        result.audioBytes += chunk.length;
        if (!result.timings.firstAudioMs) {
          result.timings.firstAudioMs = Date.now() - started;
        }
        await emit(chunk);
      }), SPEAK_DEADLINE_MS, '语音合成');
    }).catch(async (error) => {
      result.failure ??= `语音合成失败：${message(error)}`;
      await emit({ type: 'error', message: `语音合成失败：${message(error)}` });
    });
  };

  try {
    result.reply = await withDeadline(deps.complete([...history, { role: 'user', content: transcript }], (delta) => {
      for (const sentence of splitter.push(delta)) speak(sentence);
    }), MODEL_DEADLINE_MS, '模型请求');
  } catch (error) {
    result.failure = `模型请求失败：${message(error)}`;
    await emit({ type: 'error', message: result.failure });
    for (const sentence of splitter.flush()) speak(sentence);
    await chain;
    await emit({ type: 'done' });
    return result;
  }

  /*
   * 模型已经出完，回复文本此刻就能发——它原来放在最后（等所有句子合成并播完），
   * 结果是设备日志里的 `answer:` 时间戳看着像"模型很慢"，其实是"模型 + 全部合成"。
   * 下游没有任何东西依赖它最后到达，所以提前发，日志才可用于定位延迟。
   */
  result.timings.modelMs = Date.now() - started;
  if (result.reply.trim()) await emit({ type: 'reply', text: result.reply.trim() });

  for (const sentence of splitter.flush()) speak(sentence);
  await chain;
  /* Synthesis failures are reported as they happen, but they are also the only
   * thing that went wrong when the model answered and no audio came out. */
  if (!result.audioBytes) result.failure ??= '语音合成失败：没有生成任何音频';
  result.timings.totalMs = Date.now() - started;
  await emit({ type: 'done' });
  return result;
}

function message(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
