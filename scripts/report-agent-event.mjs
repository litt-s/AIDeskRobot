/*
 * 把"编程 agent 的状态"上报给 AIDesk（T042 第一期：只通知，不批准）。
 *
 * 给 Codex 的 hook 调用，例如：
 *   node scripts/report-agent-event.mjs --kind needs_confirmation --source Codex --title "要执行 rm -rf dist"
 *   node scripts/report-agent-event.mjs --kind finished --source Codex --title "改完 3 个文件"
 *   node scripts/report-agent-event.mjs --kind failed --source Codex --detail "npm test 失败：3 个用例"
 *
 * 凭据（二选一，都不入库）：
 *   环境变量 AIDESK_AGENT_KEY
 *   文件 data/agent-key.txt
 *
 * 默认**不阻断** agent：出错只打一行警告并退出 0（hook 里最怕因为通知失败把任务搞挂）。
 * 想让它返回非 0（调试用）加 --strict。
 */
import {existsSync, mkdirSync, readFileSync, writeFileSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

/*
 * 凭据文件固定在「仓库根目录/data/agent-key.txt」，**不跟随当前工作目录**：
 * hook 的工作目录是不确定的，用相对路径会时灵时不灵。要自定义就设 AIDESK_AGENT_KEY_FILE。
 */
const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const keyFile = process.env.AIDESK_AGENT_KEY_FILE || resolve(repoRoot, 'data', 'agent-key.txt');
const args = process.argv.slice(2);
const value = name => {
  const index = args.indexOf(`--${name}`);
  return index >= 0 && args[index + 1] ? args[index + 1] : '';
};
const strict = args.includes('--strict');
const base = (value('url') || process.env.AIDESK_EVENT_URL || 'https://aidesk-console-312359-12-1485927190.sh.run.tcloudbase.com').replace(/\/+$/, '');
const key = () => {
  if (process.env.AIDESK_AGENT_KEY) return process.env.AIDESK_AGENT_KEY.trim();
  try {
    return readFileSync(keyFile, 'utf8').trim();
  } catch {
    return '';
  }
};

const warning = message => {
  console.warn(`[aidesk] ${message}`);
  process.exit(strict ? 1 : 0);
};

const saveKey = value('save-key');
if (saveKey) {
  mkdirSync(dirname(keyFile), { recursive: true });
  writeFileSync(keyFile, `${saveKey.trim()}\n`, { encoding: 'utf8', mode: 0o600 });
  console.log(`[aidesk] 凭据已写入：${keyFile}`);
}
const agentKey = key();
if (args.includes('--check')) {
  console.log(`[aidesk] 凭据来源：${process.env.AIDESK_AGENT_KEY ? '环境变量 AIDESK_AGENT_KEY' : `文件 ${keyFile}（${existsSync(keyFile) ? '存在' : '不存在'}）`}`);
  console.log(`[aidesk] 凭据：${agentKey ? `已读到（长度 ${agentKey.length}，尾号 ${agentKey.slice(-4)}）` : '没读到'}`);
  console.log(`[aidesk] 上报地址：${base}/api/agent/events`);
  process.exit(agentKey ? 0 : 1);
}
if (!agentKey) warning(`没有上报凭据：设置 AIDESK_AGENT_KEY，或先跑 --save-key <凭据>（文件位置：${keyFile}）`);
const kind = value('kind');
if (!['needs_confirmation', 'failed', 'finished'].includes(kind)) warning(`--kind 必须是 needs_confirmation / failed / finished（收到 "${kind}"）`);
const body = {
  source: value('source') || 'Codex',
  kind,
  title: value('title').slice(0, 200),
  detail: value('detail').slice(0, 1000),
};

try {
  const response = await fetch(`${base}/api/agent/events`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', Authorization: `Agent ${agentKey}`},
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(5000),
  });
  const text = await response.text();
  if (!response.ok) warning(`上报失败：HTTP ${response.status} ${text.slice(0, 160)}`);
  console.log(`[aidesk] 已上报：${text.slice(0, 160)}`);
} catch (error) {
  warning(`上报出错：${error instanceof Error ? error.message : String(error)}`);
}
