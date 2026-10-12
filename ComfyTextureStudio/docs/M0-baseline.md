# M0 基线测量

> 全部为**本机实测**，不是估计。采集日期 2026-10-12，脚本 `Tools/run_live_check.py`。
> 这些数字是 M1+ 的性能验收基准，也供插件的 OOM 预检与超时设置使用。

## 环境

| 项 | 值 |
|---|---|
| GPU | NVIDIA RTX 5090 D v2 · **VRAM 23.88 GiB** |
| ComfyUI | Desktop v0.38.2 (commit `daeb5e53`) @ `http://127.0.0.1:8000` |
| 出图尺寸 | 1024 × 1024 |
| 采样 | 30 steps · CFG 6.5 · `dpmpp_2m` / `karras` |
| 队列状态 | 测量时空队列（running=0 / pending=0） |

---

## 1. 阶段 A（生成基础贴图）

`stage_a_base_texture.json` · 9 节点 · SDXL base 1.0 + 沙地 LoRA v2

| 情形 | 耗时 | VRAM 增量 | VRAM 常驻 |
|---|---|---|---|
| **冷启**（模型未加载） | **11.4 s** | 7.00 GiB | 9.04 GiB |
| **热跑**（SDXL 已驻留） | **7.3 s** 中位（7.11 / 7.43） | 0.18 – 0.22 GiB | 8.70 – 8.74 GiB |

> 冷热差 ≈ 4.1s，即 **SDXL 权重加载一次性成本约 4s + 7.0 GiB**。

## 2. 阶段 B（生成 PBR 全套，裁剪版无 MoGe）

`stage_b_pbr.json` · 15 节点 · LoadImage → DSINE → 泊松积分 → normal/AO/rough/ORM + 去光照

| 情形 | 耗时 | VRAM 增量 |
|---|---|---|
| 互异输入 ×3 | 4.57 / 5.10 / 5.07 → **中位 5.07 s** | 0.29 – 0.45 GiB |

> 「互异输入」= 每次用**内容不同**的阶段 A 产物，确保整条链真正重算（见 §5 坑 1）。

## 3. MoGe 对照支路（量化决策 D2 的收益）

仅 `LoadImage → 30:LoadMoGeModel → 31:SeamlessMoGeInference → 32:MoGeRender → 37:SaveImage`

| 情形 | 耗时 | VRAM 增量 |
|---|---|---|
| 首次（含 MoGe 权重加载） | **7.1 s** | 1.44 GiB |
| 互异输入 ×3（模型已加载） | 1.24 / 1.03 / 1.01 → **中位 1.03 s** | 0.17 – 0.74 GiB |

**⇒ D2（裁掉 MoGe 支路）的真实收益：**

| 项 | 收益 |
|---|---|
| 稳态每轮耗时 | **省 ≈ 1.03 s**（相对阶段 B 的 5.07s，约 **20%**） |
| 一次性 | 省 MoGe 权重加载 ≈ **6 s** |
| 显存 | 省 ≈ **1.4 GiB** |
| 依赖 | 去掉 `moge_2_vitl_normal_fp16.safetensors` |
| 与本流程的相关性 | **零** —— 该支路是"对照产物"，与 6 个交付输出无任何连接 |

> ⚠️ 早先曾把 D2 的收益说成"省一次 ViT-L 推理"就显得很大，实测只有 ~1s/轮。
> **收益是真实但温和的**，主要价值在于去掉依赖、简化图，而不在提速。

## 4. 批量与显存边界（阶段 A）

| `batch_size` | 耗时 | VRAM 增量 | OOM |
|---|---|---|---|
| 1（热） | 7.3 s | 0.22 GiB | 否 |
| **4** | **19.2 s** | 9.66 GiB（冷启口径） | **否** |
| **8** | **37.5 s** | 6.51 GiB（SDXL 常驻口径） | **否** |

- 单张摊薄耗时：batch1 7.3s → batch4 **4.8 s/张** → batch8 **4.7 s/张**，批处理效率良好。
- **在 23.88 GiB 上 batch 8 仍不 OOM**，峰值常驻 14.84 GiB。⇒ 插件的 OOM 预检阈值可以放宽，
  但仍应在提交前查 `/system_stats` 的 `vram_free`（SDXL 冷加载需 ~7 GiB 余量）。

## 5. IO 与端到端

| 项 | 实测 |
|---|---|
| `POST /prompt` 往返 | 3 – 33 ms |
| `POST /upload/image`（2.0 MB PNG） | 46 – 92 ms |
| `GET /history/{id}` 轮询间隔 | 1.0 s（实测够用） |
| `GET /view` 下载 | 6 张共 ≈ 6 MB，非瓶颈 |
| **上传 + 下载总开销** | **< 0.2 s**，相对计算可忽略 |

**端到端（单材质，单张基础贴图 → 全套 PBR）**

| 情形 | 计算 | IO | 合计 |
|---|---|---|---|
| 热（模型均已驻留） | 7.3 + 5.1 = 12.4 s | ~0.2 s | **≈ 12.6 s** |
| 冷（ComfyUI 刚启动） | 11.4 + 5.1 = 16.5 s | ~0.2 s | **≈ 16.7 s** |

⇒ **用户点"生成"到看到全套贴图，量级是十几秒**，不是分钟级。这决定了 UI 不需要长任务式的复杂进度条，
1.0s 轮询 + 简单进度指示即可。

## 6. 测量方法论（踩过的坑，务必沿用）

| # | 坑 | 表现 | 正确做法 |
|---|---|---|---|
| 1 | **ComfyUI 有节点级结果缓存** | 图不变时重提交直接命中，阶段 B 实测 **5.1s → 1.0s**，会把耗时测成 1/5 | 每次测量用**内容不同**的输入图（或改一个上游参数） |
| 2 | VRAM 指标口径混淆 | `常驻 = total - min_free` 把 ComfyUI 缓存的 SDXL 也算进去，阶段 B 报成 8.19 GiB（实际增量 0.4 GiB） | 用 **增量 = `free_before` − `min_free`** |
| 3 | 显存采样挂在 `/history` 轮询上 | 1.0s 采样在 5s 任务里只取到 5 个点，漏峰值 | 独立后台线程 ≥ 5 Hz 采样 |
| 4 | 无缝判据用「均值 ÷ 中位数」 | 内部相邻差右偏，凭空放大 ~1.46×，把无缝图报成有接缝（1.06 → 报 1.55） | 同口径**均值 ÷ 均值** + z 检验（见 `Tools/check_normal_convention.py`） |

## 7. 几何正确性验证（T6 结论）

对阶段 B 产物 `sand_v2_height_00001_.png` / `sand_v2_normal_00001_.png`：

| 判据 | 实测 | note 真值 | 判定 |
|---|---|---|---|
| `corr(-n.x, ∂h/∂x)` | **+0.9583** | +0.975 | ✅ 量级吻合 |
| `corr(+n.y, ∂h/∂y)` | **+0.9581** | +0.971 | ✅ 量级吻合 |
| `\|n\|` 均值 | **0.9963**（σ 0.0030） | — | ✅ 单位化 |
| `n.z` 负值占比 | **0.000%** | — | ✅ 全朝前 |
| normal 水平环绕倍数 | **1.058**（z=0.48, p=0.63） | 1.04 | ✅ 不显著，无接缝 |
| normal 垂直环绕倍数 | **1.016**（z=0.12, p=0.90） | 0.96 | ✅ 不显著，无接缝 |

**`directx` 开关语义（live 对照）**：同图跑 `50.directx` = false / true 两次 ——

| 通道 | 结果 |
|---|---|
| R | **相同** |
| G | **取反**（最大偏差 0.0078 = 8-bit 舍入） |
| B | **相同** |

⇒ 确定性结论：**我们出的那张就是 `directx:false`（OpenGL）的那张**；UE 需要它的绿通道取反版本，
故导入时 `bFlipGreenChannel = true`（PRD 决策 D3 成立）。

> ❗ **仍未完成**：真正的单光源渲染复核需要 UE 材质 —— 那是 **M2 的验收项**。
> 法线通道的**绝对**符号无法靠统计量判定（工作流 note 已论证：`std`/`tilt`/`|n_xy|` 对翻转不敏感），
> 本节的结论是"我们出的是哪一张" + "两张之间差什么"，不是"哪一张物理上正确"。

## 8. 复现方式

```bash
PY=C:/Users/1142/.workbuddy/binaries/python/versions/3.14.3/python.exe

# 冷/热 阶段A + 阶段B（各 3 次）
$PY Tools/run_live_check.py --stage both --repeat 3

# 批量边界
$PY Tools/run_live_check.py --stage a --repeat 1 --batch-size 8

# 几何正确性（需 numpy + Pillow）
C:/Users/1142/.workbuddy/binaries/python/envs/default/Scripts/python.exe \
  Tools/check_normal_convention.py \
  --height Saved/M0/<run>/stage_b/run1_sand_v2_height_00001_.png \
  --normal Saved/M0/<run>/stage_b/run1_sand_v2_normal_00001_.png \
  --live-directx-check
```

原始记录：`Saved/M0/<runId>/report.json`（每次运行的 prompt_id、耗时、显存、产物清单）。
