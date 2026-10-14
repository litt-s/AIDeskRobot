CREATE TABLE chat_generations (
 conversation_id TEXT PRIMARY KEY REFERENCES conversations(id) ON DELETE CASCADE,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 assistant_id TEXT NOT NULL,
 token TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('running','paused','completed','failed')),
 context_json TEXT NOT NULL,
 partial TEXT NOT NULL DEFAULT '',
 lease_until INTEGER NOT NULL,
 updated_at TEXT NOT NULL,
 error TEXT
);
CREATE INDEX idx_generations_user ON chat_generations(user_id);
