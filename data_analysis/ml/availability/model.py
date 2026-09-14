"""Ordinal free-charger-count models shared by training, evaluation and inference.

``label_available_count_hXX`` only takes the integer values ``0..capacity`` (this batch has
exactly 3 chargers per station and 22.9% zero mass), so a regression on that variable optimises
the wrong thing and hands the page a fractional stock.  Both models here therefore keep the full
predictive distribution and derive everything else from it:

* contract point value = a whole number of chargers, chosen by :func:`point_value` under the
  model's ``point_rule`` (``median`` is the MAE-optimal rule, ``mode`` is what the 0.2.0 bundles
  actually shipped, so old bundles keep serving what their recorded metrics measured)
* reported RMSE uses the distribution expectation, which is the better squared-error target
* ``p_depletion`` = P(0 free chargers), the operational risk signal
* a shortest central interval whose mass is calibrated on the validation split

:class:`HierarchicalForecastModel` wraps the boosted classifier with the station x hour empirical
prior from :mod:`data_analysis.ml.availability.prior`.  It takes the same ``(x, step)`` arguments
as the plain model plus a ``keys`` frame carrying each row's ``station_id`` / ``hour_of_day`` /
``site_key`` / ``city_id``, so the serving path can drive either model the same way.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingClassifier

from data_analysis.ml.availability.prior import (
    KEY_LEVELS,
    MIN_LEVEL_ROWS,
    NO_PRIOR,
    SUPPORT_GRID,
    GLOBAL_LEVEL,
    HourCellPrior,
    format_pseudo_count,
    is_off,
    prior_weight,
    prior_weights,
)
from data_analysis.ml.common import metrics

LEVEL_GRID = np.round(np.arange(0.50, 0.995, 0.025), 4)
POINT_RULES = ("median", "mode")
#: Levels the mixture can be shrunk towards; ``global`` always has zero support, so it never is.
PRIOR_LEVELS = [level for level, _ in KEY_LEVELS] + [GLOBAL_LEVEL]
#: The budget every published bundle was fitted with (``ml_avail_run1`` 0.2.0, ``ml_avail_run2`` 0.3.0).
#: ``fit_step`` reproduces it exactly at ``rounds_multiplier=1.0``, so the only way to change a
#: shipped number is to ask for it out loud in the command and in the bundle's report.
BASE_PARAMS = {
    "max_iter": 250,
    "learning_rate": 0.06,
    "max_leaf_nodes": 31,
    "min_samples_leaf": 40,
    "early_stopping": True,
    "validation_fraction": 0.1,
    "n_iter_no_change": 15,
}


def scaled_rounds(multiplier: float) -> dict:
    """:data:`BASE_PARAMS` with the boosting budget stretched by ``multiplier``.

    Gradient boosting has no epochs - one "epoch" of it is one tree, and early stopping already ends
    the fit well before ``max_iter``.  The honest equivalent of "train five times longer" is
    therefore a larger round budget *and* a proportionally more patient stopping rule, otherwise the
    extra budget is never reached.  ``multiplier=1.0`` returns :data:`BASE_PARAMS` untouched.
    """
    if not multiplier > 0:
        raise ValueError(f"rounds multiplier must be positive, got {multiplier}")
    params = dict(BASE_PARAMS)
    if multiplier != 1.0:
        params["max_iter"] = max(1, int(round(params["max_iter"] * multiplier)))
        params["n_iter_no_change"] = max(1, int(round(params["n_iter_no_change"] * multiplier)))
    return params


def point_value(classes: np.ndarray, probability: np.ndarray, rule: str) -> np.ndarray:
    """Turn a predictive pmf into the integer number of chargers the contract asks for."""
    classes = np.asarray(classes, dtype=float)
    if rule == "median":
        cumulative = np.cumsum(probability, axis=1)
        return classes[(cumulative < 0.5).sum(axis=1)]
    if rule == "mode":
        return classes[np.argmax(probability, axis=1)]
    raise ValueError(f"unknown point rule {rule!r}, expected one of {POINT_RULES}")


def blend(model_mass: np.ndarray, prior_mass: np.ndarray, weight: np.ndarray) -> np.ndarray:
    """Row-wise pmf mixture; ``weight`` is the prior's share for that row (see :func:`prior_weights`)."""
    weight = np.asarray(weight, dtype=float)[:, None]
    blended = (1.0 - weight) * np.asarray(model_mass, dtype=float) + weight * np.asarray(prior_mass, dtype=float)
    return blended / blended.sum(axis=1, keepdims=True)


def choose_interval_level(model, step: int, probability: np.ndarray, truth: np.ndarray) -> dict:
    """Smallest interval level on :data:`LEVEL_GRID` that still reaches nominal coverage."""
    chosen = float(LEVEL_GRID[-1])
    for level in LEVEL_GRID:
        low, high = model.interval_for(probability, float(level))
        if np.mean((truth >= low) & (truth <= high)) >= model.nominal_coverage:
            chosen = float(level)
            break
    low, high = model.interval_for(probability, chosen)
    model.interval_levels[step] = chosen
    return {
        "level": chosen,
        "validationCoverage": float(np.mean((truth >= low) & (truth <= high))),
        "validationMeanWidth": float(np.mean(high - low)),
    }


@dataclass
class OrdinalForecastModel:
    classes: np.ndarray
    steps: dict[int, HistGradientBoostingClassifier]
    feature_columns: list[str]
    capacity: int
    interval_levels: dict[int, float] = field(default_factory=dict)
    nominal_coverage: float = 0.80
    #: bundles saved before 0.3.0 have no stored rule and behaved as ``mode``
    point_rule: str = "mode"

    def fit_step(self, step: int, x_train, y_train, seed: int, *,
                 rounds_multiplier: float = 1.0) -> None:
        """Fit one hour's classifier; ``rounds_multiplier`` is recorded in the report by the caller."""
        model = HistGradientBoostingClassifier(random_state=seed + step,
                                               **scaled_rounds(rounds_multiplier))
        model.fit(x_train, y_train)
        self.steps[step] = model

    @property
    def hierarchical(self) -> bool:
        return False

    def distribution(self, x, step: int) -> np.ndarray:
        return self.distribution_for(x, step)

    def distribution_for(self, x, step: int, keys: pd.DataFrame | None = None) -> np.ndarray:
        """Predictive mass over ``self.classes``, padded for classes absent from TRAIN."""
        trained = self.steps[step]
        raw = trained.predict_proba(x)
        full = np.zeros((raw.shape[0], len(self.classes)), dtype=float)
        positions = {int(value): index for index, value in enumerate(self.classes)}
        for index, value in enumerate(trained.classes_):
            full[:, positions[int(value)]] = raw[:, index]
        return full

    def median(self, x, step: int, keys: pd.DataFrame | None = None) -> np.ndarray:
        rule = getattr(self, "point_rule", "mode")
        return point_value(self.classes, self.distribution_for(x, step), rule)

    def expectation(self, x, step: int, keys: pd.DataFrame | None = None) -> np.ndarray:
        return self.distribution_for(x, step) @ self.classes

    def p_depletion(self, x, step: int, keys: pd.DataFrame | None = None) -> np.ndarray:
        probability = self.distribution_for(x, step)
        index = int(np.where(self.classes == 0)[0][0])
        return probability[:, index]

    def interval_for(self, probability: np.ndarray, level: float) -> tuple[np.ndarray, np.ndarray]:
        """Shortest integer interval holding at least ``level`` predictive mass."""
        counts = probability.shape[1]
        best_low = np.zeros(len(probability), dtype=int)
        best_high = np.zeros(len(probability), dtype=int)
        for row, mass in enumerate(probability):
            best = None
            for low in range(counts):
                for high in range(low, counts):
                    width = mass[low:high + 1].sum()
                    if width + 1e-12 < level:
                        continue
                    span = self.classes[high] - self.classes[low]
                    if best is None or span < best[0]:
                        best = (span, self.classes[low], self.classes[high])
            if best is None:
                best = (0, int(mass.argmax()), int(mass.argmax()))
            best_low[row], best_high[row] = best[1], best[2]
        return best_low, best_high

    def calibrate(self, x_validation, y_validation: dict[int, np.ndarray],
                  keys_validation: pd.DataFrame | None = None) -> dict[int, dict]:
        """Pick the smallest interval level per step that reaches nominal coverage on validation."""
        report = {}
        for step in sorted(self.steps):
            probability = self.distribution_for(x_validation, step)
            truth = np.asarray(y_validation[step], dtype=float)
            entry = choose_interval_level(self, step, probability, truth)
            entry.update({"pointRule": getattr(self, "point_rule", "mode"),
                          "pseudoCount": None,
                          "validationMae": metrics.mae(truth, self.median(x_validation, step)),
                          "modelMae": metrics.mae(truth, point_value(self.classes, probability,
                                                                     getattr(self, "point_rule", "mode"))),
                          "priorOnlyMae": None})
            report[step] = entry
        return report

    def to_dict(self) -> dict:
        return {
            "classes": self.classes.tolist(),
            "capacity": self.capacity,
            "featureColumns": list(self.feature_columns),
            "nominalCoverage": self.nominal_coverage,
            "pointRule": getattr(self, "point_rule", "mode"),
            "hierarchical": self.hierarchical,
            "pseudoCount": {str(step): None for step in self.steps},
            "intervalLevels": {str(key): value for key, value in self.interval_levels.items()},
        }


@dataclass
class HierarchicalForecastModel:
    """Boosted per-hour distribution, shrunk towards the station x hour empirical prior.

    Nothing is re-fitted: the classifier comes from ``train.py`` untouched, and the only free
    parameters are one pseudo-count ``k`` and one interval level per step, both chosen on
    VALIDATION.  ``k = inf`` degenerates to the pure model, so the wrapper can only move away from
    the model as far as local history justifies - and a station with no TRAIN cell in any level
    (``n = 0``) gets the model's own distribution whatever ``k`` is.
    """

    base: OrdinalForecastModel
    prior: HourCellPrior
    pseudo_count: dict[int, float] = field(default_factory=dict)
    interval_levels: dict[int, float] = field(default_factory=dict)
    point_rule: str = "median"
    diagnostics: dict[int, dict] = field(default_factory=dict)

    @property
    def classes(self) -> np.ndarray:
        return self.base.classes

    @property
    def capacity(self) -> int:
        return self.base.capacity

    @property
    def feature_columns(self) -> list[str]:
        return self.base.feature_columns

    @property
    def nominal_coverage(self) -> float:
        return self.base.nominal_coverage

    @property
    def steps(self) -> dict:
        return self.base.steps

    @property
    def hierarchical(self) -> bool:
        return True

    def mixture(self, model_mass: np.ndarray, step: int, keys: pd.DataFrame,
                pseudo_counts: dict | None = None) -> tuple[np.ndarray, dict]:
        """Blend the two pmfs row by row; ``keys`` supplies each row's cell and its support count."""
        prior_mass, support, levels = self.prior.probabilities(step, keys)
        counts = self.pseudo_count[step] if pseudo_counts is None else pseudo_counts
        if not counts:
            raise ValueError(f"step {step} has no fitted pseudo-counts; call calibrate first")
        weight = prior_weights(support, levels, counts)
        blended = blend(model_mass, prior_mass, weight)
        return blended, {
            "priorWeightMean": round(float(weight.mean()), 4),
            "priorWeightMin": round(float(weight.min()), 4),
            "priorWeightMax": round(float(weight.max()), 4),
            "levelRows": {str(level): int(np.sum(levels == level)) for level in np.unique(levels)},
            "supportMedian": float(np.median(support)),
            "zeroSupportShare": round(float(np.mean(support == 0)), 4),
        }

    def distribution_for(self, x, step: int, keys: pd.DataFrame | None = None) -> np.ndarray:
        if keys is None:
            raise ValueError("the hierarchical model needs station_id / hour_of_day / site_key / city_id "
                             "for every row; call it with keys")
        blended, diagnostics = self.mixture(self.base.distribution_for(x, step), step, keys)
        self.diagnostics[step] = diagnostics
        return blended

    def median(self, x, step: int, keys: pd.DataFrame | None = None) -> np.ndarray:
        return point_value(self.classes, self.distribution_for(x, step, keys), self.point_rule)

    def expectation(self, x, step: int, keys: pd.DataFrame | None = None) -> np.ndarray:
        return self.distribution_for(x, step, keys) @ self.classes

    def p_depletion(self, x, step: int, keys: pd.DataFrame | None = None) -> np.ndarray:
        probability = self.distribution_for(x, step, keys)
        index = int(np.where(self.classes == 0)[0][0])
        return probability[:, index]

    def interval_for(self, probability: np.ndarray, level: float) -> tuple[np.ndarray, np.ndarray]:
        return self.prior.shortest_interval(probability, level)

    def base_median(self, x, step: int) -> np.ndarray:
        """What the wrapped classifier alone would have said - kept for the audit tables."""
        return point_value(self.classes, self.base.distribution_for(x, step), self.point_rule)

    def calibrate(self, x_validation, keys_validation: pd.DataFrame,
                  y_validation: dict[int, np.ndarray]) -> dict[int, dict]:
        """Choose the point rule and one pseudo-count per hierarchy level, on VALIDATION only.

        Levels partition the rows, so minimising overall MAE decomposes into one search per level:
        a station's own cell, a pooled site cell and "no cell at all" are each trusted as far as
        their own validation evidence reaches, and ``off`` (:data:`NO_PRIOR`) is one of the offered
        values - a level whose table is worse than the model on its own validation rows is turned
        off rather than diluted.  A level with too few validation rows to say anything also gets
        :data:`NO_PRIOR`, and is why a bundle never silently leans on a prior it could not measure.
        """
        report = {}
        for step in sorted(self.steps):
            mass = self.base.distribution_for(x_validation, step)
            prior_mass, support, levels = self.prior.probabilities(step, keys_validation)
            truth = np.asarray(y_validation[step], dtype=float)
            # ``global`` rows have zero support, so their shrinkage weight is 0 for every k; tuning
            # them would only produce a meaningless "off or not" choice, so they are counted, not swept.
            groups = {level: np.flatnonzero(levels == level) for level in PRIOR_LEVELS
                      if level != GLOBAL_LEVEL and bool((levels == level).any())}
            candidates: list[tuple[float, str, dict, dict, dict, list]] = []
            for rule in POINT_RULES:
                counts, per_level, swept, thin = {}, {}, {}, []
                for level, index in groups.items():
                    # The whole curve is always computed, so a level that ends up switched off can
                    # still be reported with the evidence that switched it off.
                    sweep = {float(k): metrics.mae(
                                 truth[index],
                                 point_value(self.classes,
                                             blend(mass[index], prior_mass[index],
                                                   prior_weight(support[index], k)), rule))
                             for k in SUPPORT_GRID}
                    swept[level] = sweep
                    if len(index) < MIN_LEVEL_ROWS:
                        thin.append(level)
                        counts[level], per_level[level] = NO_PRIOR, float(sweep[NO_PRIOR])
                        continue
                    chosen, value = min(sweep.items(), key=lambda item: item[1])
                    counts[level], per_level[level] = float(chosen), float(value)
                for level in PRIOR_LEVELS:
                    counts.setdefault(level, NO_PRIOR)
                weight = prior_weights(support, levels, counts)
                total = metrics.mae(truth, point_value(self.classes, blend(mass, prior_mass, weight), rule))
                candidates.append((float(total), rule, dict(counts), dict(per_level), dict(swept), list(thin)))
            total, rule, counts, per_level, swept, thin = min(candidates, key=lambda item: item[0])
            self.point_rule = rule
            self.pseudo_count[step] = counts
            weight = prior_weights(support, levels, counts)
            probability = blend(mass, prior_mass, weight)
            entry = choose_interval_level(self, step, probability, truth)
            model_by_level = {level: metrics.mae(truth[index], point_value(self.classes, mass[index], rule))
                              for level, index in groups.items()}
            prior_by_level = {level: metrics.mae(truth[index], point_value(
                                  self.classes,
                                  blend(mass[index], prior_mass[index], np.ones(len(index))), rule))
                              for level, index in groups.items()}
            entry.update({
                "pointRule": rule,
                "pseudoCount": counts,
                "priorLevelsOff": sorted(level for level in PRIOR_LEVELS
                                         if is_off(counts.get(level, NO_PRIOR))),
                "thinLevels": sorted(thin),
                "validationMae": round(total, 4),
                "modelMae": round(metrics.mae(truth, point_value(self.classes, mass, rule)), 4),
                "priorOnlyMae": round(metrics.mae(
                    truth, point_value(self.classes, blend(mass, prior_mass, np.ones(len(mass))), rule)), 4),
                "validationRowsByLevel": {str(level): int(len(index)) for level, index in groups.items()},
                "unmatchedRows": int(np.sum(levels == GLOBAL_LEVEL)),
                "validationMaeByLevel": {str(level): round(value, 4) for level, value in per_level.items()},
                "modelMaeByLevel": {str(level): round(value, 4) for level, value in model_by_level.items()},
                "priorOnlyMaeByLevel": {str(level): round(value, 4) for level, value in prior_by_level.items()},
                "priorWeightMean": round(float(weight.mean()), 4),
                "supportMedian": float(np.median(support)),
                "zeroSupportShare": round(float(np.mean(support == 0)), 4),
                "curve": {f"{level}@{format_pseudo_count(k)}": round(value, 4)
                          for level, sweep in swept.items() for k, value in sweep.items()},
                "ruleTotals": {name: round(value, 4) for value, name, *_ in candidates},
            })
            report[step] = entry
        return report

    def to_dict(self) -> dict:
        return {
            "classes": self.classes.tolist(),
            "capacity": self.capacity,
            "featureColumns": list(self.feature_columns),
            "nominalCoverage": self.nominal_coverage,
            "pointRule": self.point_rule,
            "hierarchical": True,
            "pseudoCount": {str(step): value for step, value in self.pseudo_count.items()},
            "intervalLevels": {str(step): value for step, value in self.interval_levels.items()},
            "priorLevels": [level for level, _ in KEY_LEVELS],
        }
