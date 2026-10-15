# CloudBase 云托管（容器）镜像：一个 Node 进程同时提供前端静态资源与 /api。
#
# 与 Cloudflare Worker 版是同一份 server/ 代码，只换了运行时（见 server/node/index.ts 的说明）。
# 构建分两段：build 段装全量依赖并产出 dist/ 与 dist-server/，运行段只留生产依赖。
FROM node:24-slim AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY . .
RUN npm run build && npm run build:node

FROM node:24-slim
WORKDIR /app
ENV NODE_ENV=production
ENV PORT=80
COPY package.json package-lock.json ./
RUN npm ci --omit=dev && npm cache clean --force
COPY --from=build /app/dist ./dist
COPY --from=build /app/dist-server ./dist-server
COPY migrations ./migrations
# 可选的一次性播种（把已有账号/设备/模型/语音凭据搬进新库）；seed/seed.sql 不入库，缺失时启动会自动跳过。
COPY seed ./seed
ENV SEED_FILE=/app/seed/seed.sql
EXPOSE 80
# 语音识别走 ws（server/node/asr-ws.ts），不需要额外系统依赖。
CMD ["node", "dist-server/index.mjs"]
