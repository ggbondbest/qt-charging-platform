"""Contract and leakage tests for the availability forecaster.

Six claims the handoff document demands are checked mechanically here instead of asserted in a
slide: the online feature transform equals the Spark-generated export column-for-column; every
shipped bundle is bound to the raw dataset manifest and published batch it was trained on; the
model inputs never contain a ``label_`` or ``split_`` column; no future information can reach a
prediction, because the exported lag block is built strictly from hours before ``reference_time``;
the station x hour prior leans on a station's history in proportion to that history and
falls back to the model alone where there is none - the direction the 0.2.0 audit script had the
wrong way round; and every shipped artefact passes the minimal inference test, on the values it
actually serves rather than the metrics it happens to record.

The ``generator`` CI job runs ``unittest discover`` with no scientific stack installed, so every
test is guarded by :func:`_depends_available` and the module skips instead of failing.
"""

from __future__ import annotations

import importlib.util
import json
import math
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock
from dataclasses import replace
from pathlib import Path

DATA_ANALYSIS = Path(__file__).resolve().parents[2]
EXPORT_DIR = DATA_ANALYSIS / "datasets" / "analytics_full_180d_v1"
#: The run directories that are authoritative for the batch currently in the repository.  A data
#: re-publication mints a new ``publishedBatchId`` and ``predict`` then refuses the old bundles, so
#: these names move with the batch; ``RunPointerTest`` below fails if they drift apart from the
#: ``BASE_RUN`` / ``HIERARCHY_RUN`` constants the modules themselves use as defaults.
RUN_NAMES = ("ml_availability_delivery_base", "ml_availability_delivery")
#: Every published run is checked the same way, but a run missing from this machine is silently
#: omitted from the bundle iteration (``_bundle_directories``) rather than failing - which is only
#: safe because ``RunPointerTest`` below stops a half-present pair from passing for a complete one.
RUN_DIRS = [DATA_ANALYSIS / "outputs" / name for name in RUN_NAMES]
#: Superseded runs, trained against ``analytics-5f8e9342…``.  Kept pointed at by name because two
#: resume cases exist precisely to prove the guards work on artefacts that predate the resume
#: record, and measured on disk, only these do: ``ml_avail_run2`` is the one published run whose
#: bundles carry no ``seed`` (run1's record 20260913), and neither legacy run records
#: ``payloadEntry`` - that field is written by ``build_hierarchical.py``, so only ``ml_avail_run6``
#: has it.  Nothing newer can drive those branches.
LEGACY_RUN_NAMES = ("ml_avail_run1", "ml_avail_run2")

# Exported doubles travel through Spark's CSV text representation, so parity is asserted to the
# precision of that round-trip rather than to the last bit of the IEEE-754 result.
PARITY_RTOL = 1e-9
PARITY_ATOL = 1e-9


def _depends_available() -> bool:
    return all(importlib.util.find_spec(name) for name in ("pandas", "numpy", "sklearn", "joblib"))


requires_stack = unittest.skipUnless(_depends_available(), "pandas/numpy/scikit-learn/joblib not installed")
requires_run = unittest.skipUnless(any(directory.exists() for directory in RUN_DIRS),
                                   "no trained run under data_analysis/outputs; run ml.availability.train")
#: the legacy-compat resume cases need one bundle of each generation: a base estimator and the
#: wrapped version of it, both from the superseded batch, because ``ml_avail_run2`` is the only
#: published run that carries no ``seed`` and neither legacy run carries a ``payloadEntry``
requires_legacy_runs = unittest.skipUnless(all(
    (DATA_ANALYSIS / "outputs" / run / "h01" / "model_metadata.json").exists() for run in
    LEGACY_RUN_NAMES), f"needs {' and '.join(LEGACY_RUN_NAMES)} on disk (superseded batch)")
#: the end-to-end tests below train, which needs the exported batch itself, not just a published run
requires_export = unittest.skipUnless((EXPORT_DIR / "serving_manifest.json").exists(),
                                      f"no exported batch at {EXPORT_DIR}")

if _depends_available():
    import numpy as np
    import pandas as pd

    from data_analysis.contracts.model import PredictionContext, validate_prediction
    from data_analysis.ml.availability import BASE_RUN, HIERARCHY_RUN
    from data_analysis.ml.availability import build_hierarchical, evaluate, predict, train
    from data_analysis.ml.availability import model as model_module
    from data_analysis.ml.availability.model import blend, point_value
    from data_analysis.ml.availability.prior import (
        GLOBAL_LEVEL,
        NO_PRIOR,
        HourCellPrior,
        add_site_key,
        answer_hour,
        is_off,
        prior_weight,
        prior_weights,
        site_key_of,
    )
    from data_analysis.ml.availability.predict import AvailabilityForecaster
    from data_analysis.ml.common import artifacts, features as feat, forecaster
    from data_analysis.ml.common.tasks import AVAILABILITY


def _history(hourly: "pd.DataFrame", station_id: str, reference_time: "pd.Timestamp") -> list[dict]:
    window = hourly[(hourly["station_id"] == station_id)
                    & (hourly["recorded_at"] >= reference_time - pd.Timedelta(hours=24))
                    & (hourly["recorded_at"] < reference_time)]
    return window.sort_values("recorded_at").to_dict("records")


def _bundle_directories() -> list[tuple[Path, Path]]:
    """``(run directory, bundle directory)`` for every saved bundle of every trained run."""
    return [(run, directory)
            for run in RUN_DIRS if run.exists()
            for directory in artifacts.bundle_directories(run)]


def _label(run: Path, bundle: Path) -> str:
    return f"{run.name}/{bundle.relative_to(run).as_posix()}"


@requires_stack
class FeatureTransformTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.frame = forecaster.build_frame(EXPORT_DIR)

    def test_online_transform_matches_the_export(self):
        """The 24 raw history rows must reproduce every exported feature column, not just the lags."""
        offenders: list[str] = []
        checked = 0
        for station_id, group in self.frame.data.groupby("station_id"):
            for row in group.iloc[np.linspace(0, len(group) - 1, num=2, dtype=int)].to_dict("records"):
                history = _history(self.frame.hourly, station_id, row["reference_time"])
                self.assertEqual(len(history), 24, f"{station_id} has no 24h window behind a published point")
                derived = feat.derive_features(
                    history,
                    station=self.frame.stations[station_id],
                    reference_time=row["reference_time"],
                    calendar=self.frame.calendar,
                    cities=self.frame.cities,
                )
                for column in (*feat.POWER_LAGS, *feat.EXPORT_ROLLINGS, "last_available_count",
                               *feat.AVAILABLE_LAGS, "rolling_mean_available_24h"):
                    expected, actual = float(row[column]), float(derived[column])
                    if not math.isclose(expected, actual, rel_tol=PARITY_RTOL, abs_tol=PARITY_ATOL):
                        offenders.append(f"{station_id}@{row['reference_time']:%H:%M} {column}: "
                                         f"export {expected!r} != online {actual!r}")
                for column in ("hour_of_day", "day_of_week"):
                    self.assertEqual(int(row[column]), derived[column], f"{station_id} {column}")
                self.assertEqual(str(row["business_date"]), derived["business_date"], station_id)
                self.assertEqual(int(row["is_weekend"]), int(derived["is_weekend"]), station_id)
                self.assertEqual(int(row["is_public_holiday"]), int(derived["is_public_holiday"]), station_id)
                self.assertEqual(int(row["is_adjusted_workday"]), int(derived["is_adjusted_workday"]), station_id)
                for city in self.frame.cities:
                    self.assertEqual(int(row[f"city_{city.lower()}"]), derived[f"city_{city.lower()}"], city)
                checked += 1
        self.assertEqual(checked, 50, "two prediction points per station were expected")
        self.assertEqual(offenders[:5], [], f"{len(offenders)} offline/online feature mismatches")

    def test_the_lag_window_ends_one_hour_before_the_first_answer(self):
        """``lag_available_h01`` is the hour ending at reference_time, never the answer hour itself."""
        sample = self.frame.data.sample(400, random_state=7)
        hourly = self.frame.hourly[["station_id", "recorded_at", "end_available_count"]]
        behind = hourly.rename(columns={"recorded_at": "reference_time",
                                        "end_available_count": "free_at_hour_ending_at_t"}).assign(
            reference_time=lambda frame_: frame_["reference_time"] + pd.Timedelta(hours=1))
        ahead = hourly.rename(columns={"recorded_at": "reference_time",
                                       "end_available_count": "free_during_answer_hour"})
        lagged = sample.merge(behind, on=["station_id", "reference_time"], how="inner")
        aligned = sample.merge(ahead, on=["station_id", "reference_time"], how="inner")
        self.assertEqual(len(lagged), len(sample), "some sampled points have no hour ending at reference_time")
        np.testing.assert_array_equal(lagged["lag_available_h01"].to_numpy(dtype=float),
                                      lagged["free_at_hour_ending_at_t"].to_numpy(dtype=float))
        agreement = (aligned["lag_available_h01"].to_numpy(dtype=float)
                     == aligned["free_during_answer_hour"].to_numpy(dtype=float))
        self.assertLess(agreement.mean(), 0.9,
                        "the exported lag block is indistinguishable from reading the answer hour")

    def test_incomplete_or_invalid_history_is_refused(self):
        row = self.frame.data.iloc[5]
        history = _history(self.frame.hourly, row["station_id"], row["reference_time"])
        station = self.frame.stations[row["station_id"]]
        # The same 24 rows one hour later: still 24, still contiguous, still valid - but it
        # contains the first answer hour, so the transform must reject the whole request.
        shifted = _history(self.frame.hourly, row["station_id"], row["reference_time"] + pd.Timedelta(hours=1))
        self.assertEqual(len(shifted), 24)
        cases = (
            (history[:-1], "24 history rows"),
            (history + [dict(history[0], recorded_at=row["reference_time"])], "24 history rows"),
            (shifted, "not contiguous"),
            ([dict(item) for item in history][:-1] + [dict(history[0])], "not contiguous"),
            ([dict(item, is_complete=False) for item in history], "not complete and valid"),
        )
        for broken, message in cases:
            with self.assertRaisesRegex(feat.HistoryError, message):
                feat.derive_features(broken, station=station, reference_time=row["reference_time"],
                                     calendar=self.frame.calendar, cities=self.frame.cities)

    def test_the_label_is_the_free_count_of_the_hour_it_answers(self):
        """``label_available_count_hNN`` is ``end_available_count`` of hour ``t + (NN-1)h``.

        :func:`answer_hour` keys the station x hour prior on this, and the serving path would
        quietly look up the wrong hour if it were off by one, so the alignment is pinned here
        against the raw hourly table rather than argued in prose.
        """
        hourly = self.frame.hourly
        labels = (self.frame.data.drop_duplicates("reference_time")
                  .sample(400, random_state=11).to_dict("records"))
        window = hourly[hourly["station_id"].isin({row["station_id"] for row in labels})]
        counts = {(station, recorded): float(count) for station, recorded, count in zip(
            window["station_id"].tolist(), window["recorded_at"].tolist(),
            window["end_available_count"].tolist())}
        for step in (1, 2, 6, 24):
            label = AVAILABILITY.label_column(step)
            checked = agreed = 0
            wrong_checked = wrong_agreed = 0
            for row in labels:
                expected = float(row[label])
                answer = (row["station_id"], row["reference_time"] + pd.Timedelta(hours=step - 1))
                late = (row["station_id"], row["reference_time"] + pd.Timedelta(hours=step))
                if answer in counts:
                    checked += 1
                    agreed += counts[answer] == expected
                if late in counts:
                    wrong_checked += 1
                    wrong_agreed += counts[late] == expected
            self.assertGreater(checked, 300, f"only {checked} points have an h{step:02d} answer hour")
            self.assertEqual(agreed, checked,
                             f"label_available_count_h{step:02d} disagrees with the raw hourly count "
                             f"for {checked - agreed} of {checked} sampled points")
            if wrong_checked > 50:
                self.assertLess(wrong_agreed / wrong_checked, 0.95,
                                f"the label is indistinguishable from the hour after its answer at h{step:02d}")
        # ``hour_of_day`` is the feature the prior keys its cells on, so it has to be one fixed
        # clock ahead of ``reference_time`` - a per-row drift would silently mis-key every cell.
        offsets: dict[str, set[int]] = {}
        for row in labels:
            offsets.setdefault(row["city_id"], set()).add((int(row["hour_of_day"])
                                                           - row["reference_time"].hour) % 24)
        for city, values in offsets.items():
            self.assertEqual(len(values), 1, f"{city}: hour_of_day drifts against reference_time")

    def test_no_label_or_split_column_is_an_input(self):
        """Labels and split tags exist in the frame, and none of them are model inputs."""
        inputs = set(self.frame.feature_columns)
        labels = {column for column in self.frame.data.columns if column.startswith("label_")}
        splits = {column for column in self.frame.data.columns if column.startswith("split_")}
        self.assertTrue(labels, "the joined target table disappeared from the frame")
        self.assertEqual(splits, {"split_1h", "split_6h", "split_24h"})
        self.assertFalse(inputs & labels, inputs & labels)
        self.assertFalse(inputs & splits, inputs & splits)
        for column in inputs:
            self.assertNotIn("weather", column)
            self.assertNotIn("label", column)


@requires_stack
class PriorShrinkageTest(unittest.TestCase):
    """The lookup-table half of the model, on a synthetic frame with no export needed.

    These pin the two things the 0.2.0 audit script got backwards - which side of the blend a
    long-standing station should get, and which clock hour a step's prior belongs to - because both
    failures are silent: they leave every headline number plausible.
    """

    DAYS = 5

    def setUp(self):
        def count_at(hour: int) -> int:
            return 0 if hour % 24 in (0, 1, 2) else 3

        rows = []
        for day in range(self.DAYS):
            for hour in range(24):
                for station, city in (("A", "X"), ("B", "X")):
                    rows.append({
                        "station_id": station,
                        "hour_of_day": hour,
                        "city_id": city,
                        "site_depots": 1,
                        "site_malls": 0,
                        "label_available_count_h01": count_at(hour),
                        "label_available_count_h02": count_at(hour + 1),
                    })
        self.classes = np.array([0.0, 1.0, 2.0, 3.0])
        self.data = add_site_key(pd.DataFrame(rows))
        self.prior = HourCellPrior.fit(self.data, steps=[1, 2], classes=self.classes)

    def _mass(self, step: int, hour: int, station: str = "A") -> np.ndarray:
        row = self.data[(self.data["station_id"] == station)
                        & (self.data["hour_of_day"] == hour)].iloc[:1]
        mass, support, level = self.prior.probabilities(step, row)
        self.assertEqual(level[0], "station")
        return mass[0]

    def test_a_cell_is_keyed_on_the_hour_it_describes(self):
        """Two ways of reaching 00:00 - tonight's step 1 and 23:00's step 2 - read the same cell.

        This is what makes ``support`` mean "TRAIN hours of that clock hour" and a borrowed
        site/city cell mean "what 14:00 looks like here" rather than "what rows taken at 13:00
        looked like"; the 0.2.0 audit script keyed on the row's own hour, which leaves a single
        step's numbers alone but quietly changes what those two quantities count.
        """
        self.assertEqual(int(answer_hour(23, 2)), 0, "the hour key must wrap midnight")
        self.assertTrue(np.allclose(self._mass(1, 0), self._mass(2, 23)),
                        "the h02 prior is keyed on the row's hour, not on the hour it answers")
        np.testing.assert_allclose(self._mass(1, 0), [1.0, 0.0, 0.0, 0.0])
        np.testing.assert_allclose(self._mass(1, 9), [0.0, 0.0, 0.0, 1.0])

    def test_support_counts_TRAIN_hours_not_rows_in_the_world(self):
        _, support, level = self.prior.probabilities(1, self.data.iloc[:3])
        self.assertEqual(list(level), ["station"] * 3)
        np.testing.assert_array_equal(support, np.full(3, self.DAYS, dtype=float))

    def test_more_history_leans_on_the_table_not_on_the_model(self):
        """``n/(n+k)`` is the *prior's* weight: a station with 900 hours must be believed."""
        weight = prior_weight([0, 10, 900], pseudo_count=100)
        np.testing.assert_allclose(weight, [0.0, 10 / 110, 900 / 1000])
        self.assertGreater(weight[2], weight[1])
        # ...and a cell with no TRAIN hours falls back to the model however large k is.
        np.testing.assert_array_equal(prior_weight([0, 0, 0], pseudo_count=0.0), [0.0, 0.0, 0.0])
        self.assertTrue(is_off(NO_PRIOR))
        np.testing.assert_allclose(prior_weight([900], NO_PRIOR), [0.0])

    def test_levels_are_shrunk_with_their_own_pseudo_count(self):
        support = np.array([100.0, 100.0])
        levels = np.array(["station", "site"], dtype=object)
        weights = prior_weights(support, levels, {"station": 100.0, "site": NO_PRIOR})
        np.testing.assert_allclose(weights, [0.5, 0.0], atol=1e-9)

    def test_a_row_no_level_has_never_seen_is_served_by_the_model(self):
        unseen = self.data.iloc[:1].assign(station_id="Z", city_id="Q", site_key="rooftop")
        mass, support, level = self.prior.probabilities(1, unseen)
        self.assertEqual(level[0], GLOBAL_LEVEL)
        self.assertEqual(float(support[0]), 0.0)
        self.assertAlmostEqual(float(mass[0].sum()), 1.0)

    def test_a_borrowed_cell_is_pooled_over_the_station_that_is_missing(self):
        unseen_station = self.data.iloc[:3].assign(station_id="Z")
        mass, support, level = self.prior.probabilities(1, unseen_station)
        self.assertEqual(set(level), {"site"})
        # site X's two stations are identical here, so the pooled cell keeps the same shape but
        # carries twice the hours - which is what makes its weight legitimate rather than assumed.
        np.testing.assert_allclose(mass, np.tile([1.0, 0.0, 0.0, 0.0], (3, 1)))
        np.testing.assert_array_equal(support, np.full(3, 2 * self.DAYS, dtype=float))

    def test_the_median_rule_beats_the_mode_on_a_tied_distribution(self):
        """0.2.0 shipped argmax under the name ``median``; the two disagree on skewed pmfs."""
        pmf = np.array([[0.30, 0.0, 0.0, 0.70],     # mode 3, median 3
                        [0.49, 0.02, 0.0, 0.49],    # argmax ties low, the CDF does not
                        [0.40, 0.0, 0.25, 0.35]])
        self.assertEqual(list(point_value(self.classes, pmf, "mode")), [3.0, 0.0, 0.0])
        self.assertEqual(list(point_value(self.classes, pmf, "median")), [3.0, 1.0, 2.0])
        with self.assertRaises(ValueError):
            point_value(self.classes, pmf, "mean")

    def test_the_blend_is_a_pmf_and_hits_its_endpoints(self):
        model = np.array([[0.9, 0.1], [0.5, 0.5]])
        table = np.array([[0.0, 1.0], [1.0, 0.0]])
        np.testing.assert_allclose(blend(model, table, np.array([0.0, 0.0]))[:, 0], [0.9, 0.5])
        np.testing.assert_allclose(blend(model, table, np.array([1.0, 1.0]))[:, 0], [0.0, 1.0])
        middle = blend(model, table, np.array([0.5, 0.5]))
        np.testing.assert_allclose(middle.sum(axis=1), [1.0, 1.0])
        np.testing.assert_allclose(middle[:, 0], [0.45, 0.75])


@requires_stack
class AuditScriptTest(unittest.TestCase):
    """``evaluate.py`` re-implements the shrinkage to ask its own question, so it can drift alone.

    The 0.2.x-era audit applied ``n/(n+k)`` to the *model* and ``k/(n+k)`` to the table: stations
    with long history were overruled by the lookup and stations with none were handed nothing but
    the lookup, which is the exact opposite of what its report claimed.  These tests pin the
    direction, the clock a cell is named for, and the refusal to rewrite a published report.
    """

    def _frame(self, hours, values):
        return pd.DataFrame({"station_id": ["S"] * len(hours),
                             "site_key": ["mall"] * len(hours),
                             "city_id": ["BJ"] * len(hours),
                             "hour_of_day": hours,
                             AVAILABILITY.label_column(2): values,
                             "lag_available_h01": values})

    def test_more_history_moves_the_answer_towards_the_table_not_away_from_it(self):
        model, table = np.array([0.0, 0.0, 0.0]), np.array([3.0, 3.0, 3.0])
        weight = prior_weight(np.array([0.0, 10.0, 1000.0]), 100.0)
        blended = evaluate._blend_value(model, table, weight)
        # n = 0 keeps the model, n -> infinity approaches the table, monotonically in between.
        self.assertAlmostEqual(blended[0], 0.0)
        np.testing.assert_array_less(np.abs(blended[1:] - 3.0), np.abs(blended[:-1] - 3.0))
        self.assertAlmostEqual(evaluate._blend_value(model, table, 1.0)[0], 3.0)
        self.assertAlmostEqual(evaluate._blend_value(model, table, 0.0)[0], 0.0)

    def test_a_cell_is_named_for_the_hour_its_label_describes(self):
        # 22:00 observed -> the h02 label describes 23:00; 23:00 observed -> it describes 00:00.
        train = self._frame([22, 23], [3.0, 0.0])
        tables = evaluate._baseline_tables(train, [2])
        scored = evaluate._baseline_values(tables, 2, train)["stationHourMedian"]
        np.testing.assert_allclose(scored, [3.0, 0.0])
        # Reading the cells back by the hour they describe is the point of the key: the 22:00
        # observation's h02 label belongs to 23:00, and the 23:00 one crosses midnight.
        table = tables[2]["stationHourMedian"]
        self.assertEqual(float(table.loc[("S", 23)]), 3.0)
        self.assertEqual(float(table.loc[("S", 0)]), 0.0)
        np.testing.assert_array_equal(
            sorted(table.index.get_level_values(evaluate.HOUR_KEY).to_numpy()), [0, 23])

    def test_a_published_report_is_never_rewritten(self):
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            (directory / "evaluation_report.json").write_text("{}", encoding="utf-8")
            # Both the default target (the run directory itself) and an explicit --output are refused.
            for extra in ([], ["--output", str(directory)]):
                with self.assertRaises(SystemExit) as stopped:
                    evaluate.main(["--run-dir", str(directory), *extra])
                self.assertIn("already holds", str(stopped.exception))
            self.assertEqual((directory / "evaluation_report.json").read_text(encoding="utf-8"), "{}")


@requires_run
@requires_stack
class ShippedBundleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.export = forecaster.open_export(EXPORT_DIR)
        cls.frame = forecaster.build_frame(EXPORT_DIR)

    def _context(self, predictor, row):
        return PredictionContext(
            dataset_id=self.export.dataset_id,
            published_batch_id=self.export.published_batch_id,
            station_id=row["station_id"],
            reference_time=row["reference_time"].strftime("%Y-%m-%dT%H:%M:%SZ"),
            horizon_hours=predictor.horizon_hours,
            model_id=predictor.metadata["modelId"],
        )

    def test_bundle_is_bound_to_the_batch_it_reads(self):
        directories = _bundle_directories()
        self.assertGreater(len(directories), 0)
        for run, directory in directories:
            with self.subTest(bundle=_label(run, directory)):
                bundle, metadata = artifacts.load_bundle(directory)
                self.assertEqual(metadata["sourceManifestSha256"], self.export.source_manifest_sha256,
                                 "binds a different raw dataset manifest than it trained on")
                self.assertNotEqual(metadata["sourceManifestSha256"], self.export.manifest_sha256,
                                    "sourceManifestSha256 must be the raw manifest, not the export manifest")
                self.assertEqual(metadata["trainingPublishedBatchId"], self.export.published_batch_id)
                self.assertEqual(metadata["datasetId"], self.export.dataset_id)
                self.assertEqual(metadata["featureVersion"], self.export.feature_version)
                self.assertEqual(metadata["featureColumns"], self.frame.feature_columns)
                self.assertEqual(bundle["publishedBatchId"], self.export.published_batch_id)
                self.assertEqual(bundle["datasetId"], self.export.dataset_id)
                self.assertEqual(sorted(metadata["metrics"]), ["mae", "rmse", "testSamples", "unit"])
                artifacts.validate_metadata(metadata)

    def test_prediction_satisfies_the_public_contract(self):
        for run, directory in _bundle_directories():
            with self.subTest(bundle=_label(run, directory)):
                self._one_contract_check(directory)

    def _one_contract_check(self, directory: Path) -> None:
        predictor = AvailabilityForecaster.load(directory)
        if predictor.bundle["excludeCity"]:
            return  # a cold-start bundle must not be served for a city it never saw
        horizon = predictor.horizon_hours
        row = self.frame.data.query(f"{self.export.split_column(horizon)} == 'TEST'").iloc[3]
        history = _history(self.frame.hourly, row["station_id"], row["reference_time"])
        context = self._context(predictor, row)
        result = predictor.predict(history, context)
        capacity = float(self.frame.stations[row["station_id"]]["capacity"])
        validate_prediction(result, context, capacity, task="availability")
        self.assertEqual(len(result["points"]), horizon)
        self.assertEqual([point["timestamp"] for point in result["points"]],
                         [(row["reference_time"] + pd.Timedelta(hours=step)).strftime("%Y-%m-%dT%H:%M:%SZ")
                          for step in range(horizon)])
        for point in result["points"]:
            self.assertIsInstance(point["value"], int, "free chargers are whole units")
            self.assertGreaterEqual(point["value"], 0)
            self.assertLessEqual(point["value"], capacity)
        risk = predictor.risk(history, context)
        self.assertEqual(len(risk["hours"]), horizon)
        for hour in risk["hours"]:
            self.assertAlmostEqual(sum(hour["distribution"].values()), 1.0, places=6)
            self.assertLessEqual(hour["intervalLow"], hour["medianChargers"])
            self.assertLessEqual(hour["medianChargers"], hour["intervalHigh"])
            self.assertLessEqual(0.0, hour["probabilityDepleted"])
            self.assertGreaterEqual(1.0, hour["probabilityDepleted"])

    def test_a_hierarchical_bundle_serves_its_own_prior(self):
        """The served distribution must follow the pseudo-count the bundle ships.

        Where the bundle's own ``k`` puts weight on a cell, the served pmf has to differ from what
        the wrapped classifier said; where it puts none - a station the run never saw, or a level
        validation switched off - the served pmf has to be the classifier's, unchanged.  Either
        outcome is legitimate; a bundle that ignores its recorded ``k`` on the serving path is not.
        """
        blended_at_least_once = False
        for run, directory in _bundle_directories():
            predictor = AvailabilityForecaster.load(directory)
            if not predictor.bundle.get("hierarchical"):
                continue
            with self.subTest(bundle=_label(run, directory)):
                model = predictor.model
                self.assertTrue(model.hierarchical)
                self.assertTrue(model.pseudo_count, "a shipped hierarchical model was never calibrated")
                horizon = predictor.horizon_hours
                rows = self.frame.data.query(f"{self.export.split_column(horizon)} == 'TEST'")
                for position in (5, 11, 17, 23):
                    row = rows.iloc[position]
                    x = self.frame.data.loc[[row.name], list(predictor.bundle["featureColumns"])]
                    keys = pd.DataFrame([{"station_id": row["station_id"],
                                          "hour_of_day": int(row["hour_of_day"]),
                                          "site_key": site_key_of(predictor.bundle["stations"][row["station_id"]]),
                                          "city_id": row["city_id"]}])
                    for step in (min(model.steps), max(model.steps)):
                        _mass, support, levels = model.prior.probabilities(step, keys)
                        level = str(levels[0])
                        weight = float(prior_weight(
                            [support[0]], float(model.pseudo_count[step].get(level, NO_PRIOR)))[0])
                        blended = model.distribution_for(x, step, keys)[0]
                        alone = model.base.distribution_for(x, step)[0]
                        self.assertAlmostEqual(float(blended.sum()), 1.0, places=12)
                        if weight > 1e-9:
                            self.assertFalse(np.allclose(blended, alone),
                                             f"{level} cell with weight {weight:.3f} left the served "
                                             "distribution exactly as the classifier had it")
                            blended_at_least_once = True
                        else:
                            np.testing.assert_allclose(blended, alone, atol=1e-12,
                                                       err_msg=f"{level} has weight {weight:g} but was blended")
        self.assertTrue(blended_at_least_once, "no shipped hierarchical bundle ever used its prior on the serving path")

    def test_station_profiles_stay_the_numeric_block_the_online_builder_reads(self):
        """Nothing but the ``site_<type>`` one-hots may live under that prefix in a served profile.

        The prior's site level keys on a site *label*, which is recovered from the one-hots; filing
        that string inside the profile itself makes every reader that scans the prefix - the online
        feature builder included - try to float("office").
        """
        for run, directory in _bundle_directories():
            bundle, _metadata = artifacts.load_bundle(directory)
            with self.subTest(bundle=_label(run, directory)):
                for station_id, profile in bundle["stations"].items():
                    for key, value in profile.items():
                        if key.startswith("site_"):
                            self.assertNotEqual(key, "site_key", f"{station_id} stores a derived label "
                                                                  "under the one-hot prefix")
                            self.assertIsInstance(value, (int, float), f"{station_id}.{key} is {value!r}")
                    recorded = bundle.get("stationSiteKeys", {}).get(station_id)
                    if recorded is not None:
                        self.assertEqual(recorded, site_key_of(profile),
                                         "the bundle records a site label its own one-hots disagree with")

    def test_online_row_is_fully_populated_from_the_bundle_profile(self):
        """Regression: station-profile one-hots must come from the bundle, not from the history rows."""
        directories = [bundle for _run, bundle in _bundle_directories()
                       if not AvailabilityForecaster.load(bundle).bundle["excludeCity"]]
        predictor = AvailabilityForecaster.load(directories[0])
        horizon = predictor.horizon_hours
        row = self.frame.data.query(f"{self.export.split_column(horizon)} == 'TEST'").iloc[9]
        history = _history(self.frame.hourly, row["station_id"], row["reference_time"])
        online = forecaster.online_feature_row(predictor.bundle_profile(), row["reference_time"],
                                               row["station_id"], history)
        self.assertEqual(list(online), predictor.bundle["featureColumns"])
        empty = [column for column, value in online.items() if value is None or float(value) != float(value)]
        self.assertEqual(empty, [], "the online feature row has holes the model was never trained with")
        offline = row[list(predictor.bundle["featureColumns"])].to_dict()
        for column, value in online.items():
            self.assertAlmostEqual(float(offline[column]), float(value), places=6, msg=column)

    def test_a_request_cannot_smuggle_the_answer_in_as_history(self):
        predictor = AvailabilityForecaster.load(_bundle_directories()[0][1])
        horizon = predictor.horizon_hours
        row = self.frame.data.query(f"{self.export.split_column(horizon)} == 'TEST'").iloc[7]
        context = self._context(predictor, row)
        with self.assertRaisesRegex(feat.HistoryError, "24 history rows"):
            predictor.predict(_history(self.frame.hourly, row["station_id"], row["reference_time"])
                              + [_history(self.frame.hourly, row["station_id"],
                                          row["reference_time"] + pd.Timedelta(hours=1))[0]], context)
        with self.assertRaises(feat.HistoryError):
            predictor.predict([], context)
        mismatched = replace(context, published_batch_id="some_other_batch")
        from data_analysis.ml.availability.predict import PredictionError
        with self.assertRaises(PredictionError):
            predictor.predict(_history(self.frame.hourly, row["station_id"], row["reference_time"]), mismatched)


@requires_run
@requires_stack
class MinimalInferenceTest(unittest.TestCase):
    """The 最小推理测试 the handoff lists as a deliverable - as a test, and as a command.

    ``predict._check_one`` is what ``python -m data_analysis.ml.availability.predict
    --self-check --run-dir ...`` runs per bundle; these tests hold that code to the claims the
    report makes, and check that it can actually fail rather than always printing PASS.
    """

    @classmethod
    def setUpClass(cls):
        cls.frame = forecaster.build_frame(EXPORT_DIR)

    def _served_bundles(self) -> list[Path]:
        return [bundle for _run, bundle in _bundle_directories()
                if not AvailabilityForecaster.load(bundle).bundle["excludeCity"]]

    def test_every_shipped_bundle_passes_or_is_explicitly_skipped(self):
        records = [predict._check_one(self.frame, bundle, sample_index=3)
                   for _run, bundle in _bundle_directories()]
        self.assertGreater(len(records), 0)
        for record in records:
            with self.subTest(bundle=record["bundle"]):
                self.assertIn(record["status"], ("PASS", "SKIPPED"),
                              record.get("problems") or record.get("error") or record.get("reason"))
        served = [record for record in records if record["status"] == "PASS"]
        self.assertTrue({1, 6, 24} <= {record["horizonHours"] for record in served},
                        "an ordinary 1/6/24-hour bundle is missing, so the check proves nothing")

    def test_served_values_are_whole_chargers_within_the_station_capacity(self):
        """The two channels stay distinct: integers for inventory, a float for the expectation."""
        for bundle in self._served_bundles():
            record = predict._check_one(self.frame, bundle, sample_index=3)
            with self.subTest(model=record["modelId"]):
                self.assertEqual(record["status"], "PASS", record.get("problems") or record.get("error"))
                self.assertEqual(len(record["values"]), record["horizonHours"])
                self.assertEqual(len(record["expectedChargers"]), record["horizonHours"])
                for value in record["values"]:
                    self.assertIsInstance(value, int, "空闲桩数必须是整数，不能是小数预计值")
                    self.assertGreaterEqual(value, 0)
                    self.assertLessEqual(value, record["capacity"])
                for value in record["expectedChargers"]:
                    self.assertIsInstance(value, float)

    def test_a_bundle_whose_artefact_hash_is_wrong_is_reported_as_a_failure(self):
        """A check that cannot fail is decoration: corrupting a copy must turn PASS into FAIL."""
        smallest = min(self._served_bundles(), key=lambda path: (path / artifacts.ARTIFACT_NAME).stat().st_size)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / smallest.name
            directory.mkdir()
            (directory / artifacts.ARTIFACT_NAME).write_bytes((smallest / artifacts.ARTIFACT_NAME).read_bytes())
            metadata = json.loads((smallest / artifacts.METADATA_NAME).read_text(encoding="utf-8"))
            metadata["artifactSha256"] = "f" * 64
            (directory / artifacts.METADATA_NAME).write_text(json.dumps(metadata), encoding="utf-8")
            record = predict._check_one(self.frame, directory, sample_index=3)
            self.assertEqual(record["status"], "FAIL")
            self.assertIn("does not match", record["error"])

    def test_the_self_check_command_exits_zero_on_a_shipped_bundle(self):
        """The deliverable is a command the lead can run, so its wiring is asserted too."""
        bundle = self._served_bundles()[0]
        self.assertEqual(predict.main(["--self-check", "--bundle", str(bundle),
                                       "--export", str(EXPORT_DIR)]), 0)


@requires_run
@requires_stack
class RunDirectoryGuardTest(unittest.TestCase):
    """A published run cannot be rewritten, and refusing must cost nothing.

    ``evaluate``'s refusal is covered by :class:`AuditScriptTest`; these two are the training
    entries, where a missing guard would silently replace the artefacts somebody already quoted in
    a report.  Both checks fire before the frame is built, so the test takes milliseconds instead
    of the 70 minutes a real training pass costs.
    """

    def _published(self) -> Path:
        return next(run for run in RUN_DIRS if run.exists() and artifacts.bundle_directories(run))

    def test_train_refuses_a_directory_that_already_holds_bundles(self):
        with self.assertRaises(SystemExit) as stopped:
            train.main(["--output", str(self._published())])
        self.assertIn("already holds", str(stopped.exception))

    def test_a_run_left_half_written_is_refused_too(self):
        """A directory with only ``train_summary.json`` is a finished claim, not an empty target."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "train_summary.json").write_text("{}", encoding="utf-8")
            with self.assertRaises(SystemExit) as stopped:
                train.main(["--output", str(directory)])
            self.assertIn("already holds", str(stopped.exception))
            self.assertFalse(artifacts.bundle_directories(directory), "the refusal must not write anything")

    def test_build_hierarchical_refuses_a_directory_with_content(self):
        published = self._published()
        with self.assertRaises(SystemExit) as stopped:
            build_hierarchical.main(["--source-run", str(published), "--output", str(published)])
        self.assertIn("already has content", str(stopped.exception))

    def test_a_finished_run_cannot_be_resumed_into(self):
        """``--resume`` relaxes the bundle check, never the summary: quoted numbers stay closed."""
        with self.assertRaises(SystemExit) as stopped:
            train.main(["--output", str(self._published()), "--resume"])
        self.assertIn("already holds", str(stopped.exception))

    def test_a_profile_written_first_does_not_make_a_run_directory_look_published(self):
        """``prepare_data`` is documented to write into the run directory before training does."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "data_profile.json").write_text("{}", encoding="utf-8")
            self.assertEqual(artifacts.bundle_directories(directory), [])


@requires_stack
class BoostingBudgetTest(unittest.TestCase):
    """``--rounds-multiplier`` is the boosted-tree version of "train longer", and it is opt-in.

    Gradient boosting has no epochs: one round is one tree, and early stopping ends the fit early.
    A run therefore has to be able to say what budget it was fitted with, and the published recipe
    has to stay reachable without editing a constant.
    """

    def test_the_published_recipe_is_pinned_in_the_code(self):
        self.assertEqual(model_module.BASE_PARAMS, {
            "max_iter": 250, "learning_rate": 0.06, "max_leaf_nodes": 31, "min_samples_leaf": 40,
            "early_stopping": True, "validation_fraction": 0.1, "n_iter_no_change": 15})

    def test_multiplier_one_is_the_published_recipe_unchanged(self):
        self.assertEqual(model_module.scaled_rounds(1.0), model_module.BASE_PARAMS)

    def test_a_stretched_budget_scales_patience_as_well_as_rounds(self):
        """Stopping after 15 fruitless rounds makes a 1250-round budget unreachable, so patience moves too."""
        stretched = model_module.scaled_rounds(5.0)
        self.assertEqual(stretched["max_iter"], 1250)
        self.assertEqual(stretched["n_iter_no_change"], 75)
        self.assertEqual(stretched["learning_rate"], model_module.BASE_PARAMS["learning_rate"],
                         "the learning rate is not part of 'train longer' and must not drift")

    def test_a_shrunken_budget_rounds_to_whole_trees(self):
        budget = model_module.scaled_rounds(0.4)
        self.assertEqual(budget["max_iter"], 100)
        self.assertEqual(budget["n_iter_no_change"], 6)

    def test_a_negative_or_zero_multiplier_is_refused(self):
        for multiplier in (0.0, -1.0):
            with self.assertRaises(ValueError):
                model_module.scaled_rounds(multiplier)

    def test_a_stretched_bundle_is_named_apart_from_the_published_one(self):
        self.assertEqual(train._variant(1.0), "")
        self.assertEqual(train._variant(5.0), "-r5")

    def test_a_wrapped_bundle_inherits_its_base_rounds_tag(self):
        """A 0.3.0 wrapper around a 5x-budget estimator is not the published hierarchical model."""
        plain = build_hierarchical._naming({"horizonHours": 1, "excludeCity": None})
        self.assertEqual(plain, ("avail-hier-h01", "0.3.0", 1.0))
        stretched = build_hierarchical._naming({"horizonHours": 6, "excludeCity": "DL", "roundsMultiplier": 5.0})
        self.assertEqual(stretched, ("avail-hier-h06-r5-coldDL", "0.3.0-r5", 5.0))


@requires_stack
class ResumeGuardTest(unittest.TestCase):
    """Reusing a checkpoint is allowed by ``--resume``, but only the checkpoint this command made.

    Everything here is synthetic: the saved bundle and the expected recipe are built from the same
    placeholder batch, so each case changes exactly one field and asserts what the guard says about
    it.  The values themselves are not the point and are not read off disk.
    """

    RECIPE = {
        "modelVersion": "0.2.0",
        "featureVersion": "history24-v1",
        "datasetId": "dataset-under-test",
        "trainingPublishedBatchId": "batch-under-test",
        "sourceManifestSha256": "a" * 64,
        "seed": 20260913,
        "horizonHours": 1,
        "holdoutCity": None,
        "roundsMultiplier": 1.0,
        "steps": ["1"],
        "featureColumns": ["lag_available_h01", "hour_of_day"],
    }

    def _frame(self):
        export = types.SimpleNamespace(
            feature_version=self.RECIPE["featureVersion"], dataset_id=self.RECIPE["datasetId"],
            published_batch_id=self.RECIPE["trainingPublishedBatchId"],
            source_manifest_sha256=self.RECIPE["sourceManifestSha256"])
        return types.SimpleNamespace(export=export, feature_columns=list(self.RECIPE["featureColumns"]))

    def _bundle(self, directory: Path, **overrides) -> dict:
        """Write a complete bundle whose sidecars record ``overrides`` as how it was made."""
        recipe = dict(self.RECIPE)
        recipe.update(overrides)
        metadata = artifacts.build_metadata(
            model_id="avail-ord-h01", model_version=recipe["modelVersion"], target="availability",
            feature_version=recipe["featureVersion"], dataset_id=recipe["datasetId"],
            source_manifest_sha256=recipe["sourceManifestSha256"],
            training_published_batch_id=recipe["trainingPublishedBatchId"],
            splits={"trainEnd": "2026-04-01", "validationEnd": "2026-04-20", "end": "2026-05-27"},
            feature_columns=recipe["featureColumns"], artifact_file=artifacts.ARTIFACT_NAME,
            artifact_sha256="0" * 64, supported_horizons=[recipe["horizonHours"]],
            metrics={"mae": 0.5, "rmse": 0.8, "testSamples": 100, "unit": "chargers"})
        report = {
            "modelId": "avail-ord-h01", "horizonHours": recipe["horizonHours"],
            "holdoutCity": recipe["holdoutCity"], "seed": recipe["seed"],
            "roundsMultiplier": recipe["roundsMultiplier"],
            "perStep": {step: {"boostingRounds": 114} for step in recipe["steps"]},
            "pooledTest": {"mae": 0.5, "points": 100}, "preparedAt": artifacts.stamp(),
        }
        artifacts.save_bundle(directory, {"model": "not a fitted estimator"}, metadata, report)
        return recipe

    def _reuse(self, directory: Path, **command):
        return train._reuse(directory, self._frame(), horizon=command.pop("horizon", 1),
                            seed=command.pop("seed", self.RECIPE["seed"]),
                            exclude_city=command.pop("exclude_city", None),
                            available_steps=command.pop("steps", [1]),
                            rounds_multiplier=command.pop("rounds_multiplier", 1.0))

    def test_a_bundle_from_the_same_recipe_is_reused_untouched(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "h01"
            self._bundle(directory)
            digest = artifacts.sha256_file(directory / artifacts.ARTIFACT_NAME)
            entry = self._reuse(directory)
            self.assertTrue(entry["reused"])
            self.assertEqual(entry["pooled"]["mae"], 0.5)
            self.assertEqual(entry["modelId"], "avail-ord-h01")
            self.assertEqual(set(entry["boostingRounds"]), {"min", "max", "budget"},
                             "a reused checkpoint is reported in the same shape as a freshly fitted one")
            self.assertEqual(artifacts.sha256_file(directory / artifacts.ARTIFACT_NAME), digest,
                             "reusing a checkpoint must not rewrite it")

    def test_nothing_saved_is_not_a_refusal(self):
        """An empty slot is the normal case for a resumed run: it is what still has to be trained."""
        with tempfile.TemporaryDirectory() as temporary:
            self.assertIsNone(self._reuse(Path(temporary) / "h06"))

    def test_a_different_seed_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "h01"
            self._bundle(directory, seed=99)
            with self.assertRaises(SystemExit) as stopped:
                self._reuse(directory)
            self.assertIn("seed", str(stopped.exception))

    def test_a_different_batch_or_manifest_is_refused(self):
        for key, value in (("datasetId", "another-dataset"),
                           ("trainingPublishedBatchId", "another-batch"),
                           ("sourceManifestSha256", "b" * 64)):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary) / "h01"
                self._bundle(directory, **{key: value})
                with self.assertRaises(SystemExit) as stopped:
                    self._reuse(directory)
                self.assertIn(key, str(stopped.exception))

    def test_a_stretched_round_budget_cannot_be_reused_by_the_published_recipe(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "h01"
            self._bundle(directory, roundsMultiplier=5.0, modelVersion="0.2.0-r5")
            with self.assertRaises(SystemExit) as stopped:
                self._reuse(directory)
            message = str(stopped.exception)
            self.assertIn("roundsMultiplier", message)
            self.assertIn("modelVersion", message)
            # ...and the other way round: the -r5 command must not claim the plain bundle either.
            self._bundle(directory)
            with self.assertRaises(SystemExit):
                self._reuse(directory, rounds_multiplier=5.0)

    def test_a_bundle_from_another_horizon_or_holdout_city_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "h01"
            self._bundle(directory, horizonHours=6)
            with self.assertRaises(SystemExit) as stopped:
                self._reuse(directory)
            self.assertIn("horizonHours", str(stopped.exception))
            self._bundle(directory, holdoutCity="DL")
            with self.assertRaises(SystemExit) as stopped:
                self._reuse(directory)
            self.assertIn("holdoutCity", str(stopped.exception))

    def test_a_bundle_fitted_for_fewer_steps_is_refused(self):
        """A ``--limit-steps`` smoke bundle only holds some of the hours a horizon has to serve."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "h24"
            self._bundle(directory, steps=["1", "2"])
            with self.assertRaises(SystemExit) as stopped:
                self._reuse(directory, horizon=24, steps=list(range(1, 25)))
            self.assertIn("steps", str(stopped.exception))

    def test_a_half_saved_bundle_is_reported_as_unusable_rather_than_refitted(self):
        """Metadata without its report is an interrupted write; the recipe cannot be checked at all."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "h01"
            self._bundle(directory)
            (directory / artifacts.REPORT_NAME).unlink()
            with self.assertRaises(SystemExit) as stopped:
                self._reuse(directory)
            self.assertIn("interrupted save", str(stopped.exception))

    def test_a_corrupted_artefact_is_refused_before_it_is_loaded(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "h01"
            self._bundle(directory)
            artifact = directory / artifacts.ARTIFACT_NAME
            artifact.write_bytes(artifact.read_bytes() + b"tampered")
            with self.assertRaises(SystemExit) as stopped:
                self._reuse(directory)
            self.assertIn("does not match the hash", str(stopped.exception))

    def test_a_directory_that_does_not_exist_yet_holds_nothing_published(self):
        """``--output`` names a directory training is about to create, so the scan must not raise."""
        with tempfile.TemporaryDirectory() as temporary:
            self.assertEqual(train.published_content(Path(temporary) / "ml_avail_run_new"), [])

    def test_only_bundles_and_a_summary_count_as_published_content(self):
        """``prepare_data`` writes its profile into the run directory first; that is not evidence."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "data_profile.json").write_text("{}", encoding="utf-8")
            self._bundle(root / "h01")
            self.assertEqual(train.published_content(root), ["h01"])
            (root / "train_summary.json").write_text("{}", encoding="utf-8")
            self.assertEqual(train.published_content(root), ["train_summary.json", "h01"])


@requires_stack
@requires_legacy_runs
class HierarchicalResumeTest(unittest.TestCase):
    """``build_hierarchical --resume`` keeps a wrapped bundle only if it names this base and batch.

    The fixtures are the **superseded** published artefacts - ``ml_avail_run1`` as the base
    estimators and ``ml_avail_run2`` as the wrapped output - copied into a temporary directory and
    edited there, so what the guard reads is the real report shape a resumed run has to face, not an
    approximation.  They are deliberately the old generation: two of these cases exist to prove the
    guard copes with artefacts that record no ``seed`` and no ``payloadEntry``, and every bundle the
    re-bound runs (``ml_avail_run5`` / ``ml_avail_run6``) write does record both, so a newer fixture
    could not test those paths at all.
    """

    BASE = DATA_ANALYSIS / "outputs" / LEGACY_RUN_NAMES[0]
    WRAPPED = DATA_ANALYSIS / "outputs" / LEGACY_RUN_NAMES[1]

    def _copy(self, root: Path, **report_edits) -> Path:
        target = root / "h01"
        target.mkdir(parents=True, exist_ok=True)
        for name in (artifacts.ARTIFACT_NAME, artifacts.METADATA_NAME, artifacts.REPORT_NAME):
            shutil.copyfile(self.WRAPPED / "h01" / name, target / name)
        if report_edits:
            report = json.loads((target / artifacts.REPORT_NAME).read_text(encoding="utf-8"))
            report.update(report_edits)
            (target / artifacts.REPORT_NAME).write_text(json.dumps(report, ensure_ascii=False), encoding="utf-8")
        return target

    def _frame(self, **override):
        metadata = json.loads((self.WRAPPED / "h01" / artifacts.METADATA_NAME).read_text(encoding="utf-8"))
        export = types.SimpleNamespace(
            feature_version=override.get("featureVersion", metadata["featureVersion"]),
            dataset_id=override.get("datasetId", metadata["datasetId"]),
            published_batch_id=override.get("trainingPublishedBatchId", metadata["trainingPublishedBatchId"]),
            source_manifest_sha256=override.get("sourceManifestSha256", metadata["sourceManifestSha256"]))
        # The real Frame carries feature_columns at top level (forecaster.Frame); the resume guard
        # reads it to reject a base wrapped on another feature set, so the stub must mirror it.
        return types.SimpleNamespace(export=export,
                                     feature_columns=override.get("featureColumns", metadata["featureColumns"]))

    def _reuse(self, target: Path, frame=None, *, seed=20260913):
        return build_hierarchical._reuse(target, base_bundle=self.BASE / "h01", source=self.BASE,
                                         frame=frame or self._frame(), seed=seed)

    def test_a_bundle_that_records_no_seed_is_not_claimed(self):
        """``ml_avail_run2`` predates seed recording, so nothing may be resumed on top of it."""
        with tempfile.TemporaryDirectory() as temporary:
            # notIn, not assertIsNone(get(...)): the shipped artefact omits the key entirely, and a
            # null-valued seed would be a different bug (a run that recorded "no seed" on purpose).
            self.assertNotIn("seed", json.loads((self.WRAPPED / "h01" / artifacts.REPORT_NAME)
                                                .read_text(encoding="utf-8")))
            with self.assertRaises(SystemExit) as stopped:
                self._reuse(self._copy(Path(temporary)))
            self.assertIn("seed", str(stopped.exception))

    def test_the_same_bundle_with_a_recorded_seed_is_reused(self):
        with tempfile.TemporaryDirectory() as temporary:
            entry = self._reuse(self._copy(Path(temporary), seed=20260913))
            self.assertTrue(entry["reused"])
            self.assertEqual(entry["directory"], "h01")
            self.assertEqual(entry["test"]["mae"], 0.4816)
            self.assertEqual(entry["testModelAlone"]["mae"], 0.5274)
            self.assertEqual(entry["pointRule"], "median")
            self.assertEqual(list(entry["pseudoCount"]), ["1"])

    def test_a_bundle_wrapped_from_another_base_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = self._copy(Path(temporary), seed=20260913, reusedEstimatorFrom="ml_avail_run9/h06")
            with self.assertRaises(SystemExit) as stopped:
                self._reuse(target)
            self.assertIn("reusedEstimatorFrom", str(stopped.exception))

    def test_a_row_the_run_wrote_down_is_read_back_instead_of_rebuilt(self):
        """``payloadEntry`` is the row the finished run published, so it wins over any recomputation."""
        published = self._published_entry()
        stored = dict(published, pointRule="a-rule-no-rebuild-would-choose")
        with tempfile.TemporaryDirectory() as temporary:
            entry = self._reuse(self._copy(Path(temporary), seed=20260913, payloadEntry=stored))
        self.assertEqual({key: value for key, value in entry.items() if key != "reused"}, stored)
        self.assertEqual(entry["pointRule"], "a-rule-no-rebuild-would-choose",
                         "the row was not read back, it was recomputed from the metrics")

    def test_a_row_rebuilt_from_an_older_bundle_matches_the_published_one(self):
        """The published 0.3.0 run recorded no ``payloadEntry``, and rebuilding it must still be exact.

        Every column - including the two derived percentages, which are the ones a four-decimal MAE
        cannot reproduce - has to come back as the run wrote it, or ``--resume`` on that run would
        print different numbers than the report everyone quoted.
        """
        published = self._published_entry()
        with tempfile.TemporaryDirectory() as temporary:
            self.assertNotIn("payloadEntry", json.loads((self.WRAPPED / "h01" / artifacts.REPORT_NAME)
                                                        .read_text(encoding="utf-8")))
            entry = self._reuse(self._copy(Path(temporary), seed=20260913))
        self.assertEqual({key: value for key, value in entry.items() if key != "reused"}, published)

    def _published_entry(self) -> dict:
        payload = json.loads((self.WRAPPED / "hierarchical_report.json").read_text(encoding="utf-8"))
        return {key: value for key, value in payload["bundles"]["h01"].items() if key != "reused"}

    def test_a_bundle_bound_to_another_batch_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = self._copy(Path(temporary), seed=20260913)
            with self.assertRaises(SystemExit) as stopped:
                self._reuse(target, self._frame(datasetId="another-dataset"))
            self.assertIn("datasetId", str(stopped.exception))

    def test_a_resumed_run_renders_the_published_report_unchanged(self):
        """A resumed run has to produce the same ``hierarchical_report.md``, or the resume lied."""
        published_markdown = (self.WRAPPED / "hierarchical_report.md").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as temporary:
            entry = self._reuse(self._copy(Path(temporary), seed=20260913))
        payload = json.loads((self.WRAPPED / "hierarchical_report.json").read_text(encoding="utf-8"))
        payload["bundles"]["h01"] = entry
        self.assertEqual(build_hierarchical._markdown(payload), published_markdown)

    def test_a_partial_run_says_so_instead_of_printing_nan(self):
        """An in-domain-only run has no cold-start evidence to average, and must not write ``nan``."""
        payload = json.loads((self.WRAPPED / "hierarchical_report.json").read_text(encoding="utf-8"))
        payload["bundles"] = {name: entry for name, entry in payload["bundles"].items()
                              if not entry["holdoutCity"]}
        markdown = build_hierarchical._markdown(payload)
        self.assertIn("冷启动包（0 个）", markdown)
        self.assertNotIn("nan", markdown)


def _without_wall_clock(path: Path) -> str:
    """A rendered report with the line stating how long its run took removed.

    Two runs of the same command differ there and nowhere else, so that line is the one thing a
    resumed run is allowed to print differently.
    """
    return "\n".join(line for line in path.read_text(encoding="utf-8").splitlines()
                     if "耗时" not in line)


@requires_stack
@requires_export
class ResumeEndToEndTest(unittest.TestCase):
    """``--resume`` on the real command line against a checkpoint, not against a stub.

    The guard tests above prove what a recipe mismatch refuses.  This proves the thing a member B
    actually bets on when a long run dies halfway: continuing it from disk finishes the run and
    changes no artefact, no score and no rendered table.
    """

    def test_an_interrupted_train_run_continues_from_its_checkpoints(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fitted, continued = root / "run", root / "continued"
            train.main(["--output", str(fitted), "--horizons", "1"])
            first = json.loads((fitted / "train_summary.json").read_text(encoding="utf-8"))
            shutil.copytree(fitted, continued)
            artifact = continued / "h01" / artifacts.ARTIFACT_NAME
            digest = artifacts.sha256_file(artifact)
            # What an interrupted run really looks like: bundles on disk, no summary claiming them.
            (continued / "train_summary.json").unlink()

            with self.assertRaises(SystemExit) as refused:
                train.main(["--output", str(continued), "--horizons", "1", "--seed", "7", "--resume"])
            self.assertIn("seed", str(refused.exception))
            self.assertEqual(artifacts.sha256_file(artifact), digest, "a refusal must not touch the checkpoint")

            train.main(["--output", str(continued), "--horizons", "1", "--resume"])
            again = json.loads((continued / "train_summary.json").read_text(encoding="utf-8"))
            self.assertEqual(again["resumedBundles"], [str(continued / "h01")])
            self.assertEqual(again["run"]["h01"]["pooled"], first["run"]["h01"]["pooled"])
            self.assertEqual(again["modelVersion"], first["modelVersion"])
            self.assertEqual(artifacts.sha256_file(artifact), digest, "a reused bundle is not refitted")

    def test_an_interrupted_wrapping_run_prints_the_same_table(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base, fresh, continued = root / "base", root / "fresh", root / "continued"
            train.main(["--output", str(base), "--horizons", "1", "--limit-steps", "2"])
            build_hierarchical.main(["--source-run", str(base), "--output", str(fresh), "--only", "h01"])
            shutil.copytree(fresh, continued)
            (continued / "hierarchical_report.json").unlink()

            build_hierarchical.main(["--source-run", str(base), "--output", str(continued),
                                     "--only", "h01", "--resume"])
            payload = json.loads((continued / "hierarchical_report.json").read_text(encoding="utf-8"))
            self.assertEqual(payload["reusedBundles"], ["h01"])
            self.assertEqual(artifacts.sha256_file(continued / "h01" / artifacts.ARTIFACT_NAME),
                             artifacts.sha256_file(fresh / "h01" / artifacts.ARTIFACT_NAME),
                             "the resumed run wrapped nothing again, so no artefact may differ")
            self.assertEqual(_without_wall_clock(continued / "hierarchical_report.md"),
                             _without_wall_clock(fresh / "hierarchical_report.md"),
                             "a resumed run has to print the table the finished run printed")


@requires_stack
class InvocationRecordTest(unittest.TestCase):
    """The reproducible command a report states has to be a command that actually runs."""

    def test_a_run_under_dash_m_records_the_module_not_main(self):
        spec = types.SimpleNamespace(name="data_analysis.ml.availability.evaluate")
        with mock.patch.dict(sys.modules, {"__main__": types.SimpleNamespace(__spec__=spec)}):
            self.assertEqual(
                artifacts.invocation("__main__", ["--run-dir", "data_analysis/outputs/ml_avail_run6"]),
                "python -m data_analysis.ml.availability.evaluate --run-dir data_analysis/outputs/ml_avail_run6")

    def test_a_run_from_a_file_path_records_the_path(self):
        """``python -m <file path>`` is not a command, so a direct run must be quoted as one."""
        main = types.SimpleNamespace(__spec__=None)
        argv = ["data_analysis/ml/availability/evaluate.py", "--output", "x"]
        with mock.patch.dict(sys.modules, {"__main__": main}), \
                mock.patch.object(sys, "argv", argv):
            self.assertEqual(artifacts.invocation("__main__"),
                             "python data_analysis/ml/availability/evaluate.py --output x")

    def test_the_flags_are_recorded_verbatim(self):
        """Seed, run directory and holdout cities are the things shell history loses."""
        recorded = artifacts.invocation("data_analysis.ml.availability.train",
                                        ["--output", "o", "--seed", "20260913", "--holdout-city", "DL"])
        self.assertEqual(recorded, "python -m data_analysis.ml.availability.train "
                                   "--output o --seed 20260913 --holdout-city DL")


@requires_stack
class DataBindingTest(unittest.TestCase):
    def test_export_binds_the_raw_manifest(self):
        export = forecaster.open_export(EXPORT_DIR)
        self.assertRegex(export.source_manifest_sha256, r"^[0-9a-f]{64}$")
        self.assertNotEqual(export.source_manifest_sha256, export.manifest_sha256,
                            "the raw manifest hash and the export manifest hash must not be confused")
        self.assertEqual(export.manifest["sourceManifestSha256"], export.source_manifest_sha256)

    def test_an_unverifiable_export_is_refused(self):
        """``open_export`` must fail closed when the raw-manifest binding does not verify."""
        from data_analysis.ml.common.data_io import ExportError

        with self.assertRaises(ExportError):
            forecaster.open_export(DATA_ANALYSIS / "outputs")

    def test_target_table_never_enters_serving(self):
        """``ml_targets_hourly`` is a training-only table; the serving contract must exclude it."""
        from data_analysis.contracts.serving import SERVING_TABLES, TABLE_KEYS

        self.assertIn("ml_targets_hourly", TABLE_KEYS)
        self.assertNotIn("ml_targets_hourly", SERVING_TABLES)
        self.assertIn("station_hourly_metrics", SERVING_TABLES)


@requires_stack
class RunPointerTest(unittest.TestCase):
    """Which run directories are authoritative, and how a re-publication says so.

    The names are written twice on purpose - the test module must know them before the science stack
    is imported - so the first case is the seam that turns a silent split into a failure.  The second
    is the one that matters to a reviewer: the data layer re-publishes by minting a new
    ``publishedBatchId`` (PR #64 did exactly that), after which every older bundle is dead on the
    serving path.  Without this case that shows up as the 45 confusing failures PR #64 actually
    caused; with it, the suite says which directory stopped being bound to which batch.

    The second case skips only when *neither* authoritative directory is trained.  A half-present
    pair fails instead: re-binding is a two-step job (``train.py``, then ``build_hierarchical.py``
    on top of it), so one directory on disk and one missing is a re-bind that stopped halfway, not
    an absent optional input - and the failure names which one is missing.
    """

    def test_the_test_pointers_match_the_module_constants(self):
        self.assertEqual(RUN_NAMES, (Path(BASE_RUN).name, Path(HIERARCHY_RUN).name))

    def test_the_authoritative_runs_are_bound_to_the_batch_in_the_repository(self):
        export = forecaster.open_export(EXPORT_DIR)
        trained = [run for run in RUN_DIRS if (run / "h01" / artifacts.METADATA_NAME).exists()]
        if not trained:
            # a clone that never ran the pipeline: nothing here claims to be bound to anything
            self.skipTest(f"none of {' / '.join(RUN_NAMES)} is trained on this machine")
        # A half-present pair is not a reason to skip: the re-bind stopped halfway, and whichever
        # directory is missing is one the module constants still claim is the authoritative one.
        # Naming it here beats letting it surface as a missing-bundle error somewhere else.
        missing = [run.name for run in RUN_DIRS if run not in trained]
        self.assertEqual(
            len(trained), len(RUN_DIRS),
            f"only part of the authoritative pair is on disk (present: "
            f"{[run.name for run in trained]}, missing: {missing}); finish the re-bind before "
            "trusting the batch check")
        for run in trained:
            with self.subTest(run=run.name):
                metadata = json.loads(
                    (run / "h01" / artifacts.METADATA_NAME).read_text(encoding="utf-8"))
                self.assertEqual(metadata["trainingPublishedBatchId"], export.published_batch_id,
                                 f"{run.name} is bound to a superseded batch: retrain into a new "
                                 f"directory and repoint RUN_NAMES / BASE_RUN / HIERARCHY_RUN")


if __name__ == "__main__":
    unittest.main()
