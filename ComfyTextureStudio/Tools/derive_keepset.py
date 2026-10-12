#!/usr/bin/env python3
"""反向可达性推导 keep/drop 集合 —— 禁止手写节点 id 列表。

原理
----
ComfyUI 前端格式（nodes/links/groups）改成 API 格式、或把它拆成多张图时，
"要保留哪些节点"必须由**输出节点的反向可达集**推导，而不是人肉列举。
一旦源图增删节点或改接线，手写的 id 列表就会静默过期。

本工具以 manifest 里声明的输出节点为种子，逐段推导：
    阶段A keep   = 反向可达(stageA.outputNodeIds)
    阶段B keep   = 反向可达(stageB.outputNodeIds) \\ 阶段A keep
    裁剪集       = 全图节点 \\ (阶段A keep ∪ 阶段B keep)

（阶段 B 图文件另含 1 个**新增**的 LoadImage 用于替换拆分点，该节点不在源图内，
  所以不计入"裁剪"。）

用法
----
    python Tools/derive_keepset.py                      # 打印推导结果
    python Tools/derive_keepset.py --diff                # 与 manifest 声明比对
    python Tools/derive_keepset.py --diff --strict       # 不一致则退出码 1（进 CI / 门禁）
    python Tools/derive_keepset.py --json out.json       # 落盘

退出码：0 = 一致；1 = 不一致或出错。
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULT_MANIFEST = ROOT / "Config" / "Workflows" / "workflows.manifest.json"
DEFAULT_SOURCE = pathlib.Path(
    "D:/PersonalDocuments/hongxuan_document/comfyUI_tools/workflows/sdxl_sand_pbr_v2_geom.json"
)


# --------------------------------------------------------------------------- #
# 图工具
# --------------------------------------------------------------------------- #
class FrontendGraph:
    """前端格式工作流的最小读取接口。"""

    def __init__(self, path: pathlib.Path):
        self.path = pathlib.Path(path)
        raw = json.loads(self.path.read_text(encoding="utf-8"))
        self.raw = raw
        self.nodes: list[dict] = raw.get("nodes", [])
        self.links: list[list] = raw.get("links", [])
        self.by_id: dict[int, dict] = {n["id"]: n for n in self.nodes}

        # upstream 邻接：target -> [sources]
        self._up: dict[int, list[int]] = {}
        # link_id -> (src_node, src_slot)
        self.link_by_id: dict[int, tuple[int, int]] = {}
        for l in self.links:
            link_id, src, src_slot, dst = l[0], l[1], l[2], l[3]
            self._up.setdefault(dst, []).append(src)
            self.link_by_id[link_id] = (src, src_slot)

    # -- 反向可达 ---------------------------------------------------------- #
    def rev_reach(self, seeds) -> set[int]:
        """seeds 及其全部上游祖先。"""
        seen: set[int] = set(seeds)
        stack = list(seeds)
        while stack:
            cur = stack.pop()
            for src in self._up.get(cur, []):
                if src not in seen:
                    seen.add(src)
                    stack.append(src)
        return seen

    def resolve_input(self, node: dict, input_name: str):
        """取某输入的上游端点，返回 (src_node, src_slot) 或 None。"""
        for inp in node.get("inputs", []) or []:
            if inp.get("name") == input_name:
                lid = inp.get("link")
                if lid is None:
                    return None
                return self.link_by_id.get(lid)
        return None

    def types_of(self, ids) -> dict[int, str]:
        return {i: self.by_id[i]["type"] for i in ids if i in self.by_id}

    def describe(self, ids) -> list[str]:
        return [f"{i}:{self.by_id[i]['type']}" for i in sorted(ids) if i in self.by_id]


# --------------------------------------------------------------------------- #
# 推导
# --------------------------------------------------------------------------- #
def derive(graph: FrontendGraph, manifest: dict) -> dict:
    stages = manifest["stages"]
    seeds_a = set(stages["stageA"]["outputNodeIds"].values())
    seeds_b = set(stages["stageB"]["outputNodeIds"].values())

    keep_a = graph.rev_reach(seeds_a)
    # 阶段 B 沿用源图的节点 = 其反向可达集去掉阶段 A 的全部节点
    # （现图中阶段 B 通过 node 8 与阶段 A 耦合，拆分时用 LoadImage 顶替，故此处必须剔除）
    keep_b_orig = graph.rev_reach(seeds_b) - keep_a

    all_ids = set(graph.by_id)
    pruned = all_ids - keep_a - keep_b_orig

    return {
        "stageA_keep": sorted(keep_a),
        "stageB_keep_orig": sorted(keep_b_orig),
        "pruned": sorted(pruned),
        "arith": {
            "total": len(all_ids),
            "stageA": len(keep_a),
            "stageB_orig": len(keep_b_orig),
            "pruned": len(pruned),
            "identity": f"{len(all_ids)} = {len(keep_a)} + {len(keep_b_orig)} + {len(pruned)}",
            "ok": len(all_ids) == len(keep_a) + len(keep_b_orig) + len(pruned),
            # 阶段 B **图文件**的节点数 = 沿用 + 1（新增 LoadImage），该节点不在源图 43 个内
            "stageB_file_nodes": len(keep_b_orig) + 1,
        },
    }


def compare(derived: dict, manifest: dict) -> list[str]:
    """返回不一致项（空 = 全绿）。"""
    problems: list[str] = []
    stages = manifest["stages"]

    decl_a = set(stages["stageA"]["keptNodeIds"])
    if decl_a != set(derived["stageA_keep"]):
        problems.append(
            f"stageA.keptNodeIds 不一致\n"
            f"    推导: {sorted(decl_a ^ set(derived['stageA_keep']))} 对不上\n"
            f"    推导={sorted(derived['stageA_keep'])}\n"
            f"    声明={sorted(decl_a)}"
        )

    decl_b = {x for x in stages["stageB"]["keptNodeIds"] if isinstance(x, int)}
    if decl_b != set(derived["stageB_keep_orig"]):
        problems.append(
            f"stageB.keptNodeIds（去掉新增节点后）不一致\n"
            f"    差异={sorted(decl_b ^ set(derived['stageB_keep_orig']))}\n"
            f"    推导={sorted(derived['stageB_keep_orig'])}\n"
            f"    声明={sorted(decl_b)}"
        )

    decl_p = set(manifest["prunedNodes"]["ids"])
    if decl_p != set(derived["pruned"]):
        problems.append(
            f"prunedNodes.ids 不一致\n"
            f"    差异={sorted(decl_p ^ set(derived['pruned']))}\n"
            f"    推导={sorted(derived['pruned'])}\n"
            f"    声明={sorted(decl_p)}"
        )

    # 节点数与图文件节点数
    if manifest["stages"]["stageA"]["nodeCount"] != len(derived["stageA_keep"]):
        problems.append(
            f"stageA.nodeCount 声明 {manifest['stages']['stageA']['nodeCount']} "
            f"≠ 推导 {len(derived['stageA_keep'])}"
        )
    if manifest["stages"]["stageB"]["nodeCount"] != derived["arith"]["stageB_file_nodes"]:
        problems.append(
            f"stageB.nodeCount 声明 {manifest['stages']['stageB']['nodeCount']} "
            f"≠ 推导 {derived['arith']['stageB_file_nodes']}"
        )
    if len(manifest["prunedNodes"]["ids"]) != len(derived["pruned"]):
        problems.append(
            f"prunedNodes 条数声明 {len(manifest['prunedNodes']['ids'])} "
            f"≠ 推导 {len(derived['pruned'])}"
        )
    if not derived["arith"]["ok"]:
        problems.append(f"数目不自洽：{derived['arith']['identity']}")
    return problems


# --------------------------------------------------------------------------- #
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="反向可达性推导 keep/drop 集合")
    ap.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    ap.add_argument("--workflow", default=None, help="前端格式源图；默认取 manifest.source 路径映射")
    ap.add_argument("--diff", action="store_true", help="与 manifest 声明比对")
    ap.add_argument("--strict", action="store_true", help="不一致时退出码 1")
    ap.add_argument("--json", default=None, help="把推导结果写到该文件")
    args = ap.parse_args(argv)

    mpath = pathlib.Path(args.manifest)
    manifest = json.loads(mpath.read_text(encoding="utf-8"))

    wf = pathlib.Path(args.workflow) if args.workflow else DEFAULT_SOURCE
    if not wf.is_file():
        print(f"[ERR] 源图不存在: {wf}", file=sys.stderr)
        return 1

    graph = FrontendGraph(wf)
    derived = derive(graph, manifest)

    print(f"源图      : {wf}")
    print(f"总节点    : {derived['arith']['total']}")
    print(f"阶段A keep: {len(derived['stageA_keep'])}  {graph.describe(derived['stageA_keep'])}")
    print(f"阶段B keep: {len(derived['stageB_keep_orig'])} （沿用源图；图文件另 +1 个新增 LoadImage"
          f" = {derived['arith']['stageB_file_nodes']}）")
    print(f"           {graph.describe(derived['stageB_keep_orig'])}")
    print(f"裁剪掉    : {len(derived['pruned'])}  {graph.describe(derived['pruned'])}")
    print(f"数目自洽  : {derived['arith']['identity']}  ->  "
          f"{'OK' if derived['arith']['ok'] else '*** FAIL ***'}")

    rc = 0
    if args.diff:
        problems = compare(derived, manifest)
        print()
        if problems:
            print("与 manifest 声明比对：*** 不一致 ***")
            for p in problems:
                print(f"  - {p}")
            rc = 1
        else:
            print("与 manifest 声明比对：全绿（阶段A / 阶段B / 裁剪集 三集合一致）")

    if args.json:
        out = pathlib.Path(args.json)
        out.write_text(json.dumps(derived, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"\n已写入 {out}")

    return rc if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
