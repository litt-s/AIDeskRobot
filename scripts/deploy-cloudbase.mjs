/*
 * 一条命令完成"部署到云托管"：先拉回线上库 → 重建种子 → 打包源码 → 部署。
 *
 * 为什么要有这个包装（而不是直接 tcb cloudrun deploy）：
 *   容器没有持久盘，库里有什么完全取决于代码包里那份 seed/seed.sql。
 *   如果先拉回线上库、再重建种子，那么"控制台里刚做的改动"会跟着这次发布一起过去，
 *   丢数据的窗口从"上次拉取到现在"缩短到"开始打包到发布完成"（几分钟）。
 *
 * 用法：node scripts/deploy-cloudbase.mjs [--no-pull] [--no-seed]
 *   拉取失败（服务没起、令牌不对）不算致命：会用现有种子继续部署，只打一行警告。
 */
import {execFileSync, spawnSync} from 'node:child_process';
import {copyFileSync, existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync} from 'node:fs';
import {join, resolve, sep} from 'node:path';

const SERVICE = process.env.SERVICE_NAME || 'aidesk-console';
const STAGE = process.env.CLOUDBASE_STAGE || 'data/cr-deploy';
const SOURCES = ['package.json', 'package-lock.json', 'tsconfig.json', 'vite.config.ts', 'vitest.config.ts',
  'Dockerfile', '.dockerignore', '.gitattributes', 'cloudbaserc.json', 'server', 'web', 'shared', 'migrations',
  'types', 'seed', 'scripts', 'cloudbase', '.gitignore'];
const CLI_JS = process.env.TCB_CLI_JS || 'C:/Users/15163/AppData/Roaming/npm/node_modules/@cloudbase/cli/dist/standalone/cli.js';
const args = process.argv.slice(2);
const postgres = args.includes('--postgres') || process.env.DATABASE_BACKEND === 'cloudbase-pg';
const envId = JSON.parse(readFileSync('cloudbaserc.json', 'utf8')).envId;
const skipPull = args.includes('--no-pull');
const skipSeed = args.includes('--no-seed');

const step = message => console.log(`\n=== ${message} ===`);

if (!postgres && !skipPull) {
  step('1/4 拉回线上库（失败不致命）');
  const pull = spawnSync(process.execPath, ['scripts/pull-live-db.mjs'], {stdio: 'inherit'});
  if (pull.status !== 0) console.warn('⚠️ 拉取失败：本次部署将使用现有 seed/seed.sql');
}

if (!postgres && !skipSeed && existsSync('data/live.sqlite')) {
  step('2/4 用拉回的库重建种子');
  execFileSync(process.execPath, ['scripts/export-seed.mjs'], {
    stdio: 'inherit',
    env: {...process.env, D1_FILE: 'data/live.sqlite'},
  });
}

step('3/4 打包源码');
const stagePath = resolve(STAGE);
const allowedRoot = resolve('data') + sep;
if (!stagePath.startsWith(allowedRoot)) throw new Error('部署暂存目录必须位于当前工程 data/ 内');
if (existsSync(STAGE)) rmSync(STAGE, {recursive: true, force: true});
mkdirSync(STAGE, {recursive: true});
const zip = 'data/stage-src.zip';
rmSync(zip, {force: true});
execFileSync('git', ['archive', '--format=zip', `--output=${zip}`, 'HEAD', '--', ...SOURCES], {stdio: 'inherit'});
/* Windows 自带的 tar 就能解 zip，避免依赖 PowerShell 的 Expand-Archive。 */
execFileSync('tar', ['-xf', zip, '-C', STAGE], {stdio: 'inherit'});
rmSync(zip, {force: true});
if (!postgres && existsSync('seed/seed.sql')) copyFileSync('seed/seed.sql', join(STAGE, 'seed', 'seed.sql'));
const seedBytes = existsSync(join(STAGE, 'seed', 'seed.sql')) ? statSync(join(STAGE, 'seed', 'seed.sql')).size : 0;
console.log(`打包完成：${readdirSync(STAGE).length} 个顶层条目，seed/seed.sql ${seedBytes} 字节`);

step('4/4 部署到云托管');
console.log('提示：CLI 会问 "Enable gray deployment?"，选 No（直接回车）即全量切换。');
const deploy = spawnSync(process.execPath, [CLI_JS, 'cloudrun', 'deploy', '-e', envId, '-s', SERVICE, '--port', '80',
  '--source', STAGE, '--open-access-types', 'PUBLIC', '--wait', ...(args.includes('--force') ? ['--force'] : [])], {stdio: 'inherit'});
process.exit(deploy.status ?? 1);
