"""把物理光照参数映射为 UE 灯光参数。

约定：
- 平行光(DirectionalLight)负责太阳直射，强度来自 direct_lux，方向来自太阳高度角/方位角。
- 天光(SkyLight)负责天空散射，强度来自 diffuse_lux。
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

from solar import SunSample, kelvin_to_linear_rgb

# 物理照度(lux) 到 UE 灯光强度的换算系数。
# 正午晴天地面直射约 90000~100000 lux，对应 UE 平行光强度约 9~10，故取 1e-4。
LUX_TO_UE = 1e-4


@dataclass
class LightTarget:
    """单个灯光目标的写入内容。"""

    key: str
    actor_ref: str = ""
    component_ref: str = ""
    rotation: dict | None = None
    properties: dict = field(default_factory=dict)
    previous_properties: dict = field(default_factory=dict)


@dataclass
class UEProfile:
    sun: LightTarget
    sky: LightTarget
    meta: dict = field(default_factory=dict)


def sun_rotator(altitude: float, azimuth: float, north_axis: str = "y") -> dict:
    """由太阳高度角/方位角求平行光的 UE Rotator。

    平行光沿 Actor 局部 +X 传播，因此 forward 需指向"由太阳射向地面"的方向。
    """
    pitch = -altitude
    if north_axis == "x":
        # X=North, Y=East
        yaw = (azimuth + 180.0) % 360.0
    else:
        # X=East, Y=North
        yaw = math.degrees(
            math.atan2(-math.cos(math.radians(azimuth)), -math.sin(math.radians(azimuth)))
        ) % 360.0
    return {"pitch": round(pitch, 3), "yaw": round(yaw, 3), "roll": 0.0}


def _clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def _map_intensity(lighting: dict, lux: float, kind: str, scale: float) -> float:
    """照度 -> UE 强度。

    linear:     纯物理线性映射，黄昏/夜间的数值会非常低。
    perceptual: 以正午为参考点做幂律压缩，模拟人眼/相机的自适应，
                视觉上更接近"现场观感"，但不再是严格线性物理值。
    """
    response = lighting.get("response", "perceptual")
    if response == "linear" or lux <= 0:
        return lux * LUX_TO_UE * scale

    gamma = float(lighting.get("perceptual_gamma", 0.6))
    ref_lux = float(lighting.get(f"{kind}_reference_lux", 100000.0 if kind == "sun" else 10000.0))
    ref_intensity = float(
        lighting.get(f"{kind}_reference_intensity", 10.0 if kind == "sun" else 1.0)
    )
    return ref_intensity * ((lux / ref_lux) ** gamma) * scale


def build_profile(sample: SunSample, cfg: dict) -> UEProfile:
    lighting = cfg.get("lighting", {})
    scale = float(lighting.get("intensity_scale", 1.0))
    color_mode = lighting.get("color_mode", "temperature")
    north_axis = lighting.get("north_axis", "y")

    sun_cct = sample.sun_cct + float(lighting.get("sun_cct_offset", 0))
    sky_cct = sample.sky_cct + float(lighting.get("sky_cct_offset", 0))

    sun_intensity = _clamp(
        _map_intensity(lighting, sample.direct_lux, "sun", scale),
        float(lighting.get("sun_min_intensity", 0.05)),
        float(lighting.get("sun_max_intensity", 20.0)),
    )
    sky_intensity = _clamp(
        _map_intensity(lighting, sample.diffuse_lux, "sky", scale),
        float(lighting.get("sky_min_intensity", 0.02)),
        float(lighting.get("sky_max_intensity", 6.0)),
    )

    # 平行光颜色：优先走引擎黑体色温；否则直接写线性色
    sun_props: dict = {"intensity": round(sun_intensity, 4)}
    if color_mode == "temperature":
        sun_props["bUseTemperature"] = True
        sun_props["temperature"] = round(sun_cct, 1)
        # 色温生效时 LightColor 应留白，否则会二次染色
        sun_props["lightColor"] = {"r": 1.0, "g": 1.0, "b": 1.0, "a": 1.0}
    else:
        r, g, b = kelvin_to_linear_rgb(sun_cct)
        sun_props["bUseTemperature"] = False
        sun_props["lightColor"] = {
            "r": round(r, 5),
            "g": round(g, 5),
            "b": round(b, 5),
            "a": 1.0,
        }

    # 天光组件不支持色温，只能写线性色
    sr, sg, sb = kelvin_to_linear_rgb(sky_cct)
    sky_props: dict = {
        "intensity": round(sky_intensity, 4),
        "lightColor": {"r": round(sr, 5), "g": round(sg, 5), "b": round(sb, 5), "a": 1.0},
    }

    return UEProfile(
        sun=LightTarget(
            key="sun",
            rotation=sun_rotator(sample.altitude, sample.azimuth, north_axis),
            properties=sun_props,
        ),
        sky=LightTarget(key="sky", properties=sky_props),
        meta={
            "altitude": round(sample.altitude, 2),
            "azimuth": round(sample.azimuth, 2),
            "air_mass": None if not math.isfinite(sample.air_mass) else round(sample.air_mass, 3),
            "direct_lux": round(sample.direct_lux, 1),
            "diffuse_lux": round(sample.diffuse_lux, 1),
            "sun_cct": round(sun_cct, 0),
            "sky_cct": round(sky_cct, 0),
            "cloud_cover": sample.cloud_cover,
            "color_mode": color_mode,
            "north_axis": north_axis,
            "response": lighting.get("response", "perceptual"),
        },
    )
