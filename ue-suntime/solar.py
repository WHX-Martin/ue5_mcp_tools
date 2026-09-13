"""太阳位置与地面照度计算。

纯本地实现（NOAA 太阳位置算法 + Meinel 大气衰减经验公式），不依赖任何外部服务。
所有角度单位为度，照度单位为 lux，色温单位为 K。
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import datetime, timezone

SOLAR_CONSTANT = 1361.0  # W/m^2

# 太阳色温经验表：(太阳高度角, 色温K)
_SUN_CCT_TABLE = [
    (-18.0, 1800.0),
    (-6.0, 1900.0),
    (0.0, 2100.0),
    (2.0, 2600.0),
    (5.0, 3300.0),
    (10.0, 4300.0),
    (20.0, 5100.0),
    (40.0, 5500.0),
    (60.0, 5700.0),
    (90.0, 5800.0),
]

# 天空积分色温经验表：(太阳高度角, 色温K)
_SKY_CCT_TABLE = [
    (-18.0, 9000.0),
    (-6.0, 7000.0),
    (0.0, 4800.0),
    (5.0, 5200.0),
    (15.0, 6600.0),
    (30.0, 8200.0),
    (60.0, 10000.0),
    (90.0, 11000.0),
]


@dataclass
class SunSample:
    """某一时刻某一地点的光照采样结果。"""

    altitude: float  # 太阳高度角(度)，地平线以下为负
    azimuth: float  # 太阳方位角(度)，正北为0，顺时针(东90/南180/西270)
    air_mass: float  # 大气质量
    direct_lux: float  # 水平面直射照度
    diffuse_lux: float  # 水平面散射(天空)照度
    sun_cct: float  # 太阳色温
    sky_cct: float  # 天空色温
    cloud_cover: float | None  # 云量百分比，None 表示未知


def julian_day(dt: datetime) -> float:
    """把带时区的 datetime 转成儒略日。"""
    dt = dt.astimezone(timezone.utc)
    year, month = dt.year, dt.month
    day = dt.day + (dt.hour + dt.minute / 60.0 + dt.second / 3600.0) / 24.0
    if month <= 2:
        year -= 1
        month += 12
    a = year // 100
    b = 2 - a + a // 4
    return (
        int(365.25 * (year + 4716))
        + int(30.6001 * (month + 1))
        + day
        + b
        - 1524.5
    )


def solar_position(latitude: float, longitude: float, dt: datetime) -> tuple[float, float]:
    """返回 (高度角, 方位角)，单位度。方位角自正北起顺时针。"""
    jd = julian_day(dt)
    t = (jd - 2451545.0) / 36525.0

    # 几何平黄经 / 平近点角 / 离心率
    l0 = (280.46646 + t * (36000.76983 + t * 0.0003032)) % 360.0
    m = 357.52911 + t * (35999.05029 - 0.0001537 * t)
    e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t)
    m_rad = math.radians(m)

    # 中心差与真黄经
    center = (
        math.sin(m_rad) * (1.914602 - t * (0.004817 + 0.000014 * t))
        + math.sin(2 * m_rad) * (0.019993 - 0.000101 * t)
        + math.sin(3 * m_rad) * 0.000289
    )
    true_long = l0 + center
    omega = math.radians(125.04 - 1934.136 * t)
    lam = math.radians(true_long - 0.00569 - 0.00478 * math.sin(omega))

    # 黄赤交角与赤纬
    eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0
    eps = math.radians(eps0 + 0.00256 * math.cos(omega))
    decl = math.asin(math.sin(eps) * math.sin(lam))

    # 时差(分钟)
    y = math.tan(eps / 2.0) ** 2
    l0_rad = math.radians(l0)
    eq_time = 4 * math.degrees(
        y * math.sin(2 * l0_rad)
        - 2 * e * math.sin(m_rad)
        + 4 * e * y * math.sin(m_rad) * math.cos(2 * l0_rad)
        - 0.5 * y * y * math.sin(4 * l0_rad)
        - 1.25 * e * e * math.sin(2 * m_rad)
    )

    utc = dt.astimezone(timezone.utc)
    minutes = utc.hour * 60.0 + utc.minute + utc.second / 60.0
    true_solar_time = (minutes + eq_time + 4.0 * longitude) % 1440.0
    hour_angle = true_solar_time / 4.0 - 180.0
    if hour_angle < -180.0:
        hour_angle += 360.0

    lat_rad = math.radians(latitude)
    ha_rad = math.radians(hour_angle)
    sin_alt = math.sin(lat_rad) * math.sin(decl) + math.cos(lat_rad) * math.cos(decl) * math.cos(ha_rad)
    altitude = math.degrees(math.asin(max(-1.0, min(1.0, sin_alt))))

    cos_alt = math.cos(math.radians(altitude))
    if abs(cos_alt) < 1e-9:
        azimuth = 180.0
    else:
        cos_az = (math.sin(decl) - math.sin(math.radians(altitude)) * math.sin(lat_rad)) / (
            cos_alt * math.cos(lat_rad)
        )
        azimuth = math.degrees(math.acos(max(-1.0, min(1.0, cos_az))))
        if hour_angle > 0:
            azimuth = 360.0 - azimuth

    return altitude, azimuth


def air_mass(altitude: float) -> float:
    """Kasten-Young 大气质量。太阳在地平线以下时返回 inf。"""
    if altitude <= 0:
        return float("inf")
    return 1.0 / (math.sin(math.radians(altitude)) + 0.50572 * (altitude + 6.07995) ** -1.6364)


def _interp(table: list[tuple[float, float]], x: float) -> float:
    if x <= table[0][0]:
        return table[0][1]
    if x >= table[-1][0]:
        return table[-1][1]
    for (x0, y0), (x1, y1) in zip(table, table[1:]):
        if x0 <= x <= x1:
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return table[-1][1]


def sample(
    latitude: float,
    longitude: float,
    dt: datetime,
    cloud_cover: float | None = None,
) -> SunSample:
    """计算该地点该时刻的物理光照参数。"""
    altitude, azimuth = solar_position(latitude, longitude, dt)
    am = air_mass(altitude)

    if altitude > 0 and math.isfinite(am):
        # Meinel 公式：法向直射辐照度
        dni = SOLAR_CONSTANT * (0.7 ** (am**0.678))
        sin_alt = math.sin(math.radians(altitude))
        direct_w = dni * sin_alt
        diffuse_w = 0.10 * dni * (sin_alt**0.3)
    else:
        direct_w = 0.0
        diffuse_w = 0.0

    # 云量修正：直射被遮挡，散射增强
    if cloud_cover is not None:
        c = max(0.0, min(100.0, cloud_cover)) / 100.0
        direct_w *= 1.0 - 0.75 * c
        diffuse_w *= 1.0 + 1.6 * c

    # 暮光辉光：太阳落到地平线以下后仍有余晖
    if altitude <= 0 and altitude > -18.0:
        diffuse_w += 0.12 * math.exp(altitude / 3.0)

    # 光视效能：直射随高度角升高而增大，散射取固定高值(天空偏蓝)
    eff_direct = 90.0 + 15.0 * max(0.0, math.sin(math.radians(altitude)))
    eff_diffuse = 115.0

    return SunSample(
        altitude=altitude,
        azimuth=azimuth,
        air_mass=am,
        direct_lux=direct_w * eff_direct,
        diffuse_lux=diffuse_w * eff_diffuse,
        sun_cct=_interp(_SUN_CCT_TABLE, altitude),
        sky_cct=_interp(_SKY_CCT_TABLE, altitude),
        cloud_cover=cloud_cover,
    )


def kelvin_to_linear_rgb(kelvin: float) -> tuple[float, float, float]:
    """色温转线性 sRGB（Tanner Helland 近似 + sRGB 线性化）。返回 0..1 三元组。"""
    t = max(1000.0, min(40000.0, kelvin)) / 100.0

    if t <= 66.0:
        r = 255.0
        g = 99.4708025861 * math.log(t) - 161.1195681661
    else:
        r = 329.698727446 * ((t - 60.0) ** -0.1332047592)
        g = 288.1221695283 * ((t - 60.0) ** -0.0755148492)

    if t >= 66.0:
        b = 255.0
    elif t <= 19.0:
        b = 0.0
    else:
        b = 138.5177312231 * math.log(t - 10.0) - 305.0447927307

    def to_linear(c: float) -> float:
        v = max(0.0, min(255.0, c)) / 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4

    return to_linear(r), to_linear(g), to_linear(b)
