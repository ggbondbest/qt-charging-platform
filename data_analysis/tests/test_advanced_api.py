"""Serving semantics over tiny independent aggregates; no Spark required."""

from collections import defaultdict
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

HAS_API = all(importlib.util.find_spec(name) for name in ("fastapi", "httpx"))
if HAS_API:
    from fastapi.testclient import TestClient
    from data_analysis.backend.advanced import (analyze, flow_analysis, hour_metrics, load_bundle,
                                                pearson, retention_analysis)
    from data_analysis.backend.app import create_app
    from data_analysis.backend.errors import ApiError
    from data_analysis.backend.advanced_models import AdvancedAnalytics
    from data_analysis.tests.test_analytics_api import fixture


def write_bundle(root, metadata):
    stations = [dict(station_id="S1", city_id="C1", station_name="A站", city_name="甲市", site_type="OFFICE",
                     interfaces=[dict(connector_type="AC", charger_count=1, rated_power_kw=7),
                                 dict(connector_type="DC", charger_count=2, rated_power_kw=180)]),
                dict(station_id="S2", city_id="C1", station_name="B站", city_name="甲市", site_type="MALL", interfaces=[])]
    day = dict(station_id="S1", city_id="C1", business_date="2025-12-01", site_type="OFFICE", energy_wh=4000,
               observed_hours=2, complete_hours=2, complete_charging_samples=3, complete_sample_count=10,
               queue_wait_seconds_sum=600, queue_wait_count=2, occupied_seconds_sum=1200,
               connected_seconds_sum=3600, net_paid_cents=499, session_count=2, attempt_count=3,
               successful_attempt_count=2, weather_temperature_sum=20, weather_hour_count=2)
    hour = dict(station_id="S1", city_id="C1", business_date="2025-12-01", local_hour=0,
                is_complete=True, has_observation=True, sample_count=2, charging_samples=1,
                energy_wh=1000, temperature_c=-1)
    attempt = dict(station_id="S1", city_id="C1", business_date="2025-12-01", access_path="DIRECT",
                   outcome="STARTED", session_status="COMPLETED", attempt_count=2, local_hour=0, failure_reason="NONE")
    segment = dict(station_id="S1", city_id="C1", business_date="2025-12-01", site_type="OFFICE",
                   user_segment="COMMUTER", battery_capacity_band="GE70", connector_type="DC", session_count=2,
                   energy_wh=4000, active_seconds_sum=2400, connected_seconds_sum=3600, occupied_seconds_sum=1200,
                   active_duration_count=2, connected_duration_count=2, occupied_duration_count=2)
    tables = {
        "station_day": [day],
        "station_hour": [hour, dict(hour, business_date="2025-12-02", sample_count=8, charging_samples=2,
                                     energy_wh=3000, temperature_c=2),
                         dict(hour, business_date="2025-12-03", local_hour=1, is_complete=False,
                              has_observation=False, sample_count=0, charging_samples=0, energy_wh=None)],
        "attempt_flow": [attempt, dict(attempt, access_path="QUEUE", outcome="ABANDONED",
                                        session_status="NO_SESSION", attempt_count=1, failure_reason="QUEUE_PATIENCE")],
        "session_segments": [segment],
        "user_behavior": [dict(station_id="S1", city_id="C1", business_date="2025-12-01", user_segment="COMMUTER",
                               gap_bucket="FIRST_OBSERVED", energy_bucket="LT10", session_count=1, interval_seconds_sum=0,
                               interval_count=0, first_observed_count=1, energy_wh=1000),
                          dict(station_id="S1", city_id="C1", business_date="2025-12-01", user_segment="COMMUTER",
                               gap_bucket="1_TO_3D", energy_bucket="LT10", session_count=1, interval_seconds_sum=86400,
                               interval_count=1, first_observed_count=0, energy_wh=3000)],
        "service_hour": [dict(station_id="S1", city_id="C1", business_date="2025-12-01", local_hour=0,
                              queue_wait_seconds_sum=600, queue_wait_count=2, session_count=2,
                              occupied_seconds_sum=1200, connected_seconds_sum=3600)],
        "retention": [dict(scope_type="ALL", scope_id="ALL", cohort_month="2025-12-01", month_offset=0, n=2, cohort_size=2),
                      dict(scope_type="ALL", scope_id="ALL", cohort_month="2025-12-01", month_offset=1, n=1, cohort_size=2),
                      dict(scope_type="ALL", scope_id="ALL", cohort_month="2026-01-01", month_offset=0, n=3, cohort_size=3)]}
    manifest = {key: metadata[key] for key in ("datasetId", "publishedBatchId", "pipelineRunId", "sourceManifestSha256")}
    manifest.update(schemaVersion="1.0.0", analysisVersion="2.0.0", analysisId="test-analysis", generatedAt="2026-03-01T00:00:00Z",
                    engine="PySpark", sparkVersion="3.5.6", rawFactsCollected=False, referenceAggregatesUsedAsInput=False,
                    authenticatedCleanInventoryVerified=True,
                    invariants={"attemptsConserved": True}, periodStart="2025-12-01", periodEnd="2026-03-01",
                    completeMonthsThrough="2026-02-28", stations=stations, tables={})
    for name, rows in tables.items():
        path = root / (name + ".json.gz")
        path.write_bytes(gzip.compress(json.dumps(rows).encode()))
        manifest["tables"][name] = dict(file=path.name, rows=len(rows), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    (root / "advanced_manifest.json").write_text(json.dumps(manifest))
    (root / "_SUCCESS").touch()
    return manifest, tables


@unittest.skipUnless(HAS_API, "Install requirements-api.txt for advanced analytics")
class AdvancedApiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="advanced-api-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.db = self.root / "fixture.sqlite"
        self.meta = fixture(self.db)
        self.manifest, self.tables = write_bundle(self.root, self.meta)
        self.filters = dict(start="2025-12-01", end="2025-12-04", city=None, station=None)

    def test_weighted_utilization_not_mean_of_ratios(self):
        metrics = hour_metrics(self.tables["station_hour"])
        self.assertEqual(metrics["chargingUtilization"], 3 / 10)
        self.assertEqual(metrics["meanPowerKw"], 2)
        self.assertEqual(metrics["sampleHours"], 2)
        missing = hour_metrics(self.tables["station_hour"][2:])
        self.assertIsNone(missing["energyKwh"])
        self.assertIsNone(missing["chargingUtilization"])
        zero = hour_metrics([dict(self.tables["station_hour"][0], energy_wh=0, charging_samples=0)])
        self.assertEqual(zero["energyKwh"], 0)
        self.assertEqual(zero["chargingUtilization"], 0)

    def test_exact_scope_and_typed_payload(self):
        result = analyze(self.meta, self.filters, directory=self.root)
        AdvancedAnalytics.model_validate(result)
        self.assertEqual(result["summary"]["sessionCount"], 2)
        self.assertEqual(result["summary"]["attemptCount"], 3)
        self.assertEqual(len(result["heatmap"]), 168)
        self.assertEqual(result["weather"][0]["temperatureBin"], -5)
        self.assertEqual(result["stations"][0]["meanWaitMinutes"], 5)
        self.assertAlmostEqual(result["stations"][0]["overstayShare"], 1/3)
        self.assertEqual(result["stations"][0]["netCashYuan"], 4.99)
        self.assertEqual(result["segments"][0]["meanChargeMinutes"], 20)
        empty = analyze(self.meta, self.filters, site_type="MALL", directory=self.root)
        self.assertEqual(empty["summary"]["sessionCount"], 0)
        self.assertTrue(all(cell["energyKwh"] is None for cell in empty["heatmap"]))
        self.assertEqual(empty["retention"]["cohorts"], [])
        with self.assertRaises(ApiError) as caught:
            analyze(self.meta, self.filters, site_type="ANY_SQL", directory=self.root)
        self.assertEqual(caught.exception.status, 422)

    def test_conserved_sankey_not_mixed_queue_payment_denominators(self):
        flow = flow_analysis(self.tables["attempt_flow"])
        incoming, outgoing = defaultdict(int), defaultdict(int)
        for link in flow["links"]:
            incoming[link["target"]] += link["value"]
            outgoing[link["source"]] += link["value"]
        self.assertEqual(outgoing["全部充电尝试"], 3)
        for node in set(incoming) & set(outgoing):
            self.assertEqual(incoming[node], outgoing[node])
        self.assertIn("未成功 · 等候超出耐心", incoming)
        self.assertEqual(incoming["成功 · 开始充电"], 2)
        self.assertFalse(any("会话 ·" in node["name"] for node in flow["nodes"]))

    def test_behavior_first_observed_excluded_and_energy_denominator_separate(self):
        result = analyze(self.meta, self.filters, directory=self.root)["behavior"]
        self.assertEqual((result["sessionCount"], result["intervalCount"], result["firstObservedCount"]), (2, 1, 1))
        self.assertEqual(result["segments"][0]["meanIntervalDays"], 1)
        self.assertEqual(result["segments"][0]["meanEnergyKwh"], 2)
        intervals = {r["bucket"]: r for r in result["intervals"]}
        self.assertNotIn("FIRST_OBSERVED", intervals)
        self.assertEqual(intervals["1_TO_3D"]["share"], 1)
        self.assertEqual(intervals["LT1D"]["count"], 0)
        self.assertEqual(sum(r["count"] for r in result["energy"]), 2)
        self.assertEqual(sum(r["share"] for r in result["energy"]), 1)

    def test_first_only_group_is_unknown_interval_not_zero_day_return(self):
        from data_analysis.backend.advanced import recharge_behavior
        result = recharge_behavior(self.tables["user_behavior"][:1])
        self.assertEqual(result["intervalCount"], 0)
        self.assertTrue(all(cell["share"] is None for cell in result["intervals"]))
        self.assertIsNone(result["segments"][0]["meanIntervalDays"])
        self.assertEqual(result["energy"][0]["share"], 1)

    def test_site_hour_context_linked_but_not_mixed_event_denominators(self):
        result = analyze(self.meta, self.filters, directory=self.root)["service"]
        self.assertEqual((result["attemptCount"], result["successfulAttempts"], result["failedAttempts"]), (3, 2, 1))
        self.assertEqual(len(result["cells"]), 48)
        cell = next(r for r in result["cells"] if r["siteType"] == "OFFICE" and r["hour"] == 0)
        self.assertEqual(cell["successRate"], 2/3)
        self.assertEqual(cell["meanWaitMinutes"], 5)
        self.assertEqual(cell["queueWaitCount"], 2)
        self.assertEqual(cell["overstayShare"], 1/3)
        self.assertEqual(cell["chargingUtilization"], 3/10)
        self.assertEqual(cell["completeStationHours"], 2)
        self.assertEqual(sum(r["chargerCount"] for r in cell["interfaces"]), 3)  # Not multiplied by days/hours.
        self.assertEqual(sum(r["ratedPowerKw"] for r in cell["interfaces"]), 187)
        cause = result["failures"][0]
        self.assertEqual((cause["reason"], cause["count"], cause["shareOfFailures"], cause["shareOfAttempts"]),
                         ("QUEUE_PATIENCE", 1, 1, 1/3))
        empty = next(r for r in result["cells"] if r["siteType"] == "OFFICE" and r["hour"] == 1)
        self.assertEqual(empty["attemptCount"], 0)
        self.assertIsNone(empty["successRate"])
        self.assertIsNone(empty["meanWaitMinutes"])
        self.assertIsNone(empty["overstayShare"])
        self.assertIsNone(empty["chargingUtilization"])
        self.assertEqual(empty["failures"], [])

    def test_new_views_honor_site_date_and_station_filters(self):
        for filters, site_type in [(self.filters, "MALL"), (dict(self.filters, station="S2"), None),
                                    (dict(self.filters, start="2025-12-02"), None)]:
            result = analyze(self.meta, filters, site_type=site_type, directory=self.root)
            self.assertEqual(result["behavior"]["sessionCount"], 0)
            self.assertEqual(result["service"]["attemptCount"], 0)
            self.assertTrue(all(r["successRate"] is None for r in result["service"]["cells"]))

    def test_failure_causes_not_collapsed_and_unknown_reason_safe(self):
        from data_analysis.backend.advanced import failure_summary
        template = self.tables["attempt_flow"][1]
        attempts = [dict(template, failure_reason=reason, attempt_count=n)
                    for reason, n in [("NO_AVAILABLE_CHARGER", 9), ("QUEUE_PATIENCE", 3), ("CALL_TIMEOUT", 1), ("bad", 1)]]
        reasons = failure_summary(attempts)
        self.assertEqual(sum(r["count"] for r in reasons), 14)
        self.assertEqual({r["reason"] for r in reasons}, {"NO_AVAILABLE_CHARGER", "QUEUE_PATIENCE", "CALL_TIMEOUT", "UNKNOWN"})
        self.assertEqual(reasons[0]["shareOfFailures"], 9/14)
        self.assertEqual(sum(link["value"] for link in flow_analysis(attempts)["links"]), 28)

    def test_first_observed_retention_censoring_and_exact_denominator(self):
        filters = dict(self.filters, end="2026-02-20")
        data = retention_analysis(self.tables["retention"], filters, self.manifest, {}, None)
        self.assertEqual(data["observationEnd"], "2026-02-01")
        first, second = data["cohorts"]
        self.assertEqual(first["cells"][1]["rate"], .5)
        self.assertIsNone(second["cells"][1]["rate"])
        self.assertIsNone(second["cells"][1]["users"])
        later = retention_analysis(self.tables["retention"], dict(filters, end="2026-03-01"), self.manifest, {}, None)
        self.assertEqual(later["cohorts"][0]["cells"][2]["rate"], 0)

    def test_correlation_is_pairwise_and_constant_is_not_zero(self):
        self.assertEqual(pearson([(1, 2), (2, 4), (3, 6)]), 1)
        self.assertEqual(pearson([(1, 6), (2, 4), (3, 2)]), -1)
        self.assertIsNone(pearson([(1, 2)] * 5))
        self.assertIsNone(pearson([(1, 2), (2, 4)]))

    def test_batch_mismatch_hash_tamper_and_missing_fail_closed(self):
        with self.assertRaises(ApiError) as caught:
            load_bundle(dict(self.meta, publishedBatchId="another-batch"), self.root)
        self.assertEqual(caught.exception.status, 409)
        load_bundle(self.meta, self.root)
        target = self.root / "station_day.json.gz"
        target.write_bytes(gzip.compress(b"[]"))
        with self.assertRaises(ApiError) as caught:
            load_bundle(self.meta, self.root)
        self.assertEqual(caught.exception.status, 503)
        with self.assertRaises(ApiError):
            load_bundle(self.meta, self.root / "missing")

    def test_incomplete_or_running_bundle_is_rejected_even_after_cache_hit(self):
        load_bundle(self.meta, self.root)
        marker = self.root / "_SUCCESS"
        marker.unlink()
        with self.assertRaises(ApiError):
            load_bundle(self.meta, self.root)
        marker.touch()
        (self.root / "_RUNNING").touch()
        with self.assertRaises(ApiError):
            load_bundle(self.meta, self.root)

    def test_unverified_clean_inventory_is_not_accepted_as_same_batch(self):
        manifest = dict(self.manifest, authenticatedCleanInventoryVerified=False)
        (self.root / "advanced_manifest.json").write_text(json.dumps(manifest))
        with self.assertRaises(ApiError) as caught:
            load_bundle(self.meta, self.root)
        self.assertEqual(caught.exception.status, 503)

    def test_http_batch_filter_validation_and_safe_errors(self):
        with patch.dict("os.environ", {"ANALYTICS_ADVANCED_BUNDLE": str(self.root)}):
            client = TestClient(create_app(self.db))
            response = client.get("/api/v1/dashboard/advanced", params={"datasetId": "fixture", "cityId": "C1"})
            self.assertEqual(response.status_code, 200, response.text)
            self.assertEqual(response.json()["meta"]["publishedBatchId"], "batch-1")
            self.assertEqual(client.get("/api/v1/dashboard/advanced?cityId=missing").status_code, 404)
            self.assertEqual(client.get("/api/v1/dashboard/advanced?endDate=2026-01-01").status_code, 422)
            self.assertEqual(client.get("/api/v1/dashboard/advanced?publishedBatchId=stale").status_code, 409)
            self.assertEqual(client.get("/api/v1/dashboard/advanced?unsupported=yes").status_code, 422)
            (self.root / "station_day.json.gz").write_bytes(b"invalid")
            invalid = client.get("/api/v1/dashboard/advanced")
            self.assertEqual(invalid.status_code, 503)
            self.assertNotIn(str(self.root), invalid.text)


if __name__ == "__main__":
    unittest.main()
