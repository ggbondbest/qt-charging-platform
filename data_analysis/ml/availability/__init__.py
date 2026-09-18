"""Future hourly free-charger counts; current reproducible delivery output locations.

These artifacts are generated locally, not copied from a contributor's ignored run
directories. See DELIVERY.md for the two training commands and serving semantics.
Old run1..run6 names belong only to the historical experiments in REPORT.md.
A different dataset publication still requires retraining, never metadata relabelling.
"""

#: the three complete 1/6/24-hour base estimators, without redundant cold-city research runs
BASE_RUN = "data_analysis/outputs/ml_availability_delivery_base"
#: the serving artefacts: base estimators plus the per-layer prior and the chosen point rule
HIERARCHY_RUN = "data_analysis/outputs/ml_availability_delivery"
