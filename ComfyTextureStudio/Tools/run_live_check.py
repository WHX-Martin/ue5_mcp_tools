#!/usr/bin/env python3
"""M0 实跑器：live 提交两张 API 图，采集耗时 / 显存 / 产物。

它同时也是 **UE 插件 M1 那几个 REST 端点的参考实现** —— 只用到 6 个端点：
    GET  /system_stats    探活 + 采显存
    POST /prompt          提交
    GET  /history/{id}    轮询
    GET  /view            下载产物
    POST /upload/image    阶段 B 的输入投喂
    POST /interrupt       取消（本脚本用不到，留给插件）

用法
----
    python Tools/run_live_check.py --stage a                 # T1：跑阶段 A
    python Tools/run_live_check.py --stage b --input <png>    # T2：跑阶段 B
    python Tools/run_live_check.py --stage both               # T1+T2 全链路
    python Tools/run_live_check.py --stage a --repeat 2       # 冷/热各一次

产物落在 `Saved/M0/<runId>/`，并写 `report.json`。

退出码：0 全部成功；1 有失败。
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import pathlib
import statistics
import sys
import threading
import time
import urllib.parse
import urllib.request
import uuid

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
CFG = ROOT / "Config" / "Workflows"
PLACEHOLDER = "__CTS_RUNTIME_INPUT__.png"

# 本机服务绝不能被系统代理劫持：环境里若设了 http_proxy（如 Clash 的 127.0.0.1:7852），
# urllib 会把发往 127.0.0.1:8000 的请求也塞给代理，拿回一个 502 Bad Gateway，
# 看起来像「ComfyUI 挂了」其实服务好好的。
_NO_PROXY_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


# --------------------------------------------------------------------------- #
# 最小 REST 客户端
# --------------------------------------------------------------------------- #
class ComfyClient:
    def __init__(self, base: str, poll: float = 1.0):
        self.base = base.rstrip("/")
        self.poll = poll

    # -- 基础 -------------------------------------------------------------- #
    def _req(self, url: str, data: bytes | None = None, headers: dict | None = None,
             method: str | None = None, timeout: float = 120):
        req = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
        with _NO_PROXY_OPENER.open(req, timeout=timeout) as r:
            return r.status, r.read()

    def _get_json(self, path: str, timeout: float = 120):
        s, b = self._req(self.base + path, timeout=timeout)
        return json.loads(b.decode("utf-8"))

    # -- 端点 -------------------------------------------------------------- #
    def system_stats(self) -> dict:
        return self._get_json("/system_stats", timeout=30)

    def vram(self) -> tuple[float, float]:
        d = self.system_stats()["devices"][0]
        return d["vram_free"], d["vram_total"]

    def queue(self) -> dict:
        return self._get_json("/queue", timeout=30)

    def submit(self, graph: dict) -> str:
        body = json.dumps({"prompt": graph, "client_id": str(uuid.uuid4())}).encode("utf-8")
        s, b = self._req(self.base + "/prompt", data=body,
                         headers={"Content-Type": "application/json"})
        if s != 200:
            raise RuntimeError(f"POST /prompt -> {s}: {b[:400]!r}")
        j = json.loads(b.decode("utf-8"))
        if j.get("node_errors"):
            raise RuntimeError(f"提交被拒，node_errors={json.dumps(j['node_errors'], ensure_ascii=False)[:600]}")
        return j["prompt_id"]

    def history(self, pid: str) -> dict:
        j = self._get_json(f"/history/{pid}")
        return j.get(pid) if isinstance(j, dict) else None

    def interrupt(self) -> None:
        try:
            self._req(self.base + "/interrupt", data=b"{}",
                      headers={"Content-Type": "application/json"}, method="POST")
        except Exception as e:      # noqa: BLE001
            print(f"  [warn] interrupt 失败: {e}")

    def download(self, filename: str, subfolder: str, ftype: str, dest: pathlib.Path) -> int:
        q = urllib.parse.urlencode({"filename": filename, "subfolder": subfolder, "type": ftype})
        s, b = self._req(f"{self.base}/view?{q}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(b)
        return len(b)

    def upload(self, path: pathlib.Path, name: str | None = None) -> str:
        """POST /upload/image（multipart）。返回可填进 LoadImage 的名字。"""
        name = name or path.name
        bnd = "----CTS" + uuid.uuid4().hex
        parts: list[bytes] = []
        parts.append(
            f'--{bnd}\r\nContent-Disposition: form-data; name="image"; '
            f'filename="{name}"\r\nContent-Type: image/png\r\n\r\n'.encode("utf-8")
        )
        parts.append(path.read_bytes())
        parts.append(b"\r\n")
        for k, v in (("type", "input"), ("overwrite", "true")):
            parts.append(
                f'--{bnd}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode("utf-8")
            )
        parts.append(f"--{bnd}--\r\n".encode("utf-8"))
        body = b"".join(parts)
        s, b = self._req(self.base + "/upload/image", data=body,
                         headers={"Content-Type": f"multipart/form-data; boundary={bnd}"}, timeout=300)
        if s != 200:
            raise RuntimeError(f"upload -> {s}: {b[:300]!r}")
        j = json.loads(b.decode("utf-8"))
        nm = j.get("name") or name
        sub = j.get("subfolder") or ""
        return f"{sub}/{nm}" if sub else nm

    # -- 跑一张图 ---------------------------------------------------------- #
    def run(self, graph: dict, label: str) -> dict:
        print(f"\n--- {label} ---")
        free0, total = self.vram()
        print(f"  提交前 VRAM free {free0/2**30:.2f} / {total/2**30:.2f} GiB")

        # 显存采样必须独立于 /history 轮询：轮询间隔 1s 会漏掉峰值。
        # 后台线程 0.2s 采一次，取整段运行的最小 free。
        samples: list[float] = []
        stop = threading.Event()

        def _sampler():
            while not stop.is_set():
                try:
                    f, _ = self.vram()
                    samples.append(f)
                except Exception:      # noqa: BLE001
                    pass
                stop.wait(0.2)

        th = threading.Thread(target=_sampler, daemon=True)
        th.start()

        t0 = time.perf_counter()
        pid = self.submit(graph)
        t_submit = time.perf_counter() - t0
        print(f"  prompt_id = {pid}  （提交耗时 {t_submit*1000:.0f} ms）")

        last = None
        while True:
            time.sleep(self.poll)
            h = self.history(pid)
            if h and h.get("status", {}).get("completed"):
                last = h
                break
            if h and h.get("status", {}).get("status_str") == "error":
                last = h
                break
        elapsed = time.perf_counter() - t0

        stop.set()
        th.join(timeout=3)

        min_free = min(samples) if samples else free0
        # 增量 = 本次任务真正多占的显存（关键指标）
        inc = max(0.0, free0 - min_free) / 2**30
        # 常驻 = 采样时刻的总占用（含 ComfyUI 仍缓存的其它模型，如 SDXL）
        resident = (total - min_free) / 2**30

        st = (last or {}).get("status", {})
        ok = st.get("status_str") == "success"
        print(f"  状态 = {st.get('status_str')}   耗时 {elapsed:.1f}s")
        print(f"  VRAM: 增量峰值 {inc:.2f} GiB（free0 {free0/2**30:.2f} -> min {min_free/2**30:.2f}）"
              f"  常驻 {resident:.2f} GiB  [采样 {len(samples)} 次]")

        images: list[dict] = []
        for nid, out in (last or {}).get("outputs", {}).items():
            for im in out.get("images", []) or []:
                images.append({"node": nid, **im})
        print(f"  产物 {len(images)} 个")

        res = {
            "label": label, "prompt_id": pid, "ok": ok,
            "status_str": st.get("status_str"),
            "elapsed_s": round(elapsed, 2),
            "submit_ms": round(t_submit * 1000, 1),
            "vram_free_before_gib": round(free0 / 2**30, 2),
            "vram_min_free_gib": round(min_free / 2**30, 2),
            "vram_incremental_gib": round(inc, 2),
            "vram_resident_gib": round(resident, 2),
            "vram_samples": len(samples),
            "images": images,
        }
        if not ok:
            msgs = [m for m in st.get("messages", []) if m and m[0] in ("execution_error",)]
            res["error"] = json.dumps(msgs, ensure_ascii=False)[:1500]
            print(f"  *** 失败: {res['error'][:400]}")
        return res


# --------------------------------------------------------------------------- #
def load_graph(path: pathlib.Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def override_prefixes(graph: dict, prefix_base: str) -> dict:
    """把所有 SaveImage 的 filename_prefix 指到本次运行的子目录下，便于收集产物。"""
    out = json.loads(json.dumps(graph))
    for nid, e in out.items():
        if e["class_type"] == "SaveImage":
            tail = str(e["inputs"].get("filename_prefix", "img")).rsplit("/", 1)[-1]
            e["inputs"]["filename_prefix"] = f"{prefix_base}/{tail}"
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="M0 live 实跑")
    ap.add_argument("--stage", choices=["a", "b", "both"], default="a")
    ap.add_argument("--input", default=None, help="阶段 B 的输入 PNG；'both' 时自动用阶段 A 产物")
    ap.add_argument("--server", default="http://127.0.0.1:8000")
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--repeat", type=int, default=1, help="阶段 A 重复次数（区分冷/热启动）")
    ap.add_argument("--batch-size", type=int, default=None,
                    help="覆盖阶段 A 的 5.batch_size（用于 OOM 边界测试）")
    ap.add_argument("--poll", type=float, default=1.0)
    args = ap.parse_args(argv)

    run_id = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = pathlib.Path(args.out_dir) if args.out_dir else ROOT / "Saved" / "M0" / run_id
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"运行目录 : {out_dir}")

    c = ComfyClient(args.server, poll=args.poll)
    stats = c.system_stats()
    d = stats["devices"][0]
    print(f"ComfyUI  : {args.server}  VRAM {d['vram_total']/2**30:.2f} GiB  "
          f"free {d['vram_free']/2**30:.2f} GiB")
    q = c.queue()
    running = len(q.get("queue_running", []))
    pending = len(q.get("queue_pending", []))
    print(f"队列     : running={running} pending={pending}"
          + ("   ⚠ 已有任务在跑，耗时测量会失真" if running or pending else ""))

    results: list[dict] = []
    stage_a_out: pathlib.Path | None = None

    # ---------------- 阶段 A ----------------
    if args.stage in ("a", "both"):
        ga = load_graph(CFG / "stage_a_base_texture.json")
        if args.batch_size:
            for e in ga.values():
                if e["class_type"] == "EmptyLatentImage":
                    e["inputs"]["batch_size"] = args.batch_size
            print(f"[覆盖] 阶段 A batch_size = {args.batch_size}")
        for i in range(args.repeat):
            g = override_prefixes(ga, f"cts_m0/{run_id}/stage_a")
            # 第二次及以后换 seed，避免命中缓存造成耗时偏乐观
            if i > 0:
                for e in g.values():
                    if e["class_type"] == "KSampler":
                        e["inputs"]["seed"] = int(e["inputs"]["seed"]) + i
            r = c.run(g, f"阶段A run{i+1}{'（冷）' if i == 0 else '（热）'}")
            results.append(r)
            if r["ok"]:
                for im in r["images"]:
                    dest = out_dir / "stage_a" / im["filename"]
                    n = c.download(im["filename"], im.get("subfolder", ""), im.get("type", "output"), dest)
                    print(f"      ↓ {dest.name} ({n/1024:.0f} KB)")
                    im["local"] = str(dest)
                    if stage_a_out is None and im["filename"].endswith(".png"):
                        stage_a_out = dest

    # ---------------- 阶段 B ----------------
    if args.stage in ("b", "both"):
        src = pathlib.Path(args.input) if args.input else stage_a_out
        if src is None or not src.is_file():
            print("\n*** 阶段 B 缺少输入图，请用 --input 指定，或先跑阶段 A ***")
            return 1
        print(f"\n阶段 B 输入: {src.name} ({src.stat().st_size/1024:.0f} KB)")
        gb_tpl = load_graph(CFG / "stage_b_pbr.json")

        names = {"height": "41", "normal": "54", "ao": "63", "roughness": "64",
                 "orm": "65", "albedo": "71"}
        for i in range(args.repeat):
            # ⚠ ComfyUI 有**节点级结果缓存**：图完全不变时重提交会直接命中缓存，
            # 测出来的不是计算耗时（实测会从 5s 掉到 1s）。所以每次重跑都换一个
            # 输入文件名，让 LoadImage 的输入变化，从而失效整条下游链。
            t0 = time.perf_counter()
            uploaded = c.upload(src, name=f"cts_{run_id}_r{i+1}.png")
            up_ms = (time.perf_counter() - t0) * 1000

            gb = json.loads(json.dumps(gb_tpl))
            n_rep = 0
            for e in gb.values():
                if e["class_type"] == "LoadImage" and e["inputs"].get("image") == PLACEHOLDER:
                    e["inputs"]["image"] = uploaded
                    n_rep += 1
            if n_rep != 1:
                print(f"*** LoadImage 占位符替换了 {n_rep} 次（应为 1）")
                return 1
            gb = override_prefixes(gb, f"cts_m0/{run_id}/stage_b")

            if i == 0:
                print(f"  上传为 input/{uploaded}   耗时 {up_ms:.0f} ms")
            r = c.run(gb, f"阶段B run{i+1}{'（冷）' if i == 0 else '（热）'}")
            r["upload_ms"] = round(up_ms, 1)
            r["input_name"] = uploaded
            results.append(r)
            if r["ok"]:
                got = {}
                for im in r["images"]:
                    dest = out_dir / "stage_b" / f"run{i+1}_{im['filename']}"
                    c.download(im["filename"], im.get("subfolder", ""), im.get("type", "output"), dest)
                    im["local"] = str(dest)
                    got[im["node"]] = dest.name
                missing = [f"{k}({v})" for k, v in names.items() if v not in got]
                print(f"      ↓ {len(got)} 个产物；交付齐备: {'是' if not missing else '否，缺 ' + str(missing)}")
                r["deliverables"] = got
                r["missing"] = missing

    # ---------------- 报告 ----------------
    report = {"runId": run_id, "server": args.server, "outDir": str(out_dir), "results": results}
    (out_dir / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")

    print("\n" + "=" * 66)
    print("汇总")
    print("=" * 66)
    for r in results:
        print(f"  {r['label']:<20} {'OK ' if r['ok'] else 'FAIL'}  {r['elapsed_s']:>7.1f}s  "
              f"VRAM增量 {r['vram_incremental_gib']:>5.2f} GiB  产物 {len(r['images'])}")
    ok_a = [r["elapsed_s"] for r in results if r["ok"] and r["label"].startswith("阶段A")]
    if len(ok_a) > 1:
        warm = ok_a[1:]
        print(f"  阶段A 冷启 {ok_a[0]:.1f}s   热跑中位 {statistics.median(warm):.1f}s  （{warm}）")
    print(f"报告: {out_dir / 'report.json'}")
    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
