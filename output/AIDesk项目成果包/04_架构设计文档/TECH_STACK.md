# AIDesk 技术栈说明

项目名称：AIDesk 基于 ESP32 的办公陪伴机器人及多端控制台

编制日期：2026-10-23。个人信息以选题申报表、开题报告和答辩 PPT 中的填写内容为准。

## 前端

React 19 与 TypeScript，Vite 构建，Lucide React 图标。Web 以同源 /api 调用服务端，通过 fetch ReadableStream 处理 SSE，使用媒体查询支持手机、平板与桌面。版本范围以源码 package.json 为准，实际锁定版本以 package-lock.json 为准。

## 服务端与数据库

Hono 路由运行于 Cloudflare Worker 或 Node.js。Worker 使用 D1；Node 使用 node:sqlite 的 DatabaseSync，经自建 D1 兼容层复用业务 SQL。Node ASR 使用 ws，静态文件由同一进程提供。SQLite 语句包含 rowid、RETURNING 与 ON CONFLICT 等语义，不能将数据库无修改替换为 MySQL/PostgreSQL。

## 端侧与设备

Android 使用 Java 17、WebView、Gradle 工程；PC 使用 Electron 与 electron-builder。微信小程序使用 WXML、WXSS、JavaScript，wx.request 分块处理 SSE。ESP32 使用 ESP-IDF、FreeRTOS、I2S 音频、OLED、舵机与 NVS 配置。所有平台形态的安装、分发和真机状态分别记录，不以 Web 构建替代原生端验收。

## AI 与语音服务

LLM 通过服务端访问 OpenAI 兼容 Chat Completions；供应商域名受白名单控制。ASR 与 TTS 通过火山语音适配层接入。AIUI 已按 T028 移除。MCP、自动声纹和完整长音频存储不作为既有技术能力。

## 安全与验证

WebCrypto 提供 PBKDF2、SHA-256 与 AES-GCM。Vitest、TypeScript、Vite 与 esbuild 为主要本地门禁。本次 Node 24 环境跑通 99 项测试；接口集成测试通过隔离 Node 服务验证，不冒称再次验证了 Worker 云端。
