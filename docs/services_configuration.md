# AI 与腾讯地图统一配置

项目使用同一份 `config/services.local.json`，按两端职责读取：**Qt用户客户端只使用 `tencentMap`，Qt管理端的AI服务只使用 `ai`**。客户端正常启动或从Qt Creator运行即可读取地图配置，不需要Python启动脚本，也不需要将Key写进Vue。

## 填写配置

首次将 `config/services.example.json` 复制为同目录下的 `services.local.json`。如果本地文件已存在，直接编辑，保留原内容。

```json
{
  "version": 1,
  "ai": {
    "enabled": "auto",
    "apiKey": "",
    "baseUrl": "https://aiping.cn/api/v1",
    "model": "DeepSeek-V4.1-Flash",
    "providerLabel": "AIPing"
  },
  "tencentMap": {
    "webServiceKey": "",
    "jsKey": "",
    "secretKey": ""
  }
}
```

通常填写 `ai.apiKey` 和 `tencentMap.webServiceKey` 两项即可。JSON请使用英文双引号，不添加注释或末尾多余逗号。

| 字段 | 用途 |
| --- | --- |
| `ai.apiKey` | AI服务商提供的Key，供后端AI运营参谋调用 |
| `ai.baseUrl`、`ai.model`、`ai.providerLabel` | 模板沿用项目已有AIPing地址、模型和展示名称；使用其它服务商时一起修改，模型须与你的账户权限一致 |
| `ai.enabled` | `"auto"`表示填Key后启用；`false`明确关闭在线AI；未填Key时兼容原AI专用配置或使用本地问答 |
| `tencentMap.webServiceKey` | Qt用户客户端的腾讯WebService Key，供地址、距离和道路路线查询使用 |
| `tencentMap.jsKey` | 可选，客户端地图使用的JavaScript Key；留空时复用WebService Key，因此同一个Key需具有相应产品权限 |
| `tencentMap.secretKey` | 可选，腾讯控制台启用SN签名校验时填写对应SK |

AI地址须为HTTPS API根地址，不要包含末尾的 `/chat/completions`；目前使用兼容Chat Completions的服务协议。腾讯Key的产品权限、IP/域名限制与签名设置应与实际运行环境一致，参见 [腾讯WebService Key配置](https://lbs.qq.com/faq/serverFaq/webServiceKey) 和 [JavaScript地图入门](https://lbs.qq.com/webApi/javascriptGL/glGuide/glBasic)。Vue历史地图底图仍使用原有OpenStreetMap；服务端不使用这份文件中的腾讯地图配置。

## 使用配置

本次客户端代码更新后需先重新构建一次 `charging_client`。之后修改地图Key只需保存文件并重启客户端；修改AI Key后重启Qt管理端及其托管API，无需重新编译或构建网页。AI参谋需要已准备的完整历史服务；填写Key不会创建MySQL数据库、训练模型或将 `qt-live-only` 自动改为完整模式。

直接运行客户端二进制、Qt Creator运行客户端，以及可选的Widgets客户端均会读取地图配置。客户端不依赖管理端或Python来提供地图Key，仍需要连接Qt业务服务完成登录、预约等业务。

`python -m data_analysis.delivery.cli serve` 的AI参谋同样只读取 `ai`。全项目启动器 `scripts/launch_demo.py` 按角色传递环境：Qt管理端和分析API接收AI配置，用户客户端接收腾讯地图配置。

原客户端启动脚本保留为可选入口，方便选择构建目录和业务服务地址：

```bash
python scripts/launch_client.py --build-dir build/Qt_6_12_0_for_macOS_Debug
```

其它构建目录替换 `--build-dir`；可用 `--host 127.0.0.1 --port 9527` 连接对应业务服务。

历史找站实验默认继续使用情景估计。为兼容独立部署，ChargePilot保留显式设置后端 `TENCENT_MAP_API_KEY` / `TENCENT_MAP_SECRET_KEY` 的旧入口；它不会从统一文件获取客户端地图Key。演示启动器也不会把客户端地图变量传给后端。

## 优先级与排查

- AI配置优先级：进程中的同名环境变量、统一文件的 `ai`、原AI专用 `data_analysis/ml/advisor/.env.local`。已有显式空值也可能关闭对应功能。
- 客户端地图优先级：非空的 `TENCENT_MAP_API_KEY` / `TENCENT_MAP_SECRET_KEY`、旧版 `CHARGING_TENCENT_MAP_KEY` / `CHARGING_TENCENT_MAP_SECRET`、文件中的WebService Key / SK。JavaScript Key使用 `TENCENT_MAP_JS_KEY`，其次文件的 `jsKey`，最后复用最终WebService Key；原环境变量用法仍兼容。
- 客户端默认从程序目录及其上级寻找 `config/services.local.json`，找不到时使用编译时记录的源码仓库路径，适用于Qt Creator的外置构建目录，与启动工作目录无关。分发客户端时可将 `config/services.local.json` 放在程序旁的 `config` 目录；macOS也支持 `.app` 旁的 `config` 目录。构建产物只记录查找路径，不内嵌Key。
- Python服务默认按源码仓库定位配置。需要指定其它文件时，两端均可设置 `CHARGING_SERVICES_CONFIG` 为绝对路径；两个可选启动器也支持 `--services-config`。显式路径错误不会偷偷回退到其它文件。
- 检查在线AI配置可访问运行中服务的 `/api/v1/intelligence/advisor`，确认 `data.onlineAvailable`；此接口只返回能力状态，不返回Key，也不会发送模型请求。实际Key有效性、账户余额与权限需用自己的服务测试。
- 配置文件只解析字面量，不执行命令；只校验各自使用的配置段，AI字段错误不阻碍客户端地图，地图字段错误不阻碍服务端AI。整个JSON的语法或版本仍需正确。AI Key和腾讯SK不进入Vue静态文件，本地配置不提交Git。
