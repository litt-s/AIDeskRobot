import {readFileSync, writeFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {DatabaseSync} from 'node:sqlite';
import {createPostgresD1} from '../server/node/d1-postgres.ts';

const envId = process.env.CLOUDBASE_ENV_ID || 'my-base01-d0gacogbi5e4deac3';
const apiKey = process.env.CLOUDBASE_DB_API_KEY || readFileSync('data/postgres-api-key.txt', 'utf8').trim();
const source = process.argv[2];
if (!source) throw new Error('用法：node scripts/migrate-postgres.mjs <只读SQLite快照> [--verify]');
const verifyOnly = process.argv.includes('--verify');
const sqlite = new DatabaseSync(source, {readOnly: true});
const target = createPostgresD1({envId, apiKey});
const quote = value => '"' + value.replaceAll('"', '""') + '"';
const literal = value => "'" + String(value).replaceAll("'", "''") + "'";
const tables = sqlite.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' AND name NOT LIKE '\\_%' ESCAPE '\\' ORDER BY rowid").all().map(row => row.name);
const known = new Set((await target.prepare("SELECT table_name FROM information_schema.tables WHERE table_schema='aidesk'").all()).results.map(row => row.table_name));
const rowsByTable = new Map();
const inserts = [];
try {
  for (const table of tables) {
    if (!known.has(table)) throw new Error(`目标库缺少表：${table}`);
    const rows = sqlite.prepare(`SELECT rowid AS rowid,* FROM ${quote(table)} ORDER BY rowid`).all();
    rowsByTable.set(table, rows);
    if (!verifyOnly) {
      const count = await target.prepare(`SELECT COUNT(*) AS n FROM ${quote(table)}`).first('n');
      if (count !== 0) throw new Error(`目标表 ${table} 非空，拒绝覆盖；需要核验请加 --verify`);
      if (rows.length) {
        const columns = Object.keys(rows[0]);
        const json = JSON.stringify(rows);
        inserts.push(target.prepare(`INSERT INTO ${quote(table)} (${columns.map(quote).join(',')}) SELECT ${columns.map(quote).join(',')} FROM jsonb_populate_recordset(NULL::${quote(table)},${literal(json)}::jsonb)`));
        inserts.push(target.prepare(`SELECT setval(pg_get_serial_sequence(${literal('aidesk.' + table)},'rowid'),(SELECT MAX(rowid) FROM ${quote(table)}))`));
      }
    }
  }
  // Foreign-key dependencies may cross creation order (e.g. devices refer to roles).
  // The schema retains the original nullable device references; all rows import in one transaction.
  if (!verifyOnly && inserts.length) await target.batch(inserts);
  const manifest = {envId, source, verifiedAt: new Date().toISOString(), tables: {}};
  const digest = rows => createHash('sha256').update(JSON.stringify(rows)).digest('hex');
  for (const [table, sourceRows] of rowsByTable) {
    const columns = sqlite.prepare(`PRAGMA table_info(${quote(table)})`).all().map(row => row.name);
    const names = ['rowid', ...columns];
    const remote = (await target.prepare(`SELECT ${names.map(quote).join(',')} FROM ${quote(table)} ORDER BY rowid`).all()).results;
    const canonical = rows => rows.map(row => Object.fromEntries(names.map(name => [name, row[name]])));
    if (digest(canonical(sourceRows)) !== digest(canonical(remote))) throw new Error(`数据校验失败：${table}`);
    manifest.tables[table] = {rows: sourceRows.length, sha256: digest(canonical(sourceRows))};
    console.log(`${table}: ${sourceRows.length} 行，一致`);
  }
  writeFileSync('data/postgres-migration-manifest.json', JSON.stringify(manifest, null, 2));
  console.log(verifyOnly ? '逐表全字段校验通过' : '导入及逐表全字段校验通过');
} finally {
  sqlite.close();
}
