"""Station x hour-of-day empirical prior over free-charger counts.

The boosted distribution model is the interesting part of this task, but it is not automatically the
best MAE predictor: availability at a fixed station and a fixed hour is very stable in this batch,
so a lookup of "how often was station S free of n chargers at this clock hour during TRAIN" competes
with it head to head.  Rather than hide that baseline, the system uses it the way a hierarchical
model should - as a prior the model is shrunk towards, in proportion to how much local history
stands behind the prior (:meth:`HierarchicalForecastModel.distribution_for`):

    weight on the prior = n / (n + k)          weight on the model = k / (n + k)

where ``n`` is the number of TRAIN hours in the deepest cell that exists for the row.  A station
with a long history at that clock hour is believed; a station the batch never saw - a held-out
city, or one that just opened - has ``n = 0`` at every level and is served by the model alone.

Two details that are load-bearing here:

* the key of every cell is the **predicted** clock hour, ``(hour_of_day + step - 1) % 24``, so a
  cell means "station S was free of n chargers at 14:00".  ``label_available_count_hNN`` is the
  count at the end of the hour starting ``reference_time + (NN-1)h`` (verified against the raw
  hourly table by :mod:`data_analysis.ml.tests.test_ml_contract`).  Since each table is fitted for
  exactly one step, this is a convention about what a cell *means* more than a numeric correction -
  what it buys is that ``support`` counts hours of the answered clock hour and a borrowed site/city
  cell can be read as "what 14:00 looks like here".  The 0.2.0 script keyed on the row's own hour,
  which says something else and reads as a bug even where it computes the same number.
* the tables are fitted on TRAIN rows only, and a row no level has a cell for gets ``support = 0``,
  which under :func:`prior_weight` means *the model alone* - the fallback chain ends at the model,
  not at a pooled median of everybody else's stations.  This one was a real numeric error: 0.2.0
  passed ``n/(n+k)`` as the model's weight, so a station with no history fell back to the pooled
  table and a station with a long history ignored it.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
import pandas as pd

from data_analysis.ml.common.tasks import AVAILABILITY

#: Finest-first fallback chain; the deepest level with a TRAIN cell for the row wins.
KEY_LEVELS = (
    ("station", "station_id"),
    ("site", "site_key"),
    ("city", "city_id"),
)
#: Every level is keyed on the predicted clock hour, so the second key column is fixed.
HOUR_KEY = "answer_hour"
#: Level reported when no table has a cell for the row; its support is zero by construction.
GLOBAL_LEVEL = "global"
#: A pseudo-count so large that the prior contributes nothing: the honest default when a level
#: cannot be measured on a run's own validation rows.
NO_PRIOR = 1e12
#: Candidate pseudo-counts for the shrinkage weight, searched on VALIDATION per step and level.
#: The last entry is :data:`NO_PRIOR`, so "this level's table is not worth borrowing at all" is one
#: of the options the validation sweep can pick rather than something the code has to assume.
SUPPORT_GRID = (0.0, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0, 256.0, 512.0, 1024.0, NO_PRIOR)
#: Fewer validation rows than this at a level, and the level's weight is not estimated.
MIN_LEVEL_ROWS = 200


def answer_hour(hour_of_day, step: int) -> np.ndarray:
    """Local clock hour of the hour predicted by ``step`` (step 1 is the hour starting at t)."""
    return (np.asarray(hour_of_day, dtype=np.int64) + step - 1) % 24


#: Reserved column name holding the derived label.  It must never be mistaken for one of the
#: ``site_<type>`` one-hots it is derived from, because both readers scan the same prefix.
SITE_KEY = "site_key"


def _one_hot_columns(names) -> list[str]:
    return [name for name in names if name.startswith("site_") and name != SITE_KEY]


def site_labels(data: pd.DataFrame) -> pd.Series:
    """Recover a groupable site-type label from the station-profile one-hot block."""
    columns = sorted(_one_hot_columns(data.columns))
    if not columns:
        return pd.Series("unknown", index=data.index)
    indices = np.argmax(data[columns].to_numpy(dtype=float), axis=1)
    return pd.Series([columns[index] for index in indices], index=data.index).str[len("site_"):]


def add_site_key(data: pd.DataFrame) -> pd.DataFrame:
    data = data.copy()
    data[SITE_KEY] = site_labels(data)
    return data


def site_key_of(profile: dict) -> str:
    """Same label as :func:`site_labels`, read from one station's stored profile one-hots."""
    columns = sorted(_one_hot_columns(profile))
    if not columns:
        return "unknown"
    return max(columns, key=lambda key: float(profile[key] or 0.0))[len("site_"):]


def is_off(pseudo_count) -> bool:
    """True when a level's fitted pseudo-count disables the prior for that level's rows."""
    return float(pseudo_count) >= NO_PRIOR


def format_pseudo_count(pseudo_count) -> str:
    """:data:`NO_PRIOR` printed as ``off`` so reports never show a bare ``1e+12``."""
    return "off" if is_off(pseudo_count) else f"{float(pseudo_count):g}"


def prior_weight(support, pseudo_count: float) -> np.ndarray:
    """Empirical-Bayes shrinkage: a cell backed by ``n`` TRAIN hours carries ``n/(n+k)`` weight.

    ``k = 0`` trusts the lookup table completely, ``k = inf`` (or :data:`NO_PRIOR`) trusts the
    boosted model completely, and a cell with ``support = 0`` always lands on the model whatever
    ``k`` says - including ``k = 0``, where ``0/0`` would otherwise hand the blend a NaN pmf.
    """
    counts = np.asarray(support, dtype=float)
    if is_off(pseudo_count):
        return np.zeros(counts.shape, dtype=float)  # exactly the model, not the model plus 1e-10
    weight = np.zeros(counts.shape, dtype=float)
    return np.divide(counts, counts + float(pseudo_count), out=weight, where=counts > 0.0)


def prior_weights(support, levels, pseudo_counts: dict) -> np.ndarray:
    """:func:`prior_weight` with one pseudo-count per hierarchy level, chosen separately."""
    if not isinstance(levels, np.ndarray):
        levels = np.asarray(levels)
    support = np.asarray(support, dtype=float)
    weight = np.zeros(len(support), dtype=float)
    for level in np.unique(levels):
        rows = levels == level
        # One call per level keeps the zero-support rule of :func:`prior_weight` in a single place.
        weight[rows] = prior_weight(support[rows], pseudo_counts.get(str(level), NO_PRIOR))
    return weight


@dataclass
class HourCellPrior:
    classes: np.ndarray
    #: step -> level -> (probability mass table, TRAIN hours behind each cell)
    tables: dict[int, dict[str, tuple[pd.DataFrame, pd.Series]]] = field(default_factory=dict)
    #: step -> marginal label distribution, used when no level has a cell (``support = 0``)
    fallback: dict[int, np.ndarray] = field(default_factory=dict)

    @classmethod
    def fit(cls, train: pd.DataFrame, *, steps, classes) -> "HourCellPrior":
        classes = np.asarray(classes, dtype=float)
        prior = cls(classes=classes)
        for step in sorted(steps):
            label = AVAILABILITY.label_column(step)
            frame = train.assign(**{HOUR_KEY: answer_hour(train["hour_of_day"], step)})
            prior.tables[step] = {}
            for level, identity in KEY_LEVELS:
                counts = frame.groupby([identity, HOUR_KEY, label], observed=True).size().unstack(fill_value=0)
                counts.columns = pd.Index(counts.columns, dtype=float)
                counts = counts.reindex(columns=classes, fill_value=0)
                total = counts.sum(axis=1)
                mass = counts.div(total, axis=0)
                prior.tables[step][level] = (mass, total)
            prior.fallback[step] = (frame[label].value_counts(normalize=True)
                                     .reindex(pd.Index(classes), fill_value=0.0).to_numpy(dtype=float))
        return prior

    def probabilities(self, step: int, rows: pd.DataFrame) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        """Per-row pmf, the TRAIN hours behind it, and the name of the level each row resolved to.

        The third array is what makes the shrinkage honest: a row served from its own station cell
        and a row served from a pooled site cell carry very different amounts of evidence, so they
        must not be shrunk with one shared pseudo-count.
        """
        hours = answer_hour(rows["hour_of_day"].to_numpy(), step)
        result = np.full((len(rows), len(self.classes)), np.nan)
        support = np.zeros(len(rows), dtype=float)
        levels = np.full(len(rows), GLOBAL_LEVEL, dtype=object)
        outstanding = np.arange(len(rows))
        for level, identity in KEY_LEVELS:
            if not len(outstanding):
                break
            mass, total = self.tables[step][level]
            index = pd.MultiIndex.from_arrays([rows[identity].to_numpy()[outstanding], hours[outstanding]])
            looked_up = mass.reindex(index)
            totals = total.reindex(index)
            found = ~np.isnan(looked_up.to_numpy(dtype=float)).any(axis=1)
            hit = outstanding[found]
            result[hit] = looked_up.to_numpy(dtype=float)[found]
            support[hit] = totals.to_numpy(dtype=float)[found]
            levels[hit] = level
            outstanding = outstanding[~found]
        if len(outstanding):
            result[outstanding] = self.fallback[step]
        return result, support, levels

    def row(self, step: int, *, station_id: str, hour_of_day: int, site_key: str,
            city_id: str) -> tuple[np.ndarray, float, str]:
        """Serving-path lookup for one station-hour; ``support = 0`` means "trust the model"."""
        hour = int(answer_hour([hour_of_day], step)[0])
        values = {"station": (station_id, hour), "site": (site_key, hour), "city": (city_id, hour)}
        for level, identity in KEY_LEVELS:
            key = values[level]
            mass, total = self.tables[step][level]
            if key in mass.index:
                return mass.loc[key].to_numpy(dtype=float), float(total.loc[key]), level
        return self.fallback[step], 0.0, GLOBAL_LEVEL

    # -- summaries shared with the model and the evaluation script -------------------------------
    def median(self, probability: np.ndarray) -> np.ndarray:
        """MAE-optimal integer point forecast: the first class whose CDF reaches one half."""
        cumulative = np.cumsum(probability, axis=1)
        return self.classes[(cumulative < 0.5).sum(axis=1)]

    def mode(self, probability: np.ndarray) -> np.ndarray:
        return self.classes[np.argmax(probability, axis=1)]

    def expectation(self, probability: np.ndarray) -> np.ndarray:
        return probability @ self.classes

    def shortest_interval(self, probability: np.ndarray, level: float) -> tuple[np.ndarray, np.ndarray]:
        """Narrowest class window holding at least ``level`` predictive mass, vectorised over rows."""
        classes = np.asarray(self.classes, dtype=float)
        cumulative = np.cumsum(probability, axis=1)
        low = np.zeros(len(probability), dtype=float)
        high = np.zeros(len(probability), dtype=float)
        best_span = np.full(len(probability), classes[-1] - classes[0] + 1.0)
        for start in range(probability.shape[1]):
            for end in range(start, probability.shape[1]):
                mass = cumulative[:, end] - (cumulative[:, start - 1] if start else 0.0)
                span = classes[end] - classes[start]
                take = (mass + 1e-12 >= level) & (span < best_span)
                if take.any():
                    low[take], high[take], best_span[take] = classes[start], classes[end], span
        return low, high
