"""Trusted local artifacts and shared as-of reconstruction for replay inference."""
from __future__ import annotations

from functools import lru_cache
import hashlib
import json
from pathlib import Path

import joblib
import numpy as np
import sklearn
from threadpoolctl import threadpool_limits

from .data import FEATURE_NAMES, Replay, epoch, iso, source_provenance, split_boundaries
from .train import temperature


class ArrivalPredictor:
    def __init__(self, output_dir):
        output = Path(output_dir)
        self.metadata = json.loads((output / "arrival.metadata.json").read_text())
        trained_version = self.metadata["versions"]["sklearn"]
        if trained_version != sklearn.__version__:
            raise ValueError(f"scikit-learn version mismatch: trained={trained_version}, runtime={sklearn.__version__}")
        # Do not deserialize an unexpected artifact. Paths are deployment-controlled, never HTTP input.
        for name in ["arrival.joblib", "replay.npz", "replay.json"]:
            actual = hashlib.sha256((output / name).read_bytes()).hexdigest()
            expected = self.metadata["artifacts"][name]["sha256"]
            if actual != expected:
                raise ValueError(f"Arrival artifact integrity mismatch: {name}")
        provenance = source_provenance(self.metadata["source"])
        replay_config = json.loads((output / "replay.json").read_text())
        if self.metadata.get("provenance") != provenance or replay_config.get("provenance") != provenance:
            raise ValueError("Arrival model/replay source provenance mismatch")
        self.bundle = joblib.load(output / "arrival.joblib")
        if self.bundle.get("provenance") != provenance:
            raise ValueError("Arrival trained artifact source provenance mismatch")
        if self.bundle["featureNames"] != FEATURE_NAMES or self.metadata["featureNames"] != FEATURE_NAMES:
            raise ValueError("Arrival feature contract mismatch")
        self.replay = Replay.load(output)
        self.catalog = self.replay.catalog
        self.cities = self.replay.config["cities"]
        limits = split_boundaries(self.replay.config)
        self._start = limits["TRAIN"][0]
        # Every supported ETA (<=60 minutes) has a complete target interval within the internal days.
        self._end = limits["TEST"][1] - 3600 - 300
        self.bounds = {"start": iso(self._start), "end": iso(self._end)}

    def _reference(self, value):
        reference = epoch(value)
        if not self._start <= reference <= self._end:
            raise ValueError(f"Reference time outside replay bounds {self.bounds}")
        return reference

    def _station(self, station_id):
        try:
            return self.replay.lookup[station_id]
        except KeyError:
            raise ValueError(f"Unknown station: {station_id}") from None

    @lru_cache(maxsize=16_384)
    def _raw(self, s, ref, eta):
        x = self.replay.features([s], [ref], [eta])
        with threadpool_limits(limits=2):
            distribution = temperature(self.bundle["availability"].predict_proba(x),
                                       self.bundle["calibration"]["availabilityTemperature"])[0]
            probability = temperature(self.bundle["success"].predict_proba(x),
                                      self.bundle["calibration"]["successTemperature"])[0, 1]
            mean = max(0.0, float(self.bundle["waitMean"].predict(x)[0]))
            q90 = max(mean, float(self.bundle["waitP90"].predict(x)[0]))
        return distribution, probability, mean, q90

    def predict(self, station_id: str, reference_time: str, eta_minutes: float) -> dict:
        s = self._station(station_id)
        ref = self._reference(reference_time)
        eta = float(eta_minutes)
        if not np.isfinite(eta) or not 0 <= eta <= 60:
            raise ValueError("ETA must be finite and between 0 and 60 minutes")
        p, success, mean, high = self._raw(s, ref, eta)
        j = (ref - self.replay.start) // 300 - 1
        arrival = ref + int(round(eta * 60))
        forecast = self.replay.start + ((arrival - self.replay.start) // 300) * 300
        return {"currentFree": int(self.replay.counts[s, j, 0]),
                "expectedFree": float(p @ np.arange(len(p))),
                "availableProbability": float(1 - p[0]),
                "availabilityDistribution": p.tolist(), "waitMinutes": mean,
                "waitP90Minutes": high, "serviceProbability": float(success),
                "arrivalTime": iso(arrival), "forecastTime": iso(forecast),
                "resolutionMinutes": 5,
                "loadRatio": float(np.clip(self.replay.power[s, j] / self.replay.rated[s], 0, 1)),
                "featureAsOf": iso(self.replay.start + j * 300),
                "waitCondition": "SUCCESSFUL_NON_RESERVATION_CHARGING"}

    def snapshot(self, reference_time: str) -> list[dict]:
        ref = self._reference(reference_time)
        j = (ref - self.replay.start) // 300 - 1
        return [{"stationId": item["stationId"], "currentFree": int(self.replay.counts[s, j, 0]),
                 "loadRatio": float(np.clip(self.replay.power[s, j] / self.replay.rated[s], 0, 1)),
                 "queued": int(self.replay.waiting[s, j]),
                 "featureAsOf": iso(self.replay.start + j * 300)}
                for s, item in enumerate(self.catalog)]

    def price(self, station_id, timestamp):
        """Known simulated retail tariff, CNY/kWh, for the arrival's local hour."""
        s = self._station(station_id)
        hour = ((epoch(timestamp) + 8 * 3600) // 3600) % 24
        return self.replay.config["prices"][f"{self.catalog[s]['cityId']}:{hour}"]

    def actual(self, station_id, timestamp):
        """OFFLINE ENVIRONMENT ONLY: same-slot truth and future first free slot.

        This is deliberately separate from predict/snapshot and never called by them.
        It reflects recorded background demand; an experiment must separately account
        for allocations caused by its own policy and label its routing assumptions.
        """
        s = self._station(station_id)
        stamp = epoch(timestamp)
        j = (stamp - self.replay.start) // 300
        if not 0 <= j < self.replay.counts.shape[1]:
            raise ValueError("Ground truth outside recorded dataset")
        free = int(self.replay.counts[s, j, 0])
        if not hasattr(self, "_next_free"):
            length = self.replay.counts.shape[1]
            free_index = np.where(self.replay.counts[:, :, 0] > 0, np.arange(length, dtype=np.int32), length)
            self._next_free = np.minimum.accumulate(free_index[:, ::-1], axis=1)[:, ::-1]
        following = int(self._next_free[s, j])
        next_time = None if following >= self.replay.counts.shape[1] else max(stamp, self.replay.start + following * 300)
        return {"stationId": station_id, "currentFree": free, "availableCount": free,
                "loadRatio": float(np.clip(self.replay.power[s, j] / self.replay.rated[s], 0, 1)),
                "queued": int(self.replay.waiting[s, j]),
                "observedTime": iso(self.replay.start + j * 300), "resolutionMinutes": 5,
                "nextFreeTime": None if next_time is None else iso(next_time),
                "waitMinutes": None if next_time is None else (next_time - stamp) / 60,
                "source": "OFFLINE_SIMULATOR_GROUND_TRUTH"}
