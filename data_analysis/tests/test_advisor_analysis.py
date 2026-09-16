"""Deterministic analysis uses published aggregate DTOs, never an online model."""
from copy import deepcopy
from itertools import combinations
import re
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

from data_analysis.backend.errors import ApiError
from data_analysis.ml.advisor import analysis


def advanced_fixture():
    stations = [dict(stationId=sid, stationName=name, siteType=kind, attemptCount=n, successRate=success,
                     meanWaitMinutes=wait, chargingUtilization=.4)
        for sid, name, kind, n, success, wait in (
            ("S1", "第一关注站", "OFFICE", 100, .3, 9), ("S2", "同型对照站", "OFFICE", 90, .9, 1),
            ("S3", "第二关注站", "MALL", 80, .4, 7), ("S4", "第三关注站", "RESIDENTIAL", 70, .5, 4),
            ("S5", "小样本站", "OFFICE", 1, 0, None))]
    failures = [dict(reason=key, label=label, count=n, shareOfFailures=n / 10) for key, label, n in (
        ("CALL_TIMEOUT", "叫号未确认", 2), ("NO_AVAILABLE_CHARGER", "无可用桩", 5), ("QUEUE_PATIENCE", "等待离队", 3))]
    access = [dict(path=key, label=label, attemptCount=n, successfulAttempts=n * .9, successRate=.9)
              for key, label, n in (("DIRECT", "直接", 60), ("QUEUE", "排队", 30), ("RESERVATION", "预约", 10))]
    peak = dict(siteType="OFFICE", hour=18, attemptCount=98, successfulAttempts=88, successRate=88 / 98,
                meanWaitMinutes=8, queueWaitCount=5, chargingUtilization=.4, completeStationHours=12)
    segments = [dict(userSegment=key, sessionCount=n, intervalCount=intervals, firstObservedCount=n-intervals,
                     meanIntervalDays=gap, meanEnergyKwh=energy) for key, n, intervals, gap, energy in (
        ("COMMUTER", 100, 80, 3, 20), ("RIDE_HAILING", 120, 100, 1, 30),
        ("FLEET", 90, 80, 2, 50), ("FAMILY", 80, 60, 5, 15))]
    return dict(stations=stations, service=dict(attemptCount=100, successfulAttempts=90, failedAttempts=10,
        failures=failures, accessPaths=access, cells=[peak, dict(peak, siteType="CAMPUS", hour=5, attemptCount=2,
                                                             successfulAttempts=2, successRate=1)]),
        behavior=dict(sessionCount=390, intervalCount=320, firstObservedCount=70, segments=segments))


class AdvisorAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.snapshot = SimpleNamespace(metadata=dict(datasetId="data-1", publishedBatchId="batch-1", source="SIMULATED",
            startDate="2025-12-01", endDate="2026-01-01"))
        self.filters = dict(start="2025-12-11", end="2025-12-16", city="C1", station="S1")
        self.now = {"metrics": dict(energyWh=120000, netPaidCents=15000, chargingUtilizationRate=.4,
            startedSessions=20, activeUsers=12, observedHours=60, completeHours=50, stationCount=5,
            chargerCount=15, paidCents=18000, refundCents=3000)}
        self.before = {"metrics": {**self.now["metrics"], "energyWh": 100000, "netPaidCents": 10000,
                                   "chargingUtilizationRate": .25}}
        self.current_advanced = advanced_fixture()
        self.previous_advanced = advanced_fixture()
        self.previous_advanced["service"].update(successfulAttempts=60, failedAttempts=40)
        self.overview = patch.object(analysis.service, "overview", side_effect=lambda snapshot, filters:
            deepcopy(self.now if filters["start"] == self.filters["start"] else self.before)).start()
        self.advanced = patch.object(analysis.advanced, "analyze", side_effect=lambda metadata, filters, **kwargs:
            deepcopy(self.current_advanced if filters["start"] == self.filters["start"] else self.previous_advanced)).start()
        self.addCleanup(patch.stopall)

    def build(self, topics=("overview",), filters=None, **kwargs):
        return analysis.build_analysis(self.snapshot, filters or self.filters, topics, **kwargs)

    def by_id(self, result):
        return {row["id"]: row for row in result["evidence"]}

    def test_equal_length_previous_period_values_units_formulas_and_source_ranges(self):
        result = self.build()
        rows = self.by_id(result)
        for key, expected in {"overview.energy": 120, "overview.energy_previous": 100, "overview.energy_change": 20,
                              "overview.energy_change_pct": 20, "overview.net_paid": 150, "overview.net_paid_previous": 100,
                              "overview.net_paid_change": 50, "overview.net_paid_change_pct": 50,
                              "overview.utilization_change": 15, "overview.success_rate_change": 30}.items():
            self.assertAlmostEqual(rows[key]["value"], expected)
        self.assertEqual(rows["overview.utilization_change"]["unit"], " 个百分点")
        self.assertIn("derived:", rows["overview.net_paid_change_pct"]["source"]["field"])
        for key, start, end in (("overview.energy", "2025-12-11", "2025-12-16"),
                                ("overview.energy_previous", "2025-12-06", "2025-12-11")):
            params = parse_qs(urlsplit(rows[key]["source"]["endpoint"]).query)
            self.assertEqual((params["startDate"], params["endDate"]), ([start], [end]))
            self.assertEqual((params["cityId"], params["stationId"], params["publishedBatchId"]), (["C1"], ["S1"], ["batch-1"]))
        self.assertEqual((self.overview.call_count, self.advanced.call_count), (2, 2))
        self.assertTrue(all(call.kwargs["snapshot"] is self.snapshot for call in self.advanced.call_args_list))
        self.assertEqual(result["observedTopics"], ["overview"])

    def test_insufficient_history_never_shortens_or_queries_a_partial_previous_period(self):
        result = self.build(filters={**self.filters, "start": "2025-12-03", "end": "2025-12-08"})
        self.assertEqual((self.overview.call_count, self.advanced.call_count), (1, 1))
        self.assertFalse(any("previous" in row["id"] or "change" in row["id"] for row in result["evidence"]))
        self.assertTrue(any("不截短" in note for note in result["limitations"]))

    def test_zero_baselines_and_missing_ratio_denominators_stay_unknown(self):
        self.before["metrics"].update(energyWh=0, netPaidCents=0, chargingUtilizationRate=None)
        self.previous_advanced["service"].update(attemptCount=0, successfulAttempts=0)
        rows = self.by_id(self.build())
        self.assertEqual(rows["overview.energy_change"]["value"], 120)
        for key in ("overview.energy_change_pct", "overview.net_paid_change_pct", "overview.utilization_change",
                    "overview.success_rate_previous", "overview.success_rate_change"):
            self.assertIsNone(rows[key]["value"], key)

    def test_station_priorities_keep_three_focus_and_an_independent_control_with_sample_counts(self):
        result = self.build(["stations"])
        rows = self.by_id(result)
        self.assertEqual(rows["stations.station_0_attempts"]["value"], 100)
        self.assertIn("第一关注站", rows["stations.station_0_success"]["label"])
        self.assertIn("同型对照站", rows["stations.control_0_success"]["label"])
        self.assertIn("同站型对照", rows["stations.control_0_success"]["label"])
        self.assertIn("stations.station_2_success", rows)
        self.assertAlmostEqual(rows["stations.focus_control_success_gap"]["value"], -60)
        self.assertFalse(any("小样本站" in row["label"] for row in rows.values()))
        self.assertEqual(rows["stations.comparable_count"]["value"], 4)
        self.assertEqual(self.overview.call_count, 0)
        self.assertEqual(self.advanced.call_count, 1)

    def test_single_or_small_station_samples_explain_missing_control_without_fabrication(self):
        self.current_advanced["stations"] = self.current_advanced["stations"][:1]
        result = self.build(["stations"])
        self.assertFalse(any("control" in row["id"] for row in result["evidence"]))
        self.assertTrue(any("没有独立对照" in note for note in result["limitations"]))
        self.current_advanced["stations"][0]["attemptCount"] = 1
        result = self.build(["stations"])
        self.assertEqual(self.by_id(result)["stations.comparable_count"]["value"], 0)
        self.assertTrue(any("不给出站点优先级" in note for note in result["limitations"]))

    def test_failures_entries_peak_demand_and_different_event_cohorts_are_preserved(self):
        result = self.build(["bottlenecks"])
        rows = self.by_id(result)
        self.assertEqual([rows[f"bottlenecks.failure_{i}_count"]["value"] for i in range(3)], [5, 3, 2])
        self.assertEqual([rows[f"bottlenecks.failure_{i}_share"]["value"] for i in range(3)], [50, 30, 20])
        self.assertEqual([rows[f"bottlenecks.entry_{i}_attempts"]["value"] for i in range(3)], [60, 30, 10])
        self.assertEqual(rows["bottlenecks.peak_attempts"]["value"], 98)
        self.assertEqual(rows["bottlenecks.peak_wait_count"]["value"], 5)
        self.assertIn("18:00", rows["bottlenecks.peak_attempts"]["label"])
        self.assertEqual(rows["bottlenecks.success_rate_change"]["value"], 30)
        self.assertTrue(any("并非随机" in note and "不是每站到达率" in note for note in result["limitations"]))

    def test_all_four_behavior_groups_keep_independent_sample_denominators_and_differences(self):
        result = self.build(["behavior"])
        rows = self.by_id(result)
        self.assertEqual(rows["behavior.segment_0_interval_count"]["value"], 100)
        self.assertEqual(rows["behavior.segment_0_sessions"]["value"], 120)
        self.assertIn("网约车用户", rows["behavior.segment_0_interval"]["label"])
        self.assertIn("家庭用户", rows["behavior.segment_1_interval"]["label"])
        self.assertEqual(rows["behavior.interval_gap"]["value"], 4)
        self.assertEqual(rows["behavior.energy_gap"]["value"], -15)
        self.assertEqual(rows["behavior.first_observed"]["value"], 70)
        self.assertEqual(sum(key.endswith("_interval_count") for key in rows), 4)
        self.assertTrue(any("不取其倒数" in note for note in result["limitations"]))

    def test_unobserved_intervals_do_not_become_zero_frequency_or_supported_comparisons(self):
        for row in self.current_advanced["behavior"]["segments"]:
            row.update(intervalCount=0, meanIntervalDays=None)
        rows = self.by_id(self.build(["behavior"]))
        self.assertNotIn("behavior.interval_gap", rows)
        self.assertIsNone(rows["behavior.segment_0_interval"]["value"])
        self.assertEqual(rows["behavior.segment_0_interval_count"]["value"], 0)

    def test_budget_keeps_whole_groups_four_behavior_segments_and_derived_dependencies(self):
        for topics in combinations(analysis.TOPICS, 3):
            with self.subTest(topics=topics):
                result = self.build(topics)
                rows = self.by_id(result)
                self.assertLessEqual(len(rows), 48)
                self.assertEqual(len(rows), len(result["evidence"]))
                if "behavior" in topics:
                    for index in range(4):
                        self.assertTrue(all(f"behavior.segment_{index}_{suffix}" in rows
                                            for suffix in ("sessions", "interval_count", "interval", "energy")))
                if "stations" in topics:
                    self.assertIn("stations.control_0_attempts", rows)
                    for index in range(3):
                        prefix = f"stations.station_{index}_"
                        matches = [key for key in rows if key.startswith(prefix)]
                        self.assertIn(len(matches), (0, 4))
                for row in rows.values():
                    if row["source"]["field"].startswith("derived:"):
                        for dependency in re.findall(r"(?:overview|bottlenecks|stations|behavior)\.[a-z0-9_]+", row["source"]["field"]):
                            self.assertIn(dependency, rows)

    def test_empty_current_observations_do_not_inherit_previous_or_static_inventory(self):
        self.now["metrics"] = dict(observedHours=0, startedSessions=0, activeUsers=0, paidCents=0, refundCents=0,
                                   stationCount=25, chargerCount=75, energyWh=None)
        self.current_advanced = dict(stations=[], service=dict(attemptCount=0, successfulAttempts=0, failedAttempts=0),
                                     behavior=dict(sessionCount=0, intervalCount=0, firstObservedCount=0))
        result = self.build(analysis.TOPICS)
        self.assertFalse(result["observed"])
        self.assertEqual(result["observedTopics"], [])
        self.assertTrue(any("不能补作本期观测" in note for note in result["limitations"]))

    def test_cancelled_expired_and_integrity_errors_stop_without_fallback(self):
        cancelled = threading.Event()
        cancelled.set()
        for kwargs in ({"cancelled": cancelled}, {"deadline": time.monotonic() - 1}):
            with self.assertRaises(ApiError) as caught:
                self.build(**kwargs)
            self.assertEqual(caught.exception.code, "ADVISOR_TIMEOUT")
        self.overview.assert_not_called()
        self.advanced.assert_not_called()
        for status, code in ((409, "BATCH_MISMATCH"), (503, "DATA_NOT_READY")):
            self.advanced.side_effect = ApiError(status, code, "不可用")
            with self.assertRaises(ApiError) as caught:
                self.build(["bottlenecks"])
            self.assertEqual(caught.exception.code, code)

    def test_model_topic_is_left_for_existing_registry_and_invalid_dates_never_query(self):
        self.assertEqual(self.build(["models"])["evidence"], [])
        with self.assertRaises(ApiError) as caught:
            self.build(filters={**self.filters, "start": "2025-11-01"})
        self.assertEqual(caught.exception.code, "DATE_OUT_OF_RANGE")
        self.overview.assert_not_called()
        self.advanced.assert_not_called()


if __name__ == "__main__":
    unittest.main()
