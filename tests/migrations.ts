import {readFileSync,readdirSync} from 'node:fs';

/*
 * 测试夹具直接跑 migrations/ 下的全部 SQL（按文件名排序）。
 *
 * 之前每个测试文件都手写一份迁移清单，加一条迁移就得同步改四处；2026-10-15 加
 * 0006_wake.sql 时漏了，于是 device.test.ts 两条用例撞上 "no such column:
 * wake_applied" 的 500。改成读目录之后，新迁移自动生效，夹具不会再和现实脱节。
 */
export function applyMigrations(sqlite:{exec(sql:string):unknown}){
 const dir=new URL('../migrations/',import.meta.url);
 for(const file of readdirSync(dir).filter(name=>name.endsWith('.sql')).sort()){
  sqlite.exec(readFileSync(new URL(file,dir),'utf8'));
 }
}
