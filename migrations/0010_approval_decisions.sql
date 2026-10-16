/* T043 远程批准：把"允许/拒绝"变成能远程下发的决定，并全程留痕。
 *
 * 设计纪律（都在服务端代码里落实）：
 *  ① 一个事件只认**第一个**决定（后来的返回 409），且只对这一次批准生效；
 *  ② 超过有效期（Codex 那边早就不等了）不接受，避免"补票式批准"；
 *  ③ 没人表态就什么都不做——PC 侧的 hook 会放弃等待，让 Codex 弹它自己的确认框。 */

ALTER TABLE agent_events ADD COLUMN decision TEXT;        /* allow | deny | NULL（还没人表态） */
ALTER TABLE agent_events ADD COLUMN decided_at TEXT;
ALTER TABLE agent_events ADD COLUMN decided_from TEXT;    /* phone | console | mini（谁下的决定） */
