"""UE MCP 客户端。

这个 MCP 服务只暴露 3 个元工具（list_toolsets / describe_toolset / call_tool），
真正的编辑器能力都通过 call_tool 按 toolset + tool 两级调用。
"""

from __future__ import annotations

import json
from contextlib import AsyncExitStack
from typing import Any

# mcp 1.x 与 2.x 的客户端入口名字不同，这里做兼容处理
try:  # mcp >= 2.0
    from mcp.client.session import ClientSession
    from mcp.client.streamable_http import streamable_http_client
except ImportError:  # mcp 1.x
    from mcp import ClientSession
    from mcp.client.streamable_http import streamablehttp_client

    streamable_http_client = streamablehttp_client


class UEBridge:
    """与 UE ModelContextProtocol 插件的 HTTP 会话。"""

    def __init__(self, url: str) -> None:
        self.url = url
        self._stack: AsyncExitStack | None = None
        self._session: ClientSession | None = None

    async def __aenter__(self) -> "UEBridge":
        self._stack = AsyncExitStack()
        # mcp 1.x 返回 (read, write, get_session_id)，2.x 返回 (read, write)
        streams = await self._stack.enter_async_context(
            streamable_http_client(self.url)
        )
        read, write = streams[0], streams[1]
        self._session = await self._stack.enter_async_context(ClientSession(read, write))
        await self._session.initialize()
        return self

    async def __aexit__(self, *exc_info) -> None:
        if self._stack is not None:
            # 部分 MCP 服务端在关闭时返回 202，终止失败不影响已完成的写入
            try:
                await self._stack.aclose()
            except Exception:
                pass
            self._stack = None
            self._session = None

    async def call(
        self,
        toolset: str,
        tool: str,
        arguments: dict | None = None,
    ) -> Any:
        """调用指定工具集内的工具，返回 returnValue（已做 JSON 解析）。"""
        if self._session is None:
            raise RuntimeError("UEBridge 未连接，请在 async with 中使用")

        result = await self._session.call_tool(
            "call_tool",
            {
                "toolset_name": toolset,
                "tool_name": tool,
                "arguments": arguments or {},
            },
        )

        if getattr(result, "isError", False):
            raise RuntimeError(self._text(result) or f"{toolset}.{tool} 调用失败")

        return self._extract(result)

    @staticmethod
    def _text(result) -> str:
        parts = []
        for content in getattr(result, "content", None) or []:
            text = getattr(content, "text", None)
            if text:
                parts.append(text)
        return "\n".join(parts)

    @classmethod
    def _extract(cls, result) -> Any:
        structured = getattr(result, "structuredContent", None)
        if isinstance(structured, dict) and "returnValue" in structured:
            return structured["returnValue"]

        raw = cls._text(result)
        if not raw:
            return None
        try:
            obj = json.loads(raw)
        except json.JSONDecodeError:
            # 工具以纯文本返回了错误说明
            return raw
        if isinstance(obj, dict) and "returnValue" in obj:
            return obj["returnValue"]
        return obj
