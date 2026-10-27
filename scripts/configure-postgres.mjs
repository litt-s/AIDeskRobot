/* Run locally after migration; credentials and rollback configuration stay in data/. */
import {existsSync, readFileSync, writeFileSync} from 'node:fs';
import {spawnSync} from 'node:child_process';

const envId = JSON.parse(readFileSync('cloudbaserc.json', 'utf8')).envId;
const cli = process.env.TCB_CLI_JS || 'C:/Users/15163/AppData/Roaming/npm/node_modules/@cloudbase/cli/dist/standalone/cli.js';
function call(args) {
  const response = spawnSync(process.execPath, [cli, ...args], {encoding: 'utf8', maxBuffer: 4*1024*1024});
  if (response.status !== 0) throw new Error('CloudBase 配置命令失败；未回显可能含凭据的输出');
  // Generic `tcb api --json` can prepend an informational line.
  const start = response.stdout.indexOf('{');
  if (start < 0) throw new Error('CloudBase 未返回 JSON；请先查询当前配置，不要盲目重试');
  try {return JSON.parse(response.stdout.slice(start)).data;} catch {throw new Error('CloudBase 响应解析失败；请先查询当前配置，不要盲目重试');}
}
const before = call(['cloudrun', 'detail', '-e', envId, '-s', 'aidesk-console', '--json']);
if (!existsSync('data/postgres-cloudrun-config-backup.json')) writeFileSync('data/postgres-cloudrun-config-backup.json', JSON.stringify(before, null, 2));
const env = typeof before.ServerConfig.EnvParams === 'string' ? JSON.parse(before.ServerConfig.EnvParams) : before.ServerConfig.EnvParams;
if (!env?.ENCRYPTION_KEY) throw new Error('缺少既有加密密钥，拒绝切换');
const key = process.env.CLOUDBASE_DB_API_KEY || readFileSync('data/postgres-api-key.txt', 'utf8').trim();
const merged = {...env, DATABASE_BACKEND: 'cloudbase-pg', CLOUDBASE_ENV_ID: envId, CLOUDBASE_DB_API_KEY: key};
if (!process.argv.includes('--apply')) {
  console.log('预览：保留既有环境变量，追加 DATABASE_BACKEND/CLOUDBASE_ENV_ID/CLOUDBASE_DB_API_KEY；使用 --apply 执行');
} else {
  const body = {EnvId: envId, ServerName: 'aidesk-console', Items: [{Key: 'EnvParam', Value: JSON.stringify(merged)}]};
  call(['api', 'tcbr', 'SubmitServerConfigChangeDiff', '--api-version', '2022-02-17', '--body', JSON.stringify(body), '--json']);
  console.log('服务端 PostgreSQL 配置已提交，既有加密密钥已保留（密钥隐藏）');
}
