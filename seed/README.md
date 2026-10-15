# seed/

可选的一次性数据播种目录，**默认是空的**。

`seed.sql`（不提交、已 gitignore）用来把已经存在的数据搬进新库：账号、模型、角色、对话、设备、语音凭据。
因为密文用的还是同一个 `ENCRYPTION_KEY`，模型密钥与语音 API Key 搬过去即可直接解密；设备的 `key_hash`
不变，所以**机器人不需要重新配网**。

规则（见 `server/node/index.ts` 的 `applySeed()`）：

- 只在**库为空**（`users` 表没有行）时执行，重启与重新发布都不会重复灌；
- 文件**不能放在数据卷里**——卷为空才需要播种，放卷里就永远读不到，所以镜像里固定在 `/app/seed/seed.sql`
  （Dockerfile 用 `ENV SEED_FILE` 指过去）；
- 生成方式与本次使用记录见 `memory_bank/DEPLOY.md`「从已有数据播种（可选）」。
