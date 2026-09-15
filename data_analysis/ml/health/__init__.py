"""第九线：设备健康度 / 预测性维护（"这台桩未来 7 天内来修的概率"）。

模块：features 建桩×日稠密面板与四族 as-of 特征；train 选候选集定阈值出 bundle；
evaluate 盲测一次出报告；rolling 十轮滚动重训（段间 purge）；predict 在线口径入口。
"""
