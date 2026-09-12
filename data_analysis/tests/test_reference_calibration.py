"""Aggregate calibration tests use fictional local fixtures, never user data."""

from contextlib import redirect_stderr, redirect_stdout
import csv
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest

from data_analysis.charging_data.calibrate_reference import calibrate_reference, main, write_profile


FIELDS = ["sessionId", "kwhTotal", "chargeTimeHrs", "startTime", "weekday", "userId",
          "stationId", "locationId", "facilityType", "created", "ended", "endTime"]


class ReferenceCalibrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="reference-calibration-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "private-reference"
        self.source.mkdir()
        # Zero-energy records remain in arrival counts, but must not contaminate
        # the energy/connection joint calibration cohort.
        values = [(2, 1, 9, "Mon"), (4, 2, 11, "Tue"), (6, 3, 17, "Fri"),
                  (8, 4, 21, "Sat"), (0, 8, 23, "Sun")]
        self.rows = [dict(zip(FIELDS, [f"secret-session-{index}", energy, duration, hour, day,
                         f"private-user-{index % 2}", f"hidden-station-{index % 2}",
                         "private-location", "sensitive-category", "0014-11-18 09:00:00",
                         "0014-11-18 10:00:00", 10]))
                     for index, (energy, duration, hour, day) in enumerate(values)]
        self._write_rows()
        with (self.source / "nvv2t_md_end.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, ["stationId", "locationId", "facilityType", "address"])
            writer.writeheader()
            writer.writerows([{"stationId": f"hidden-station-{index}", "locationId": "private-location",
                               "facilityType": "sensitive-category", "address": "private-address"}
                              for index in range(2)])

    def _write_rows(self):
        with (self.source / "nvv2t.csv").open("w", encoding="utf-8-sig", newline="") as stream:
            writer = csv.DictWriter(stream, FIELDS)
            writer.writeheader()
            writer.writerows(self.rows)

    def test_stable_arrival_contract_and_joint_positive_filter(self):
        profile = calibrate_reference(self.source)
        arrival, session = profile["arrival"], profile["session_metrics"]
        for field in ("global_hour_counts", "weekday_hour_counts", "weekend_hour_counts"):
            self.assertEqual(len(arrival[field]), 24)
        self.assertEqual(len(arrival["weekday_counts"]), 7)
        self.assertEqual(sum(arrival["global_hour_counts"]), 5)
        self.assertEqual(arrival["global_hour_counts"][23], 1)
        self.assertEqual(sum(arrival["weekday_hour_counts"]), 3)
        self.assertEqual(sum(arrival["weekend_hour_counts"]), 2)
        self.assertEqual(session["sample_count"], 4)
        self.assertEqual(session["energy_kwh"]["quantiles"]["p50"], 5)
        self.assertEqual(session["connection_hours"]["quantiles"]["p50"], 2.5)
        self.assertAlmostEqual(session["energy_connection_pearson"], 1)
        joint = session["energy_connection_joint"]["counts"]
        self.assertEqual(sum(map(sum, joint)), 4)
        self.assertEqual([sum(row) for row in joint], session["energy_kwh"]["histogram"]["counts"])
        self.assertEqual([sum(row[index] for row in joint) for index in range(len(joint[0]))],
                         session["connection_hours"]["histogram"]["counts"])

    def test_output_is_private_aggregate_only_and_deterministic(self):
        destination = self.root / "profile.json"
        before = (self.source / "nvv2t.csv").read_bytes()
        profile = write_profile(self.source, destination)
        self.assertEqual(profile, calibrate_reference(self.source / "nvv2t.csv"))
        text = destination.read_text(encoding="utf-8")
        for private in (str(self.root), "private-user", "hidden-station", "private-location",
                        "private-address", "secret-session", "sensitive-category"):
            self.assertNotIn(private, text)
        self.assertEqual(profile["provenance"]["source_files"][0]["sha256"], hashlib.sha256(before).hexdigest())
        self.assertEqual(before, (self.source / "nvv2t.csv").read_bytes())
        second = self.root / "second.json"
        write_profile(self.source, second)
        self.assertEqual(destination.read_bytes(), second.read_bytes())

    def test_years_are_diagnosed_not_corrected_and_metadata_is_checked(self):
        profile = calibrate_reference(self.source)
        self.assertEqual(profile["quality"]["created_year_before_2000"], 5)
        self.assertEqual(profile["quality"]["created_timezone_unspecified"], 5)
        self.assertEqual(profile["quality"]["metadata"]["missing_station_references"], 0)
        self.assertEqual(profile["users"]["repeat_user_count"], 2)
        self.assertNotIn("daily_arrival_frequency", profile["arrival"])

    def test_nonpositive_duration_excluded_from_both_marginals(self):
        self.rows[0]["chargeTimeHrs"] = 0
        self._write_rows()
        profile = calibrate_reference(self.source)
        self.assertEqual(profile["session_metrics"]["sample_count"], 3)
        self.assertEqual(sum(profile["arrival"]["global_hour_counts"]), 5)
        self.assertEqual(profile["quality"]["excluded_from_energy_connection_metrics"], 2)

    def test_peak_hour_ties_use_earliest_hour_without_exporting_station_ids(self):
        original = dict(self.rows[0])
        self.rows = [dict(original, sessionId=f"secret-station-visit-{index}", startTime=17 if index < 10 else 9)
                     for index in range(20)]
        self._write_rows()
        heterogeneity = calibrate_reference(self.source)["station_heterogeneity"]
        self.assertEqual(heterogeneity["eligible_station_count"], 1)
        self.assertEqual(heterogeneity["peak_hour_station_counts"][9], 1)
        self.assertEqual(heterogeneity["peak_hour_station_counts"][17], 0)

    def test_extreme_finite_values_are_kept_in_terminal_histogram_bin(self):
        self.rows[0].update(kwhTotal=1000, chargeTimeHrs=200)
        self._write_rows()
        session = calibrate_reference(self.source)["session_metrics"]
        self.assertEqual(session["energy_kwh"]["histogram"]["edges"][-1], 1000)
        self.assertEqual(session["connection_hours"]["histogram"]["edges"][-1], 200)
        self.assertEqual(session["energy_kwh"]["histogram"]["counts"][-1], 1)

    def test_existing_output_is_never_overwritten(self):
        destination = self.root / "existing.json"
        destination.write_text("keep this", encoding="utf-8")
        with self.assertRaises(FileExistsError):
            write_profile(self.source, destination)
        self.assertEqual(destination.read_text(encoding="utf-8"), "keep this")
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            main(["--input", str(self.source), "--output", str(destination)])
        self.assertNotEqual(error.exception.code, 0)

    def test_invalid_values_and_duplicate_sessions_fail_before_writing(self):
        for index, changes in enumerate(({"kwhTotal": "nan"}, {"startTime": 24},
                                         {"weekday": "unknown"}, {"sessionId": self.rows[1]["sessionId"]})):
            with self.subTest(changes=changes):
                original = dict(self.rows[0])
                self.rows[0].update(changes)
                self._write_rows()
                destination = self.root / f"invalid-{index}.json"
                with self.assertRaises(ValueError):
                    write_profile(self.source, destination)
                self.assertFalse(destination.exists())
                self.rows[0] = original

    def test_cli_success_and_optional_metadata(self):
        (self.source / "nvv2t_md_end.csv").unlink()
        destination = self.root / "cli.json"
        with redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--input", str(self.source), "--output", str(destination)]), 0)
        self.assertFalse(json.loads(destination.read_text(encoding="utf-8"))["quality"]["metadata"]["present"])

    def test_no_positive_cohort_and_missing_header_fail_before_writing(self):
        self.rows = [dict(row, kwhTotal=0) for row in self.rows]
        self._write_rows()
        destination = self.root / "no-positive.json"
        with self.assertRaisesRegex(ValueError, "No positive-energy"):
            write_profile(self.source, destination)
        self.assertFalse(destination.exists())
        (self.source / "nvv2t.csv").write_text("wrong,header\n1,2\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Missing required columns"):
            calibrate_reference(self.source)

    def test_constant_energy_correlation_is_unavailable_not_nan(self):
        self.rows = [dict(row, kwhTotal=4) for row in self.rows]
        self._write_rows()
        profile = write_profile(self.source, self.root / "constant.json")
        self.assertIsNone(profile["session_metrics"]["energy_connection_pearson"])


if __name__ == "__main__":
    unittest.main()
