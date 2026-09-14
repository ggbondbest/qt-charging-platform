"""Availability forecasting: future free-charger count per station hour.

``BASE_RUN`` / ``HIERARCHY_RUN`` name the run directories that are authoritative **for the batch
currently in the repository**.  They live here, once, instead of being copy-pasted into argument
defaults, module docstrings and test paths, because a re-publication of the data layer mints a new
``publishedBatchId`` and every bundle trained against the old one is then refused by
``predict.AvailabilityForecaster`` (``BATCH_MISMATCH``) -- a stale name buried in a default would
quietly describe numbers that were never produced.

History of that pointer, kept in one place:

===========================================  ==========================================
directory                                    what it is
===========================================  ==========================================
``ml_avail_run1`` / ``ml_avail_run2``        bound to ``analytics-5f8e9342…`` (superseded
                                             by the refresh on develop, PR #64; still on
                                             disk, and the resume tests use them precisely
                                             because they predate ``seed``/``payloadEntry``)
``ml_avail_run3_r5`` / ``run4_r5_resume``    5x boosting-budget experiments (``-r5``), never
                                             published; also on the superseded batch
``ml_avail_run5`` / ``ml_avail_run6``        **current**: same recipe, same seed, same rows,
                                             re-bound to ``analytics-298aa3ee…``; every
                                             published metric stayed bit-identical (measured in
                                             ``REPORT.md`` section 5.12)
===========================================  ==========================================
"""

#: ordered-classification estimators + cold-city holdouts, the input the shipped wrapper reuses
BASE_RUN = "data_analysis/outputs/ml_avail_run5"
#: the serving artefacts: base estimators plus the per-layer prior and the chosen point rule
HIERARCHY_RUN = "data_analysis/outputs/ml_avail_run6"
