# 管理端基本功能接入补充

本改动基于业务交付与数据契约补充，补齐以下独立管理页面功能。

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
空响应、不同长度、负值、无效/倒序日期会清空图形，不回退到 Mock 数据。
自定义日期按钮仍保持未开放；保留的组件方法只过滤已加载数据，不能生成新数据。

`DeliveryDeviceStatusWidget` 按 AVAILABLE 空闲、CHARGING 在用、FAULT 故障、
RESERVED 预约、OFFLINE 离线绘制五类互斥分布，外侧显示数量和总数占比。
“在线”与其中四类存在重叠，因此不作为状态分布的独立分类。

## 构建与验证

Ubuntu 22.04 构建依赖增加 `qt6-charts-dev`，Qt 基线仍为 6.2.4。
新增 `delivery_admin_pages` 测试，包含图表单测和真实
Gateway → Runtime → Service → Repository → SQLite 页面集成测试。

本分支不修改旧 `dashboard_visual_widgets.*`；PR #42 负责的汇总卡片接入、
新增电站及原审查问题不在此改动范围。两个分支修改了部分相同页面，
合并时应保留这里的 Qt Charts、五类状态和新字段展示，重新跑页面测试。
