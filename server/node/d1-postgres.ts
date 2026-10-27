interface Statement {
  sql: string;
  values: unknown[];
}
interface Result {
  success: boolean;
  results: Record<string, unknown>[];
  meta: {changes: number};
}
export interface PostgresConfig {
  envId: string;
  apiKey: string;
  fetch?: typeof globalThis.fetch;
}

/** Only replace parameters outside SQL literals, identifiers and comments. */
export function postgresSql(sql: string, values: unknown[]): string {
  if (/^INSERT INTO rate_limits\b/i.test(sql)) {
    sql = sql.replace(/\bexpires_at</g, 'rate_limits.expires_at<').replace(/\bELSE count\b/g, 'ELSE rate_limits.count').replace(/\bELSE expires_at\b/g, 'ELSE rate_limits.expires_at');
  }
  let index = 0;
  const converted = sql.replace(/'(?:''|[^'])*'|"(?:""|[^"])*"|--[^\n]*|\/\*[\s\S]*?\*\/|\?/g, token => {
    if (token !== '?') return token;
    if (index >= values.length) throw new Error('SQL 参数不足');
    const value = values[index++];
    if (value === null) return 'NULL';
    if (typeof value === 'string') {
      if (value.includes('\0')) throw new Error('SQL 参数不能包含空字符');
      return "'" + value.replaceAll("'", "''") + "'";
    }
    if (typeof value === 'number' && Number.isFinite(value)) return String(value);
    if (typeof value === 'boolean') return value ? 'TRUE' : 'FALSE';
    throw new Error('不支持的 SQL 参数类型');
  });
  if (index !== values.length) throw new Error('SQL 参数过多');
  return converted;
}

function result(value: unknown): value is Result {
  if (!value || typeof value !== 'object') return false;
  const row = value as Partial<Result>;
  return row.success === true && Array.isArray(row.results) && typeof row.meta?.changes === 'number';
}

export function createPostgresD1(config: PostgresConfig): D1Database {
  if (!/^[a-z0-9-]+$/.test(config.envId) || !config.apiKey) throw new Error('PostgreSQL 配置不完整');
  const endpoint = `https://${config.envId}.api.tcloudbasegateway.com/v1/rdb/rest/rpc/aidesk_query`;
  const send = config.fetch ?? globalThis.fetch;
  const statements = new WeakMap<object, Statement>();
  const execute = async (pending: Statement[]): Promise<Result[]> => {
    // Do not retry writes: a lost response can follow a committed transaction.
    const response = await send(endpoint, {
      method: 'POST',
      headers: {'Content-Type': 'application/json', Authorization: `Bearer ${config.apiKey}`},
      body: JSON.stringify({statements: pending.map(item => ({sql: postgresSql(item.sql, item.values), returns_rows: /^\s*SELECT\b|\bRETURNING\b/i.test(item.sql)}))}),
      signal: AbortSignal.timeout(45000),
      redirect: 'error',
    });
    if (!response.ok) {
      const failure: unknown = await response.json().catch(() => null);
      if (failure && typeof failure === 'object' && 'code' in failure && ['23505', 'DATABASE_23505'].includes(String(failure.code))) {
        throw new Error('UNIQUE constraint failed');
      }
      console.error('PostgreSQL request failed', {status: response.status});
      throw new Error(`云数据库请求失败（HTTP ${response.status}）`);
    }
    const data: unknown = await response.json();
    if (!Array.isArray(data) || data.length !== pending.length || !data.every(result)) throw new Error('云数据库响应格式无效');
    return data;
  };
  const wrap = (sql: string, values: unknown[] = []) => {
    const statement: Statement = {sql, values};
    const run = async () => (await execute([statement]))[0];
    const prepared = {
      bind: (...params: unknown[]) => wrap(sql, params),
      first: async (column?: string) => {
        const row = (await run()).results[0];
        return row ? (column ? row[column] : row) : null;
      },
      all: run,
      run,
    };
    statements.set(prepared, statement);
    return prepared;
  };
  return {
    prepare: (sql: string) => wrap(sql),
    batch: (prepared: object[]) => execute(prepared.map(item => {
      const statement = statements.get(item);
      if (!statement) throw new Error('事务包含其他数据库的语句');
      return statement;
    })),
  } as unknown as D1Database;
}
