# DeskEmoji ESP32-S3 固件工程

本仓库的机器人固件位于 [`esp32/`](esp32/README.md)：硬件接线、配网、设备通道、云端语音、
表情动作、构建烧录与排障说明都在那份 README 里。

> 历史说明：`Hardware/` 原本还带一份讯飞 AIUI 的桌面参考工程（`CMakeLists.txt`、`cmake/`、
> `src/`、`include/`，含 AIUI SDK 与 portaudio 桌面音频）。由于本项目已不再使用 AIUI，
> 该桌面工程于 2026-10-15 随 AIUI 一并移除（见 memory_bank/TASKS.md 的 T028）；
> 需要时可用 `git log -- Hardware/src` 找回。