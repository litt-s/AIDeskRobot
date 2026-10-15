/*
 * 重建部署用的"种子"（seed/seed.sql）。
 *
 * 为什么需要：云托管容器没有持久盘，每次发布/重启都会回到"空库 + 播种"。种子里有
 * 什么，决定了发布之后控制台和机器人还能不能用。它**不进仓库**（含口令哈希、设备
 * 密钥哈希与密文），只在本机生成、只进代码包。
 *
 * 用法：
 *   node scripts/export-seed.mjs                     # 默认导出有设备的那位用户
 *   SEED_USERNAME=li6679 node scripts/export-seed.mjs
 *   D1_FILE=<path.sqlite> node scripts/export-seed.mjs
 *
 * 除了导出开发库里的数据，还会**额外补一台"固件编译期默认设备码"的设备**：
 * 配网页设备码填的就是固件里的 CONFIG_ROBOT_CLOUD_KEY，而种子生成时它未必在那份
 * 开发库里，于是"重新部署后机器人 401"就成了必然。这里按 sdkconfig 里的值补上，
 * 保证任何一次部署之后那把码都能对上。
 */
import {readFileSync, readdirSync, statSync, writeFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {join} from 'node:path';
import {DatabaseSync} from 'node:sqlite';

const D1_DIR = '.wrangler/state/v3/d1/miniflare-D1DatabaseObject';
const SDKKCONFIG = 'Hardware/esp32/sdkconfig';
const OUTPUT = process.env.SEED_OUT || 'seed/seed.sql';

function hasWakeColumn(file) {
  try {
    const db = new DatabaseSync(file, {readOnly: true});
    const columns = db.prepare('PRAGMA table_info(devices)').all().map(row => row.name);
    db.close();
    return columns.includes('wake_label');
  } catch {
    return false;
  }
}

function pickD1File() {
  if (process.env.D1_FILE) return process.env.D1_FILE;
  const candidates = readdirSync(D1_DIR)
    .filter(name => name.endsWith('.sqlite') && name !== 'metadata.sqlite')
    .map(name => join(D1_DIR, name))
    .filter(hasWakeColumn)
    .sort((a, b) => statSync(b).mtimeMs - statSync(a).mtimeMs);
  if (!candidates.length) {
    throw new Error(`找不到带 wake_label 列的本地 D1 库，试试 D1_FILE=<path>.（在 ${D1_DIR} 下）`);
  }
  return candidates[0];
}

const d1File = pickD1File();
const db = new DatabaseSync(d1File, {readOnly: true});
console.log(`源库: ${d1File}`);

function pickUser() {
  const named = process.env.SEED_USERNAME;
  if (named) {
    const row = db.prepare('SELECT id, username FROM users WHERE username=?').get(named);
    if (!row) throw new Error(`用户名不存在: ${named}`);
    return row;
  }
  const owner = db.prepare('SELECT u.id, u.username FROM users u WHERE EXISTS (SELECT 1 FROM devices d WHERE d.user_id = u.id) ORDER BY u.created_at LIMIT 1').get();
  if (owner) return owner;
  const any = db.prepare('SELECT id, username FROM users ORDER BY created_at LIMIT 1').get();
  if (!any) throw new Error('源库里一个用户都没有');
  return any;
}

const user = pickUser();
console.log(`导出用户: ${user.username} (${user.id})`);

const quote = value => {
  if (value === null || value === undefined) return 'NULL';
  if (typeof value === 'number') return String(value);
  if (value instanceof Uint8Array) return `X'${Buffer.from(value).toString('hex')}'`;
  return `'${String(value).split("'").join("''")}'`;
};

/*
 * agent_keys / agent_events 必须在清单里：它们是"这台电脑能不能上报"和"事件历史"的全部依据，
 * 而容器每次发布都从种子重建库——漏了这两张表，发布一次就等于吊销一次 PC 上的上报凭据
 * （2026-10-16 实测：部署完成后上报立刻 401）。
 * device_commands 故意不带：那是"待下发"的队列，属于易失状态，重放旧指令比丢掉更糟。
 */
const tables = ['users', 'models', 'roles', 'conversations', 'messages', 'devices', 'voice_settings', 'agent_keys', 'agent_events'];
const lines = [
  '-- 由 scripts/export-seed.mjs 生成：把已有账号/设备/模型/语音凭据搬进空库。',
  '-- 不进仓库（含凭据哈希与密文）；发布代码包时带上即可。',
  'PRAGMA foreign_keys = OFF;',
];
const summary = [];
for (const table of tables) {
  const columns = db.prepare(`PRAGMA table_info(${table})`).all().map(row => row.name);
  /* users 的主键叫 id，其余表按 user_id 归属。 */
  const filter = table === 'users' ? 'id=?' : 'user_id=?';
  const rows = db.prepare(`SELECT ${columns.join(',')} FROM ${table} WHERE ${filter}`).all(user.id);
  for (const row of rows) {
    lines.push(`INSERT OR REPLACE INTO ${table} (${columns.join(',')}) VALUES (${columns.map(column => quote(row[column])).join(',')});`);
  }
  summary.push(`${table}=${rows.length}`);
}
console.log(`导出: ${summary.join(' ')}`);

/* 固件编译期默认设备码：按 sdkconfig 补一台设备，让"重新部署后 401"不再发生。 */
const deviceColumns = db.prepare('PRAGMA table_info(devices)').all().map(row => row.name);
const existingMasks = new Set(db.prepare('SELECT key_mask FROM devices WHERE user_id=?').all(user.id).map(row => row.key_mask));
db.close();

const sdkconfig = readFileSync(SDKKCONFIG, 'utf8');
const match = /^CONFIG_ROBOT_CLOUD_KEY="?([^"]*)"?$/m.exec(sdkconfig);
if (match && match[1].trim()) {
  const normalized = match[1].toUpperCase().replace(/[^0-9A-Z]/g, '').replace(/[IL]/g, '1').replace(/O/g, '0');
  const mask = `••••-••••-${normalized.slice(-4)}`;
  if (existingMasks.has(mask)) {
    console.log(`固件默认设备码已在种子里（${mask}），不重复添加`);
  } else {
    const hash = createHash('sha256').update(Buffer.from(normalized, 'utf8')).digest('base64');
    const now = new Date().toISOString();
    const id = 'c0ffee00-0000-4000-8000-0000000000f1';
    lines.push(
      '',
      '-- 固件编译期默认设备码（sdkconfig CONFIG_ROBOT_CLOUD_KEY）：配网页设备码填的就是它，',
      '-- 所以这里必须有一台对应设备，否则每次发布之后机器人都连不上。',
      `INSERT OR REPLACE INTO devices (${deviceColumns.join(',')}) VALUES (` +
        deviceColumns
          .map(column => {
            switch (column) {
              case 'id': return `'${id}'`;
              case 'user_id': return `'${user.id}'`;
              case 'name': return "'DeepSeek（固件默认码）'";
              case 'key_hash': return `'${hash}'`;
              case 'key_mask': return `'${mask}'`;
              case 'status': return "'offline'";
              case 'created_at':
              case 'updated_at': return `'${now}'`;
              default: return 'NULL';
            }
          })
          .join(',') +
        ');',
      `UPDATE devices SET role_id=(SELECT id FROM roles WHERE user_id='${user.id}' ORDER BY created_at DESC LIMIT 1) WHERE id='${id}';`,
    );
    console.log(`补入固件默认设备码: ${mask}`);
  }
} else {
  console.log('sdkconfig 里没有 CONFIG_ROBOT_CLOUD_KEY，跳过固件默认设备');
}

lines.push('PRAGMA foreign_keys = ON;', '');
writeFileSync(OUTPUT, lines.join('\n'));
console.log(`写出 ${OUTPUT}（${lines.length} 行）`);
