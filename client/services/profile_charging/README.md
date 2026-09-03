# Profile/Charging Services（成员 3）

个人资料、充值、订单、充电与结算的页面用例放在本目录。

当前为 `STATIC` target（成员3 本地开发中，未提交）。已有：
`WalletService`（余额/充值/充值记录）、`IRequestTransport`（传输抽象，等待组长
ClientConnection 就绪后做适配器）、统一错误文案映射与展示格式化工具。

`mock_request_transport.*` 是**临时视觉开发件**：真实接口放行后由 transport 适配器
替换并删除，禁止进入正式应用装配。
