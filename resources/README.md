# 共享资源约定

资源随真实功能 PR 创建，不预留大量空目录。统一分类为：

- `icons/`：SVG 图标；
- `images/`：界面图片；
- `qss/`：全局 token 与模块样式。

文件名使用 `lower_snake_case`，Qt resource alias 必须稳定。禁止提交地图 Key、本机
绝对路径、未授权素材或只在单一平台存在的字体。全局 QSS 变更属于高冲突契约，
需在 PR 中附 Client 和 Server 关键页面截图。
