"""用大模型把自然语言需求解析为结构化查询参数。

大模型只负责"理解"：地名 -> 经纬度，相对时间 -> 绝对时刻。
物理计算与写值全部由确定性代码完成，避免模型幻觉影响结果。
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone

from dotenv import load_dotenv
from openai import AsyncOpenAI

load_dotenv()

SYSTEM_PROMPT = """你是地理与时间解析助手，服务于一个 Unreal Engine 光照还原工具。

你的唯一职责是把用户的自然语言描述解析成结构化参数，然后调用 resolve_scene_query 工具。

规则：
1. 地点：给出该地点的代表性经纬度，精度到小数点后 4 位即可。用户说"上海外滩"就给外滩的坐标，不要给上海市中心。
2. 时间：把"明天下午五点""上周三傍晚"这类相对表述换算成具体本地时间，再按当地时区补上 UTC 偏移小时数（东八区为 8，西五区为 -5）。夏令时按目标日期实际情况计算。
3. 若用户没有明确日期，默认指今天；没有明确时刻，默认指当地正午 12:00。
4. 只调用一次工具，不要输出多余解释。"""

TOOL_SCHEMA = {
    "type": "function",
    "function": {
        "name": "resolve_scene_query",
        "description": "解析用户描述的地理位置与时刻",
        "parameters": {
            "type": "object",
            "properties": {
                "place": {"type": "string", "description": "地点名称"},
                "latitude": {"type": "number", "description": "纬度，北纬为正"},
                "longitude": {"type": "number", "description": "经度，东经为正"},
                "utc_offset": {
                    "type": "number",
                    "description": "目标日期当地的 UTC 偏移小时数，例如东八区为 8",
                },
                "local_datetime": {
                    "type": "string",
                    "description": "本地时间的 ISO8601 表示，例如 2026-09-10T17:00:00",
                },
                "note": {"type": "string", "description": "可选的补充说明"},
            },
            "required": [
                "place",
                "latitude",
                "longitude",
                "utc_offset",
                "local_datetime",
            ],
        },
    },
}


@dataclass
class SceneQuery:
    place: str
    latitude: float
    longitude: float
    utc_offset: float
    local_datetime: datetime
    note: str = ""

    @property
    def utc_datetime(self) -> datetime:
        return self.local_datetime.astimezone(timezone.utc)


class LLMParseError(RuntimeError):
    pass


def _to_aware(local_iso: str, utc_offset: float) -> datetime:
    naive = datetime.fromisoformat(local_iso)
    return naive.replace(tzinfo=timezone(timedelta(hours=utc_offset)))


async def parse_query(text: str, cfg: dict, now: datetime) -> SceneQuery:
    llm_cfg = cfg.get("llm", {})
    api_key = os.getenv(llm_cfg.get("api_key_env", ""), "").strip()
    if not api_key:
        raise LLMParseError(
            f"未配置 {llm_cfg.get('api_key_env')}，可改用 --lat/--lon/--datetime 直接指定参数"
        )

    client = AsyncOpenAI(
        base_url=llm_cfg.get("base_url"),
        api_key=api_key,
        timeout=llm_cfg.get("timeout", 60),
    )

    user_prompt = (
        f"当前时间：{now.astimezone().isoformat()}\n"
        f"用户需求：{text}\n\n"
        "请解析并调用 resolve_scene_query。"
    )

    response = await client.chat.completions.create(
        model=llm_cfg.get("model"),
        messages=[
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_prompt},
        ],
        tools=[TOOL_SCHEMA],
        tool_choice={"type": "function", "function": {"name": "resolve_scene_query"}},
        temperature=llm_cfg.get("temperature", 0.2),
    )

    message = response.choices[0].message
    payload: dict | None = None

    if message.tool_calls:
        payload = json.loads(message.tool_calls[0].function.arguments)
    elif message.content:
        # 部分模型不返回 tool_calls，退化到从文本中提取 JSON
        raw = message.content.strip()
        if raw.startswith("```"):
            raw = raw.strip("`").replace("json", "", 1).strip()
        payload = json.loads(raw)

    if not payload:
        raise LLMParseError("大模型未返回可解析的参数")

    return SceneQuery(
        place=payload.get("place", ""),
        latitude=float(payload["latitude"]),
        longitude=float(payload["longitude"]),
        utc_offset=float(payload["utc_offset"]),
        local_datetime=_to_aware(payload["local_datetime"], float(payload["utc_offset"])),
        note=payload.get("note", ""),
    )
