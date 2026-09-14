"""Task definitions shared by the load and availability packages."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Task:
    key: str
    target: str                      # value of "target" in model_metadata.schema.json
    package: str                     # ml/<key>
    label_prefix: str                # column prefix in ml_targets_hourly
    unit: str                        # contract unit for predictions
    capacity_column: str              # feature column supplying the upper bound
    id_prefix: str
    risk_label: str | None = None    # optional binary risk target, availability only
    risk_definition: str = ""
    notes: str = ""

    def label_column(self, horizon_step: int) -> str:
        return f"{self.label_prefix}h{horizon_step:02d}"

    def horizons(self) -> tuple[int, ...]:
        return (1, 6, 24)


LOAD = Task(
    key="load",
    target="load",
    package="ml/load",
    label_prefix="label_power_kw_",
    unit="kW",
    capacity_column="rated_capacity_kw",
    id_prefix="load",
    notes="Station mean power for each future hour; upper bound is the station rated kW.",
)

AVAILABILITY = Task(
    key="availability",
    target="availability",
    package="ml/availability",
    label_prefix="label_available_count_",
    unit="chargers",
    capacity_column="capacity",
    id_prefix="avail",
    risk_label="depletion",
    risk_definition="P(zero free chargers at the last sample of the predicted hour, hh:55)",
    notes=(
        "Free charger count is ordinal over {0..capacity}; the point prediction is the modelled "
        "expectation (a decimal is allowed but must be labelled 预计空闲桩数), and the risk head "
        "reports P(0 free) instead of pretending a fractional stock is real."
    ),
)

TASKS: dict[str, Task] = {task.key: task for task in (LOAD, AVAILABILITY)}
