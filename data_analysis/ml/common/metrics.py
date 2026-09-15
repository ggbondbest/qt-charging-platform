"""Regression and risk metrics reported by both forecasting tasks."""

from __future__ import annotations

import numpy as np


def _pair(y_true, y_pred):
    truth = np.asarray(y_true, dtype=float)
    guess = np.asarray(y_pred, dtype=float)
    keep = np.isfinite(truth) & np.isfinite(guess)
    return truth[keep], guess[keep]


def mae(y_true, y_pred) -> float:
    truth, guess = _pair(y_true, y_pred)
    return float(np.mean(np.abs(truth - guess))) if truth.size else float("nan")


def rmse(y_true, y_pred) -> float:
    truth, guess = _pair(y_true, y_pred)
    return float(np.sqrt(np.mean((truth - guess) ** 2))) if truth.size else float("nan")


def pinball(y_true, y_pred, tau: float) -> float:
    """Quantile (pinball) loss for a single predicted quantile."""
    truth, guess = _pair(y_true, y_pred)
    if not truth.size:
        return float("nan")
    error = truth - guess
    return float(np.mean(np.maximum(tau * error, (tau - 1.0) * error)))


def coverage(y_true, lower, upper) -> float:
    """Empirical share of observations inside the interval (PICP)."""
    truth = np.asarray(y_true, dtype=float)
    low = np.asarray(lower, dtype=float)
    high = np.asarray(upper, dtype=float)
    keep = np.isfinite(truth) & np.isfinite(low) & np.isfinite(high)
    if not keep.any():
        return float("nan")
    return float(np.mean((truth[keep] >= low[keep]) & (truth[keep] <= high[keep])))


def interval_width(lower, upper) -> float:
    low = np.asarray(lower, dtype=float)
    high = np.asarray(upper, dtype=float)
    keep = np.isfinite(low) & np.isfinite(high)
    return float(np.mean(high[keep] - low[keep])) if keep.any() else float("nan")


def winkler(y_true, lower, upper, alpha: float) -> float:
    """Winkler score (mean interval width plus a penalty for each miss)."""
    truth = np.asarray(y_true, dtype=float)
    low = np.asarray(lower, dtype=float)
    high = np.asarray(upper, dtype=float)
    keep = np.isfinite(truth) & np.isfinite(low) & np.isfinite(high)
    if not keep.any():
        return float("nan")
    truth, low, high = truth[keep], low[keep], high[keep]
    width = high - low
    penalty = np.where(truth < low, (low - truth) / alpha, 0.0) + np.where(
        truth > high, (truth - high) / alpha, 0.0)
    return float(np.mean(width + penalty))


def legality(y_pred, lower_limit, upper_limit) -> float:
    """Share of predictions inside the physically valid range."""
    guess = np.asarray(y_pred, dtype=float)
    keep = np.isfinite(guess)
    if not keep.any():
        return float("nan")
    return float(np.mean((guess[keep] >= lower_limit) & (guess[keep] <= upper_limit)))


def roc_auc(y_true, score) -> float:
    truth = np.asarray(y_true, dtype=float)
    ranked = np.asarray(score, dtype=float)
    keep = np.isfinite(truth) & np.isfinite(ranked)
    truth, ranked = truth[keep], ranked[keep]
    positives, negatives = truth == 1, truth == 0
    if not positives.any() or not negatives.any():
        return float("nan")
    order = np.argsort(ranked, kind="mergesort")
    ranks = np.empty(len(ranked), dtype=float)
    ranks[order] = np.arange(1, len(ranked) + 1)
    # Midranks for ties keep the statistic honest when the score is a probability bucket.
    _, start, counts = np.unique(ranked, return_index=True, return_counts=True)
    for index, size in zip(start, counts):
        if size > 1:
            block = np.arange(index, index + size)
            ranks[order[block]] = ranks[order[block]].mean()
    sum_positive = ranks[positives].sum()
    count_positive, count_negative = int(positives.sum()), int(negatives.sum())
    return float((sum_positive - count_positive * (count_positive + 1) / 2) / (count_positive * count_negative))
