"""Reviewed project knowledge with a small, local BM25 retrieval index.

These passages summarize the repository's published metric contracts. They
contain no sample totals, private paths, credentials, or individual records.
Source fields identify the maintained contract section, not a machine path.
"""
from collections import Counter
from copy import deepcopy
import math
import re


def _passage(key, title, text, document, section, topics):
    path = "data_analysis/ml/README.md" if document == "ml.md" else "data_analysis/docs/" + document
    return dict(id="knowledge." + key, title=title, text=text,
                source=path + " · " + section, topics=topics)


PASSAGES = (
    _passage("project", "项目数据与能力边界",
        "本项目是充电平台教学模拟。充电业务、车辆、费用与故障事件均为SIMULATED，不是真实城市运营数据，也不是ACN实测。"
        "数据先清洗，再由Spark生成聚合成果，服务将成果与当前数据库发布批次核对。在线问题只使用已发布聚合和项目说明，"
        "不能据模拟结果宣称真实城市盈利能力、设备安全结论或真实市场预测准确率。缺失成果应明确报错，不用示例数字填补。",
        "advanced_analytics.md", "1. 数据是否足够；4. 架构与复现", ("overview", "models")),
    _passage("scope", "日期范围、业务日和数据批次",
        "统计范围由页面筛选指定，同一数据集和发布批次不可混算。业务日期采用Asia/Shanghai时区和左闭右开[startDate,endDate)，"
        "结束日期当天不计入。正文提及其他日期、城市或站点，应先调整页面筛选，不能把整个筛选范围的值解释成单日或单站。"
        "电量按遥测区间日期，会话按命名的开始或结束日期，现金按支付发生日期；跨日时这些数量不必一致。",
        "operator_metrics.md", "2. 所有指标共同遵守的口径", ("overview", "bottlenecks", "stations", "behavior")),
    _passage("utilization", "充电利用率、电量与缺测",
        "页面完整小时充电利用率=完整小时内充电样本之和/完整小时内全部桩样本之和。先合计分子分母，不平均各站百分比。"
        "缺失遥测的小时不填零；分母为零时为空值，不是0%或100%。观测电量从Wh除以1000显示为kWh。"
        "平均单站小时功率使用完整小时电量除以完整站点小时数，不是全网同时峰值。正在充电、预约和充后占位是不同状态。",
        "advanced_analytics.md", "3. 关键统计边界 1–4", ("overview", "stations", "bottlenecks")),
    _passage("service", "服务成功率与失败原因的分母",
        "服务分析以所选日期内创建的充电尝试为群体，成功指批次最终开始充电。成功率的分母是对应入口或站型小时的尝试数；"
        "失败原因占比的分母是全部未成功尝试。无可用电桩、等待超出耐心、叫号未确认是不同原因，不合并成主动放弃。"
        "一次到访只有一次尝试，指标不是电气握手首次启动成功率；最终结局也不冒充历史某刻已知状态。",
        "advanced_analytics.md", "2. 服务机会；3. 关键统计边界 6", ("bottlenecks", "stations")),
    _passage("waiting", "等待、充后占位与因果限制",
        "总览平均排队等待按排队解决日期归属，没有可用等待事件时为空。服务小时成功率按尝试创建小时，等待按排队加入小时，"
        "完整会话占位按开始小时，利用率按同小时遥测，各指标使用不同事件群体。充后占位是停止供电后到拔枪的时间。"
        "并列出现高等待、高占位和低成功率只能提示复核方向，不能证明占位、接口配置或天气造成失败，也不能保证扩容收益。",
        "advanced_analytics.md", "3. 关键统计边界 3、11", ("bottlenecks", "stations", "overview")),
    _passage("comparison", "站点比较与小样本",
        "参谋只在至少30次尝试的电站中，按开始充电成功率从低到高列出至多三个待复核站。30次只是展示门槛，不是统计显著性检验，"
        "不保证统计可靠性。站型、设备供给和样本量不同，成功率低不等于设备故障或综合质量差。"
        "没有尝试的站不显示0%成功率。应同时查看尝试数、等待和完整小时利用率，再提出需要验证的运营方向。",
        "advanced_analytics.md", "2. 站型小时排查；3. 关键统计边界", ("stations", "bottlenecks")),
    _passage("behavior", "补能间隔与用户类型",
        "补能间隔是同一用户在本平台全网两次开始充电时间之差。先回看全批次历史前序，再筛选本次会话的日期、城市和电站，"
        "避免筛选造成伪首次观察。首次观察没有前序，不能计为零间隔。分布按会话或可观测间隔次数加权，不是去重用户占比；"
        "高频用户贡献更多记录。平台外补能不在此间隔内，观察结束后没有再次出现不能直接判为流失。",
        "advanced_analytics.md", "3. 关键统计边界 7–8", ("behavior",)),
    _passage("money", "净收款、电量和模拟成本",
        "净收款=成功收款−成功退款，不是净利润。订单应收与已收到现金不同；失败支付不计收入，退款不是负电量。"
        "金额以人民币分存储，展示人民币元时除以100。模拟费用、价格、成本是教学情景，不是运营商报价。"
        "没有检索到当前范围的对应金额聚合时，只能解释口径，不能推算或捏造营收数字。",
        "operator_metrics.md", "3. 营收、成本与经营表现", ("overview",)),
    _passage("models", "已接入模型各自回答什么",
        "小时平均负荷模型输出未来小时平均kW；小时末空闲桩模型的目标是小时末采样，不替代分钟到站库存。"
        "分钟到站与等待模型辅助到站后的服务判断。用户流失模型对历史观察日之后14天未回访进行风险排序；会话异常模型辅助复核已完成的TEST会话。"
        "网页模型状态和评测值以当前可信工件为准，模型未就绪应明确说明。页面日期与站点筛选不改变模型训练或评测范围。",
        "ml.md", "已整合的CPU机器学习模块；评估与使用边界", ("models",)),
    _passage("model_scores", "评测分数、概率和历史留出",
        "异常筛查精确率描述已告警测试样本中标注异常的占比；召回率描述标注异常测试样本中被筛出的占比。"
        "这些是固定历史留出样本评测，不是当前筛选城市或今天的准确率。异常分数不是故障概率，流失分数不是校准概率，"
        "没有超过阈值不代表不存在异常。模型不提供实时设备安全诊断；模拟数据上的成绩不保证现实部署表现。",
        "ml.md", "评估与使用边界", ("models",)),
)


def tokenize(text):
    """Latin words and overlapping Chinese bigrams need no external tokenizer."""
    tokens = re.findall(r"[a-z0-9_]+", text.casefold())
    for run in re.findall(r"[\u4e00-\u9fff]+", text):
        tokens.extend(run[index:index + 2] for index in range(len(run) - 1))
        if len(run) == 1:
            tokens.append(run)
    return tokens


_TERMS = tuple(Counter(tokenize(p["title"] + " " + p["title"] + " " + p["text"])) for p in PASSAGES)
_LENGTHS = tuple(sum(terms.values()) for terms in _TERMS)
_AVERAGE = sum(_LENGTHS) / len(_LENGTHS)
_FREQUENCIES = Counter(token for terms in _TERMS for token in terms)


def retrieve(question, topics=(), *, limit=4):
    """Rank actual lexical matches; topic boosts cannot retrieve unrelated text."""
    query = Counter(tokenize(question))
    ranked = []
    for passage, terms, length in zip(PASSAGES, _TERMS, _LENGTHS):
        score = 0.0
        for token, query_count in query.items():
            frequency = terms.get(token, 0)
            if not frequency:
                continue
            inverse = math.log(1 + (len(PASSAGES) - _FREQUENCIES[token] + .5) / (_FREQUENCIES[token] + .5))
            score += inverse * frequency * 2.2 / (frequency + 1.2 * (.25 + .75 * length / _AVERAGE)) * min(query_count, 2)
        if score:
            score *= 1 + .12 * len(set(topics).intersection(passage["topics"]))
            ranked.append((score, passage["id"], passage))
    ranked.sort(key=lambda row: (-row[0], row[1]))
    return [{key: deepcopy(value) for key, value in passage.items() if key != "topics"}
            for _, _, passage in ranked[:max(0, min(limit, 4))]]
