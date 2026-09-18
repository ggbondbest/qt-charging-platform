"""Evidence-informed scenarios, not measured city-market demand models.

The supplied session sample calibrates a daytime AC archetype. It has unknown
timezone/site semantics and cannot calibrate all Chinese users. Other shapes and
elasticities below are explicit scenario parameters; see docs/behavior_evidence.md.
Arrival, active charging and connected occupancy are deliberately separate.
"""

from datetime import timedelta
import json
import math
from pathlib import Path

CONFIG_DIR = Path(__file__).resolve().parents[1] / "config"
BEHAVIOR_VERSION = "2.0.0"
SEGMENTS = ("COMMUTER", "RIDE_HAILING", "FAMILY", "FLEET")


def normalized(values):
    total = sum(values)
    return [value * len(values) / total for value in values]


class BehaviorModel:
    def __init__(self, profile=None):
        self.profile = profile or json.loads((CONFIG_DIR / "reference_profile.json").read_text(encoding="utf-8"))
        counts = self.profile["arrival"]["weekday_hour_counts"]
        observed = normalized([.25 * counts[(h-1) % 24] + .5 * counts[h] +
                               .25 * counts[(h+1) % 24] + 1 for h in range(24)])
        # Smoothing and blending prevent treating one unknown facility sample as
        # a precise national hourly probability distribution.
        office = normalized([.05 if h < 7 or h >= 21 else 2.8 if 8 <= h <= 10
                             else 1.8 if 11 <= h <= 17 else .5 for h in range(24)])
        self.shapes = {
            "OFFICE": normalized([.65 * observed[h] + .35 * office[h] for h in range(24)]),
            "CAMPUS": normalized([.5 * observed[h] + .5 * office[h] for h in range(24)]),
            "RESIDENTIAL": normalized([.8, .45, .18, .10, .10, .15, .25, .45,
                .35, .25, .22, .25, .4, .4, .35, .4, .65, 1.5, 2.8, 3.4, 3.5, 3.2, 2.5, 1.4]),
            "SHOPPING": normalized([.10, .06, .04, .04, .04, .04, .06, .1,
                .25, .6, 1.2, 1.7, 2.0, 1.9, 1.6, 1.6, 1.7, 1.9, 2.2, 2.4, 2.0, 1.4, .6, .25]),
            # Broad public fast-charge shoulders from the 2022 behavior report;
            # absolute heights are assumed, not digitized measurements.
            "TRANSIT": normalized([1.8, 1.2, .55, .45, .7, 1.5, 1.7, 1.1,
                .65, .55, .7, 1.1, 1.8, 1.9, 1.8, 1.6, 1.15, .9, 1.0, 1.0, .8, .7, 1.1, 1.8]),
        }
        self.urban = json.loads((CONFIG_DIR / "urban_ev_profile.json").read_text(encoding="utf-8"))
        self.calendar = json.loads((CONFIG_DIR / "calendar_reference.json").read_text(encoding="utf-8"))
        joint = self.profile["session_metrics"]["energy_connection_joint"]
        self.joint = joint
        self.joint_cells = [(i, j) for i, line in enumerate(joint["counts"]) for j, count in enumerate(line) if count]
        self.joint_weights = [joint["counts"][i][j] for i, j in self.joint_cells]

    def arrival_weight(self, site_type, when, phase_shift=0):
        shape = self.shapes[site_type]
        hour = (when.hour - phase_shift) % 24
        fraction = when.minute / 60
        value = shape[hour] * (1-fraction) + shape[(hour+1) % 24] * fraction
        event = self.calendar_event(when)
        if event.startswith("PUBLIC_HOLIDAY_"):
            value *= {"OFFICE": .18, "CAMPUS": .25, "SHOPPING": 1.20,
                      "RESIDENTIAL": 1.02, "TRANSIT": 1.20}[site_type]
        elif not self.is_workday(when):
            value *= {"OFFICE": .32, "CAMPUS": .55, "SHOPPING": 1.25,
                      "RESIDENTIAL": 1.07, "TRANSIT": 1.04}[site_type]
        return value

    def calendar_event(self, when):
        day = when.date().isoformat()
        for start, end, name in self.calendar["holiday_ranges"]:
            if start <= day <= end:
                return "PUBLIC_HOLIDAY_" + name
        if day in self.calendar["adjusted_workdays"]:
            return "ADJUSTED_WORKDAY"
        return "NONE"

    def is_workday(self, when):
        event = self.calendar_event(when)
        return event == "ADJUSTED_WORKDAY" or (event == "NONE" and when.weekday() < 5)

    def city_public_pressure(self, city, site_type, when):
        if city != "SZ" or site_type != "TRANSIT":
            return 1.0
        day = "weekend" if when.weekday() >= 5 else "weekday"
        observed = self.urban["utilization_profiles"][day]
        # Utilization is NOT arrival. This small proxy is an explicit modeling
        # assumption, kept separate from the measured reference in the manifest.
        return .75 + .25 * observed["regularized_normalized_mean_one"][when.hour]

    def segment_weights(self, site_type, hour):
        weights = {"OFFICE": [70, 6, 17, 7], "CAMPUS": [60, 5, 30, 5],
                   "RESIDENTIAL": [38, 9, 48, 5], "SHOPPING": [25, 12, 55, 8],
                   "TRANSIT": [10, 58, 10, 22]}[site_type]
        return dict(zip(SEGMENTS, weights))

    def choose_charger(self, available, vehicle, site_type, hour, rng):
        segment = vehicle["vehicle_class"]
        long_stay = site_type in {"RESIDENTIAL", "OFFICE", "CAMPUS"} and segment in {"COMMUTER", "FAMILY"}
        weights = []
        for charger in available:
            if charger["connector_type"] == "AC":
                weight = 6.0 if long_stay else .12 if segment in {"RIDE_HAILING", "FLEET"} else .4
            else:
                weight = (.8 if long_stay else 1.0) * math.sqrt(min(charger["rated_power_kw"], vehicle["max_charge_kw"]) / 60)
            weights.append(weight)
        return rng.choices(available, weights)[0]

    def session_plan(self, vehicle, charger, site_type, when, rng):
        capacity = vehicle["battery_capacity_kwh"] * 1000
        if not math.isfinite(capacity) or capacity <= 0 or not 0 <= vehicle["soc_pct"] < 100:
            raise ValueError("A session requires positive battery capacity and SOC in [0, 100)")
        available = int(capacity * (100-vehicle["soc_pct"]) / 100)
        if available < 1:
            raise ValueError("Battery has no whole Wh of remaining capacity")
        ac = charger["connector_type"] == "AC"
        lower_soc = max(76, vehicle["soc_pct"] + min(3, (100-vehicle["soc_pct"])/2))
        upper_soc = min(100, max(89, lower_soc+1))
        desired_soc = rng.uniform(85, 95) if ac else rng.uniform(lower_soc, upper_soc)
        target = max(500, int(capacity * (desired_soc-vehicle["soc_pct"]) / 100))
        connected = parking = 0
        if ac and site_type in {"OFFICE", "CAMPUS"}:
            i, j = rng.choices(self.joint_cells, self.joint_weights)[0]
            # Draw within a coarse joint cell, not an independently shuffled
            # pair, and do not copy any person's original record.
            energy = rng.uniform(*self.joint["energy_edges"][i:i+2])
            hours = rng.uniform(*self.joint["connection_hour_edges"][j:j+2])
            target = max(500, int(min(30, energy) * 1000))
            connected = max(6, min(144, round(hours * 12)))
            limit = connected
        elif ac and site_type == "RESIDENTIAL" and (when.hour >= 17 or when.hour < 6):
            departure = when.replace(hour=7, minute=0, second=0) + timedelta(minutes=rng.randint(0, 120))
            if departure <= when:
                departure += timedelta(days=1)
            connected = max(12, min(168, round((departure-when).total_seconds() / 300)))
            limit = connected
        elif ac:
            connected = rng.randint(12, 36) if site_type == "SHOPPING" else rng.randint(12, 60)
            limit = connected
        else:
            limit = rng.randint(12, 24) if vehicle["vehicle_class"] in {"RIDE_HAILING", "FLEET"} else rng.randint(10, 22)
            parking = rng.choices([0, 1, 2, 3, 6], [55, 23, 13, 7, 2])[0]
        return dict(target_wh=min(available, target), max_charge_ticks=limit,
                    connected_ticks=connected, parking_ticks=parking)

    def min_revisit_ticks(self, segment):
        return {"COMMUTER": 144, "FAMILY": 216, "RIDE_HAILING": 24, "FLEET": 24}[segment]

    def driving_prefix(self, segment, times):
        if segment == "COMMUTER":
            shape = [4 if 7 <= h < 10 or 17 <= h < 20 else .4 if 10 <= h < 22 else .04 for h in range(24)]
        elif segment == "FAMILY":
            shape = [1 if 9 <= h < 21 else .06 for h in range(24)]
        else:
            shape = [1 if 6 <= h < 24 else .25 for h in range(24)]
        total = sum(shape) * 12
        prefix = [0.0]
        for when in times[:-1]:
            weekend = .55 if segment == "COMMUTER" else 1.25 if segment == "FAMILY" else 1.0
            prefix.append(prefix[-1] + shape[when.hour] / total * (weekend if not self.is_workday(when) else 1))
        return prefix
