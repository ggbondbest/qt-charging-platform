# 参赛文档可维护源

正式作品名：**充能智析 电动汽车充电平台与智能运营系统**。本目录保存 2026-10-19 参赛文档的可公开内容、图表源和事实索引；不含个人学号、手机号、私人环境配置或有效 Key。

| 内容源 | 生成文档 | 已检查页数 |
| --- | --- | --- |
| `source/content.json` | 项目说明、系统设计 | 5 + 5 |
| `source/handoff-content.json` | 部署文档、环境配置说明 | 3 + 3 |
| `source/additional-content.json` | 开源组件、模型、API与数据集使用说明；团队分工与既有基础说明 | 5 + 3 |

`source/build_documents.py` 使用 python-docx 和 Pillow。正文以 JSON 为准；三个 PNG / SVG 图均由生成器用基本几何与文字绘制。所有最终文档已移除带旧第三方人物素材的界面截图。

安装 `python-docx` 与 `Pillow` 后，从仓库根目录生成：

```sh
python3 docs/competition_submission/source/build_documents.py \
  --content docs/competition_submission/source/content.json \
  --output-dir /absolute/output/01_项目文档

python3 docs/competition_submission/source/build_documents.py \
  --content docs/competition_submission/source/additional-content.json \
  --output-dir /absolute/output/03_资源与团队
```

部署与环境文档同样可生成：

```sh
python3 docs/competition_submission/source/build_documents.py \
  --content docs/competition_submission/source/handoff-content.json \
  --output-dir /absolute/output/02_部署与环境
```

本次使用 Codex bundled Python 与 LibreOffice 进行生成和渲染。图表字体默认使用 macOS `/System/Library/Fonts/STHeiti Light.ttc`，文档字体为 Heiti SC；在其他系统生成时应选择可覆盖中文的字体并重新渲染验证。不要只用解压文本或页数判断排版。

`base_document()`、`table()`、`build()` 可作为配套材料的样式 helper。图像段落必须保持单倍行距，避免被正文固定行高裁切；标题无装饰边框。长表格保留重复表头并禁止行内跨页。

事实索引：

- `source/source-map.json`：项目与系统设计的代码依据、数据和验证边界。
- `source/resource-contribution-evidence.json`：公开组件、模型、资料、账号与贡献索引；按最新素材替换事实修订。
- `team-pr-evidence.json`：团队 PR 历史核验记录。
- `source/qa-manifest.json`：本次 DOCX 哈希、页数与逐页渲染检查记录；后续改稿后须重新生成。

整体许可尚待团队明确，Qt Charts 为 GPLv3 或商业许可。原参考样本的来源/授权链仍需补齐；原始样本不随包提供。原鲸娘图已替换，新的充电助手与上游代码许可边界见 `data_analysis/frontend/src/assets/PROVENANCE.md`。后续若这些状态变化，应同步更新源文档、运行包声明与验证记录。

报名表包含个人信息，未存入此公开目录。运行包、评审 Word 文件及内部渲染 PNG/PDF 由提交目录单独管理。
