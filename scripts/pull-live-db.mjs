/*
 * 把"线上库"拉回本机（云托管容器没有持久盘，本机这份就是唯一的持久载体）。
 *
 * 用法：
 *   ADMIN_TOKEN=<service 上的那个令牌> node scripts/pull-live-db.mjs
 *   ADMIN_TOKEN=… LIVE_URL=https://… node scripts/pull-live-db.mjs
 *
 * 拿到的是服务端 VACUUM INTO 出来的一致副本，落到 data/live.sqlite。
 * 接着用 scripts/export-seed.mjs 从它生成 seed/seed.sql（部署时带上），
 * 控制台里的改动就跟着下一次发布一起活下去。
 */
import {writeFileSync, statSync} from 'node:fs';

const url = (process.env.LIVE_URL || 'https://aidesk-console-312359-12-1485927190.sh.run.tcloudbase.com')
  .replace(/\/+$/, '');
const token = process.env.ADMIN_TOKEN || '';
if (!token) {
  console.error('缺少 ADMIN_TOKEN（云托管服务的环境变量里那个值）');
  process.exit(1);
}
const output = process.env.LIVE_DB || 'data/live.sqlite';

const started = Date.now();
const response = await fetch(`${url}/api/admin/snapshot`, {
  headers: {Authorization: `Bearer ${token}`},
});
if (!response.ok) {
  console.error(`拉取失败：HTTP ${response.status} ${(await response.text()).slice(0, 160)}`);
  process.exit(1);
}
const bytes = new Uint8Array(await response.arrayBuffer());
/* SQLite 文件头：任何不是库的响应都当场拦住，免得把错误页写成"数据库"。 */
const magic = new TextDecoder().decode(bytes.subarray(0, 15));
if (magic !== 'SQLite format 3') {
  console.error(`拉回来的不是 SQLite 文件（前 15 字节：${magic}）`);
  process.exit(1);
}
writeFileSync(output, bytes);
console.log(`已写入 ${output}：${bytes.length} 字节（${Date.now() - started} ms），${statSync(output).mtime.toISOString()}`);
console.log(`下一步：D1_FILE=${output} node scripts/export-seed.mjs`);
