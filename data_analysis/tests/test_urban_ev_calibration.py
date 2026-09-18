"""Small fictional fixtures exercise UrbanEV aggregation without downloading data."""

import csv
from datetime import datetime, timedelta
import hashlib
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from data_analysis.charging_data import calibrate_urban_ev as urban


class UrbanEVCalibrationTests(unittest.TestCase):
    def test_day_type_level_and_shape_are_separate(self):
        start = datetime(2024, 1, 1)  # Monday; exactly two complete weeks.
        rows = []
        for index in range(14 * 24):
            when = start + timedelta(hours=index)
            scale = 2 if when.weekday() >= 5 else 1
            rows.append((when, scale * (when.hour + 1)))
        profiles = urban._profiles(rows, regularization=0.2)
        self.assertEqual(profiles["weekday"]["days"], 10)
        self.assertEqual(profiles["weekend"]["days"], 4)
        self.assertEqual(profiles["weekday"]["normalized_mean_one"],
                         profiles["weekend"]["normalized_mean_one"])
        self.assertAlmostEqual(profiles["weekend"]["intensity_relative_to_all_days"] /
                               profiles["weekday"]["intensity_relative_to_all_days"], 2)
        for profile in profiles.values():
            self.assertEqual(profile["peak_hour"], 23)
            self.assertEqual(profile["minimum_hour"], 0)
            self.assertAlmostEqual(math.fsum(profile["normalized_mean_one"]), 24)
            self.assertAlmostEqual(math.fsum(profile["regularized_normalized_mean_one"]), 24)
            first = profile["normalized_mean_one"][0]
            self.assertAlmostEqual(profile["regularized_normalized_mean_one"][0], first * 0.8 + 0.2)

    def test_source_hashes_are_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            content = b"fixture source\n"
            (folder / "fixture.csv").write_bytes(content)
            expected = hashlib.sha256(content).hexdigest()
            with patch.object(urban, "SOURCE_HASHES", {"fixture.csv": expected}):
                sources = urban._source_info(folder)
                self.assertEqual(sources[0]["sha256"], expected)
                (folder / "fixture.csv").write_bytes(b"changed\n")
                with self.assertRaisesRegex(ValueError, "SHA256"):
                    urban._source_info(folder)

    def test_duration_is_additive_and_incomplete_hours_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "duration.csv"
            start = datetime(2024, 1, 1)
            end = start + timedelta(hours=2)
            rows = [[start.strftime("%Y-%m-%d %H:%M:%S"), 1.5, 0.25],
                    [(start + timedelta(hours=1)).strftime("%Y-%m-%d %H:%M:%S"), 0.5, 0.75]]

            def write(values):
                with path.open("w", encoding="utf-8", newline="") as stream:
                    writer = csv.writer(stream)
                    writer.writerow(["time", "a", "b"])
                    writer.writerows(values)

            with patch.object(urban, "START", start), patch.object(urban, "END", end):
                write(rows)
                totals, quality = urban._read_hourly(path, {"a": 2, "b": 1})
                self.assertEqual([value for _, value in totals], [1.75, 1.25])
                self.assertEqual(quality["rows"], 2)
                write(rows[:1])
                with self.assertRaisesRegex(ValueError, "Incomplete"):
                    urban._read_hourly(path, {"a": 2, "b": 1})
                write([rows[0], rows[0]])
                with self.assertRaisesRegex(ValueError, "Missing, duplicate"):
                    urban._read_hourly(path, {"a": 2, "b": 1})
                rows[1][1] = 2.1
                write(rows)
                with self.assertRaisesRegex(ValueError, "Invalid duration"):
                    urban._read_hourly(path, {"a": 2, "b": 1})


if __name__ == "__main__":
    unittest.main()
