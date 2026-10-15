import {DatabaseSync} from 'node:sqlite';

/*
 * 把 node:sqlite 包成"服务端代码已经在用的那一小撮 D1 接口"。
 *
 * 只实现我们真正调用的形状：prepare(sql).bind(...).first()/all()/run() 与 db.batch()。
 * 这样 server/** 里那些 SQL 与调用点一行都不用改——迁移到 CloudBase 时把 D1 换成这里的实现，
 * 与 Cloudflare 版共用同一份业务代码。绑参走位置参数，与 D1 一致。
 */
export function createD1(db: DatabaseSync): D1Database {
  const wrap = (sql: string, params: unknown[] = []): any => ({
    bind: (...values: unknown[]) => wrap(sql, values),
    first: async () => db.prepare(sql).get(...(params as any[])) ?? null,
    all: async () => ({ results: db.prepare(sql).all(...(params as any[])) }),
    run: async () => {
      const r = db.prepare(sql).run(...(params as any[]));
      return { meta: { changes: Number(r.changes), last_row_id: Number(r.lastInsertRowid) } };
    },
  });
  return {
    prepare: (sql: string) => wrap(sql),
    batch: async (statements: any[]) => {
      db.exec('BEGIN');
      try {
        const out = [];
        for (const s of statements) out.push(await s.run());
        db.exec('COMMIT');
        return out;
      } catch (error) {
        db.exec('ROLLBACK');
        throw error;
      }
    },
  } as unknown as D1Database;
}
