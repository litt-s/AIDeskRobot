CREATE TABLE devices (
 id TEXT PRIMARY KEY,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 name TEXT NOT NULL,
 key_hash TEXT NOT NULL UNIQUE,
 key_mask TEXT NOT NULL,
 firmware TEXT,
 battery INTEGER,
 status TEXT NOT NULL DEFAULT 'offline',
 last_seen_at TEXT,
 created_at TEXT NOT NULL,
 updated_at TEXT NOT NULL
);
CREATE INDEX idx_devices_user ON devices(user_id);

CREATE TABLE device_commands (
 id TEXT PRIMARY KEY,
 device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 type TEXT NOT NULL,
 payload TEXT NOT NULL DEFAULT '{}',
 status TEXT NOT NULL CHECK(status IN ('pending','sent','done','failed')),
 result TEXT,
 created_at TEXT NOT NULL,
 updated_at TEXT NOT NULL,
 acked_at TEXT
);
CREATE INDEX idx_device_commands_pending ON device_commands(device_id,status);
