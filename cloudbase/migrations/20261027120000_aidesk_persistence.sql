CREATE SCHEMA aidesk;
REVOKE ALL ON SCHEMA aidesk FROM PUBLIC, anon, authenticated;
SET search_path = aidesk;
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
CREATE TABLE IF NOT EXISTS rate_limits (key TEXT PRIMARY KEY, count BIGINT NOT NULL, expires_at BIGINT NOT NULL);
CREATE TABLE IF NOT EXISTS chat_locks (conversation_id TEXT PRIMARY KEY REFERENCES conversations(id) ON DELETE CASCADE, expires_at BIGINT NOT NULL);

CREATE TABLE chat_generations (
 conversation_id TEXT PRIMARY KEY REFERENCES conversations(id) ON DELETE CASCADE,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 assistant_id TEXT NOT NULL,
 token TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('running','paused','completed','failed')),
 context_json TEXT NOT NULL,
 partial TEXT NOT NULL DEFAULT '',
 lease_until BIGINT NOT NULL,
 updated_at TEXT NOT NULL,
 error TEXT
);
CREATE INDEX idx_generations_user ON chat_generations(user_id);

CREATE TABLE devices (
 id TEXT PRIMARY KEY,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 name TEXT NOT NULL,
 key_hash TEXT NOT NULL UNIQUE,
 key_mask TEXT NOT NULL,
 firmware TEXT,
 battery BIGINT,
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

/* 自定义唤醒词：控制台存"用户写的"与"转成的拼音"，设备在 hello 里回报"当前生效的"。 */

/* wake_label：用户填的原文（中文或拼音），用于界面回显；
 * wake_phrase：转换后的拼音音节串，既用于下发也用于和设备回报值对账；
 * wake_applied：设备回报的当前词（换固件、清 NVS 后能立刻看出不一致）。 */
ALTER TABLE devices ADD COLUMN wake_label TEXT;
ALTER TABLE devices ADD COLUMN wake_phrase TEXT;
ALTER TABLE devices ADD COLUMN wake_applied TEXT;

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
 announced BIGINT NOT NULL DEFAULT 0,  /* 是否已经排过播报 */
 created_at TEXT NOT NULL,
 acked_at TEXT
);
CREATE INDEX idx_agent_events_user ON agent_events(user_id, created_at);

/* T042 第一期补充：需要确认的事件要带上"当时的选项"。
 * Codex 弹窗里是哪几个选项，机器人就念哪几个——不在电脑前的人才知道自己在批什么。
 * 存 JSON 数组文本（如 ["允许一次","本次会话都允许","拒绝"]）；第二期做远程批准时按同一份数据渲染按钮。 */

ALTER TABLE agent_events ADD COLUMN options TEXT;

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
 notified_count BIGINT NOT NULL DEFAULT 0,
 last_error TEXT
);

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
 duration_seconds BIGINT,
 created_at TEXT NOT NULL,
 updated_at TEXT NOT NULL
);
CREATE INDEX idx_meetings_user ON meetings(user_id, created_at);

CREATE TABLE meeting_turns (
 id TEXT PRIMARY KEY,
 meeting_id TEXT NOT NULL REFERENCES meetings(id) ON DELETE CASCADE,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 seq BIGINT NOT NULL,
 speaker_label TEXT NOT NULL,
 content TEXT NOT NULL,
 started_ms BIGINT NOT NULL DEFAULT 0,
 ended_ms BIGINT NOT NULL DEFAULT 0,
 created_at TEXT NOT NULL
);
CREATE INDEX idx_meeting_turns_meeting ON meeting_turns(meeting_id, seq);

/* T043 远程批准：把"允许/拒绝"变成能远程下发的决定，并全程留痕。
 *
 * 设计纪律（都在服务端代码里落实）：
 *  ① 一个事件只认**第一个**决定（后来的返回 409），且只对这一次批准生效；
 *  ② 超过有效期（Codex 那边早就不等了）不接受，避免"补票式批准"；
 *  ③ 没人表态就什么都不做——PC 侧的 hook 会放弃等待，让 Codex 弹它自己的确认框。 */

ALTER TABLE agent_events ADD COLUMN decision TEXT;        /* allow | deny | NULL（还没人表态） */
ALTER TABLE agent_events ADD COLUMN decided_at TEXT;
ALTER TABLE agent_events ADD COLUMN decided_from TEXT;    /* phone | console | mini（谁下的决定） */

ALTER TABLE users ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE sessions ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE models ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE roles ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE conversations ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE messages ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE rate_limits ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE chat_locks ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE chat_generations ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE devices ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE device_commands ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE voice_settings ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE agent_keys ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE agent_events ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE agent_watch ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE meetings ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
ALTER TABLE meeting_turns ADD COLUMN rowid BIGINT GENERATED BY DEFAULT AS IDENTITY UNIQUE;
REVOKE ALL ON ALL TABLES IN SCHEMA aidesk FROM PUBLIC, anon, authenticated;
REVOKE ALL ON ALL SEQUENCES IN SCHEMA aidesk FROM PUBLIC, anon, authenticated;
GRANT USAGE ON SCHEMA aidesk TO service_role;
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA aidesk TO service_role;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA aidesk TO service_role;
CREATE FUNCTION public.aidesk_query(statements jsonb) RETURNS jsonb
LANGUAGE plpgsql SECURITY INVOKER
SET search_path = aidesk, pg_temp
SET standard_conforming_strings = on
SET statement_timeout = '30s'
AS $body$
DECLARE
  statement jsonb;
  query_text text;
  result_rows jsonb;
  changed bigint;
  output jsonb := '[]'::jsonb;
BEGIN
  IF auth.role() IS DISTINCT FROM 'service_role' THEN
    RAISE EXCEPTION 'forbidden' USING ERRCODE = '42501';
  END IF;
  IF jsonb_typeof(statements) IS DISTINCT FROM 'array' OR jsonb_array_length(statements) NOT BETWEEN 1 AND 100 THEN
    RAISE EXCEPTION 'invalid statements';
  END IF;
  FOR statement IN SELECT jsonb_array_elements(statements) LOOP
    query_text := statement->>'sql';
    IF query_text !~* '^\s*(SELECT|INSERT|UPDATE|DELETE)\s' OR length(query_text) > 2000000 THEN
      RAISE EXCEPTION 'invalid statement';
    END IF;
    IF (statement->>'returns_rows')::boolean THEN
      EXECUTE 'WITH result AS (' || query_text || ') SELECT coalesce(jsonb_agg(to_jsonb(result)), ''[]''::jsonb) FROM result' INTO result_rows;
      changed := jsonb_array_length(result_rows);
    ELSE
      EXECUTE query_text;
      GET DIAGNOSTICS changed = ROW_COUNT;
      result_rows := '[]'::jsonb;
    END IF;
    output := output || jsonb_build_array(jsonb_build_object('success', true, 'results', result_rows, 'meta', jsonb_build_object('changes', changed)));
  END LOOP;
  RETURN output;
END;
$body$;
REVOKE ALL ON FUNCTION public.aidesk_query(jsonb) FROM PUBLIC, anon, authenticated;
GRANT EXECUTE ON FUNCTION public.aidesk_query(jsonb) TO service_role;
NOTIFY pgrst, 'reload schema';
