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
