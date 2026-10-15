/* 自定义唤醒词：控制台存"用户写的"与"转成的拼音"，设备在 hello 里回报"当前生效的"。 */

/* wake_label：用户填的原文（中文或拼音），用于界面回显；
 * wake_phrase：转换后的拼音音节串，既用于下发也用于和设备回报值对账；
 * wake_applied：设备回报的当前词（换固件、清 NVS 后能立刻看出不一致）。 */
ALTER TABLE devices ADD COLUMN wake_label TEXT;
ALTER TABLE devices ADD COLUMN wake_phrase TEXT;
ALTER TABLE devices ADD COLUMN wake_applied TEXT;
