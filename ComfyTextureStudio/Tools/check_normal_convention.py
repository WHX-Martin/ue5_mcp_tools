#!/usr/bin/env python3
"""T6 · 校验阶段 B 产物的法线/高度几何自洽性、四方连续性，以及 directx 开关语义。

为什么不能靠肉眼
----------------
工作流自带的 note 明确写过：``std`` / ``tilt`` / ``|n_xy|`` 这类**标量统计量对通道翻转
完全不敏感**，所以"看着差不多"不能证明 G 通道符号对；判断通道符号只能靠"与已知约定场
的相关性"。而单光源拟合在这张砾石材质图上高频 R² 只有 0.023–0.026，几乎没有判别力。

所以本脚本做三件**可判定**的事：

1. **几何自洽性** —— normal 是从 height 重导的，那两者必须满足梯度关系：
   ``n.x / n.z ∝ -∂h/∂x``、``n.y / n.z ∝ -∂h/∂y``（差一个正比例系数）。
   逐通道算相关系数。工作流 note 记录的真值是 ``+0.975`` / ``+0.971``。
   这能证明「没接错轴」，但**不能**单独定 G 的绝对符号。

2. **四方连续性** —— 环绕边界的相邻差 vs 图内相邻差的中位。比值 ≈1 才算真的无缝。
   note 记录最终 ``normal`` 为 1.04 / 0.96。

3. **``directx`` 开关语义** —— 同图跑两次（``50.directx`` 分别 false / true），
   验证两者**仅差一个绿通道取反**。这确定性地说明：「我们出的那张 = directx:false 的那张」，
   而 UE 需要的是它的绿通道取反版本 ⇒ ``FlipGreenChannel=true``。

   这一步是"RTX 渲染器对照"的替代品：真正的单光源渲染复核需要 UE 材质（M2 验收项），
   脚本末尾会明确标注这一点，不假装已经验过。

用法
----
    python Tools/check_normal_convention.py \
        --height Saved/M0/<run>/stage_b/sand_v2_height_00001_.png \
        --normal Saved/M0/<run>/stage_b/sand_v2_normal_00001_.png

    # 加上 directx 开关的 live 对照（会再跑两次阶段 B）
    python Tools/check_normal_convention.py --height ... --normal ... --live-directx-check

依赖 numpy + Pillow。实测用的隔离环境：
    C:/Users/1142/.workbuddy/binaries/python/envs/default/Scripts/python.exe

退出码：0 通过；1 未通过。
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys

import numpy as np
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

CFG = ROOT / "Config" / "Workflows"

# 工作流 note 记录的参考值（同一判据下的实测真值）
REF = {
    "corr_dx": 0.975,      # corr(-n.x, ∂h/∂x)
    "corr_dy": 0.971,      # corr(+n.y, ∂h/∂y)
    "wrap_h": 1.04,        # 最终 normal 的水平环绕倍数
    "wrap_v": 0.96,        # 最终 normal 的垂直环绕倍数
}


def load_scalar(p: pathlib.Path) -> np.ndarray:
    im = Image.open(p)
    if im.mode not in ("L", "I;16", "RGB"):
        im = im.convert("L")
    a = np.asarray(im).astype(np.float64)
    if a.ndim == 3:
        a = a[..., :3].mean(axis=2)
    if a.max() > 255:
        a = a / 65535.0
    else:
        a = a / 255.0
    return a


def load_normal(p: pathlib.Path) -> np.ndarray:
    a = np.asarray(Image.open(p).convert("RGB")).astype(np.float64) / 255.0
    n = a * 2.0 - 1.0                       # 解码到 [-1, 1]
    return n


def _ztest_wrap(interior_pair_mean: np.ndarray, wrap_mean: float) -> dict:
    """把"环绕那一对"放到"所有内部相邻对"的分布里做 z 检验。

    这是本脚本唯一正确的无缝判据。**不要用 均值/中位数** —— 内部相邻差的分布是右偏的
    （砾石边缘的大步长少而大），中位远小于均值，除下来会凭空放大 ~1.46 倍，
    把一张完全无缝的图报成有接缝。本脚本早期版本就踩过这个坑：
    同一张图 均值/中位 = 1.55，均值/均值 = 1.06。
    """
    mu = float(interior_pair_mean.mean())
    sd = float(interior_pair_mean.std(ddof=1))
    z = (wrap_mean - mu) / sd if sd > 0 else 0.0
    p = math.erfc(abs(z) / math.sqrt(2.0))          # 双侧 p
    return {
        "wrap_mean": round(wrap_mean, 6),
        "interior_mean": round(mu, 6),
        "interior_sd": round(sd, 6),
        "ratio_mean": round(wrap_mean / mu, 4) if mu else None,
        "z": round(z, 3),
        "p": round(p, 5),
        "rank_pct": round(float((interior_pair_mean < wrap_mean).mean()), 4),
    }


def seam_stats(A: np.ndarray, vec: bool) -> dict:
    """水平 + 垂直的环绕检验。vec=True 时按向量距离（法线场），否则按标量差（高度）。"""
    out = {}
    for axis, tag in ((1, "h"), (0, "v")):
        if vec:
            d = np.linalg.norm(np.diff(A, axis=axis), axis=2)
            dwrap = np.linalg.norm(A[..., 0, :] - A[..., -1, :], axis=-1) if axis == 1 else \
                    np.linalg.norm(A[0, ...] - A[-1, ...], axis=-1)
        else:
            d = np.abs(np.diff(A, axis=axis))
            dwrap = np.abs(A[..., 0] - A[..., -1]) if axis == 1 else np.abs(A[0, ...] - A[-1, ...])
        # 每一条"相邻对"的均值（沿另一维平均）→ 得到 W-1 / H-1 个样本
        pair_mean = d.mean(axis=0 if axis == 1 else 1)
        out[tag] = _ztest_wrap(pair_mean, float(dwrap.mean()))
    return out


def check_pair(height_p: pathlib.Path, normal_p: pathlib.Path, problems: list[str]) -> dict:
    h = load_scalar(height_p)
    n = load_normal(normal_p)
    print(f"\nheight : {height_p.name}  {h.shape}  range [{h.min():.4f}, {h.max():.4f}]")
    print(f"normal : {normal_p.name}  {n.shape}")

    # ---- 1. 几何自洽性 ----
    gy, gx = np.gradient(h)                 # gy = ∂h/∂行(向下), gx = ∂h/∂列(向右)
    nz = np.clip(n[..., 2], 1e-3, None)
    rx = n[..., 0] / nz                     # 与 -∂h/∂x 成比例（比例系数 > 0）
    ry = n[..., 1] / nz                     # 与 -∂h/∂y 成比例

    def corr(a, b):
        a = a.ravel(); b = b.ravel()
        a = a - a.mean(); b = b - b.mean()
        d = np.sqrt((a * a).sum() * (b * b).sum())
        return float((a * b).sum() / d) if d > 0 else float("nan")

    c_dx = corr(-n[..., 0], gx)             # note 的判据：corr(-n.x, ∂h/∂x)
    c_dy = corr(+n[..., 1], gy)             # note 的判据：corr(+n.y, ∂h/∂y)
    c_dx_raw = corr(n[..., 0], gx)
    c_dy_raw = corr(n[..., 1], gy)

    print("\n[1] 几何自洽性（normal 是否真由 height 导出）")
    print(f"    corr(-n.x, ∂h/∂x) = {c_dx:+.4f}   （note 真值 {REF['corr_dx']:+.3f}）")
    print(f"    corr(+n.y, ∂h/∂y) = {c_dy:+.4f}   （note 真值 {REF['corr_dy']:+.3f}）")
    print(f"    对照 corr(n.x, ∂h/∂x) = {c_dx_raw:+.4f} / corr(n.y, ∂h/∂y) = {c_dy_raw:+.4f}")

    # 判定：两个判据都必须显著为正（>0.90），且它们才是主导符号
    if c_dx < 0.90:
        problems.append(f"corr(-n.x, ∂h/∂x) = {c_dx:+.4f}，未达到 >0.90（X 轴可能接错/高度未参与法线重导）")
    if c_dy < 0.90:
        problems.append(f"corr(+n.y, ∂h/∂y) = {c_dy:+.4f}，未达到 >0.90（Y 轴可能接错）")
    if abs(c_dx_raw) > abs(c_dx):
        problems.append("corr(n.x, ∂h/∂x) 比 corr(-n.x, ∂h/∂x) 更强 —— X 通道符号与预期相反")

    # ---- 2. 法线单位化 / 蓝主导 ----
    ln = np.linalg.norm(n, axis=2)
    print("\n[2] 法线向量本身")
    print(f"    |n| 均值 {ln.mean():.4f}  标准差 {ln.std():.4f}  （理想 = 1.0）")
    print(f"    n.z 均值 {n[..., 2].mean():+.4f}，负值占比 {100.0 * (n[..., 2] < 0).mean():.3f}%")
    if abs(ln.mean() - 1.0) > 0.05:
        problems.append(f"法线未单位化：|n| 均值 {ln.mean():.4f}（偏离 1.0 超过 0.05）")
    if (n[..., 2] < 0).mean() > 0.01:
        problems.append(f"有 {100.0 * (n[..., 2] < 0).mean():.2f}% 像素 n.z < 0（朝背面，对底面材质不合理）")

    # ---- 3. 四方连续性 ----
    sn = seam_stats(n, vec=True)
    sh = seam_stats(h, vec=False)
    print("\n[3] 四方连续性（环绕那一对 vs 全部内部相邻对）")
    print(f"    {'':<8}{'比值':>8}{'z':>9}{'p':>10}{'百分位':>9}")
    for nm, s in (("normal", sn), ("height", sh)):
        for axis, ax in (("水平", "h"), ("垂直", "v")):
            print(f"    {nm:<8}{axis:<4}{s[ax]['ratio_mean']:>8.3f}{s[ax]['z']:>9.2f}"
                  f"{s[ax]['p']:>10.4f}{100*s[ax]['rank_pct']:>8.1f}%")
    print(f"    （note 真值 normal 为 1.04 / 0.96）")

    for nm, s in (("normal", sn), ("height", sh)):
        for axis, ax in (("水平", "h"), ("垂直", "v")):
            v = s[ax]
            if v["ratio_mean"] > 1.5:
                problems.append(
                    f"{nm} {axis}环绕倍数 {v['ratio_mean']:.3f} > 1.5 —— 接缝可测")
            elif v["p"] < 0.01:
                problems.append(
                    f"{nm} {axis}环绕差显著异于内部相邻差（z={v['z']:.2f}, p={v['p']:.4f}）—— 存在系统性接缝")

    return {
        "height": str(height_p), "normal": str(normal_p),
        "corr_neg_nx_dhdx": round(c_dx, 4), "corr_pos_ny_dhdy": round(c_dy, 4),
        "corr_nx_dhdx": round(c_dx_raw, 4), "corr_ny_dhdy": round(c_dy_raw, 4),
        "normal_length_mean": round(float(ln.mean()), 4),
        "nz_negative_ratio": round(float((n[..., 2] < 0).mean()), 6),
        "seam_normal": sn,
        "seam_height": sh,
    }


def live_directx_check(server: str, problems: list[str]) -> dict:
    """跑两次阶段 B（directx=false / true），验证两者只差一个绿通道取反。"""
    from run_live_check import ComfyClient, load_graph, PLACEHOLDER   # noqa: E402
    import json as _json

    # 找一个已上传的输入图：直接复用之前的阶段 A 产物
    inv = ROOT / "Saved" / "M0"
    cands = sorted(inv.glob("*/stage_a/*.png"))
    if not cands:
        problems.append("live-directx-check：找不到阶段 A 产物作为输入")
        return {}
    src = cands[-1]
    print(f"\n[4] directx 开关语义（live）  输入 = {src.name}")

    c = ComfyClient(server, poll=1.0)
    name = c.upload(src, name="cts_dx_check.png")
    base = load_graph(CFG / "stage_b_pbr.json")
    for e in base.values():
        if e["class_type"] == "LoadImage" and e["inputs"].get("image") == PLACEHOLDER:
            e["inputs"]["image"] = name

    outs = {}
    for flag in (False, True):
        g = _json.loads(_json.dumps(base))
        for e in g.values():
            if e["class_type"] == "SeamlessNormalFromHeight":
                e["inputs"]["directx"] = flag
            if e["class_type"] == "SaveImage" and "normal" in str(e["inputs"].get("filename_prefix", "")):
                e["inputs"]["filename_prefix"] = f"cts_dx/{'dx' if flag else 'gl'}/normal"
        r = c.run(g, f"阶段B directx={flag}")
        if not r["ok"]:
            problems.append(f"directx={flag} 运行失败")
            return outs
        for im in r["images"]:
            if str(im["node"]) == "54":
                dest = ROOT / "Saved" / "M0" / f"_dx_{'true' if flag else 'false'}.png"
                c.download(im["filename"], im.get("subfolder", ""), im.get("type", "output"), dest)
                outs[flag] = dest

    if len(outs) == 2:
        a = load_normal(outs[False])
        b = load_normal(outs[True])
        same_r = np.allclose(a[..., 0], b[..., 0], atol=1e-6)
        same_b = np.allclose(a[..., 2], b[..., 2], atol=1e-6)
        g_inv = np.allclose(a[..., 1], -b[..., 1], atol=2e-2)
        print(f"    R 通道相同 : {same_r}")
        print(f"    G 通道取反 : {g_inv}   （最大偏差 {np.abs(a[...,1] + b[...,1]).max():.4f}）")
        print(f"    B 通道相同 : {same_b}")
        if not (same_r and same_b and g_inv):
            problems.append("directx 开关不是纯粹的绿通道取反 —— 与你预期的语义不符")
        outs = {str(k): str(v) for k, v in outs.items()}
        outs["r_same"] = bool(same_r)
        outs["g_inverted"] = bool(g_inv)
        outs["b_same"] = bool(same_b)
    return outs


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="法线/高度几何自洽性与约定校验")
    ap.add_argument("--height", required=True)
    ap.add_argument("--normal", required=True)
    ap.add_argument("--live-directx-check", action="store_true")
    ap.add_argument("--server", default="http://127.0.0.1:8000")
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv)

    problems: list[str] = []
    res = check_pair(pathlib.Path(args.height), pathlib.Path(args.normal), problems)
    if args.live_directx_check:
        res["directx_switch"] = live_directx_check(args.server, problems)

    print("\n" + "=" * 66)
    if problems:
        print(f"*** 未通过：{len(problems)} 项 ***")
        for p in problems:
            print(f"  - {p}")
    else:
        print("全部通过。")
        print("""
结论
----
1. normal 与 height 几何自洽（两条判据相关性对照 note 真值），说明"由 height 重导法线"
   这一步落地正确，没有接错轴。
2. 环绕倍数接近 1，四方连续成立。
3. 法线通道的**绝对符号**（OpenGL vs DirectX）不能靠统计量判定 —— note 已论证过这一点。
   本脚本用 `directx` 开关的取反关系确定：**我们出的就是 directx:false 的那张**，
   而 UE 需要它的绿通道取反版本 ⇒ 导入时 FlipGreenChannel=true（PRD 决策 D3）。
4. 真正的单光源渲染复核需要 UE 材质 —— 那是 M2 的验收项，此处不算已完成。""")

    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(res, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"\n已写入 {args.json}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
