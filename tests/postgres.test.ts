import {readFileSync} from 'node:fs';
import {PGlite} from '@electric-sql/pglite';
import {afterAll, beforeAll, describe, expect, it} from 'vitest';
import {createPostgresD1, postgresSql} from '../server/node/d1-postgres';
import {app} from '../server/index';

describe('CloudBase PostgreSQL persistence', () => {
  let pg: PGlite;
  let db: D1Database;
  beforeAll(async () => {
    pg = new PGlite();
    await pg.exec(`CREATE ROLE anon; CREATE ROLE authenticated; CREATE ROLE service_role;
      CREATE SCHEMA auth;
      CREATE FUNCTION auth.role() RETURNS text LANGUAGE sql AS $$ SELECT current_setting('request.jwt.claims',true)::json->>'role' $$;
      GRANT USAGE ON SCHEMA auth TO anon, authenticated, service_role;
      SET request.jwt.claims = '{"role":"service_role"}';`);
    await pg.exec(readFileSync(new URL('../cloudbase/migrations/20261027120000_aidesk_persistence.sql', import.meta.url), 'utf8'));
    await pg.exec(readFileSync(new URL('../cloudbase/migrations/20261027132000_aidesk_import_sequences.sql', import.meta.url), 'utf8'));
    await pg.exec(readFileSync(new URL('../cloudbase/migrations/20261027133000_aidesk_result_rows.sql', import.meta.url), 'utf8'));
    await pg.exec('SET ROLE service_role');
    db = createPostgresD1({envId: 'test-env', apiKey: 'test-only', fetch: async (_url, init) => {
      const body = JSON.parse(String(init?.body));
      try {
        const response = await pg.query<{data: unknown}>('SELECT public.aidesk_query($1::jsonb) AS data', [JSON.stringify(body.statements)]);
        return Response.json(response.rows[0].data);
      } catch (error) {
        return Response.json({code: 'DATABASE_' + (error as {code: string}).code}, {status: 400});
      }
    }});
  }, 30000);
  afterAll(async () => {await pg?.close();});

  it('binds quotes, backslashes and SQL-looking values without executing them', async () => {
    const text = "x'; DELETE FROM users; -- \\ ? RETURNING count";
    expect(await db.prepare('SELECT ?::text AS content').bind(text).first('content')).toBe(text);
    expect(postgresSql("SELECT '?' AS literal, ? AS value /* ? */", ['yes'])).toBe("SELECT '?' AS literal, 'yes' AS value /* ? */");
    expect(() => postgresSql('SELECT ?', [])).toThrow('不足');
    expect(() => postgresSql('SELECT 1', [1])).toThrow('过多');
  });

  it('keeps millisecond integers and atomic rate-limit UPSERTs', async () => {
    const sql = 'INSERT INTO rate_limits(key,count,expires_at) VALUES (?,1,?) ON CONFLICT(key) DO UPDATE SET count=CASE WHEN expires_at<? THEN 1 ELSE count+1 END,expires_at=CASE WHEN expires_at<? THEN ? ELSE expires_at END RETURNING count';
    const time = 1793097600000;
    expect(await db.prepare(sql).bind('key', time+60000, time, time, time+60000).first('count')).toBe(1);
    expect(await db.prepare(sql).bind('key', time+60000, time, time, time+60000).first('count')).toBe(2);
    expect(await db.prepare('SELECT expires_at FROM rate_limits WHERE key=?').bind('key').first('expires_at')).toBe(time+60000);
  });

  it('rolls back the entire batch on constraint failure', async () => {
    await expect(db.batch([
      db.prepare("INSERT INTO users VALUES ('rollback','rollback','x','x','t')"),
      db.prepare("INSERT INTO users VALUES ('rollback','duplicate','x','x','t')"),
    ])).rejects.toThrow();
    expect(await db.prepare("SELECT id FROM users WHERE id='rollback'").first()).toBeNull();
  });

  it('supports real registration, cookie login and tenant isolation', async () => {
    const env = {DB: db, ENCRYPTION_KEY: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=', APP_ENV: 'development'};
    const register = await app.request('http://localhost/api/auth/register', {
      method: 'POST', headers: {'Content-Type': 'application/json', Origin: 'http://localhost'},
      body: JSON.stringify({username: 'pg_owner', password: 'test-password-123'}),
    }, env);
    expect(register.status).toBe(201);
    const duplicate = await app.request('http://localhost/api/auth/register', {
      method: 'POST', headers: {'Content-Type': 'application/json', Origin: 'http://localhost'},
      body: JSON.stringify({username: 'pg_owner', password: 'test-password-123'}),
    }, env);
    expect(duplicate.status).toBe(409);
    const cookie = register.headers.get('set-cookie')!.split(';')[0];
    const me = await app.request('http://localhost/api/auth/me', {headers: {Cookie: cookie}}, env);
    expect(me.status).toBe(200);
    const model = await app.request('http://localhost/api/models', {
      method: 'POST', headers: {Cookie: cookie, Origin: 'http://localhost', 'Content-Type': 'application/json'},
      body: JSON.stringify({name: 'PG test', baseURL: 'https://api.deepseek.com', model: 'deepseek-chat', apiKey: 'test-key'}),
    }, {...env, PROVIDER_ALLOWED_HOSTS: 'api.deepseek.com'});
    expect(model.status).toBe(201);
    const other = await app.request('http://localhost/api/auth/register', {
      method: 'POST', headers: {'Content-Type': 'application/json', Origin: 'http://localhost'},
      body: JSON.stringify({username: 'pg_other', password: 'test-password-123'}),
    }, env);
    const otherCookie = other.headers.get('set-cookie')!.split(';')[0];
    const listing = await app.request('http://localhost/api/models', {headers: {Cookie: otherCookie}}, env);
    expect(await listing.json()).toEqual({models: []});
    const outsider = await app.request('http://localhost/api/models', {}, env);
    expect(outsider.status).toBe(401);
  });

  it('preserves row ordering and positional INSERTs including chat records', async () => {
    await db.prepare("INSERT INTO users VALUES ('order','order','x','x','t')").run();
    await db.prepare("INSERT INTO conversations VALUES ('c','order',NULL,'chat','t','t')").run();
    await db.batch([
      db.prepare("INSERT INTO messages SELECT 'a','c','order','user','first','complete','t'"),
      db.prepare("INSERT INTO messages SELECT 'b','c','order','assistant','second','complete','t'"),
    ]);
    const rows = (await db.prepare("SELECT content FROM messages WHERE conversation_id='c' ORDER BY created_at,rowid").all()).results;
    expect(rows.map(row => row.content)).toEqual(['first', 'second']);
    await db.prepare("INSERT INTO devices (id,user_id,name,key_hash,key_mask,status,created_at,updated_at) VALUES ('device','order','device','hash','mask','online','t','t')").run();
    await db.prepare("INSERT INTO device_commands (id,device_id,user_id,type,payload,status,created_at,updated_at) VALUES ('command','device','order','volume','{}','pending','t','t')").run();
    expect(await db.prepare("SELECT * FROM device_commands WHERE id='command'").first()).toMatchObject({id: 'command', result: null});
  });

  it('denies anonymous RPC and direct table access', async () => {
    await pg.exec("SET ROLE anon; SET request.jwt.claims = '{\"role\":\"anon\"}';");
    try {
      await expect(pg.query('SELECT public.aidesk_query($1::jsonb)', [JSON.stringify([{sql: 'SELECT * FROM users', returns_rows: true}])])).rejects.toThrow();
      await expect(pg.query('SELECT * FROM aidesk.users')).rejects.toThrow();
    } finally {
      await pg.exec("RESET ROLE; SET ROLE service_role; SET request.jwt.claims = '{\"role\":\"service_role\"}';");
    }
  });
});
