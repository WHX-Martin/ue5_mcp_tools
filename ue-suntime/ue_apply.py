"""把灯光参数写入当前 UE 关卡。

流程严格遵循：查找 Actor -> 查找组件 -> 读原值 -> 写入 -> 读回校验。
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from mcp_client import UEBridge
from ue_profile import UEProfile, LightTarget

SCENE_TOOLS = "editor_toolset.toolsets.scene.SceneTools"
ACTOR_TOOLS = "editor_toolset.toolsets.actor.ActorTools"
OBJECT_TOOLS = "editor_toolset.toolsets.object.ObjectTools"


async def get_current_level(bridge: UEBridge) -> str:
    res = await bridge.call(SCENE_TOOLS, "get_current_level", {})
    return res if isinstance(res, str) else ""


async def find_actor(bridge: UEBridge, name_hint: str) -> str | None:
    """按 Actor 标签名（大小写不敏感子串）查找场景中的 Actor。"""
    res = await bridge.call(
        SCENE_TOOLS,
        "find_actors",
        {"name": name_hint, "tag": "", "collision_channels": []},
    )
    if isinstance(res, list):
        for item in res:
            ref = item.get("refPath") if isinstance(item, dict) else None
            if ref:
                return ref
    return None


async def find_component(bridge: UEBridge, actor_ref: str, keyword: str) -> str | None:
    """在 Actor 的组件中按路径关键字匹配组件。"""
    res = await bridge.call(
        ACTOR_TOOLS, "get_components", {"actor": {"refPath": actor_ref}}
    )
    if isinstance(res, list):
        lowered = keyword.lower()
        for item in res:
            ref = item.get("refPath") if isinstance(item, dict) else None
            if ref and lowered in ref.lower():
                return ref
    return None


async def read_properties(
    bridge: UEBridge, component_ref: str, names: list[str]
) -> dict:
    res = await bridge.call(
        OBJECT_TOOLS,
        "get_properties",
        {"instance": {"refPath": component_ref}, "properties": names},
    )
    if isinstance(res, str):
        try:
            res = json.loads(res)
        except json.JSONDecodeError:
            return {}
    return res if isinstance(res, dict) else {}


async def write_properties(
    bridge: UEBridge, component_ref: str, values: dict
) -> bool:
    """set_properties 的 values 参数必须是 JSON 字符串。"""
    res = await bridge.call(
        OBJECT_TOOLS,
        "set_properties",
        {"instance": {"refPath": component_ref}, "values": json.dumps(values)},
    )
    return bool(res)


async def write_rotation(bridge: UEBridge, actor_ref: str, rotation: dict) -> bool:
    res = await bridge.call(
        ACTOR_TOOLS,
        "set_actor_transform",
        {
            "actor": {"refPath": actor_ref},
            "xform": {"rotation": rotation},
            "worldspace": True,
        },
    )
    return bool(res)


async def read_rotation(bridge: UEBridge, actor_ref: str) -> dict | None:
    res = await bridge.call(
        ACTOR_TOOLS, "get_actor_transform", {"actor": {"refPath": actor_ref}}
    )
    if isinstance(res, dict):
        rotation = res.get("rotation")
        if isinstance(rotation, dict):
            return rotation
    return None


async def apply_target(
    bridge: UEBridge,
    target: LightTarget,
    target_cfg: dict,
    dry_run: bool = False,
) -> dict:
    """把单个灯光目标写入场景，返回执行报告。"""
    report: dict[str, Any] = {
        "key": target.key,
        "actor": "",
        "component": "",
        "rotation": None,
        "properties": target.properties,
        "previous": {},
        "verified": {},
        "warnings": [],
        "applied": False,
    }

    actor_ref = target_cfg.get("actor_ref") or await find_actor(
        bridge, target_cfg.get("name_hint", target.key)
    )
    if not actor_ref:
        report["warnings"].append(
            f"未找到 Actor（name_hint={target_cfg.get('name_hint')}）"
        )
        return report
    report["actor"] = actor_ref

    component_ref = target_cfg.get("component_ref") or await find_component(
        bridge, actor_ref, target_cfg.get("component_keyword", "LightComponent")
    )
    if not component_ref:
        report["warnings"].append(
            f"未找到组件（keyword={target_cfg.get('component_keyword')}）"
        )
        return report
    report["component"] = component_ref

    # 只读回将要覆盖的属性，避免请求该组件不支持的字段导致整体失败
    report["previous"] = await read_properties(
        bridge, component_ref, list(target.properties.keys())
    )

    if dry_run:
        return report

    if target_cfg.get("apply_rotation", True) and target.rotation:
        report["previous_rotation"] = await read_rotation(bridge, actor_ref)
        if not await write_rotation(bridge, actor_ref, target.rotation):
            report["warnings"].append("旋转写入失败")
        report["rotation"] = await read_rotation(bridge, actor_ref)

    if target_cfg.get("apply_intensity", True) or target_cfg.get("apply_color", True):
        props = dict(target.properties)
        if not target_cfg.get("apply_intensity", True):
            props.pop("intensity", None)
        if not target_cfg.get("apply_color", True):
            props.pop("lightColor", None)
            props.pop("temperature", None)
            props.pop("bUseTemperature", None)

        if props and not await write_properties(bridge, component_ref, props):
            report["warnings"].append("属性写入失败")

    report["verified"] = await read_properties(
        bridge, component_ref, list(target.properties.keys())
    )
    report["applied"] = not report["warnings"]
    return report


async def apply_profile(
    bridge: UEBridge,
    profile: UEProfile,
    cfg: dict,
    dry_run: bool = False,
) -> list[dict]:
    targets_cfg = cfg.get("targets", {})
    reports = []
    for target in (profile.sun, profile.sky):
        target_cfg = targets_cfg.get(target.key, {})
        reports.append(await apply_target(bridge, target, target_cfg, dry_run))
    return reports


async def apply_undo(bridge: UEBridge, undo_file: Path) -> list[str]:
    """按上次运行的记录把灯光改回原值。"""
    if not undo_file.exists():
        return ["没有找到撤销记录"]

    entries = json.loads(undo_file.read_text(encoding="utf-8"))
    messages = []
    for entry in entries:
        key = entry.get("key", "?")
        if entry.get("component") and entry.get("previous"):
            ok = await write_properties(bridge, entry["component"], entry["previous"])
            messages.append(f"{key}: 属性{'已恢复' if ok else '恢复失败'}")
        if entry.get("actor") and entry.get("previous_rotation"):
            ok = await write_rotation(bridge, entry["actor"], entry["previous_rotation"])
            messages.append(f"{key}: 旋转{'已恢复' if ok else '恢复失败'}")
    return messages
