"""ue-suntime：按真实地理位置与时间，把 UE 场景灯光还原为对应物理环境光。

用法：
    python main.py "明天下午五点的上海外滩"
    python main.py --lat 31.2397 --lon 121.4900 --datetime "2026-09-10 17:00" --utc-offset 8
    python main.py "上海外滩日落" --dry-run      # 只算参数不写场景
    python main.py "上海外滩日落" --json         # 只输出参数
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import yaml
from dotenv import load_dotenv

from llm_parse import LLMParseError, SceneQuery, parse_query
from mcp_client import UEBridge
from solar import sample
from ue_apply import apply_profile, apply_undo, get_current_level
from ue_profile import build_profile
from weather import get_cloud_cover

DEFAULT_CONFIG = Path(__file__).resolve().parent / "config.yaml"
UNDO_FILE = Path(__file__).resolve().parent / ".ue_suntime_undo.json"


def load_config(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def parse_datetime(text: str) -> datetime:
    """解析 '2026-09-10 17:00' 或 ISO8601 形式的本地时间。"""
    cleaned = text.strip().replace("T", " ")
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
        try:
            return datetime.strptime(cleaned, fmt)
        except ValueError:
            continue
    raise ValueError(f"无法解析时间：{text}")


def format_offset(offset: float) -> str:
    total = int(round(offset * 60))
    sign = "+" if total >= 0 else "-"
    total = abs(total)
    return f"UTC{sign}{total // 60:02d}:{total % 60:02d}"


def print_query(query: SceneQuery, cloud: float | None) -> None:
    print(f"地点    : {query.place}  ({query.latitude:.4f}, {query.longitude:.4f})")
    print(
        f"时间    : {query.local_datetime.strftime('%Y-%m-%d %H:%M')} "
        f"({format_offset(query.utc_offset)})"
    )
    if cloud is None:
        print("云量    : 未知（按晴天处理）")
    else:
        print(f"云量    : {cloud:.0f}%")
    if query.note:
        print(f"备注    : {query.note}")


def print_physics(profile) -> None:
    m = profile.meta
    am = "∞" if m["air_mass"] is None else f"{m['air_mass']:.2f}"
    print("-" * 56)
    print(f"太阳高度角 : {m['altitude']:.2f}°")
    print(f"太阳方位角 : {m['azimuth']:.2f}°  (正北0 / 东90 / 南180 / 西270)")
    print(f"大气质量   : {am}")
    print(f"直射照度   : {m['direct_lux']:.0f} lux")
    print(f"天空散射   : {m['diffuse_lux']:.0f} lux")
    print(f"太阳色温   : {m['sun_cct']:.0f} K")
    print(f"天空色温   : {m['sky_cct']:.0f} K")
    print("-" * 56)


def describe_target(report: dict, dry_run: bool) -> None:
    verb = "计划" if dry_run else "已应用"
    label = "平行光" if report["key"] == "sun" else "天光"
    print(f"\n[{verb}] {label}")
    if report["actor"]:
        print(f"  Actor    : {report['actor'].split('.')[-1]}")
    if report["component"]:
        print(f"  Component: {report['component'].split('.')[-1]}")

    rotation = report.get("rotation")
    if rotation:
        print(
            f"  旋转     : pitch={rotation.get('pitch'):.2f} "
            f"yaw={rotation.get('yaw'):.2f} roll={rotation.get('roll'):.2f}"
        )

    for key, value in report["properties"].items():
        print(f"  {key:<9} : {json.dumps(value, ensure_ascii=False)}")

    if report["previous"]:
        print(f"  修改前   : {json.dumps(report['previous'], ensure_ascii=False)}")
    if report["verified"] and not dry_run:
        print(f"  读回校验 : {json.dumps(report['verified'], ensure_ascii=False)}")
    for warning in report["warnings"]:
        print(f"  警告     : {warning}")


async def run(args: argparse.Namespace) -> int:
    load_dotenv()
    cfg = load_config(args.config)

    if args.undo:
        async with UEBridge(cfg["mcp"]["url"]) as bridge:
            for message in await apply_undo(bridge, UNDO_FILE):
                print(message)
        return 0

    # 1. 解析需求
    if args.lat is not None and args.lon is not None and args.datetime:
        query = SceneQuery(
            place=args.place or "自定义坐标",
            latitude=args.lat,
            longitude=args.lon,
            utc_offset=args.utc_offset,
            local_datetime=parse_datetime(args.datetime).replace(
                tzinfo=timezone(timedelta(hours=args.utc_offset))
            ),
        )
    elif args.query:
        try:
            query = await parse_query(args.query, cfg, datetime.now().astimezone())
        except LLMParseError as exc:
            print(f"需求解析失败：{exc}", file=sys.stderr)
            return 2
    else:
        print("需要提供需求文本，或使用 --lat/--lon/--datetime 指定参数", file=sys.stderr)
        return 2

    # 2. 云量（可选，失败按晴天）
    cloud = None
    weather_cfg = cfg.get("weather", {})
    if weather_cfg.get("enabled", True) and not args.no_weather:
        cloud = get_cloud_cover(
            query.latitude,
            query.longitude,
            query.utc_datetime,
            timeout=weather_cfg.get("timeout", 10),
        )

    # 3. 物理计算
    sun_sample = sample(
        query.latitude, query.longitude, query.utc_datetime, cloud
    )
    profile = build_profile(sun_sample, cfg)

    print_query(query, cloud)
    print_physics(profile)

    # 4. 只输出参数
    if args.json:
        payload = {
            "query": {
                "place": query.place,
                "latitude": query.latitude,
                "longitude": query.longitude,
                "local_datetime": query.local_datetime.isoformat(),
                "utc_offset": query.utc_offset,
            },
            "physics": profile.meta,
            "ue": {
                "sun": {
                    "rotation": profile.sun.rotation,
                    "properties": profile.sun.properties,
                },
                "sky": {"properties": profile.sky.properties},
            },
        }
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        return 0

    # 5. 写入 UE
    async with UEBridge(cfg["mcp"]["url"]) as bridge:
        level = await get_current_level(bridge)
        print(f"当前关卡  : {level or '(未获取到)'}")

        reports = await apply_profile(bridge, profile, cfg, dry_run=args.dry_run)

        # 平行光的旋转在 dry-run 下也要展示
        if args.dry_run:
            for report in reports:
                if report["key"] == "sun":
                    report["rotation"] = profile.sun.rotation

        for report in reports:
            describe_target(report, args.dry_run)

    if args.dry_run:
        print("\n--dry-run：未写入场景")
    else:
        UNDO_FILE.write_text(
            json.dumps(reports, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        print(f"\n撤销记录: {UNDO_FILE.name}（可用 --undo 还原到本次修改前）")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ue-suntime",
        description="按地理位置与时间还原 UE 场景的物理环境光",
    )
    parser.add_argument("query", nargs="?", help="自然语言需求，例如：明天下午五点的上海外滩")
    parser.add_argument("--lat", type=float, help="纬度（与 --lon/--datetime 一起使用时跳过大模型）")
    parser.add_argument("--lon", type=float, help="经度")
    parser.add_argument("--datetime", help="本地时间，如 '2026-09-10 17:00'")
    parser.add_argument("--utc-offset", type=float, default=8.0, help="UTC 偏移小时数，默认 8")
    parser.add_argument("--place", help="地点名称（手动指定参数时的显示名）")
    parser.add_argument("--dry-run", action="store_true", help="只计算并打印，不写入场景")
    parser.add_argument("--json", action="store_true", help="只输出 JSON 参数，不写入场景")
    parser.add_argument("--no-weather", action="store_true", help="跳过云量查询")
    parser.add_argument("--undo", action="store_true", help="把灯光改回上一次修改前的值")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="配置文件路径")
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    raise SystemExit(asyncio.run(run(args)))


if __name__ == "__main__":
    main()
