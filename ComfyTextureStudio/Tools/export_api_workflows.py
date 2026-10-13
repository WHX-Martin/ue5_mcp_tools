#!/usr/bin/env python3
"""前端格式工作流 → API 格式（POST /prompt 可提交）+ 两张拆分图。

为什么不能靠猜 widget 顺序
--------------------------
前端节点的 ``widgets_values`` 是一个**数组**，要靠节点定义才能映射到具名输入。
本机 ComfyUI 0.38.2 的 ``/object_info`` 里 ``input_order`` 是**空的**，所以顺序只能
从 ``input.required`` / ``input.optional`` 的键序推。而前端还有自己的注入，实测：

* 只有**标量类型**（INT / FLOAT / STRING / BOOLEAN / COMBO）的输入才会有 widget；
  ``MODEL`` / ``IMAGE`` / ``LATENT`` … 这类连线类型永远不占 widget 位（不论是否已连线）。
  反例可验证：``PackPBRChannels`` 的 optional ``ao``/``roughness``/``metalness`` 都是 IMAGE，
  所以 ``widgets_values`` 只有 1 个值（``metalness_value``），不是 4 个。
* ``KSampler.seed`` 带 ``control_after_generate: true``，前端会在它**后面**再插一个
  下拉（值形如 ``"fixed"``）——这个 widget **不在** ``/object_info`` 里。
  所以 ``KSampler`` 有 6 个标量输入却有 7 个 widget 值。

脚本对每张图断言 ``widgets_values`` 长度 == 标量输入数 + 前端注入数，对不上就报错退出，
不静默产出错位的参数。

拆分
----
* ``stage_a_base_texture``：反向可达(20)
* ``stage_b_pbr``：反向可达(41,54,63,64,65,71) \\ 阶段A，并把拆分点（``SeamlessVAEDecode``）
  的出口重定向到**新增**的 ``LoadImage``

用法
----
    python Tools/export_api_workflows.py                       # 导出 + 校验 + 更新 manifest
    python Tools/export_api_workflows.py --check-only           # 只校验不写盘
    python Tools/export_api_workflows.py --load-image-name cts_xxx.png
    python Tools/export_api_workflows.py --object-info-cache oi.json   # 离线复用

退出码：0 成功；1 有错误。
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import pathlib
import sys
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

from derive_keepset import FrontendGraph, derive  # noqa: E402

DEFAULT_MANIFEST = ROOT / "Config" / "Workflows" / "workflows.manifest.json"
DEFAULT_SOURCE = pathlib.Path(
    "D:/PersonalDocuments/hongxuan_document/comfyUI_tools/workflows/sdxl_sand_pbr_v2_geom.json"
)
DEFAULT_SERVER = "http://127.0.0.1:8000"

# 本机服务绝不能被系统代理劫持：环境里若设了 http_proxy（如 Clash 的 127.0.0.1:7852），
# urllib 会把发往 127.0.0.1:8000 的请求也塞给代理，拿回一个 502 Bad Gateway。
_NO_PROXY_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

# 会有 widget 的标量类型
WIDGET_SCALAR_TYPES = {"INT", "FLOAT", "STRING", "BOOLEAN", "COMBO"}
# 阶段 B 图里代表"运行时输入"的占位文件名
INPUT_PLACEHOLDER = "__CTS_RUNTIME_INPUT__.png"


# --------------------------------------------------------------------------- #
def fetch_object_info(server: str, cache: pathlib.Path | None) -> dict:
    if cache and cache.is_file():
        print(f"[object_info] 用缓存 {cache}")
        return json.loads(cache.read_text(encoding="utf-8"))
    url = server.rstrip("/") + "/object_info"
    print(f"[object_info] GET {url}")
    with _NO_PROXY_OPENER.open(url, timeout=120) as r:
        raw = r.read().decode("utf-8")
    if cache:
        cache.write_text(raw, encoding="utf-8")
        print(f"[object_info] 已缓存到 {cache}")
    return json.loads(raw)


def is_widget_type(t) -> bool:
    if isinstance(t, list):          # COMBO 的另一种写法：直接给候选数组
        return True
    return isinstance(t, str) and t in WIDGET_SCALAR_TYPES


def ordered_inputs(oi: dict, class_type: str) -> list[tuple[str, list]]:
    """按 object_info 的键序返回 [(name, spec)]，required 在前 optional 在后。"""
    spec = oi[class_type]["input"]
    out: list[tuple[str, list]] = []
    for sec in ("required", "optional"):
        for name, s in (spec.get(sec) or {}).items():
            out.append((name, s))
    return out


def widget_plan(oi: dict, class_type: str) -> tuple[list[str], list[int]]:
    """返回 (widget 输入名列表, 其中哪些后面会被前端注入一个额外 widget 的下标)。"""
    names: list[str] = []
    injected: list[int] = []
    for name, s in ordered_inputs(oi, class_type):
        if not is_widget_type(s[0]):
            continue
        opts = s[1] if len(s) > 1 and isinstance(s[1], dict) else {}
        if opts.get("forceInput"):
            continue
        names.append(name)
        if opts.get("control_after_generate"):
            injected.append(len(names) - 1)
    return names, injected


def map_widget_values(oi: dict, node: dict, problems: list[str]) -> dict:
    """把 widgets_values 数组映射成 {输入名: 值}。长度对不上则记 problem。"""
    ct = node["type"]
    if ct not in oi:
        problems.append(f"节点 {node['id']} 的类型 {ct} 不在 /object_info 中（节点包缺失？）")
        return {}
    names, injected = widget_plan(oi, ct)
    wv = list(node.get("widgets_values") or [])

    expect = len(names) + len(injected)
    if expect != len(wv):
        problems.append(
            f"节点 {node['id']} {ct}：widgets_values 有 {len(wv)} 个，"
            f"但按 object_info 推出 {len(names)} 个标量输入 + {len(injected)} 个前端注入 = {expect} 个。"
            f"\n      标量输入={names}\n      values={json.dumps(wv, ensure_ascii=False)[:200]}"
        )
        return {}

    out: dict = {}
    idx = 0
    for i, name in enumerate(names):
        out[name] = wv[idx]
        idx += 1
        if i in injected:      # 吃掉前端注入的那个额外 widget
            idx += 1
    return out


# --------------------------------------------------------------------------- #
def build_api_graph(
    graph: FrontendGraph,
    oi: dict,
    node_ids,
    *,
    add_nodes: dict | None = None,
    retarget_src: int | None = None,
    retarget_to: str | None = None,
    problems: list[str] | None = None,
) -> dict:
    """把前端节点集合转成 API 格式。

    add_nodes          : 追加的自定义节点 {id_str: {"class_type":..., "inputs":{...}}}
    retarget_src/_to   : 把来源为 retarget_src 的输入重定向到 retarget_to 的输出 0
    """
    problems = problems if problems is not None else []
    ids = set(node_ids)
    api: dict = {}

    for nid in sorted(ids):
        node = graph.by_id[nid]
        mode = node.get("mode", 0)
        if mode == 2:
            problems.append(f"节点 {nid} {node['type']} 处于 muted（mode=2），应从保留集中剔除")
            continue
        if mode == 4:
            problems.append(
                f"节点 {nid} {node['type']} 处于 bypass（mode=4）——bypass 需要改接线，"
                f"本脚本不支持，请先在 ComfyUI 里取消 bypass"
            )
            continue

        ct = node["type"]
        inputs = map_widget_values(oi, node, problems)

        # 连线输入
        for name, s in ordered_inputs(oi, ct):
            if is_widget_type(s[0]):
                continue
            endpoint = graph.resolve_input(node, name)
            if endpoint is None:
                continue                       # 未连线：交给 required 检查兜底
            src, slot = endpoint
            if retarget_src is not None and src == retarget_src and retarget_to is not None:
                inputs[name] = [retarget_to, 0]
            else:
                if src not in ids:
                    problems.append(
                        f"节点 {nid} {ct} 的输入 {name} 指向 {src}，但 {src} 不在本图保留集内"
                    )
                    continue
                inputs[name] = [str(src), slot]

        api[str(nid)] = {"class_type": ct, "inputs": inputs}

    if add_nodes:
        api.update(add_nodes)

    # required 完整性检查
    for nid_str, entry in api.items():
        ct = entry["class_type"]
        if ct not in oi:
            continue
        req = (oi[ct]["input"].get("required") or {}).keys()
        for name in req:
            if name not in entry["inputs"]:
                problems.append(f"节点 {nid_str} {ct} 缺少 required 输入 `{name}`")

    return api


# --------------------------------------------------------------------------- #
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="导出两张 API 格式工作流")
    ap.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    ap.add_argument("--workflow", default=str(DEFAULT_SOURCE))
    ap.add_argument("--server", default=DEFAULT_SERVER)
    ap.add_argument("--object-info-cache", default=None)
    ap.add_argument("--load-image-name", default=INPUT_PLACEHOLDER,
                    help="阶段 B 的 LoadImage 输入文件名（默认占位符，运行时由插件替换）")
    ap.add_argument("--check-only", action="store_true", help="只检查不写盘")
    args = ap.parse_args(argv)

    mpath = pathlib.Path(args.manifest)
    manifest = json.loads(mpath.read_text(encoding="utf-8"))
    wf = pathlib.Path(args.workflow)
    graph = FrontendGraph(wf)
    oi = fetch_object_info(args.server, pathlib.Path(args.object_info_cache)
                           if args.object_info_cache else None)

    problems: list[str] = []
    info: list[str] = []

    derived = derive(graph, manifest)
    keep_a = derived["stageA_keep"]
    keep_b = derived["stageB_keep_orig"]
    new_input_id = str(max(graph.by_id) + 1)

    # ---------- 阶段 A ----------
    api_a = build_api_graph(graph, oi, keep_a, problems=problems)

    # ---------- 阶段 B ----------
    split_src = manifest["stages"]["stageB"]["splitPoint"]["replacedNode"]
    load_image_node = {
        new_input_id: {"class_type": "LoadImage", "inputs": {"image": args.load_image_name}}
    }
    api_b = build_api_graph(
        graph, oi, keep_b,
        add_nodes=load_image_node,
        retarget_src=split_src,
        retarget_to=new_input_id,
        problems=problems,
    )

    # ---------- 报告 ----------
    print(f"源图          : {wf}")
    print(f"阶段 A 图      : {len(api_a)} 节点")
    print(f"阶段 B 图      : {len(api_b)} 节点（含新增 LoadImage #{new_input_id}）")
    print(f"阶段 B 输入占位 : {args.load_image_name}")

    # 连线重定向专项检查：拆分点两条边必须都落到 LoadImage
    for nid, iname in ((35, "image"), (70, "image")):
        got = api_b.get(str(nid), {}).get("inputs", {}).get(iname)
        ok = got == [new_input_id, 0]
        print(f"  拆分点 {nid}.{iname} -> {got}  {'OK' if ok else '*** 未重定向 ***'}")
        if not ok:
            problems.append(f"拆分点 {nid}.{iname} 未正确重定向到 {new_input_id}（实得 {got}）")

    # 槽位地址校验：manifest 里每个 address 必须能在对应图里解析
    # 地址里的 `{inputNode}` 是符号 token —— 阶段 B 的 LoadImage id 由导出时动态决定
    # （= 源图最大 id + 1），不能在 manifest 里写死。
    token_map = {"{inputNode}": new_input_id}

    def resolve_addr(addr: str) -> str:
        for tok, val in token_map.items():
            addr = addr.replace(tok, val)
        return addr

    for stage_key, api in (("stageA", api_a), ("stageB", api_b)):
        bad = []
        for semantic, s in manifest["stages"][stage_key]["slots"].items():
            addr = resolve_addr(s["address"])
            nid, _, wname = addr.partition(".")
            if nid not in api:
                bad.append(f"{semantic}: 节点 {nid} 不在图中（{s['address']} -> {addr}）")
                continue
            if wname and wname not in api[nid]["inputs"]:
                bad.append(f"{semantic}: 输入 `{wname}` 不存在于节点 {nid}（{s['address']} -> {addr}）")
        if bad:
            problems.extend(f"[{stage_key} 槽位漂移] {b}" for b in bad)
        else:
            print(f"  {stage_key} 槽位：{len(manifest['stages'][stage_key]['slots'])} 个地址全部解析成功")

    # 输出节点存在性
    for stage_key, api in (("stageA", api_a), ("stageB", api_b)):
        for sem, nid in manifest["stages"][stage_key]["outputNodeIds"].items():
            if str(nid) not in api:
                problems.append(f"[{stage_key}] 输出 {sem} 的节点 {nid} 不在图中")
        info.append(f"{stage_key} 输出节点 {list(manifest['stages'][stage_key]['outputNodeIds'].values())}")

    if problems:
        print("\n*** 发现 %d 个问题 ***" % len(problems))
        for p in problems:
            print(f"  - {p}")
        return 1

    print("\n检查全部通过，无问题。")

    if args.check_only:
        print("--check-only：未写盘。")
        return 0

    # ---------- 写盘 ----------
    cfg_dir = ROOT / "Config" / "Workflows"
    fa = cfg_dir / "stage_a_base_texture.json"
    fb = cfg_dir / "stage_b_pbr.json"
    fa.write_text(json.dumps(api_a, indent=2, ensure_ascii=False), encoding="utf-8")
    fb.write_text(json.dumps(api_b, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"已写出 {fa.name} ({fa.stat().st_size} B) / {fb.name} ({fb.stat().st_size} B)")

    # ---------- 回写 manifest 的指纹与运行时字段（不动手写的 slots） ----------
    src_bytes = wf.read_bytes()
    manifest["generatedAt"] = _dt.datetime.now().astimezone().isoformat(timespec="seconds")
    manifest["source"].update({
        "sha256": hashlib.sha256(src_bytes).hexdigest(),
        "bytes": len(src_bytes),
        "nodeCount": len(graph.nodes),
        "linkCount": len(graph.links),
    })
    manifest["stages"]["stageA"]["apiWorkflowFile"] = fa.name
    manifest["stages"]["stageB"]["apiWorkflowFile"] = fb.name
    manifest["stages"]["stageB"]["inputNodeId"] = new_input_id
    manifest["stages"]["stageB"]["inputNodeToken"] = "{inputNode}"
    manifest["stages"]["stageB"]["inputPlaceholder"] = INPUT_PLACEHOLDER
    manifest["stages"]["stageB"]["apiNodeCount"] = len(api_b)
    manifest["stages"]["stageA"]["apiNodeCount"] = len(api_a)
    mpath.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"已更新 {mpath.name} 的 source 指纹与 API 图字段（slots 未改动）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
