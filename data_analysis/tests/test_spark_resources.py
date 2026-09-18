"""Launcher resource configuration tests; no Java/PySpark dependency required."""

import os
import shlex
import unittest
from unittest.mock import patch

from data_analysis.spark_jobs.pipeline import configure_driver_memory


class SparkResourceTests(unittest.TestCase):
    def test_default_heap_is_set_before_launcher(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(configure_driver_memory(), "2g")
            self.assertEqual(shlex.split(os.environ["PYSPARK_SUBMIT_ARGS"]),
                             ["--driver-memory", "2g", "pyspark-shell"])

    def test_environment_heap_is_respected(self):
        with patch.dict(os.environ, {"PYSPARK_SUBMIT_ARGS": "--driver-memory 3g --conf spark.ui.enabled=false pyspark-shell"}):
            self.assertEqual(configure_driver_memory(), "3g")
            self.assertIn("spark.ui.enabled=false", shlex.split(os.environ["PYSPARK_SUBMIT_ARGS"]))

    def test_explicit_value_replaces_conf_and_retains_unrelated_options(self):
        with patch.dict(os.environ, {"PYSPARK_SUBMIT_ARGS": "--driver-memory=1g --conf spark.driver.memory=2g --conf 'spark.local.dir=/tmp/spark scratch' pyspark-shell"}):
            self.assertEqual(configure_driver_memory("4G"), "4g")
            self.assertEqual(shlex.split(os.environ["PYSPARK_SUBMIT_ARGS"]),
                             ["--driver-memory", "4g", "--conf", "spark.local.dir=/tmp/spark scratch", "pyspark-shell"])

    def test_conf_equals_heap_is_recognized(self):
        with patch.dict(os.environ, {"PYSPARK_SUBMIT_ARGS": "--conf=spark.driver.memory=2048m pyspark-shell"}):
            self.assertEqual(configure_driver_memory(), "2048m")

    def test_invalid_sizes_and_missing_values_fail_before_launch(self):
        for value in ["0g", "2", "-1g", "2g --master local[8]", "", None]:
            with self.subTest(value=value), patch.dict(os.environ, {"PYSPARK_SUBMIT_ARGS": "--driver-memory"}):
                with self.assertRaises(ValueError):
                    configure_driver_memory(value)
        with patch.dict(os.environ, {"PYSPARK_SUBMIT_ARGS": "pyspark-shell"}):
            for value in ["0g", "2", "-1g", "2g --master local[8]", ""]:
                with self.subTest(value=value), self.assertRaises(ValueError):
                    configure_driver_memory(value)


if __name__ == "__main__":
    unittest.main()
