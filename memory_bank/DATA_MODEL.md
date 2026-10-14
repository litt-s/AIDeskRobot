# AIDesk 机器人控制台 DATA_MODEL

状态：已确认；最后更新：2026-10-14。用户已明确批准全方案并连续执行，每项验收及文档同步后进入下一项。禁止部署、推送、创建云资源；不得修改 Hardware、Android、MiniProgram、Design。

## 实体
users(id,username,password_hash,salt)；sessions(token_hash,user_id,expires_at)；models(id,user_id,name,base_url,model,key_cipher,key_mask)；roles(id,user_id,name,system_prompt,task,model_id)；conversations(id,user_id,role_id,title)；messages(id,conversation_id,user_id,role,content,status)；rate_limits(key,count,window)；chat_generations(conversation_id,user_id,assistant_id,token,state,context_json,partial,lease_until,updated_at,error)（migrations/0003）；devices(id,user_id,name,key_hash,key_mask,firmware,battery,status,last_seen_at,created_at,updated_at,**role_id,conversation_id**) 与 device_commands(id,device_id,user_id,type,payload,status,result,created_at,updated_at,acked_at)（migrations/0004）；voice_settings(user_id,api_key_cipher,key_mask,asr_resource,tts_resource,speaker,created_at,updated_at)（migrations/0005）。devices.role_id 是这台设备说话用的角色，conversation_id 是它的专属会话（首次对话时创建），换角色时清空 conversation_id 让新角色从干净上下文开始。chat_locks 为旧实现遗留表，仅在新请求前做过期检查，新流程不再写入。
## 权限
所有资源 SQL 附 user_id；跨租户 ID 返回 404。模型密钥仅加密存储，不返回 cipher/key，只返回 mask。设备密钥为 12 位 Crockford base32（不含 I、L、O、U，约 60 位熵），展示为 `XXXX-XXXX-XXXX`，仅绑定时返回一次，库内只存归一化后的哈希与掩码；校验前做归一化（去非字母数字、转大写、I/L→1、O→0），因此可省略短横线、不区分大小写。设备鉴权接口按来源地址限流，防止短码被在线枚举。
## API
/api/auth/register,login,logout,me；/api/models GET/POST，/:id PATCH/DELETE，/:id/test POST；/api/roles GET/POST，/:id PATCH/DELETE；/api/conversations GET/POST，/:id GET/DELETE，/:id/chat POST SSE，/:id/generation GET，/:id/chat/pause POST，/:id/chat/resume POST，/:id/rewind POST（删除最后一条提问及其回复并清空生成状态，返回回填文本），/:id/regenerate POST（保留原提问，删除该轮旧回复并以上文重新生成，不新增用户消息）。设备：/api/devices GET/POST，/:id PATCH/DELETE（PATCH 可改 name 与 role_id），/:id/commands GET/POST（控制台，会话鉴权）；/api/device/hello、/api/device/status、/api/device/commands GET、/api/device/commands/:id/ack（设备端，Authorization: Device <key>）。语音：/api/voice/settings GET/PUT/DELETE（控制台，会话鉴权；PUT 的 apiKey 可省略，表示只改音色或资源号），/api/device/voice POST（设备端，设备密钥鉴权），/api/device/say POST（设备端，只做合成、不走模型，供控制台「播报」与无 AIUI 固件使用；请求为 `{"text":"…"}`，上限 200 字，响应与 /api/device/voice 同一套分帧）。
## 语音协议
`/api/device/voice` 的请求体为裸 PCM（16 kHz / 16 位 / 单声道 / 小端），上限 30 秒（960 KB），长度必须为偶数。两者的响应均为 `application/octet-stream` 分块流，每帧 `[类型 1 字节][长度 4 字节小端][负载]`：`0x01` 为 JSON 事件（`{"type":"asr"|"reply"|"error"|"done",...}`），`0x02` 为 PCM 片。事件与音频共用一条流，因此设备可以边收边播；任何一步失败都发 `error` 后跟 `done`，不会中途断流让设备只看到连接断开。`X-Sample-Rate: 16000` 随响应头下发。凭据按账号加密存储（同模型密钥的 AES-GCM 方案），未配置时回落平台环境变量，接口只返回 mask 与来源。
一轮结束后无论成败都会在设备的专属会话里留下记录：听到的话写作用户消息，模型的回复（或失败原因，status=failed）写作助手消息。此前失败是静默的——机器人不出声、控制台也没有任何可查的东西。
## 协议
统一 JSON error；聊天 SSE data 包含 delta/done/error。数据库保存 user 消息和完整 assistant 消息。模型请求失败返回 `{error,kind,status?}`，kind 为 http/timeout/network/protocol，status为上游HTTP状态；仅安全摘要message/code/type，不返回响应header或API key。

暂停与继续以数据库状态为准，不依赖前端内存。生成状态 state=running/paused/completed/failed；token 为本次生成标识，checkpoint、finalize、pause、resume 全部带 token 条件更新，被替换的旧 token 不能写入或覆盖新生成。每个 delta 先 checkpoint 落库再下发，pause 显式把 state 置 paused 并写 interrupted；resume 仅接受 paused/failed，CAS 换新 token 后基于 partial 与原上下文续写并追加到同一 assistant 消息，不重复插入用户消息；lease 15s，失联由 reconcile 收敛为 paused 提示可续写。上下文最多取最近100条complete消息按时间正序，页面历史仍保留全部。

## 设备侧 NVS（配网页写入，不是 D1）
`wifi_cfg`：`ssid`、`password`；`cloud_cfg`：`url`（控制台服务器地址）、`key`（设备码）。配网页**填了才写、留空不写**（逐字段保存，没填的字段保持原值）；设备码在 NVS 内明文保存，页面只显示后四位，不显示完整值。启动时逐字段回落：NVS 缺哪一项才用固件编译值（`ROBOT_CLOUD_URL`／`ROBOT_CLOUD_KEY`）。