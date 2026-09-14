# 小时级空闲桩预测：整合版

此模块来自 PR70，与智能找桩的分钟级到站模型各司其职。使用同一已发布批次、同一电站编号，
负荷预测提供 kW，小时级空闲桩预测提供未来 1/6/24 小时的桩数和无桩风险；智能找桩仍使用到站时刻模型。

## 展示口径

- `timestamp` 是预测小时的起点，真正预测的是该小时最后一次采样 `sampleTimestamp`（hh:55）。
- `value` 是整数点预测；`expectedChargers` 是概率分布的预计值，不能当作实时库存。
- `probabilityNoFree` 是该时刻零空闲桩概率，不是排队成功率。
- `lower/upper` 来自 VALIDATION 上校准的区间；`intervalLevel` 是所取概率质量，
  `nominalIntervalCoverage=0.8` 是标称覆盖目标，实际 TEST 覆盖率在模型报告中另列。
- MAE 对整数点预测计算；RMSE 对分布期望计算，两者并非同一估计量。
- 所有成绩仅适用于模拟数据测试集，不代表真实运营效果。

## 首次训练

在仓库根目录、安装项目统一机器学习依赖后执行。使用 Python 3.11/3.12 和 CPU 即可，
不需要下载他人的 pickle 或申请 GPU。所有输出忽略于 Git；相同路径已完成时训练会拒绝覆盖。

```bash
python -m data_analysis.ml.availability.train --output data_analysis/outputs/ml_availability_delivery_base --horizons 1 6 24
python -m data_analysis.ml.availability.build_hierarchical --source-run data_analysis/outputs/ml_availability_delivery_base --output data_analysis/outputs/ml_availability_delivery
python -m data_analysis.ml.availability.predict --self-check --run-dir data_analysis/outputs/ml_availability_delivery
python -m unittest data_analysis.ml.tests.test_delivery_safety data_analysis.tests.test_availability_adapter
```

此交付命令只训练三个正式跨度，不重复训练历史研究中的冷城市留出模型。训练与层级先验均只在 TRAIN 拟合，
VALIDATION 选参数和区间，TEST 只评估，不根据 TEST 调参。`--resume` 只续接中断且配方一致的运行。

## 服务适配

`chargepilot.availability_adapter.AvailabilityAdapter(output_dir)`：

- `report`：状态、实际模型 ID 列表、批次、指标及目标定义；文件缺失为 `NOT_READY`，不填造假预测。
- `predict(station_id, reference_time, horizon_hours)`：真实模型结果、过去 24 小时、校准区间和无桩概率。
- 输入时间向下取整到完整小时，从不读取尚未结束的当前小时统计。

上线检查：先验证模型元数据、文件 SHA256、Python 主次版本和 scikit-learn 版本，再反序列化同一份字节；
SHA256 不是来源签名，仅允许加载本地可信训练产物。每个模型与导出批次、数据清单、特征、
站点容量绑定；输入历史必须属于同一站点、容量一致、连续完整且不含未来。缓存命中也会重新验证源分片，
损坏缓存只从通过校验的源数据重建。异批次模型不能通过重新包装冒充新模型。

`REPORT.md` 为成员历史实验记录；统一前端读取本次训练产物中的实际指标，不把历史报告数字冒充当前推理成绩。
