# 充能智析可视化大屏

基于 Vue 3、Vite 和 ECharts 的第二阶段 Web 可视化端，包含：

- 智慧运营大屏：核心指标、小时负荷、城市贡献、净收款、站点排行和设备状态。
- 运营与数据质量：Spark 清洗质量、隔离原因和数据处理链路。
- AI 模型预测：按公共契约预留负荷和空闲桩数预测入口；模型未交付时明确显示未就绪。

页面会优先请求同源 `/api/v1`。FastAPI 未启动时进入“演示数据模式”，用于独立开发 UI；所有演示数据均明确标注为模拟数据。

## 在 VS Code 中启动

打开本目录，在终端执行：

```powershell
npm.cmd install
npm.cmd run dev
```

浏览器访问 `http://localhost:5173`。PowerShell 若拦截 `npm.ps1`，始终使用 `npm.cmd`。

## 连接 FastAPI

另开终端，从仓库根目录启动后端：

```powershell
python -m uvicorn data_analysis.backend.app:app --host 127.0.0.1 --port 8000
```

Vite 已将 `/api` 代理到 `http://127.0.0.1:8000`。MySQL 凭据只配置在后端，不写进本目录。

## 构建

```powershell
npm.cmd run build
```

构建产物在 `dist/`，该目录不应提交到 Git。
