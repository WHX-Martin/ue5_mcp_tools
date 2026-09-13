"""云量查询（Open-Meteo，免费无需 Key）。

云量只用于修正直射/散射比例。查询失败时返回 None，调用方按晴天处理。
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone

import requests

_FORECAST_URL = "https://api.open-meteo.com/v1/forecast"
_ARCHIVE_URL = "https://archive-api.open-meteo.com/v1/archive"


def _pick_nearest(times: list[str], values: list[float | None], target: datetime) -> float | None:
    """在小时序列中取与目标时刻最接近的有效值。"""
    best_value = None
    best_delta = None
    for ts, val in zip(times, values):
        if val is None:
            continue
        try:
            t = datetime.fromisoformat(ts).replace(tzinfo=timezone.utc)
        except ValueError:
            continue
        delta = abs((t - target).total_seconds())
        if best_delta is None or delta < best_delta:
            best_delta = delta
            best_value = val
    return best_value


def get_cloud_cover(
    latitude: float,
    longitude: float,
    dt: datetime,
    timeout: int = 10,
) -> float | None:
    """返回目标时刻的云量百分比，失败返回 None。"""
    dt_utc = dt.astimezone(timezone.utc)
    now_utc = datetime.now(timezone.utc)
    params_base = {
        "latitude": latitude,
        "longitude": longitude,
        "hourly": "cloud_cover",
        "timezone": "UTC",
    }

    # 未来或近期用 forecast，较早的历史用 archive(ERA5)
    if dt_utc >= now_utc - timedelta(days=5):
        url = _FORECAST_URL
        params = dict(params_base)
        params["start_date"] = min(dt_utc, now_utc).date().isoformat()
        params["end_date"] = max(dt_utc, now_utc).date().isoformat()
    else:
        url = _ARCHIVE_URL
        params = dict(params_base)
        params["start_date"] = dt_utc.date().isoformat()
        params["end_date"] = (dt_utc + timedelta(days=1)).date().isoformat()

    try:
        resp = requests.get(url, params=params, timeout=timeout)
        resp.raise_for_status()
        hourly = resp.json().get("hourly") or {}
        times = hourly.get("time") or []
        values = hourly.get("cloud_cover") or []
        if not times or not values:
            return None
        return _pick_nearest(times, values, dt_utc)
    except Exception:
        return None
