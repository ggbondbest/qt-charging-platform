# 第三方软件与数据说明

充能智析的原创源代码采用 GPL-3.0-only，见 [LICENSE](LICENSE)。下列第三方组件、素材和数据保留各自的版权及许可。构建或分发时，应同时保留所使用版本附带的 LICENSE、COPYING、NOTICE 和版权声明；本文作为索引，不替代这些原始条款。

## 桌面端与数据库

| 组件 | 项目用途 | 许可与说明 |
| --- | --- | --- |
| Qt 6 | Qt Quick/QML 客户端、Widgets 管理端、网络与 SQL 接口 | 按模块采用 LGPLv3、GPLv3 或商业许可，参见 [Qt 许可说明](https://doc.qt.io/qt-6/licensing.html)。 |
| Qt Charts | 管理端原生统计图表 | 开源版本采用 GPLv3，参见 [Qt Charts](https://doc.qt.io/qt-6/qtcharts-index.html#licenses)。 |
| Qt WebEngine | 内嵌地图和 Vue 运营中心 | Qt 部分与 Chromium 第三方部分分别适用各自条款，参见 [Qt WebEngine 许可说明](https://doc.qt.io/qt-6/qtwebengine-licensing.html)。 |
| SQLite | 当前充电业务、订单、账本与设备数据 | 公有领域，参见 [SQLite Copyright](https://www.sqlite.org/copyright.html)。 |
| MySQL Community Server | 历史统计及策略业务数据库 | GPLv2 及其随附的附加许可；作为独立数据库服务，通过 PyMySQL 访问，参见 [MySQL 许可说明](https://www.mysql.com/about/legal/licensing/oem/)。 |
| Python | 数据处理、接口服务和模型运行 | PSF 许可及所附组件许可，参见 [Python License](https://docs.python.org/3.12/license.html)。 |

## 网页、接口与模型依赖

| 组件 | 主要许可 | 上游许可入口 |
| --- | --- | --- |
| Vue | MIT | [vuejs/core](https://github.com/vuejs/core/blob/main/LICENSE) |
| Vite | MIT | [vitejs/vite](https://github.com/vitejs/vite/blob/main/LICENSE) |
| TypeScript | Apache-2.0 | [microsoft/TypeScript](https://github.com/microsoft/TypeScript/blob/main/LICENSE.txt) |
| Apache ECharts | Apache-2.0 | [apache/echarts](https://github.com/apache/echarts/blob/6.0.0/LICENSE) |
| Leaflet | BSD-2-Clause | [Leaflet/Leaflet](https://github.com/Leaflet/Leaflet/blob/v1.9.4/LICENSE) |
| FastAPI、Pydantic、PyMySQL | MIT | [FastAPI](https://github.com/fastapi/fastapi/blob/master/LICENSE)、[Pydantic](https://github.com/pydantic/pydantic/blob/main/LICENSE)、[PyMySQL](https://github.com/PyMySQL/PyMySQL/blob/main/LICENSE) |
| Uvicorn、Starlette、HTTPX | BSD-3-Clause | [Uvicorn](https://github.com/encode/uvicorn/blob/main/LICENSE.md)、[Starlette](https://github.com/encode/starlette/blob/main/LICENSE.md)、[HTTPX](https://github.com/encode/httpx/blob/master/LICENSE.md) |
| Apache Spark / PySpark | Apache-2.0 | [apache/spark](https://github.com/apache/spark/blob/v3.5.6/LICENSE) |
| Apache Arrow / PyArrow | Apache-2.0 | [apache/arrow](https://github.com/apache/arrow/blob/apache-arrow-21.0.0/LICENSE.txt) |
| scikit-learn | BSD-3-Clause | [scikit-learn](https://github.com/scikit-learn/scikit-learn/blob/1.7.2/COPYING) |
| NumPy、pandas、SciPy | BSD-3-Clause | [NumPy](https://github.com/numpy/numpy/blob/main/LICENSE.txt)、[pandas](https://github.com/pandas-dev/pandas/blob/main/LICENSE)、[SciPy](https://github.com/scipy/scipy/blob/main/LICENSE.txt) |
| joblib、threadpoolctl | BSD-3-Clause | [joblib](https://github.com/joblib/joblib/blob/main/LICENSE.txt)、[threadpoolctl](https://github.com/joblib/threadpoolctl/blob/master/LICENSE) |

以上列出直接使用的主要组件。安装包中的传递依赖、原生计算库、字体及工具链附带组件仍按其随包声明授权。

## 图标与界面素材

- 客户端 Tabler 图标：MIT，保留 [LICENSE-tabler.txt](client/qml/assets/glyphs/LICENSE-tabler.txt)；上游为 [Tabler Icons](https://github.com/tabler/tabler-icons/blob/main/LICENSE)。
- 上游浮窗组件代码：MIT，保留 MeteorNOX 的版权声明，现有记录见 [whale-girl.LICENSE.txt](data_analysis/frontend/src/assets/whale-girl.LICENSE.txt)。该文件记录的是上游代码许可，不适用于 `whale-girl.png` 图片；图片也不在本项目原创源代码的 GPL-3.0-only 授权范围内。素材说明见 [上游 PROVENANCE](https://github.com/MeteorNOX/DeepSeek-Balance-Whale-Widget/blob/main/PROVENANCE.md)。

## 数据与外部服务

- **UrbanEV**：项目参考的数据集固定版本采用 CC0-1.0，见 [对应版本的 LICENSE](https://github.com/IntelligentSystemsLab/UrbanEV/blob/44f2aa0c8d89f192bce00bafb0def74a21b39c68/LICENSE)。数据集许可不替代项目源代码许可。
- **Open-Meteo 天气数据**：API 数据采用 CC BY 4.0，见 [数据许可](https://open-meteo.com/en/licence)。保留署名 “Weather data by Open-Meteo.com”。项目按所需地点、时间和字段选取数据，并进行 UTC 时间整理与 CSV 压缩存储。
- **ERA5**：天气数据中的再分析来源为 ECMWF / Copernicus Climate Change Service（C3S），保留该来源署名，参见 [ERA5 数据条目](https://doi.org/10.24381/cds.adbb2d47)。
- **OpenStreetMap**：使用其地图数据或瓦片时保留 “© OpenStreetMap contributors” 署名，并遵循 [数据许可及版权说明](https://www.openstreetmap.org/copyright)和相应瓦片服务条款。
- **腾讯地图与在线大模型 API**：按相应服务商的使用条款和账户权限调用。API 密钥和服务账户不因项目开源而转让，也不属于 GPL 授权对象。

## 分发说明

再分发本项目或所附第三方二进制时，保留对应版权、许可与 NOTICE，并按适用的 GPL/LGPL 条款提供相应版本的对应源码及必要构建信息。第三方组件的原有许可不因本仓库采用 GPL-3.0-only 而改变。
