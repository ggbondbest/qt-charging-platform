# Server Network

放置 listener、ClientSession、帧 decoder 和 RequestDispatcher。网络对象只做非阻塞收发、
协议校验与路由；业务状态和事务交给 Server Services。组长维护本模块公共边界。
