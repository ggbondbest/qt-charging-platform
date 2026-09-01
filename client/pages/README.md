# Client Pages

放置登录、站点、个人中心、充电、结算等页面。成员按功能创建独立文件，页面只调用
Client Services，不解析 TCP 帧、不访问数据库。顶层 target 只聚合子模块，成员 2 修改
`station/`，成员 3 修改 `profile_charging/`，不在本文件追加功能源文件。
