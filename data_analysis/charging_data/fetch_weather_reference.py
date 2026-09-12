"""Download an explicit, attributed ERA5 weather cache for offline simulation.

This separate preparation command uses the network; importing this module does
not. The normal dataset generator should only read the resulting local cache.
No weather is invented or interpolated when the API returns missing data.

python -m data_analysis.charging_data.fetch_weather_reference \
    --output data_analysis/config/weather_reference.csv.gz \
    --provenance data_analysis/config/weather_reference_provenance.json
"""

import argparse
import csv
from datetime import date, datetime, time, timedelta, timezone
import gzip
import hashlib
import io
import json
import math
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


ENDPOINT = "https://archive-api.open-meteo.com/v1/archive"
DOCUMENTATION = "https://open-meteo.com/en/docs/historical-weather-api"
CITIES = (("DL", 38.9140, 121.6147), ("SY", 41.8057, 123.4315),
          ("BJ", 39.9042, 116.4074), ("SH", 31.2304, 121.4737),
          ("SZ", 22.5431, 114.0579))
FIELDS = ("city_id", "recorded_at", "temperature_c", "humidity_pct", "weather", "rainfall_mm")
HOURLY = ("temperature_2m", "relative_humidity_2m", "precipitation", "weather_code")
WMO_CODES = {0, 1, 2, 3, 45, 48, 51, 53, 55, 56, 57, 61, 63, 65, 66, 67,
             71, 73, 75, 77, 80, 81, 82, 85, 86, 95, 96, 99}
BUSINESS_TZ = timezone(timedelta(hours=8))


def _window(start_date, end_date):
    start, end = date.fromisoformat(start_date), date.fromisoformat(end_date)
    days = (end - start).days + 1
    if start.year < 2000 or not 1 <= days <= 366:
        raise ValueError("Expected a 1–366 day date window starting in or after 2000")
    return datetime.combine(start, time.min), days * 24


def _finite_number(value, label):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"Missing or invalid weather value: {label}")
    return value


def validate_city_response(payload, city_id, start_date, end_date):
    """Validate complete hourly coverage and return six-column UTC cache rows."""
    if not isinstance(payload, dict) or payload.get("error"):
        raise ValueError(f"Weather API did not return a successful city response: {city_id}")
    if payload.get("timezone") != "Asia/Shanghai" or payload.get("utc_offset_seconds") != 28800:
        raise ValueError(f"Unexpected weather timezone: {city_id}")
    local_start, expected_count = _window(start_date, end_date)
    hourly = payload.get("hourly", {})
    if any(not isinstance(hourly.get(key), list) or len(hourly[key]) != expected_count
           for key in ("time",) + HOURLY):
        raise ValueError(f"Incomplete weather hours/variables for {city_id}: expected {expected_count}")
    units = payload.get("hourly_units", {})
    expected_units = {"temperature_2m": "°C", "relative_humidity_2m": "%", "precipitation": "mm",
                      "weather_code": "wmo code", "time": "iso8601"}
    if any(units.get(key) != unit for key, unit in expected_units.items()):
        raise ValueError(f"Unexpected weather units: {city_id}")
    rows = []
    for index, timestamp in enumerate(hourly["time"]):
        try:
            local = datetime.fromisoformat(timestamp)
        except (ValueError, TypeError) as error:
            raise ValueError(f"Invalid local weather timestamp: {city_id}") from error
        if local != local_start + timedelta(hours=index):
            raise ValueError(f"Missing, repeated, unordered or out-of-window weather hour: {city_id}")
        temperature = _finite_number(hourly["temperature_2m"][index], "temperature_2m")
        humidity = _finite_number(hourly["relative_humidity_2m"][index], "relative_humidity_2m")
        precipitation = _finite_number(hourly["precipitation"][index], "precipitation")
        code = _finite_number(hourly["weather_code"][index], "weather_code")
        if not -100 <= temperature <= 70 or not 0 <= humidity <= 100 or precipitation < 0:
            raise ValueError(f"Out-of-range weather observation: {city_id}")
        if int(code) != code or code not in WMO_CODES:
            raise ValueError(f"Unsupported WMO weather code: {city_id}")
        rows.append({"city_id": city_id,
                     "recorded_at": local.replace(tzinfo=BUSINESS_TZ).astimezone(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
                     "temperature_c": temperature, "humidity_pct": humidity,
                     "weather": int(code), "rainfall_mm": precipitation})
    return rows


def _fetch_city(city, start_date, end_date, timeout):
    city_id, latitude, longitude = city
    params = {"latitude": latitude, "longitude": longitude,
              "start_date": start_date, "end_date": end_date,
              "hourly": ",".join(HOURLY), "models": "era5", "timezone": "Asia/Shanghai",
              "temperature_unit": "celsius", "precipitation_unit": "mm", "timeformat": "iso8601"}
    url = ENDPOINT + "?" + urlencode(params)
    request = Request(url, headers={"User-Agent": "qt-charging-platform-educational-dataset/1.0",
                                    "Accept": "application/json"})
    with urlopen(request, timeout=timeout) as response:
        # The selected 366-day maximum is well below this guard. Large/raw API
        # payloads stay in memory and are not committed as additional files.
        raw = response.read(20 * 1024 * 1024 + 1)
        if len(raw) > 20 * 1024 * 1024:
            raise ValueError("Weather response exceeds the configured size limit")
    payload = json.loads(raw)
    rows = validate_city_response(payload, city_id, start_date, end_date)
    info = {"city_id": city_id, "request_url": url, "request_parameters": params,
            "response_sha256": hashlib.sha256(raw).hexdigest(), "response_bytes": len(raw),
            "row_count": len(rows), "returned_grid_latitude": payload.get("latitude"),
            "returned_grid_longitude": payload.get("longitude"), "returned_elevation_m": payload.get("elevation"),
            "returned_timezone": payload.get("timezone"), "utc_offset_seconds": payload.get("utc_offset_seconds")}
    return rows, info


def fetch_weather_reference(output_path, provenance_path, start_date="2025-12-01", end_date="2026-05-29", timeout=45):
    """Fetch all five cities, validate first, then exclusively create both files.

    Any missing hour, null variable, invalid code or failed API call aborts the
    operation. There is no synthetic fallback in this reference-fetching tool.
    """
    output, provenance_output = Path(output_path), Path(provenance_path)
    if output.resolve() == provenance_output.resolve():
        raise ValueError("Weather cache and provenance must have different paths")
    for path in (output, provenance_output):
        if path.exists() or path.is_symlink():
            raise FileExistsError("Refusing to overwrite an existing weather reference output")
    local_start, hours_per_city = _window(start_date, end_date)
    if not 1 <= timeout <= 60:
        raise ValueError("HTTP timeout must be between 1 and 60 seconds")
    all_rows, requests = [], []
    for city in CITIES:
        print(f"Fetching ERA5 reanalysis background for {city[0]}...", flush=True)
        rows, info = _fetch_city(city, start_date, end_date, timeout)
        all_rows.extend(rows)
        requests.append(info)
    if len(all_rows) != len(CITIES) * hours_per_city or len({(r["city_id"], r["recorded_at"]) for r in all_rows}) != len(all_rows):
        raise ValueError("Incomplete or duplicated city-hour weather grid")
    text = io.StringIO(newline="")
    writer = csv.DictWriter(text, fieldnames=FIELDS, lineterminator="\n")
    writer.writeheader()
    writer.writerows(all_rows)
    uncompressed = text.getvalue().encode("utf-8")
    binary = io.BytesIO()
    with gzip.GzipFile(filename="", fileobj=binary, mode="wb", mtime=0, compresslevel=9) as stream:
        stream.write(uncompressed)
    compressed = binary.getvalue()
    provenance = {
        "schema_version": "1.0.0", "source": "OPEN_METEO_ERA5_REANALYSIS",
        "retrieved_at": datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "documentation_url": DOCUMENTATION, "endpoint": ENDPOINT, "model": "era5",
        "nominal_model_grid": "ERA5 0.25 degrees (approximately 25 km); API selects/downscales a land grid point",
        "local_start_date": start_date, "local_end_date_inclusive": end_date,
        "business_timezone": "Asia/Shanghai", "timestamp_timezone": "UTC",
        "period_start": local_start.replace(tzinfo=BUSINESS_TZ).astimezone(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "period_end_exclusive": (local_start + timedelta(hours=hours_per_city)).replace(tzinfo=BUSINESS_TZ).astimezone(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "cities": [city[0] for city in CITIES], "hours_per_city": hours_per_city, "row_count": len(all_rows),
        "fields": list(FIELDS), "requests": requests,
        "cache_sha256": hashlib.sha256(compressed).hexdigest(), "cache_bytes": len(compressed),
        "uncompressed_csv_sha256": hashlib.sha256(uncompressed).hexdigest(), "uncompressed_csv_bytes": len(uncompressed),
        "validation": {"complete_hourly_grid": True, "duplicate_city_hours": 0, "null_values": 0,
                       "hours_converted_from_local_to_utc": True, "synthetic_fill_rows": 0},
        "field_semantics": {
            "temperature_c": "ERA5 2 m air temperature at indicated hour, degrees Celsius",
            "humidity_pct": "ERA5 2 m relative humidity at indicated hour, percent",
            "weather": "Original integer WMO weather_code computed by Open-Meteo from reanalysis; not the simulator CLEAR/RAIN/SNOW enum",
            "rainfall_mm": "Legacy column name: API precipitation, total precipitation INCLUDING snow water-equivalent during preceding hour, not rain-only",
        },
        "license": {"name": "Creative Commons Attribution 4.0 International (CC BY 4.0)",
                    "url": "https://creativecommons.org/licenses/by/4.0/",
                    "provider_license_url": "https://open-meteo.com/en/licence",
                    "api_terms_url": "https://open-meteo.com/en/terms",
                    "api_use": "Free API used for non-commercial educational content; commercial API access is subject to provider terms"},
        "attribution": "Weather data by Open-Meteo.com; ERA5 reanalysis from ECMWF/Copernicus Climate Change Service (C3S).",
        "display_credit_url": "https://open-meteo.com/",
        "era5_dataset_doi": "https://doi.org/10.24381/cds.adbb2d47",
        "changes_from_api": ["Selected five city-center point series and four hourly variables",
                             "Converted Asia/Shanghai hourly timestamps to UTC",
                             "Renamed fields to the local six-column cache contract and compressed as CSV gzip",
                             "No synthetic values, missing-value fill, temporal interpolation or manual regional adjustments"],
        "limitations": [
            "Reanalysis combines observations and a physical model; this is not a local station measurement, charging-site sensor or verified microclimate record.",
            "One requested city-center point is shared as city background; returned grid coordinates may differ from requested coordinates.",
            "Precipitation is the preceding-hour total. Summing it by its timestamp has a one-hour boundary convention, not a forward-hour rainfall measurement.",
            "WMO weather_code is inferred from reanalysis variables and is not an independently observed weather-station label.",
            "The cache provides weather context only; generated charging demand, regional behavior, costs and outcomes remain simulated.",
            "Future observed/reanalysis weather must not be supplied as a known-future forecasting feature unless explicitly running an oracle evaluation.",
            "Keep Open-Meteo attribution and a licence link with distributed data; display the provider credit link near weather values on webpages.",
        ],
    }
    encoded_provenance = (json.dumps(provenance, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")
    output.parent.mkdir(parents=True, exist_ok=True)
    provenance_output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as stream:
        stream.write(compressed)
    with provenance_output.open("xb") as stream:
        stream.write(encoded_provenance)
    return provenance


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--provenance", type=Path, required=True)
    parser.add_argument("--start-date", default="2025-12-01")
    parser.add_argument("--end-date", default="2026-05-29")
    parser.add_argument("--timeout", type=int, default=45)
    args = parser.parse_args(argv)
    try:
        result = fetch_weather_reference(args.output, args.provenance, args.start_date, args.end_date, args.timeout)
    except (OSError, ValueError, HTTPError, URLError) as error:
        parser.error(str(error))
    print(f"Saved {result['row_count']} validated ERA5 city-hour rows; no synthetic fill")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
