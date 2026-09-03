# Profile/Charging Pages（成员 3）

个人中心、钱包、充值、订单、充电和结算页面放在本目录。

已实现：`ProfilePage`（"我的"Hub：身份头部进 `ProfileEditPage`、钱包横幅+
入口、订单四宫格带状态数量角标、更多服务占位禁用）、`ProfileEditPage`
（昵称编辑，头像上传待协议确认）、`WalletPage`（余额/充值入口/充值记录/
订单与个人中心入口）、`RechargePage`、`OrderListPage`（筛选+分页，支持外部
带筛选进入）、`OrderDetailPage`、`ChargingPage`（实时状态轮询+停止）、
`SettlementPage`（待支付/已支付）。

页面只依赖 `ClientProfileChargingServices` 的服务接口，不接触网络/SQL/业务
规则；真实接口放行前走 `MockRequestTransport`。`preview/preview_main.cpp` +
`charging-profile-preview` 仅为本地 mock 预览壳，不属于 `charging_client`
正式装配，接入导航机制后移除。
