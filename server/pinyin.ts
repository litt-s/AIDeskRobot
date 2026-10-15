import {PINYIN_BASE, PINYIN_INDEX, WAKE_SYLLABLES} from './pinyin-data';

/*
 * 唤醒词的"人写的"形式 → 设备能吃的拼音音节串。
 *
 * 设备侧的 MultiNet7 中文模型只接受拼音音节（空格分隔、不带声调）——汉字会被模型
 * 自己拒绝（MN_COMMAND: invalid command），2026-10-15 真机实测。所以这里做转换，
 * 并且用设备模型自带的 405 个音节表提前校验：能在控制台拦下的错误，不要留给设备。
 */

const SYLLABLES = new Set(WAKE_SYLLABLES);
const MIN_SYLLABLES = 2;
const MAX_SYLLABLES = 8;

export type WakePhrase = {ok: true; phrase: string; label: string} | {ok: false; error: string};

function validate(syllables: string[], label: string): WakePhrase {
  if (syllables.length < MIN_SYLLABLES) {
    return {ok: false, error: `唤醒词至少 ${MIN_SYLLABLES} 个音节，例如 你好小智`};
  }
  if (syllables.length > MAX_SYLLABLES) {
    return {ok: false, error: `唤醒词最多 ${MAX_SYLLABLES} 个音节，太长的短语容易误唤醒`};
  }
  for (const syllable of syllables) {
    if (!SYLLABLES.has(syllable)) {
      return {ok: false, error: `设备模型不认识音节「${syllable}」，换个词或直接写拼音`};
    }
  }
  return {ok: true, phrase: syllables.join(' '), label};
}

/*
 * 三种输入都收：
 *   - 纯拼音（ni hao xiao zhi）——直接校验音节
 *   - 纯中文（你好小智）——逐字查表，表里没有的字会指名报错
 *   - 中英混排 / 标点——报错，避免"看起来发了其实设备不认"
 */
export function toWakePhrase(input: string): WakePhrase {
  const text = (input ?? '').trim().replace(/\s+/g, ' ');
  if (!text) {
    return {ok: false, error: '唤醒词不能为空'};
  }
  if (/^[A-Za-z ]+$/.test(text)) {
    return validate(text.toLowerCase().split(' '), text);
  }
  const syllables: string[] = [];
  for (const char of text) {
    if (char === ' ') {
      continue;
    }
    const offset = char.codePointAt(0)! - PINYIN_BASE;
    const slot = offset >= 0 && (offset + 1) * 2 <= PINYIN_INDEX.length
      ? PINYIN_INDEX.slice(offset * 2, offset * 2 + 2)
      : '--';
    if (slot === '--') {
      return {ok: false, error: `「${char}」不在拼音表里，请换一个字或直接写拼音`};
    }
    syllables.push(WAKE_SYLLABLES[parseInt(slot, 36)]);
  }
  return validate(syllables, text);
}
