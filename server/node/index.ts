/*
 * CloudBase（云托管 / Cloud Run）容器入口。
 *
 * 与 Cloudflare Worker 版共用同一份业务代码（server/），差别只有三处平台能力：
 *   1. 数据库：Workers 的 D1 -> node:sqlite（server/node/d1-sqlite.ts 把它包成 D1 接口）；
 *   2. 语音识别：Worker 靠 "fetch + Upgrade 头" 拿 WebSocket，Node 没这套语义 -> ws（server/node/asr-ws.ts）；
 *   3. 静态资源：Worker 的 ASSETS 绑定 -> 这里直接从 dist/ 读文件，SPA 回落到 index.html。
 *
 * 这样"迁移到国内托管"不用动任何路由、SQL 或前端代码，也就不会把已经验收过的功能改坏。
 */
import {existsSync, mkdirSync, readdirSync, readFileSync, statSync} from 'node:fs';
import {dirname, extname, join, resolve, sep} from 'node:path';
import {fileURLToPath} from 'node:url';
import {DatabaseSync} from 'node:sqlite';
import {serve} from '@hono/node-server';
import {app} from '../index';
import {setTranscriber} from '../voice/volc';
import {createD1} from './d1-sqlite';
import {transcribeViaWs} from './asr-ws';

const here = dirname(fileURLToPath(import.meta.url));

/** 容器里工作目录是 /app，本机是仓库根目录；两处都试一遍，少配置一个环境变量。 */
function locate(candidates: string[]) {
  for (const candidate of candidates) {
    if (candidate && existsSync(candidate)) return candidate;
  }
  return null;
}

const staticRoot =
  locate([process.env.STATIC_DIR || '', join(process.cwd(), 'dist'), join(here, '../dist'), join(here, '../../dist')]) ??
  join(process.cwd(), 'dist');
const migrationsRoot =
  locate([
    process.env.MIGRATIONS_DIR || '',
    join(process.cwd(), 'migrations'),
    join(here, '../migrations'),
    join(here, '../../migrations'),
  ]) ?? join(process.cwd(), 'migrations');

/*
 * 迁移只在启动时跑一次，记在 _migrations 里；ALTER TABLE 这类语句重复执行会报错，
 * 所以"跑过就不跑"必须由这张表保证，不能靠 SQL 自己幂等。
 */
function applyMigrations(db: DatabaseSync) {
  db.exec('CREATE TABLE IF NOT EXISTS _migrations (name TEXT PRIMARY KEY, applied_at TEXT NOT NULL)');
  const applied = new Set(
    (db.prepare('SELECT name FROM _migrations').all() as Array<{name: string}>).map(row => row.name),
  );
  for (const file of readdirSync(migrationsRoot).filter(name => name.endsWith('.sql')).sort()) {
    if (applied.has(file)) continue;
    const sql = readFileSync(join(migrationsRoot, file), 'utf8');
    db.exec('BEGIN');
    try {
      db.exec(sql);
      db.prepare('INSERT INTO _migrations VALUES (?, ?)').run(file, new Date().toISOString());
      db.exec('COMMIT');
    } catch (error) {
      db.exec('ROLLBACK');
      throw error;
    }
    console.log(`migration applied: ${file}`);
  }
}

function openDatabase() {
  const path = process.env.DB_PATH || join(process.cwd(), 'data', 'aidesk.sqlite');
  if (path !== ':memory:') mkdirSync(dirname(path), {recursive: true});
  const db = new DatabaseSync(path);
  db.exec('PRAGMA foreign_keys = ON');
  db.exec('PRAGMA busy_timeout = 5000');
  applyMigrations(db);
  console.log(`database ready: ${path}`);
  return createD1(db);
}

function buildEnv() {
  const encryptionKey = process.env.ENCRYPTION_KEY || '';
  if (!encryptionKey) {
    /* 缺密钥时每个 /api 都会 503，不如直接说清楚为什么起不来。 */
    console.error('ENCRYPTION_KEY 未配置：请把 Cloudflare 上那个同一个密钥设置为容器环境变量。');
    process.exit(1);
  }
  return {
    DB: openDatabase(),
    ENCRYPTION_KEY: encryptionKey,
    APP_ENV: process.env.APP_ENV || 'production',
    DEV_ALLOWED_ORIGINS: process.env.DEV_ALLOWED_ORIGINS,
    PROVIDER_ALLOWED_HOSTS: process.env.PROVIDER_ALLOWED_HOSTS,
    VOLC_API_KEY: process.env.VOLC_API_KEY,
    VOLC_ASR_RESOURCE: process.env.VOLC_ASR_RESOURCE,
    VOLC_TTS_RESOURCE: process.env.VOLC_TTS_RESOURCE,
    VOLC_TTS_SPEAKER: process.env.VOLC_TTS_SPEAKER,
    /* 静态资源在这里直接读盘；Worker 版的 ASSETS 绑定在 Node 里没有对应物，业务代码也没用它。 */
    ASSETS: {},
  } as any;
}

const MIME: Record<string, string> = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.ico': 'image/x-icon',
  '.webp': 'image/webp',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.txt': 'text/plain; charset=utf-8',
  '.webmanifest': 'application/manifest+json',
};

/*
 * 静态资源。
 * 有扩展名却找不到 -> 返回 null 交给 Hono 走 404（否则一个拼错的文件名会拿到 index.html 的 HTML，
 * 浏览器会以 "Unexpected token '<'" 报错，掩盖真正的原因）；
 * 没有扩展名 -> 前端路由，回落 index.html。
 */
function staticResponse(pathname: string): Response | null {
  let decoded: string;
  try {
    decoded = decodeURIComponent(pathname);
  } catch {
    return null;
  }
  if (decoded.includes('..') || decoded.includes('\0')) return null;
  let file = join(staticRoot, decoded);
  if (!file.startsWith(staticRoot)) return null;
  if (!existsSync(file) || statSync(file).isDirectory()) {
    if (extname(decoded)) return null;
    file = join(staticRoot, 'index.html');
    if (!existsSync(file)) return null;
  }
  const ext = extname(file).toLowerCase();
  const headers: Record<string, string> = {
    'Content-Type': MIME[ext] ?? 'application/octet-stream',
    'X-Content-Type-Options': 'nosniff',
  };
  if (ext === '.html') headers['Cache-Control'] = 'no-store';
  else if (file.includes(`${sep}assets${sep}`)) headers['Cache-Control'] = 'public, max-age=31536000, immutable';
  else headers['Cache-Control'] = 'no-cache';
  return new Response(readFileSync(file), {headers});
}

const env = buildEnv();
/* 必须在处理第一个请求之前注入，之后 /api/device/voice 才走 Node 版识别。 */
setTranscriber(transcribeViaWs);

const fetch = (request: Request): Response | Promise<Response> => {
  const url = new URL(request.url);
  if (url.pathname !== '/api' && !url.pathname.startsWith('/api/')) {
    const hit = staticResponse(url.pathname);
    if (hit) return hit;
  }
  /*
   * 限流按来源 IP 分桶，Workers 里读的是 CF-Connecting-IP。容器只经平台网关暴露，
   * 这里把它补上（不写日志、不入库，只用于按 IP 限流）。
   */
  const forwarded = request.headers.get('x-forwarded-for');
  if (forwarded && !request.headers.get('cf-connecting-ip')) {
    const headers = new Headers(request.headers);
    headers.set('cf-connecting-ip', forwarded.split(',')[0].trim());
    return app.fetch(new Request(request, {headers}), env);
  }
  return app.fetch(request, env);
};

const port = Number(process.env.PORT || 80);
serve({fetch, port, hostname: '0.0.0.0'}, info => {
  console.log(`aidesk console listening on http://0.0.0.0:${info.port} (static: ${staticRoot}, migrations: ${migrationsRoot})`);
});