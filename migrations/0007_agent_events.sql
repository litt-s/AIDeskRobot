/* T042 第一期：Agent 事件（Codex 这类编程 agent 的状态上报）与上报凭据。
 * 只做"通知"，不做"远程批准"——那是第二期。 */

CREATE TABLE agent_keys (
 id TEXT PRIMARY KEY,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 name TEXT NOT NULL,
 key_hash TEXT NOT NULL UNIQUE,
 key_mask TEXT NOT NULL,
 created_at TEXT NOT NULL,
 last_used_at TEXT
);
CREATE INDEX idx_agent_keys_user ON agent_keys(user_id);

CREATE TABLE agent_events (
 id TEXT PRIMARY KEY,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 key_id TEXT REFERENCES agent_keys(id) ON DELETE SET NULL,
 source TEXT NOT NULL,           /* 哪个软件：Codex / Cursor / … */
 kind TEXT NOT NULL CHECK(kind IN ('needs_confirmation','failed','finished')),
 title TEXT,                     /* 一句话标题（机器人念的就是它） */
 detail TEXT,                    /* 细节，留到手机上看 */
 status TEXT NOT NULL DEFAULT 'open' CHECK(status IN ('open','acked')),
 announced INTEGER NOT NULL DEFAULT 0,  /* 是否已经排过播报 */
 created_at TEXT NOT NULL,
 acked_at TEXT
);
CREATE INDEX idx_agent_events_user ON agent_events(user_id, created_at);
