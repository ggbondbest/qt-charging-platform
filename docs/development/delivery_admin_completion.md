# 管理端基本功能接入补充

本改动基于业务交付与数据契约补充，已同步包含合入 develop 的 PR #42
管理端功能，在其基础上补齐以下管理页面功能。

## 注册时间和单站在线率

- 用户列表和详情读 `createdAtUtc`（兼容 `createdAt`），固定转换到
  `Asia/Shanghai` 展示。缺少注册时间时显示 `—`，不以 `updatedAt` 代替。
- 电站列表“在线率 / 在线桩”列与详情读 `onlineChargerCount`、
  `onlineRatePercent`；该百分数直接显示，不再乘 100。
- 在线桩为非 OFFLINE，包含故障桩，和“可用电桩”是不同口径。

## Qt Charts 营收趋势

`DeliveryRevenueTrendWidget` 使用 `QChartView`、`QChart`、`QLineSeries`、
`QCategoryAxis`、`QValueAxis`。页面近 7 天/30 天操作会重新请求
`dashboard.get {days:7/30}`，图表仅绘制服务返回的 trend 数据。

金额由整数分转换为展示用元，完成订单维度使用服务返回的整数笔数。
成功响应中的空序列、不同长度、负值、无效/倒序日期会清空图形，不回退到 Mock 数据。
沿用上游刷新策略：定时刷新期间保留上次确认快照，请求失败时保留该快照并显示
错误提示；重新认证等显式清空刷新会先清除旧管理员的数据。
自定义日期按钮仍保持未开放；保留的组件方法只过滤已加载数据，不能生成新数据。

`DeliveryDeviceStatusWidget` 按 AVAILABLE 空闲、CHARGING 在用、FAULT 故障、
RESERVED 预约、OFFLINE 离线绘制五类互斥分布，外侧显示数量和总数占比。
“在线”与其中四类存在重叠，因此不作为状态分布的独立分类。

## 构建与验证

Ubuntu 22.04 构建依赖增加 `libqt6charts6-dev`，Qt 基线仍为 6.2.4。
保留 PR #42 的 `admin_management_pages` 测试，并扩展
`delivery_admin_pages` 测试，包含图表单测和真实
Gateway → Runtime → Service → Repository → SQLite 页面集成测试。

## 与 PR #42 的合并边界

保留上游的管理摘要、累计营收、真实电站新增及初始电桩、编辑参数校验和
跨刷新快照、按服务 ID 保留选中项、站内电桩跳转、筛选和当前页定时刷新。
包括目标站点不在首屏筛选选项中时的定点查询修复，以及跳转后可手动切换站点。
旧 `dashboard_visual_widgets.*` 的上游改动仍保留；正式 Dashboard 使用这里的
Qt Charts 实现，不恢复旧的三类状态图。

电站表沿用真实编号、名称、地址、总桩数、可用桩数、状态和操作，追加
“在线率 / 在线桩”；用户表保留真实字段布局，注册时间明确标注北京时间。
全局 `onlineRatio` 为 0–1，单站 `onlineRatePercent` 为 0–100，均包含故障桩。
种子数据全局在线为 6 / 7（85.7%），五类状态分布合计仍为 7，不能将“在线”
和“可用”互相代替。

合并回归同时执行两组管理页面测试以及 `admin_login`、`server_runtime`。
覆盖 Release 新增坐标/电价、编辑弹窗跨刷新、首屏外站点跳转、五态与全局在线
口径、零桩站点、注册时间跨 UTC 日期边界及 7/30 天曲线；未扩大业务操作范围。
