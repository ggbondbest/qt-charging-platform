# Server Network

放置 listener、ClientSession、帧 decoder 和 RequestDispatcher。网络对象只做非阻塞收发、
协议校验与路由；业务状态和事务交给 Server Services。当前 listener、`ClientSession` 和
`RequestDispatcher` 已接通 `USER_LOGIN`；其他动作继续按协议逐项注册。组长维护本模块
公共边界。
