# 可运行 App 与源码交付

本目录以 AIDesk 的实际平台形态对应模板。Windows启动包/ 是本次构建的免安装本地 Web 控制台，包含 Node 24 运行时、Web、Node 后端、生产依赖和空库迁移。双击“启动AIDesk.cmd”，按窗口提示在浏览器访问 http://localhost:45715，首次自行注册新账号。模型和语音需自己的有效服务凭据。退出时在命令窗口按 Ctrl+C。目录内 data 由首次启动生成，应随包一起保存。

## 交付内容

- Windows启动包：可直接运行，数据与线上隔离，不含真实账号与密钥。
- AIDesk源码快照.zip：当前工作区的精选工程文件，保留源代码、迁移与测试，不包含 node_modules、真实种子、快照、私有配置、构建缓存和 Git 凭据。
- Web构建_dist：本次 Vite 构建文件，必须由同源 API 服务提供，不能仅双击 index.html 获得完整功能。
- 签名证书配置说明.md：Android 与桌面分发边界。

## 平台对应关系

Web 和 Windows 本地启动包已在本次验证。仓库已有 Electron 工程，但本包没有把旧版 Electron 构建冒充本次新版本。Android 有工程与历史 CI 构建通过记录，当前工作区未发现 APK，故不伪造 APK；可按源码说明重新构建。微信小程序需开发者工具导入并配置自己的 AppID 与服务器合法域名。项目没有鸿蒙工程，不复制模板的 HAP。

## 源码运行

需 Node.js 24 与 npm。解压源码后 npm ci、npm run build、npm run build:node，设置服务端 ENCRYPTION_KEY 与本地 DB_PATH，再 npm run start:node。正式生产必须生成独立随机 32 字节主密钥，不能沿用测试夹具里的公开示例。部署仍遵守仓库一键发布流程，本成果包不执行任何云端资源变更。
