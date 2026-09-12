# Spark batch processing

This is an actual PySpark job, separate from the Python generator's reference
aggregates. It never reads `reference_aggregates/` as an input to computation.
It does not implement a Vue application, prediction API, or a trained model.

## Runtime and local execution

Use Python 3.10-3.12, Java 17, and `pyspark==3.5.6`. Install the optional runtime:

```sh
python -m pip install -r data_analysis/requirements-spark.txt
python -m data_analysis.spark_jobs.pipeline \
  --input data_analysis/datasets/charging_sample_7d_v2 \
  --output data_analysis/outputs/charging_sample_7d_spark_v2
```

Run commands from the repository root. Choose a **new output directory** for
each run. Existing paths, source/output overlap, and nested output paths are
rejected. A failed run is retained for diagnosis, with `_RUNNING` but without the
root `_SUCCESS`; it is not an accepted batch result. Do not publish it to a
dashboard. A successful run produces a root `_SUCCESS` and removes `_RUNNING`.

To process HDFS files, provide the teacher's verified Hadoop configuration in
`HADOOP_CONF_DIR`, upload the dataset root unchanged, and replace both paths:

```sh
python -m data_analysis.spark_jobs.pipeline \
  --input hdfs://namenode:9000/charging/raw/charging_sample_7d_v2 \
  --output hdfs://namenode:9000/charging/processed/charging_sample_7d_v2_run1
```

The URI is only a placeholder: use the address in the actual Hadoop setup.
Spark can run as `local[2]` while reading/writing HDFS; this demonstrates real
HDFS storage and Spark calculation, **not a multi-node computation cluster**.
An existing Spark master may be selected with `--master`.

### Full-batch resource settings

The pipeline and `data_analysis.scripts.run_data_layer` accept
`--driver-memory 4g`. This setting is applied **before the first Java process
starts**, not by changing an already running Spark session. The default is 2g
unless `PYSPARK_SUBMIT_ARGS` already specifies a heap; an explicit CLI value
takes precedence. The 180-day dataset is run with 4g and `local[2]`. Leave enough
memory for Python, the operating system, and other applications. Start a new
Python process when changing the heap.

Parsed raw tables use disk-only persistence: the retained forensic JSON and
millions of telemetry/battery rows no longer compete with validation shuffles
for heap space. Spark needs writable local temporary storage even with HDFS
input. Reserve several GB of free scratch disk; this cache is not an output or
a substitute for the versioned HDFS/Parquet data.

When running the separate export/verifier entry points, set the launcher heap
in the environment before starting Python. Also ensure Spark's Python worker
uses the same environment as the driver:

```powershell
$env:PYSPARK_PYTHON = (Get-Command python).Source
$env:PYSPARK_SUBMIT_ARGS = '--driver-memory 4g pyspark-shell'
```

On Linux:

```sh
export PYSPARK_PYTHON="$(command -v python)"
export PYSPARK_SUBMIT_ARGS='--driver-memory 4g pyspark-shell'
```

Do not run several full Spark jobs concurrently on a development machine.
Keep failed output directories for diagnosis and choose a fresh path for a
retry; an old `_RUNNING` directory is never a completed batch.

## Processing and meanings

- The current dataset is generator `2.0.0`, schema `1.1.0`, with 23 raw tables.
  The new `vehicle_energy_intervals` table records driving consumption and
  off-network charging between platform visits. It is retained as typed Parquet
  for vehicle-energy analysis but contributes **no platform energy sales or
  revenue**. The v2 batch and its manifest must be used together; v1 results do
  not validate v2 data.
- Read the versioned manifest and exact CSV headers; explicitly cast timestamps,
  numbers, and dates. Timestamps stay UTC; business days use Asia/Shanghai.
- Compare the actual row count of **every raw table** to the manifest before
  publishing statistics. Missing rows/shards fail the batch; after cleaning,
  canonical session count is checked too. No incomplete batch receives `_SUCCESS`.
- Trim/canonicalize enums. Isolate bad session types, missing IDs, negative
  money/energy, unknown foreign keys, inconsistent ownership, time ordering,
  total-fee identities, and unrecognized session statuses.
- Validate **before** deduplicating. Thus the original valid session survives
  alongside its deliberately corrupted clones. Deduplication has deterministic
  canonical contents; rejected rows retain raw JSON and a reason.
- Fail closed on unexpected invalid typed values in other tables, malformed
  telemetry intervals, bad telemetry ownership, or duplicate charger timestamps.
  Do not silently turn such data into believable dashboard statistics.
- Produce station-hour energy, mean kW, all six state sample counts, capacity,
  sample count, and availability at the **last sample**. Mean kW is hourly Wh / 1000;
  availability is not calculated from power. Sample counts let consumers detect
  incomplete hours. Input telemetry intervals may not cross an hour boundary.
- Produce station-day energy, interval-level grid costs, ended-charge session
  counts, successful receipts/refunds by **payment occurrence date**, operating
  costs, and repair costs on the restoration date. `completed_sessions` means
  charging has ended and includes unpaid finished sessions; it is not paid count.
  Cash flow and service-delivery totals are intentionally separate metrics.

## Output tree

```text
<new-output>/
  clean/<table>/                    # typed Parquet tables
  rejected/charging_sessions/       # Parquet; raw JSON + rejection_reason
  statistics/station_hourly/        # Parquet, computed from raw telemetry
  statistics/station_daily/         # Parquet, computed from facts
  reports/quality_report/           # one part-*.json plus Spark marker
  _SUCCESS                         # present only after the complete run
```

The report includes input counts, normalization counts, rejection reasons,
clean session count, output counts, dataset version and synthetic provenance.
Parquet directories are normal Spark output, not a single file. Future Vue/API
and ML code should consume a completed, versioned output directory.

## Tests

The default dependency-free test suite skips these integration tests. To run
them with a real Java/Spark runtime, on **Linux/POSIX shells**:

```sh
RUN_SPARK_TESTS=1 python -m unittest data_analysis.tests.test_spark_pipeline -v
```

On **Windows PowerShell**, use two commands:

```powershell
$env:RUN_SPARK_TESTS = '1'
python -m unittest data_analysis.tests.test_spark_pipeline -v
```

Tests cover actual gzip CSV ingestion, enum normalization, rejection and
deduplication, hourly energy/state math, Shanghai midnight cashflow semantics,
output overwrite refusal, missing rows/shards, a small end-to-end Parquet batch, and a one-day
generated dataset compared field by field with the independent Python
`reference_aggregates/`. The v2 fixture also verifies that off-network vehicle
energy is retained as integer Wh without entering platform energy or revenue.
All seven integration tests have been exercised with
Python 3.12, Java 17, and PySpark 3.5.6. HDFS connectivity still depends on the
target environment and must be verified there; local Spark tests do not certify
the teacher's HDFS configuration.

## Full aggregate reconciliation

Run the final full batch into a new output directory, then run the
**separate verifier**. Each command is one line and works in both PowerShell
and a Linux shell:

```text
python -m data_analysis.spark_jobs.pipeline --input data_analysis/datasets/charging_full_180d_v2 --output data_analysis/outputs/spark_full_180d_v2_final --master 'local[2]' --shuffle-partitions 8 --driver-memory 4g
python -m data_analysis.spark_jobs.verify_aggregates --input data_analysis/datasets/charging_full_180d_v2 --processed data_analysis/outputs/spark_full_180d_v2_final --report data_analysis/outputs/spark_full_180d_v2_final/reports/reference_verification
```

The verifier requires the root `_SUCCESS` marker, matching dataset IDs, verified
raw manifest row counts, and provenance confirming the calculation did not use
reference aggregates. It checks every station/time primary key, uniqueness,
non-null values, all six state counts, last-sample availability, capacities,
energy, and financial fields against independently generated Python controls.
Integer fields compare exactly; mean kW uses an absolute tolerance of `1e-9`.
Reference counts must also match the manifest. Any mismatch exits unsuccessfully
instead of merely printing a warning. The optional report directory must be new;
omit `--report` for a read-only rerun. It never writes into the source dataset.

The reference aggregates are used **only after Spark calculation, for
verification**, never as inputs to the production aggregation job. A successful
full run has 108,000 station-hour rows and 4,500 station-day rows.

The verified final v2 full batch has 23 raw tables and 5,832,840 raw rows. Cleaning
preserves 121,539 canonical sessions: 1,415 invalid copies are quarantined and
502 valid duplicates are removed. Both categories appear in the rejected
session output (1,917 rows in total); 434 normalized enum rows do not reduce
the row count. Every hourly/daily summary field matched its independent
reference, including final-sample availability and Shanghai payment dates.

`full_validation_summary.json` records the exercised 180-day batch, including
its input-manifest digest, rejection counts, complete reconciliation results,
runtime versions, and measured timings. It contains no developer-machine paths.
The generated Parquet data and detailed reports are ignored by Git, not missing
source files; regenerate them with the commands above.

References: [Spark CSV options](https://spark.apache.org/docs/3.5.6/sql-data-sources-csv.html),
[Spark 3.5.6](https://spark.apache.org/docs/3.5.6/).
