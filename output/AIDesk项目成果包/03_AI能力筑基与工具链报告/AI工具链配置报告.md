# AI 工具链配置报告

项目名称：AIDesk 基于 ESP32 的办公陪伴机器人及多端控制台

编制日期：2026-10-23。个人信息以选题申报表、开题报告和答辩 PPT 中的填写内容为准。

## 一 工具链组成

AI 编程宿主为 Codex。工程使用 Node.js 24、npm 与 package-lock 锁定依赖，前端采用 React、TypeScript、Vite，服务端采用 Hono。Node 运行形态利用 node:sqlite 和 ws；Cloudflare 运行形态使用 Worker 与 D1。Android 为 Java WebView 工程，桌面端为 Electron，小程序使用微信原生文件体系，ESP32 使用 ESP-IDF 与 C/C++。

## 二 工程配置

根 package.json 集中脚本和依赖；vite.config.ts 配置静态前端与开发代理；wrangler.jsonc 定义 Worker 和 D1 绑定；Dockerfile 与 server/node/index.ts 提供容器运行入口；migrations 保存结构迁移；tests 保存安全、鉴权、会话、设备、语音、会议和批准逻辑测试。

## 三 验证命令

```powershell
npm ci
npm run typecheck
npm run build
npm run build:node
# 完整测试需要独立本地服务监听 localhost:8787
npm test
```

本次已存在依赖，未重新 npm ci，未重跑 npm audit。构建被沙箱阻止子进程后在授权环境重新执行通过。测试原始 JSON 与复现说明见 08 目录。

## 四 配置与凭据

ENCRYPTION_KEY 是服务端加密主密钥，不能放入前端。供应商 API key 由用户在服务端账户配置中保存；设备与上报密钥只在创建时展示一次。生产部署使用已有服务环境变量，本包不携带线上数据、seed、.dev.vars、admin-token 或设备默认码。Windows 启动包首次启动在自己的 data 目录生成独立加密密钥，不复用线上密钥。

## 五 本人总结的 project-memory Skill

已提供并核对本人总结的 `project-memory` 技能，完整源码含5个文件及相同内容的ZIP分发包，归档在06目录。它把需求与技术确认、九文档记忆、编号任务、验证回写和跨会话恢复组织成可复用流程。AIDesk现有九文档与任务管理和该方法对应，但不据此推断全部历史任务都曾显式调用该Skill。

入口、安装和调用说明以随附原README为准；本次只整理归档，不执行新的安装或项目初始化。documents、presentations等环境内置技能另列，不归为本人编写。

## 六 协作流程

任务开始阅读九文档，以编号任务推进。完成后记录验收与未验证项，按职责更新 PRD、技术栈、架构、数据模型、设计、任务、进度、经验和部署。当前仓库有其他未提交的工作，本次只提交成果包说明和本阶段记录，避免把其他开发改动一并提交。

## 七 发布约束

当前 CloudBase 部署必须走 node scripts/deploy-cloudbase.mjs，不能绕过线上快照与种子步骤。本次只整理本地成果，未执行发布。更换持久数据库、新增对象存储等仍属于独立任务。Skills 清单见 06 目录，不能把模板作者的工具与技能列表当作本项目的实际安装记录。
