/*
 * Codex hook → AIDesk 提醒（T042 第一期收尾）。
 *
 * 干什么：把 Codex 里"需要你批准"和"执行失败"这两件事，交给
 * scripts/report-agent-event.mjs 上报给 AIDesk —— 机器人念一句（只说软件名 + 状态），
 * 手机弹通知（标题 / 内容 / 批准选项都给全），控制台留一条记录。
 *
 * 为什么只挂这两个事件：
 *   - 需要批准：不在电脑前的人最需要知道的就是"有活儿在等我批"。命中危险命令时，
 *     本 hook 返回 permission:"ask"（让 Codex 照常弹它的确认框），同时把这件事报出去。
 *   - 失败：任务中断是第二类"必须让人知道"的状态。
 *   - 任务完成（stop 事件）默认**不报**：它每轮回复结束都会触发，会变成"每聊一句念一句"。
 *     想开就把下面的 REPORT_FINISHED 改成 true。
 *
 * 三条铁律（hook 出问题绝不能影响 Codex 本身）：
 *   1. 任何情况下都 exit 0，异常一律吞掉；
 *   2. **只有命中危险命令时才往 stdout 写 JSON**（返回 permission:"ask"），其余一律不输出 ——
 *      不输出就是不改变 Codex 原有的判定。绝不能图省事返回 "allow"：那等于把我们没判断过的
 *      命令统统放行，比不做这个 hook 还危险；
 *   3. 上报走后台进程（detached + unref），不拖慢 Codex 的每一步。
 *
 * 手动试（不会真的发通知）：`--dry-run` 只打印它打算报什么。
 *   echo '{"command":"rm -rf dist"}' | node .cursor/hooks/report-to-aidesk.mjs --event shell --dry-run
 */
import {spawn} from 'node:child_process';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

/** 改成 true：每轮回复结束也报一次"任务完成"（会有点吵）。 */
const REPORT_FINISHED = false;
/** 手机/控制台显示的批准选项；和 Codex 弹窗里的按钮不一致时改这里（或设环境变量）。 */
const APPROVAL_OPTIONS = process.env.AIDESK_APPROVAL_OPTIONS || '允许一次|本次会话都允许|拒绝';

/* 命中这些才拦下来问一句。宁可少拦，别把日常命令变成审批地狱；想加就加一行。 */
const RISKY = [
  /\brm\s+-[a-zA-Z]*r[a-zA-Z]*f|\brm\s+-[a-zA-Z]*f[a-zA-Z]*r/,
  /\bgit\s+push\b[^\n]*--force/,
  /\bgit\s+reset\s+--hard\b/,
  /\bgit\s+clean\s+-[a-z]*f/,
  /\bnpm\s+publish\b/,
  /\bgh\s+pr\s+merge\b/,
  /\b(?:curl|wget)\b[^|]*\|\s*(?:ba)?sh\b/,
  /\bsudo\b/,
  /\bchmod\s+[-+]?777\b/,
  /\b(?:DROP|TRUNCATE)\s+TABLE\b/i,
  /deploy-cloudbase\.mjs/,
];

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '..', '..');
const args = process.argv.slice(2);
const value = name => {
  const index = args.indexOf(`--${name}`);
  return index >= 0 && args[index + 1] ? args[index + 1] : '';
};
const dryRun = args.includes('--dry-run');

const firstString = (...values) =>
  values.find(item => typeof item === 'string' && item.trim()) || '';
const clip = (text, max) => (text.length > max ? `${text.slice(0, max - 1)}…` : text);

async function readInput() {
  const chunks = [];
  try {
    for await (const chunk of process.stdin) chunks.push(chunk);
  } catch {
    /* 没有 stdin（手动跑）就当空对象 */
  }
  const raw = Buffer.concat(chunks).toString('utf8').trim();
  if (!raw) return {};
  try {
    return JSON.parse(raw);
  } catch {
    return {};
  }
}

/** 交给上报脚本：默认后台跑，不阻塞 Codex。 */
function report(extraArgs) {
  const target = resolve(repoRoot, 'scripts', 'report-agent-event.mjs');
  if (dryRun) {
    /* 干跑时只往 stderr 说话：stdout 要留给"真正返回给 Codex 的 JSON"，别混在一起。 */
    console.error(`[dry-run] 会上报：node ${target} ${extraArgs.join(' ')}`);
    return;
  }
  try {
    const child = spawn(process.execPath, [target, ...extraArgs], {
      detached: true,
      stdio: 'ignore',
      windowsHide: true,
    });
    child.unref();
  } catch {
    /* 提醒失败不该影响 Codex */
  }
}

try {
  const event = value('event');
  const input = await readInput();

  if (event === 'shell') {
    const command = firstString(
      input.command,
      input.shell_command,
      input.tool_input?.command,
      input.arguments?.command,
      input.input?.command,
    );
    if (command && RISKY.some(pattern => pattern.test(command))) {
      report([
        '--kind', 'needs_confirmation',
        '--source', 'Codex',
        '--title', clip(command, 120),
        '--detail', `命中需要确认的命令${input.cwd ? `（目录：${input.cwd}）` : ''}`,
        '--options', APPROVAL_OPTIONS,
      ]);
      const answer = {
        permission: 'ask',
        user_message: `这条命令会改变现状，确认一下再执行：\n${command}`,
        agent_message: 'AIDesk 已把这次批准请求播报到机器人，并推送到手机。',
      };
      if (dryRun) {
        console.error('[dry-run] 会返回给 Codex：' + JSON.stringify(answer));
      } else {
        process.stdout.write(JSON.stringify(answer));
      }
    }
    /* 没命中：不输出任何东西 = 不改变 Codex 自己的判定。 */
  } else if (event === 'tool-failure') {
    const tool = firstString(input.tool_name, input.toolName, input.tool?.name, '工具');
    const reason = firstString(input.error, input.error_message, input.stderr, input.message);
    report([
      '--kind', 'failed',
      '--source', 'Codex',
      '--title', clip(`${tool} 执行失败`, 120),
      '--detail', clip(reason, 400),
    ]);
  } else if (event === 'stop') {
    if (REPORT_FINISHED) {
      report(['--kind', 'finished', '--source', 'Codex', '--title', '本轮回复结束']);
    }
  }
} catch {
  /* 一律吞掉：hook 崩了不能把 Codex 搞挂。 */
}

/* 用 exitCode 而不是 process.exit()：让 stdout 有机会冲刷完（写过 JSON 的那条路径尤其重要）。 */
process.exitCode = 0;
