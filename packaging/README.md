# macOS 本地评审包

`scripts/package_macos.py` 将已构建的 Qt 管理端、QML 客户端、Vue 成品、本地 Python 运行时、模型和独立 MySQL 演示种子组成一个可搬迁的 `.app`。接收方不需要 Qt Creator、Homebrew、Python、Node.js 或源码目录。

本次交付仅支持 **macOS 15 或以上、Apple Silicon（arm64）**，已在 macOS 15.6 验证。包内 Qt 为 6.12.0、Python 为 3.12.14、MySQL 为 8.4.11。没有提供或验证 Intel Mac、Windows 或 Linux 安装包。当前使用 ad-hoc 签名，未做 Apple 公证；分发后系统可能要求通过 Finder 的“打开”流程确认来源。

## 构建输入

在 Apple Silicon Mac 上准备以下输入，使用与包内二进制一致的版本：

- 已启用 WebEngine 的 Qt 构建目录，含 `server/charging-server.app` 与 `client/charging-client`；先构建 `charging_server`、`charging_client`。
- Qt SDK 的 `macos` 目录，含 `bin/macdeployqt`；SDK 的上级目录应有 Qt 源码许可文本和 Chromium 第三方 notices，安装根目录应有 `Licenses`。
- 可搬迁的 CPython 3.12 基础运行时，以及使用它创建、已安装项目完整依赖的虚拟环境。不能仅将依赖系统 Python 的 venv 当作基础运行时。
- MySQL 8.4 arm64 安装目录，包括 `bin/mysqld`、`share`、`lib/plugin` 与 `LICENSE`。脚本会递归收集并修正非系统动态库依赖。
- **新建、只含仓库模拟数据、已经正常关闭**的 MySQL 种子目录。不得复制开发者正在运行的数据目录或真实用户业务数据。
- 已生成的 `data_analysis/frontend/dist`、三个数据集目录 `analytics_full_180d_v1`、`advanced_analytics_v2`、`charging_full_180d_v2`，以及 `outputs/chargepilot`、`outputs/ml_load`、`outputs/ml_availability_delivery`、`outputs/ml_insights_delivery`。

MySQL 种子目录合同：

```text
mysql-seed/
  data/auto.cnf          # 完整且正常停机的数据目录
  environment.json      # 仅此演示库的新建账号与 ANALYTICS_/CHARGEPILOT_ MYSQL 参数
  status.json           # 至少包含 {"status":"READY_STOPPED"}
```

应先按 `data_analysis/docs/mysql_setup.md` 导入模拟分析数据，准备 ChargePilot 表、目录与回放状态，再验证四类模型及完整 API。两个演示账号分别用于只读历史分析和独立回放业务；不要放入管理员账号、现有账号凭据或 AI/map key。打包器只复制 `data` 与 `environment.json`，不会复制准备阶段的管理员配置。`READY_STOPPED` 是准备过程的显式交接记录，不代替真正的正常停机。

## 打包命令

以下路径变量由构建者设置为本机实际路径；输出目录必须尚不存在。打包机需要 Xcode Command Line Tools（`clang`、`otool`、`lipo`、`install_name_tool`、`codesign`）。

```sh
python3 scripts/package_macos.py \
  --build-dir "$QT_BUILD" \
  --qt "$QT_SDK/macos" \
  --python-base "$PORTABLE_PYTHON" \
  --venv "$PROJECT_VENV" \
  --mysql "$MYSQL_INSTALL" \
  --mysql-seed "$FRESH_DEMO_SEED" \
  --output "$PRIVATE_STAGE/充能智析_macOS15_AppleSilicon"
```

默认复制无凭据的 `config/services.example.json`。只有收到明确的私密交付授权后，才加 `--include-private-config`：它将本机 `config/services.local.json` **原样复制**到包内，不改用户文件、不把值写进源码、清单或日志，副本权限为 `0600`。带真实 key 的成品仅用于指定评委的本地交付，不能放进 Git、公开 Release 或公共下载目录。

脚本将 Qt/QML 插件、Python wheels 和 MySQL 原生依赖部署到 `.app/Contents`，去除开发机搜索路径，将 Mach-O 统一为 arm64，检查外链依赖与逃逸软链接，最后签名并验证。QtWebEngine helper 保留运行 Chromium/JIT 所需的 Qt SDK 权限。生成的 `package-manifest.json` 只记录版本、布尔校验及原生文件数量，不含凭据。

## 启动与生命周期

双击 `充能智析.app` 或同目录的 `启动评审.command`。原生 launcher 使用包内 Python 的隔离模式 `-I -B`，不依赖系统 Python 或当前工作目录。

首次启动会在 `~/Library/Application Support/ChargingPlatformReview/<包路径散列>/` 创建独立演示库副本；Qt SQLite、MySQL、配置、模型缓存与偏好均写入该状态目录，不修改签名后的应用包。默认管理账号为演示种子 `admin` / `123456`。客户端与管理端连接本次独立 TCP 端口，管理端自动托管本次 API 与 MySQL；端口按本机空闲端口选择。关闭管理端后，launcher 关闭自己的客户端并等待 API/MySQL 清理；不会停止其他用户实例。正常退出或验证成功且所属服务已停止才返回 `0`，失败返回非零。

第二次启动复用该包路径对应的演示状态。移动 `.app` 会使用新的状态目录，原状态保留。可以用 `--data-dir` 显式指定新的独立目录。不要让其他 MySQL 实例使用该目录；文件锁防止本启动器重复占用同一状态。

本地当前业务、历史统计、预测与回放可以离线运行。腾讯地图、在线 AI 及网络底图仍需要网络和有效配置；“依赖离线随包”不表示这些外部服务可以离线使用。回放种子默认暂停，演示时可在模拟控制台继续或推进时间。

## 移动目录验证

先将整个输出目录移到另一个路径，并从其他工作目录执行：

```sh
env PATH=/usr/bin:/bin /绝对路径/启动评审.command \
  --verify --data-dir /绝对路径/新的验证状态目录 \
  --report /绝对路径/移动目录验证.json
```

`--verify` 启动真实 Qt 管理端和客户端，检查当前业务快照、Vue 静态页面、四类模型状态、负荷/空闲预测、用户与异常洞察、高级历史统计及回放，再关闭本次进程并检查端口释放。验证期间使用状态目录下新建的 **AI 禁用、地图 key 为空**配置；包内真实配置不被修改，验证不调用在线 AI 或腾讯地图。可加 `--no-client` 省略客户端启动检查。GUI 渲染与页面交互仍需另做人工或自动界面验证，HTTP 通过不能代替浏览器渲染验证。

完成后再次执行签名检查，并用保留 macOS 属性和软链接的方式压缩：

```sh
codesign --verify --deep --strict /绝对路径/充能智析.app
ditto -c -k --sequesterRsrc --keepParent /绝对路径/评审目录 /绝对路径/私密评审包.zip
ditto -x -k /绝对路径/私密评审包.zip /绝对路径/新的解压目录
codesign --verify --deep --strict /绝对路径/新的解压目录/评审目录/充能智析.app
```

## 许可和资源

包内 `Resources/ThirdPartyLicenses` 保存 Qt 许可文本、Qt SPDX 与 Chromium 第三方 notices；MySQL 及原生依赖许可位于 `Resources/mysql/LICENSE` 和 `Resources/mysql/licenses`；Python 与 wheel 自带许可位于其运行时及 `site-packages/*.dist-info` 中。**Qt Charts 使用 GPLv3/商业许可，不能把所有 Qt 模块笼统写为 LGPL。** 各组件仍受各自许可约束；评审包不是新的商业授权。

本次约 1.8 GiB 展开空间，首次启动还需独立 MySQL 数据副本和运行缓存。建议准备至少 5 GiB 可用空间；精确压缩大小与 SHA-256 以实际交付清单为准。Python 并行计算线程在评审模式中限定为 2，降低普通评审电脑上的资源占用。
