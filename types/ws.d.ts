/*
 * ws 的最小类型声明。
 *
 * ws 自己不带类型；@types/ws 在 moduleResolution: "bundler" 下也解析不到，因为 ws 的
 * exports 映射把 "." 指向 wrapper.mjs 且没有 types 条件（实测 `npm i -D @types/ws` 后
 * tsc 仍报 TS7016）。
 *
 * 我们只用它很小的一块：带自定义鉴权头建连、收二进制消息、发送、关闭。
 * 就按这个表面声明——写错方法名会立刻编译失败，比 `any` 更严，又不必为类型额外引依赖。
 */
declare module 'ws' {
  export interface ClientOptions {
    headers?: Record<string, string>;
    [key: string]: unknown;
  }
  export default class WebSocket {
    constructor(address: string, options?: ClientOptions);
    on(event: 'open' | 'close', listener: () => void): this;
    on(event: 'message', listener: (data: unknown) => void): this;
    on(event: 'error', listener: (error: Error) => void): this;
    send(data: unknown, callback?: (error?: Error) => void): void;
    close(code?: number, reason?: string): void;
  }
}
