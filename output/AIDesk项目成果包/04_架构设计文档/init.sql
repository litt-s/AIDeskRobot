-- AIDesk empty schema. Execute once on a fresh SQLite database.
-- Ordered migration sources follow; no seed data or real credentials.

-- SOURCE: 0001_initial.sql
CREATE TABLE IF NOT EXISTS users (id TEXT PRIMARY KEY, username TEXT NOT NULL UNIQUE, password_hash TEXT NOT NULL, salt TEXT NOT NULL, created_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS sessions (token_hash TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, expires_at TEXT NOT NULL, created_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS models (id TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, name TEXT NOT NULL, base_url TEXT NOT NULL, model TEXT NOT NULL, key_cipher TEXT NOT NULL, key_mask TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS roles (id TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, name TEXT NOT NULL, system_prompt TEXT NOT NULL, task TEXT NOT NULL DEFAULT '', model_id TEXT REFERENCES models(id) ON DELETE SET NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS conversations (id TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, role_id TEXT REFERENCES roles(id) ON DELETE SET NULL, title TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS messages (id TEXT PRIMARY KEY, conversation_id TEXT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, role TEXT NOT NULL CHECK(role IN ('user','assistant','system')), content TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'complete', created_at TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_models_user ON models(user_id);
CREATE INDEX IF NOT EXISTS idx_roles_user ON roles(user_id);
CREATE INDEX IF NOT EXISTS idx_conversations_user ON conversations(user_id);
CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_id, created_at);

-- SOURCE: 0002_limits.sql
CREATE TABLE IF NOT EXISTS rate_limits (key TEXT PRIMARY KEY, count INTEGER NOT NULL, expires_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS chat_locks (conversation_id TEXT PRIMARY KEY REFERENCES conversations(id) ON DELETE CASCADE, expires_at INTEGER NOT NULL);


-- SOURCE: 0003_generations.sql
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


-- SOURCE: 0004_devices.sql
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


-- SOURCE: 0005_voice.sql
/* 语音对话闭环：用户的语音服务凭据 + 设备的语音角色与对话归属。 */

/* 设备要能"用某个角色说话"，所以设备需要知道自己绑定的是哪个角色、
 * 以及它的对话落在哪个会话里（这样控制台能直接看到机器人的对话记录）。 */
ALTER TABLE devices ADD COLUMN role_id TEXT;
ALTER TABLE devices ADD COLUMN conversation_id TEXT;

/* 语音凭据与模型凭据同构：只存密文与掩码，明文除签发外不落库、不出接口。
 * 未配置的用户回落到平台环境变量（开发与自用场景）。 */
CREATE TABLE IF NOT EXISTS voice_settings (
 user_id TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
 api_key_cipher TEXT NOT NULL,
 key_mask TEXT NOT NULL,
 asr_resource TEXT NOT NULL,
 tts_resource TEXT NOT NULL,
 speaker TEXT NOT NULL,
 created_at TEXT NOT NULL,
 updated_at TEXT NOT NULL
);


-- SOURCE: 0006_wake.sql
/* 自定义唤醒词：控制台存"用户写的"与"转成的拼音"，设备在 hello 里回报"当前生效的"。 */

/* wake_label：用户填的原文（中文或拼音），用于界面回显；
 * wake_phrase：转换后的拼音音节串，既用于下发也用于和设备回报值对账；
 * wake_applied：设备回报的当前词（换固件、清 NVS 后能立刻看出不一致）。 */
ALTER TABLE devices ADD COLUMN wake_label TEXT;
ALTER TABLE devices ADD COLUMN wake_phrase TEXT;
ALTER TABLE devices ADD COLUMN wake_applied TEXT;


-- SOURCE: 0007_agent_events.sql
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


-- SOURCE: 0008_agent_event_options.sql
/* T042 第一期补充：需要确认的事件要带上"当时的选项"。
 * Codex 弹窗里是哪几个选项，机器人就念哪几个——不在电脑前的人才知道自己在批什么。
 * 存 JSON 数组文本（如 ["允许一次","本次会话都允许","拒绝"]）；第二期做远程批准时按同一份数据渲染按钮。 */

ALTER TABLE agent_events ADD COLUMN options TEXT;


-- SOURCE: 0009_agent_watch.sql
/* T042 第一期续项：手机提醒的自检心跳。
 *
 * 为什么需要：手机弹不弹通知，牵涉「App 里的开关 → 常驻服务 → 会话 Cookie → 通知权限 → 系统省电策略」
 * 这一长串，出问题时人对着通知栏完全看不出卡在哪一步。手机上每轮轮询后都往这里报一次
 * （包含"这一轮为什么没成"），控制台就能显示「手机提醒：Pixel 8 在线（30 秒前，已弹 3 条）」
 * 或「已断开 / 从未连接」，把黑盒变成一行状态。 */

/*
 * 注意：并行会话也有一条 `0009_meetings.sql`（编号撞了，但按文件名排序都会执行，互不影响）。
 * 用 IF NOT EXISTS 是为了给"将来有人想把这条改号"留条活路：表已经建好时重新执行不会炸。
 */
CREATE TABLE IF NOT EXISTS agent_watch (
 user_id TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
 label TEXT,                              /* 机型，用于分辨是哪台手机在听 */
 last_seen_at TEXT NOT NULL,
 notified_count INTEGER NOT NULL DEFAULT 0,
 last_error TEXT
);


-- SOURCE: 0009_meetings.sql
/* T019 会议纪要：只保存转写文本与纪要，不保留原始音频。 */
CREATE TABLE meetings (
 id TEXT PRIMARY KEY,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 title TEXT NOT NULL,
 status TEXT NOT NULL DEFAULT 'new' CHECK(status IN ('new','transcribing','transcribed','ready','failed')),
 source TEXT NOT NULL DEFAULT 'manual' CHECK(source IN ('manual','audio')),
 transcript TEXT NOT NULL DEFAULT '',
 minutes TEXT,
 minutes_status TEXT NOT NULL DEFAULT 'none' CHECK(minutes_status IN ('none','generating','ready','failed')),
 minutes_error TEXT,
 minutes_model_id TEXT REFERENCES models(id) ON DELETE SET NULL,
 minutes_generated_at TEXT,
 error TEXT,
 duration_seconds INTEGER,
 created_at TEXT NOT NULL,
 updated_at TEXT NOT NULL
);
CREATE INDEX idx_meetings_user ON meetings(user_id, created_at);

CREATE TABLE meeting_turns (
 id TEXT PRIMARY KEY,
 meeting_id TEXT NOT NULL REFERENCES meetings(id) ON DELETE CASCADE,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 seq INTEGER NOT NULL,
 speaker_label TEXT NOT NULL,
 content TEXT NOT NULL,
 started_ms INTEGER NOT NULL DEFAULT 0,
 ended_ms INTEGER NOT NULL DEFAULT 0,
 created_at TEXT NOT NULL
);
CREATE INDEX idx_meeting_turns_meeting ON meeting_turns(meeting_id, seq);


-- SOURCE: 0010_approval_decisions.sql
/* T043 远程批准：把"允许/拒绝"变成能远程下发的决定，并全程留痕。
 *
 * 设计纪律（都在服务端代码里落实）：
 *  ① 一个事件只认**第一个**决定（后来的返回 409），且只对这一次批准生效；
 *  ② 超过有效期（Codex 那边早就不等了）不接受，避免"补票式批准"；
 *  ③ 没人表态就什么都不做——PC 侧的 hook 会放弃等待，让 Codex 弹它自己的确认框。 */

ALTER TABLE agent_events ADD COLUMN decision TEXT;        /* allow | deny | NULL（还没人表态） */
ALTER TABLE agent_events ADD COLUMN decided_at TEXT;
ALTER TABLE agent_events ADD COLUMN decided_from TEXT;    /* phone | console | mini（谁下的决定） */
