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
