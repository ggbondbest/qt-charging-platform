# 交付接口补充：头像、订单恢复与预约到期

兼容 Qt Framework 6.2.4，继续使用现有 TCP v1 envelope、4 字节大端长度帧及会话身份。
本补充不增加独立数据库，也不向用户 TCP 开放管理员动作。

## 本地头像

`UPDATE_USER_INFO` 的 `avatarKey` 保留内置键，并支持 `data:image/png;base64,...`。

- 客户端将用户明确选择的本地图片缩小为 PNG，再发送；不上传本地路径。
- 服务端只接受 PNG，解码数据最多 128 KiB，宽高均不得超过 512 像素。
- 在读取像素前先检查图片头尺寸，拒绝伪造格式、损坏数据、非法 Base64 和超限图片。
- 校验失败返回 `INVALID_ARGUMENT`，`error.details.field = "avatarKey"`，整次资料更新不落库。
- 继续存于 `users.avatar_key`；登录、用户详情、资料更新返回同一个字段，重新登录后仍保留。
- 客户端仅将已验证的 data URI 或预置图标作为头像来源，不执行其中的内容。

## 未完成订单恢复

预约遇到该会话用户的 `RESERVED`、`CHARGING`、`WAITING_PAYMENT` 订单时，保留
`INVALID_STATE_TRANSITION` 错误码，并添加以下安全字段：

```json
{
  "reason": "UNFINISHED_ORDER",
  "orderId": "123",
  "reservationId": "45",
  "status": "WAITING_PAYMENT"
}
```

以上是 `error.details` 的内容。ID 均为十进制字符串，不返回别人的订单。
QML 在预约前查询当前未完成订单，按状态恢复预约、充电或结算；服务端仍以事务内校验为最终裁决，
防止页面预查后另一客户端又创建了订单。

## 预约到期

服务工作线程启动后和每秒执行到期清理，管理员请求前也清理到期数据。
清理由工作线程自己持有的 SQLite 连接完成，不在界面线程查询数据库。
`reservations.ACTIVE → EXPIRED`、对应 `orders.RESERVED → CANCELLED`、电桩恢复 `AVAILABLE`
在同一事务中提交；中途失败整体回滚。日志不向页面泄漏 SQL、路径或内部异常。
因此用户关闭客户端后，管理端再次查询或操作也不会依赖该用户重新上线触发清理。

## 验证

- `user_api_integration`：真实 TCP 上传头像、拒绝非法输入、不部分更新、重新登录与新连接读库一致。
- 同一测试覆盖三种未完成订单状态的恢复详情和会话隔离。
- `delivery_data_contract`：到期三表一致、未到期保留、失败回滚。
- `server_runtime`：服务工作线程后台到期及管理端查询一致性。
