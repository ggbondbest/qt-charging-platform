"""Past-only hourly ML handoff tables. This module does not train a model.

Feature rows are indexed by prediction boundary t. lag_01 is [t-1h, t);
label_power_kw_h01 is [t, t+1h). All 24 feature hours must be complete and
contiguous. Future realized weather, order totals and corruption/anomaly labels
never enter this module. Labels are in a SEPARATE table, never in the API DB.
"""

from datetime import date, timedelta

from data_analysis.contracts import FEATURE_VERSION
from data_analysis.spark_jobs.pipeline import _spark_imports


def split_dates(start_date, end_date):
    """Use Shanghai business dates [start_date, end_date), excluding edge days.

    The 180-day full batch [2025-12-01, 2026-05-30) yields 118/30/30
    days after excluding its first and last business dates. Very short batches
    have no usable three-way split; their target rows must all be EXCLUDED.
    """
    source_start, source_end = date.fromisoformat(start_date), date.fromisoformat(end_date)
    if source_end <= source_start:
        raise ValueError("end_date must be after start_date (exclusive business-date boundary)")
    start = source_start + timedelta(days=1)
    end = max(start, source_end - timedelta(days=1))
    days = (end - start).days
    if days < 3:
        return {"start": start.isoformat(), "trainEnd": start.isoformat(),
                "validationEnd": start.isoformat(), "end": end.isoformat(), "usable": False}
    holdout = 30 if days >= 90 else max(1, days // 5)
    validation = end - timedelta(days=holdout * 2)
    test = end - timedelta(days=holdout)
    return {"start": start.isoformat(), "trainEnd": validation.isoformat(),
            "validationEnd": test.isoformat(), "end": end.isoformat(), "usable": True}


def build_ml_frames(hours, tables, splits):
    _, Window, F, _ = _spark_imports()
    spark = hours.sparkSession
    spark.conf.set("spark.sql.session.timeZone", "UTC")
    window = Window.partitionBy("station_id").orderBy("recorded_at")
    history_window = window.rowsBetween(-23, 0)
    valid_hour = F.coalesce(
        F.col("is_complete") & F.col("mean_power_kw").isNotNull() &
        ~F.isnan("mean_power_kw") & (F.col("mean_power_kw") >= 0) &
        (F.col("mean_power_kw") < F.lit(float("inf"))) &
        (F.col("capacity") > 0) & F.col("end_available_count").isNotNull() &
        (F.col("end_available_count") >= 0) & (F.col("end_available_count") <= F.col("capacity")),
        F.lit(False))
    # The last observed hour ends at t; its measurement is then known.
    frame = (hours.withColumn("_valid_hour", valid_hour)
             .withColumn("_hour_gap", F.when(
                 F.lag("recorded_at").over(window).isNotNull() &
                 (F.col("recorded_at").cast("long") - F.lag(F.col("recorded_at").cast("long")).over(window) != 3600),
                 1).otherwise(0))
             .withColumn("reference_time", F.col("recorded_at") + F.expr("INTERVAL 1 HOUR"))
             .withColumn("_history_count", F.count("*").over(history_window))
             .withColumn("_complete_count", F.sum(F.col("_valid_hour").cast("int")).over(history_window))
             .withColumn("_history_first", F.min("recorded_at").over(history_window))
             .withColumn("_history_ok", (F.col("_history_count") == 24) &
                         (F.col("_complete_count") == 24) &
                         (F.sum("_hour_gap").over(window.rowsBetween(-22, 0)) == 0) &
                         ((F.col("recorded_at").cast("long") - F.col("_history_first").cast("long")) == 23 * 3600)))
    for lag in range(1, 25):
        col = F.col("mean_power_kw") if lag == 1 else F.lag("mean_power_kw", lag - 1).over(window)
        frame = frame.withColumn(f"lag_power_kw_h{lag:02d}", col)
    for width in (3, 6, 24):
        trailing = window.rowsBetween(-width + 1, 0)
        frame = frame.withColumn(f"rolling_mean_kw_{width}h", F.avg("mean_power_kw").over(trailing))
    frame = (frame.withColumn("rolling_std_kw_24h", F.stddev_pop("mean_power_kw").over(history_window))
             .withColumn("rolling_max_kw_24h", F.max("mean_power_kw").over(history_window))
             .withColumn("history_start_at", F.col("_history_first"))
             .withColumn("history_end_at", F.col("reference_time"))
             .withColumn("history_complete", F.col("_history_ok"))
             .withColumn("last_available_count", F.col("end_available_count")))
    for lead in range(1, 25):
        valid = (F.lead("_valid_hour", lead).over(window) &
                 (F.lead(F.col("recorded_at").cast("long"), lead).over(window) ==
                  F.col("recorded_at").cast("long") + lead * 3600))
        frame = frame.withColumn(f"label_power_kw_h{lead:02d}",
                                 F.when(valid, F.lead("mean_power_kw", lead).over(window)))
        frame = frame.withColumn(f"label_available_count_h{lead:02d}",
                                 F.when(valid, F.lead("end_available_count", lead).over(window)))
    # Filter only AFTER window/lead construction, otherwise the split truncates history.
    local_time = F.from_utc_timestamp("reference_time", "Asia/Shanghai")
    local_day = F.to_date(local_time)
    frame = (frame.filter(F.col("_history_ok"))
             .withColumn("business_date", local_day)
             .withColumn("hour_of_day", F.hour(local_time).cast("long"))
             .withColumn("day_of_week", F.pmod(F.dayofweek(local_time) + 5, F.lit(7)).cast("long"))
             .withColumn("feature_version", F.lit(FEATURE_VERSION)))
    known_calendar = tables["calendar"].select("city_id", "business_date", "is_weekend", "scenario_event")
    frame = (frame.join(known_calendar, ["city_id", "business_date"], "left")
             .withColumn("is_public_holiday", F.coalesce(F.col("scenario_event").startswith("PUBLIC_HOLIDAY"), F.lit(False)))
             .withColumn("is_adjusted_workday", F.coalesce(F.col("scenario_event") == "ADJUSTED_WORKDAY", F.lit(False)))
             .withColumn("is_weekend", F.coalesce(F.col("is_weekend") == 1, F.col("day_of_week") >= 5)))
    capacity = tables["chargers"].groupBy("station_id").agg(F.sum("rated_power_kw").alias("rated_capacity_kw"))
    frame = frame.join(capacity, "station_id", "left")
    for horizon in (1, 6, 24):
        labels_complete = F.lit(True)
        for lead in range(1, horizon + 1):
            labels_complete = labels_complete & F.col(f"label_power_kw_h{lead:02d}").isNotNull()
        end = F.col("reference_time") + F.expr(f"INTERVAL {horizon} HOURS")
        def utc_midnight(value):
            return F.to_utc_timestamp(F.to_timestamp(F.lit(value + " 00:00:00")), "Asia/Shanghai")
        start_ok = F.col("reference_time") >= utc_midnight(splits["start"])
        split = (F.when(start_ok & labels_complete & (end <= utc_midnight(splits["trainEnd"])), "TRAIN")
                 .when((F.col("reference_time") >= utc_midnight(splits["trainEnd"])) &
                       labels_complete & (end <= utc_midnight(splits["validationEnd"])), "VALIDATION")
                 .when((F.col("reference_time") >= utc_midnight(splits["validationEnd"])) &
                       labels_complete & (end <= utc_midnight(splits["end"])), "TEST")
                 .otherwise("EXCLUDED"))
        frame = frame.withColumn(f"split_{horizon}h", split if splits.get("usable") is True else F.lit("EXCLUDED"))
    feature_cols = ["station_id", "city_id", "reference_time", "business_date", "feature_version",
                    "history_start_at", "history_end_at", "history_complete", "hour_of_day", "day_of_week",
                    "is_weekend", "is_public_holiday", "is_adjusted_workday", "capacity", "rated_capacity_kw",
                    "last_available_count"] + [f"lag_power_kw_h{i:02d}" for i in range(1, 25)] + [
                        "rolling_mean_kw_3h", "rolling_mean_kw_6h", "rolling_mean_kw_24h",
                        "rolling_std_kw_24h", "rolling_max_kw_24h"]
    target_cols = ["station_id", "reference_time", "feature_version"] + [
        f"label_available_count_h{i:02d}" for i in range(1, 25)] + [
        f"label_power_kw_h{i:02d}" for i in range(1, 25)] + ["split_1h", "split_6h", "split_24h"]
    return {"ml_features_hourly": frame.select(*feature_cols), "ml_targets_hourly": frame.select(*target_cols)}
