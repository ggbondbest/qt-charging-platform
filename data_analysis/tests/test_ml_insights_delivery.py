"""Delivery regressions: as-of features, fixed anomaly references, artifact guards."""
import copy
import json
from pathlib import Path
import shutil
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

try:
    import numpy as np
    import pandas as pd
    from data_analysis.ml import insights
    from data_analysis.ml.churn import common as churn
    from data_analysis.ml.anomaly import delivery as anomaly
    HAS_DEPS = True
except ImportError:
    HAS_DEPS = False


def fixture():
    t = pd.Timestamp("2026-05-14T16:00:00")
    attempts = pd.DataFrame({
        "attempt_id": ["old", "done", "ongoing", "queued", "returned", "late"],
        "user_id": ["U1"] * 6, "station_id": ["S1"] * 6,
        "attempted_at": [t - pd.Timedelta(days=100), t - pd.Timedelta(days=2),
                         t - pd.Timedelta(hours=1), t - pd.Timedelta(minutes=10),
                         t + pd.Timedelta(days=1), t + pd.Timedelta(days=15)],
        "outcome": ["STARTED"] * 6})
    sessions = pd.DataFrame({
        "session_id": ["s0", "s1", "s2", "s3"], "attempt_id": ["old", "done", "ongoing", "queued"],
        "user_id": ["U1"] * 4,
        "started_at": [t - pd.Timedelta(days=100), t - pd.Timedelta(days=2),
                       t - pd.Timedelta(hours=1), t + pd.Timedelta(minutes=5)],
        "ended_at": [t - pd.Timedelta(days=100) + pd.Timedelta(hours=1),
                     t - pd.Timedelta(days=2) + pd.Timedelta(hours=1),
                     t + pd.Timedelta(hours=1), t + pd.Timedelta(hours=1)],
        "energy_wh": [1000, 20000, 999000, 999000], "total_fee_cents": [100, 3000, 999000, 999000],
        "campaign_id": [None] * 4})
    users = pd.DataFrame({"user_id": ["U1", "U2", "FUTURE"], "home_city_id": ["DL"] * 3,
                          "registered_at": [t - pd.Timedelta(days=200), t - pd.Timedelta(days=1), t + pd.Timedelta(days=1)],
                          "segment": ["PRIVATE"] * 3, "acquisition_channel": ["NATURAL"] * 3,
                          "membership": ["BASIC"] * 3})
    vehicles = pd.DataFrame({"vehicle_id": ["V1", "V2", "V3"], "user_id": ["U1", "U2", "FUTURE"],
                             "battery_capacity_kwh": [60.] * 3, "max_charge_kw": [100.] * 3,
                             "vehicle_class": ["SEDAN"] * 3})
    queues = pd.DataFrame({"user_id": ["U1", "U1", "U1"],
                           "joined_at": [t - pd.Timedelta(days=5), t - pd.Timedelta(minutes=10), t + pd.Timedelta(hours=1)],
                           "resolved_at": [t - pd.Timedelta(days=5) + pd.Timedelta(minutes=10), t + pd.Timedelta(minutes=5), t + pd.Timedelta(hours=2)],
                           "status": ["SERVED", "ABANDONED", "ABANDONED"]})
    return {"charging_attempts": attempts, "charging_sessions": sessions, "users": users,
            "vehicles": vehicles, "queue_entries": queues}


@unittest.skipUnless(HAS_DEPS, "ML dependencies not installed")
class ChurnPointInTime(unittest.TestCase):
    def test_money_units_count_window_and_completed_sessions(self):
        rows = churn.build_user_table(tables=fixture()).set_index("user_id")
        self.assertEqual(rows.loc["U1", "attempts_90"], 3)
        self.assertEqual(rows.loc["U1", "started_90"], 2)
        self.assertEqual(rows.loc["U1", "energy_kwh_90"], 20)
        self.assertEqual(rows.loc["U1", "spend_yuan_90"], 30)
        self.assertEqual(rows.loc["U1", "fee_per_kwh_90"], 1.5)
        self.assertEqual(rows.loc["U1", "queues_90"], 2)
        self.assertEqual(rows.loc["U1", "queue_abandon_share"], 0)

    def test_future_fees_outcomes_and_rows_cannot_change_features(self):
        before = fixture()
        after = copy.deepcopy(before)
        after["charging_sessions"].loc[2:, ["energy_wh", "total_fee_cents"]] = 1
        after["queue_entries"].loc[1:, "status"] = "SERVED"
        after["charging_attempts"].loc[5, "attempted_at"] += pd.Timedelta(days=200)
        a = churn.build_user_table(tables=before).set_index("user_id")
        b = churn.build_user_table(tables=after).set_index("user_id")
        pd.testing.assert_frame_equal(a[churn.FEATURE_COLUMNS], b[churn.FEATURE_COLUMNS])

    def test_registration_and_label_window(self):
        tables = fixture()
        result = churn.build_user_table(tables=tables).set_index("user_id")
        self.assertNotIn("FUTURE", result.index)
        self.assertEqual(result.loc["U1", "churned_14d"], 0)
        self.assertEqual(result.loc["U2", "churned_14d"], 1)
        self.assertEqual(result.loc["U2", "attempts_90"], 0)
        self.assertTrue(pd.isna(result.loc["U2", "days_since_last"]))
        tables["charging_attempts"] = tables["charging_attempts"].drop(index=4)
        self.assertEqual(churn.build_user_table(tables=tables).set_index("user_id").loc["U1", "churned_14d"], 1)

    def test_window_anchor_is_observation_not_last_event(self):
        tables = fixture()
        cutoff = churn.OBSERVE_END
        # Leave last request at cutoff-20d. The cutoff-40d request must not
        # become a '30-day' request because the entire platform went quiet.
        tables["charging_attempts"] = tables["charging_attempts"].iloc[:2].copy()
        tables["charging_attempts"]["attempted_at"] = [cutoff - pd.Timedelta(days=40), cutoff - pd.Timedelta(days=20)]
        row = churn.build_user_table(tables=tables).set_index("user_id").loc["U1"]
        self.assertEqual(row.attempts_30, 1)
        self.assertEqual(row.attempts_14, 0)

    def test_observation_is_business_midnight_in_utc(self):
        self.assertEqual(churn.OBSERVE_END, pd.Timestamp("2026-05-14T16:00:00"))


@unittest.skipUnless(HAS_DEPS, "ML dependencies not installed")
class FixedAnomalyReferences(unittest.TestCase):
    def test_whole_session_purging(self):
        t = anomaly.TRAIN_END
        v = anomaly.VALID_END
        rows = pd.DataFrame({"started_at": [t - pd.Timedelta(hours=2), t - pd.Timedelta(hours=1), t, v],
                             "ended_at": [t - pd.Timedelta(hours=1), t + pd.Timedelta(hours=1), t + pd.Timedelta(hours=1), v + pd.Timedelta(hours=1)]})
        self.assertEqual(anomaly.split_sessions(rows).tolist(), ["TRAIN", "PURGED", "VALIDATION", "TEST"])

    def test_reference_cdf_is_independent_of_incoming_batch(self):
        reference = np.array([0., 1., 2., 3.])
        first = anomaly.empirical_cdf(reference, [1.5])[0]
        enlarged = anomaly.empirical_cdf(reference, [1.5, 999., -99.])[0]
        self.assertEqual(first, enlarged)
        self.assertEqual(first, .5)
        with self.assertRaises(ValueError):
            anomaly.empirical_cdf(reference, [np.nan])

    def test_future_context_is_not_fitted_and_unknown_charger_has_fallback(self):
        points = pd.DataFrame({"session_id": ["train", "test"], "charger_id": ["CH", "CH"],
                               "currentBin": [1, 1], "socBin": [2, 2], "max_temperature_c": [30., 300.],
                               "cellDifference": [.02, 1.], "charge_current_a": [60., 600.]})
        context = anomaly.fit_context(points, ["train"])
        target = points.iloc[[1]].copy()
        target["charger_id"] = "NEW"
        out = anomaly.context_features(target, context).iloc[0]
        self.assertEqual(out.contextTempExcess, 270)
        self.assertEqual(out.contextFallback, 1)
        self.assertTrue(np.isfinite(out[anomaly.CONTEXT_FEATURES].to_numpy(dtype=float)).all())


@unittest.skipUnless(HAS_DEPS, "ML dependencies not installed")
class ArtifactGuards(unittest.TestCase):
    def test_empty_directory_has_explicit_not_ready(self):
        with TemporaryDirectory() as temp:
            service = insights.InsightsService(temp)
            self.assertEqual(service.status()["status"], "NOT_READY")
            with self.assertRaises(RuntimeError):
                service.list_churn()

    def test_invalid_limit(self):
        for bad in (0, 101, True, 3.5, "20"):
            with self.assertRaises(ValueError):
                insights.InsightsService._limit(bad)
        self.assertEqual(insights.InsightsService._limit(20), 20)

    def test_untrusted_artifact_paths_rejected_before_unpickling(self):
        with TemporaryDirectory() as temp:
            Path(temp, insights.METADATA).write_text(json.dumps({"schemaVersion": "1.0.0", "artifactFile": "../escape.joblib"}), encoding="utf-8")
            with patch.object(insights.joblib, "load") as loader:
                with self.assertRaises(ValueError):
                    insights.InsightsService(temp)
                loader.assert_not_called()


@unittest.skipUnless(HAS_DEPS, "ML dependencies not installed")
class TrainedDeliveryIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (insights.OUTPUT / insights.METADATA).exists():
            raise unittest.SkipTest("Run python -m data_analysis.ml.insights train for artifact-level verification")
        cls.service = insights.InsightsService()

    def test_real_prediction_lists_are_test_only_and_have_no_probability_claim(self):
        users = self.service.list_churn(3)
        sessions = self.service.list_anomalies(3)
        self.assertEqual(len(users["items"]), 3)
        self.assertEqual(len(sessions["items"]), 3)
        for row in users["items"]:
            self.assertEqual(row["evaluationSplit"], "TEST")
            self.assertTrue(0 <= row["riskScore"] <= 1)
            self.assertNotIn("probability", row)
            self.assertAlmostEqual(row["riskScore"], self.service.predict_user(row["userId"])["riskScore"])
        for row in sessions["items"]:
            self.assertEqual(row["evaluationSplit"], "TEST")
            self.assertTrue(row["flagged"])
            self.assertTrue(row["explanations"])
        json.dumps({"users": users, "sessions": sessions, "report": self.service.report()}, allow_nan=False)

    def test_unknown_ids_do_not_fabricate_predictions(self):
        with self.assertRaises(KeyError):
            self.service.predict_user("unknown")
        with self.assertRaises(KeyError):
            self.service.inspect_session("unknown")

    def test_source_mismatch_rejected_before_unpickling(self):
        with patch.object(insights, "source_binding", return_value={"datasetId": "OTHER"}), patch.object(insights.joblib, "load") as loader:
            with self.assertRaises(ValueError):
                insights.InsightsService()
            loader.assert_not_called()

    def test_corrupt_bundle_rejected_before_unpickling(self):
        with TemporaryDirectory() as temp:
            for name in (insights.ARTIFACT, insights.METADATA, insights.REPORT):
                shutil.copy2(insights.OUTPUT / name, Path(temp) / name)
            with (Path(temp) / insights.ARTIFACT).open("ab") as file:
                file.write(b"tampered")
            with patch.object(insights.joblib, "load") as loader:
                with self.assertRaises(ValueError):
                    insights.InsightsService(temp)
                loader.assert_not_called()

    def test_frozen_directory_reused_without_retraining(self):
        with patch.object(insights.churn, "fit") as fitter:
            report = insights.train()
            self.assertEqual(report["status"], "READY")
            fitter.assert_not_called()


if __name__ == "__main__":
    unittest.main()
