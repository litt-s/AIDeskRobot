# AIDesk 数据模型

项目名称：AIDesk 基于 ESP32 的办公陪伴机器人及多端控制台

编制日期：2026-10-23。个人信息以选题申报表、开题报告和答辩 PPT 中的填写内容为准。

本文由空库迁移提取字段，不包含真实数据。共 17 张业务及支撑表，11 个迁移文件。两个文件以 0009 开头，按完整文件名排序，与当前启动逻辑一致。

## agent_events

- `id`：TEXT，主键
- `user_id`：TEXT，非空
- `key_id`：TEXT
- `source`：TEXT，非空
- `kind`：TEXT，非空
- `title`：TEXT
- `detail`：TEXT
- `status`：TEXT，非空
- `announced`：INTEGER，非空
- `created_at`：TEXT，非空
- `acked_at`：TEXT
- `options`：TEXT
- `decision`：TEXT
- `decided_at`：TEXT
- `decided_from`：TEXT

## agent_keys

- `id`：TEXT，主键
- `user_id`：TEXT，非空
- `name`：TEXT，非空
- `key_hash`：TEXT，非空
- `key_mask`：TEXT，非空
- `created_at`：TEXT，非空
- `last_used_at`：TEXT

## agent_watch

- `user_id`：TEXT，主键
- `label`：TEXT
- `last_seen_at`：TEXT，非空
- `notified_count`：INTEGER，非空
- `last_error`：TEXT

## chat_generations

- `conversation_id`：TEXT，主键
- `user_id`：TEXT，非空
- `assistant_id`：TEXT，非空
- `token`：TEXT，非空
- `state`：TEXT，非空
- `context_json`：TEXT，非空
- `partial`：TEXT，非空
- `lease_until`：INTEGER，非空
- `updated_at`：TEXT，非空
- `error`：TEXT

## chat_locks

- `conversation_id`：TEXT，主键
- `expires_at`：INTEGER，非空

## conversations

- `id`：TEXT，主键
- `user_id`：TEXT，非空
- `role_id`：TEXT
- `title`：TEXT，非空
- `created_at`：TEXT，非空
- `updated_at`：TEXT，非空

## device_commands

- `id`：TEXT，主键
- `device_id`：TEXT，非空
- `user_id`：TEXT，非空
- `type`：TEXT，非空
- `payload`：TEXT，非空
- `status`：TEXT，非空
- `result`：TEXT
- `created_at`：TEXT，非空
- `updated_at`：TEXT，非空
- `acked_at`：TEXT

## devices

- `id`：TEXT，主键
- `user_id`：TEXT，非空
- `name`：TEXT，非空
- `key_hash`：TEXT，非空
- `key_mask`：TEXT，非空
- `firmware`：TEXT
- `battery`：INTEGER
- `status`：TEXT，非空
- `last_seen_at`：TEXT
- `created_at`：TEXT，非空
- `updated_at`：TEXT，非空
- `role_id`：TEXT
- `conversation_id`：TEXT
- `wake_label`：TEXT
- `wake_phrase`：TEXT
- `wake_applied`：TEXT

## meeting_turns

- `id`：TEXT，主键
- `meeting_id`：TEXT，非空
- `user_id`：TEXT，非空
- `seq`：INTEGER，非空
- `speaker_label`：TEXT，非空
- `content`：TEXT，非空
- `started_ms`：INTEGER，非空
- `ended_ms`：INTEGER，非空
- `created_at`：TEXT，非空

## meetings

- `id`：TEXT，主键
- `user_id`：TEXT，非空
- `title`：TEXT，非空
- `status`：TEXT，非空
- `source`：TEXT，非空
- `transcript`：TEXT，非空
- `minutes`：TEXT
- `minutes_status`：TEXT，非空
- `minutes_error`：TEXT
- `minutes_model_id`：TEXT
- `minutes_generated_at`：TEXT
- `error`：TEXT
- `duration_seconds`：INTEGER
- `created_at`：TEXT，非空
- `updated_at`：TEXT，非空

## messages

- `id`：TEXT，主键
- `conversation_id`：TEXT，非空
- `user_id`：TEXT，非空
- `role`：TEXT，非空
- `content`：TEXT，非空
- `status`：TEXT，非空
- `created_at`：TEXT，非空

## models

- `id`：TEXT，主键
- `user_id`：TEXT，非空
- `name`：TEXT，非空
- `base_url`：TEXT，非空
- `model`：TEXT，非空
- `key_cipher`：TEXT，非空
- `key_mask`：TEXT，非空
- `created_at`：TEXT，非空
- `updated_at`：TEXT，非空

## rate_limits

- `key`：TEXT，主键
- `count`：INTEGER，非空
- `expires_at`：INTEGER，非空

## roles

- `id`：TEXT，主键
- `user_id`：TEXT，非空
- `name`：TEXT，非空
- `system_prompt`：TEXT，非空
- `task`：TEXT，非空
- `model_id`：TEXT
- `created_at`：TEXT，非空
- `updated_at`：TEXT，非空

## sessions

- `token_hash`：TEXT，主键
- `user_id`：TEXT，非空
- `expires_at`：TEXT，非空
- `created_at`：TEXT，非空

## users

- `id`：TEXT，主键
- `username`：TEXT，非空
- `password_hash`：TEXT，非空
- `salt`：TEXT，非空
- `created_at`：TEXT，非空

## voice_settings

- `user_id`：TEXT，主键
- `api_key_cipher`：TEXT，非空
- `key_mask`：TEXT，非空
- `asr_resource`：TEXT，非空
- `tts_resource`：TEXT，非空
- `speaker`：TEXT，非空
- `created_at`：TEXT，非空
- `updated_at`：TEXT，非空

## 数据关系与授权

users 为业务主体；models、roles、conversations、devices、meetings 和 agent_events 归属于用户。roles 关联 models，messages 属于 conversations，device_commands 属于 devices，meeting_turns 属于 meetings。chat_generations 保存会话生成状态，rate_limits 支撑限流。涉及用户数据的查询必须附 user_id，不把外键存在等同于授权。

## 端侧与协议

Web Cookie 和小程序 Bearer 指向服务端会话；Android 另保存服务器地址与通知开关；PC 本机模式使用自己的 SQLite 和 encryption.key。ESP32 NVS 保存 Wi-Fi、服务器地址和设备码。设备音频协议是 1 字节类型、4 字节小端长度、负载；JSON 事件与 PCM 音频共享一条响应流。会议时间标签可能是显示估计，不可作为精准音频对齐证据。

## 初始化与迁移

init.sql 只适合空 SQLite 一次性执行。已有库应使用 server/node/index.ts 的迁移历史机制，不能反复执行含 ALTER TABLE 的合并文件。向其他数据库迁移必须转换 SQL 并做真实数据验证，不可直接导入。
