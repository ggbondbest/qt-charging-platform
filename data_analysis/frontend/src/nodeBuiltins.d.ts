// 仅为 advisorActions.test.ts 读对账 JSON 用到的三个 node 内置模块补最小声明。
// 不装 @types/node:避免动 package-lock(队友共用锁文件纪律)。运行时由 vitest(node)提供。
declare module "node:fs" {
  export function readFileSync(path: string, encoding: string): string;
}
declare module "node:path" {
  export function join(...parts: string[]): string;
  export function dirname(p: string): string;
}
declare module "node:url" {
  export function fileURLToPath(url: string | URL): string;
}
