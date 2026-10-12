# 工作流接口契约 — sdxl_sand_pbr_v2_geom

> 本文件是 `docs/PRD.md` §4 的展开实现依据。所有节点 id、连线、参数默认值、枚举值均来自对
> `D:\PersonalDocuments\hongxuan_document\comfyUI_tools\workflows\sdxl_sand_pbr_v2_geom.json`
> （73,211 B · 2026-10-12 15:22 · 43 节点 / 46 link / 5 group）的实测解析，
> 以及 `comfy workflow slots`（返回 81 个槽位）与 `comfy node show` 的实时查询。

---

## 1. 源图结构总览

| 项 | 值 |
|---|---|
| 文件版本 | `version: 0.4`, `revision: 0`, `frontendVersion: 1.53.6` |
| 节点 / 连线 / 分组 | 43 / 46 / 5 |
| `extra.ds.scale` | `0.136602691073015` |
| 节点类型去重 | 22 种（`SaveImage` ×15、`TilePreview` ×5、`CLIPTextEncode` ×2、`MoGeRender` ×2、`SeamlessFlattenNormal` ×2，其余各 1） |
| 自定义节点包 | **`comfyui_seamless_texture`** v1.3.0（MIT · 18 节点，本图用到其中 12 个；裁剪后剩 10 个） |
| 节点包纳管位置 | `Resources/comfyui_seamless_texture/`（已随仓库分发，安装说明见 `Resources/README.md`） |
| 节点包外部依赖 | `comfyui_controlnet_aux`（必需）· `dsine.pt` 291,298,581 B（必需，**不自动下载**）· Pillow（可选） |
| 目标 ComfyUI | Desktop `v0.38.2` (commit `daeb5e53`)，`http://127.0.0.1:8000` |

原图分组：

| 分组 | bounding | 含义 |
|---|---|---|
| `A · 生成受光渲染图（环面 padding）` | `[-2710, 1020, 1580, 906]` | 阶段 A 主体 |
| `B · 几何反演（MoGe → height → normal/AO/rough/ORM）` | `[360, 1020, 1907.6, 2547.0]` | 阶段 B 主体 |
| `C · 去光照 albedo` | `[372.2, 3290, 547.3, 212.0]` | 去光照 |
| `2x2自检` | `[2633.5, 3343.4, 1104.8, 1327.6]` | 平铺自检（全部裁掉） |
| `输出` | `[2629.1, 1012.7, 1102.4, 2270.3]` | 交付产物 |

---

## 2. 拆分点（唯一的耦合边）

```
8:SeamlessVAEDecode.out0
   ├──> 20:SaveImage                [阶段A 出口]
   ├──> 21:TilePreview              [裁]
   ├──> 31:SeamlessMoGeInference    [裁 · 对照支路]
   ├──> 35:SeamlessDSINENormal      ★ 阶段B 入口 1
   └──> 70:SeamlessDelightAlbedo    ★ 阶段B 入口 2
```

**拆分动作**：删掉 `8`（及其上游 `1..7`），插入 `LoadImage`，把它的 `IMAGE`(out0) 分接到 `35.in0` 与 `70.in0`。

> 这两条边是**全部**耦合。漏掉任何一条，对应的产物都会失去输入而报错——这是拆分最容易出错的地方。

---

## 3. 阶段 A 图（9 节点）

| id | class_type | 关键输入（widget） |
|---|---|---|
| 1 | `CheckpointLoaderSimple` | `ckpt_name` |
| 2 | `LoraLoader` | `lora_name`, `strength_model`, `strength_clip`（+ `model`←1.out0, `clip`←1.out1） |
| 3 | `CLIPTextEncode` | `text`（positive）, `clip`←2.out1 |
| 4 | `CLIPTextEncode` | `text`（negative）, `clip`←2.out1 |
| 5 | `EmptyLatentImage` | `width`, `height`, `batch_size` |
| 6 | `SeamlessModelPadding` | `mode`, `model`←2.out0 |
| 7 | `KSampler` | `seed`, `steps`, `cfg`, `sampler_name`, `scheduler`, `denoise`；`model`←6.out0, `positive`←3.out0, `negative`←4.out0, `latent_image`←5.out0 |
| 8 | `SeamlessVAEDecode` | `margin`；`samples`←7.out0, `vae`←1.out2 |
| 20 | `SaveImage` | `filename_prefix`；`images`←8.out0 |

**输出**：1 张 1024×1024 PNG（严格四方连续的受光渲染图）

---

## 4. 阶段 B 图（15 节点）

| id | class_type | 关键输入（widget） | 上游 |
|---|---|---|---|
| `LOAD` | `LoadImage` | `image` = `cts_<GUID>.png`, `upload` = `image` | — |
| 35 | `SeamlessDSINENormal` | `fov`, `pad`, `iterations`, `flip_red`, `use_fp16` | `LOAD.out0` |
| 36 | `SeamlessFlattenNormal` | `relief_scale`, `flatten` | `35.out0` |
| 40 | `SeamlessHeightFromNormal` | `low_freq_radius`, `invert`, `headroom` | `36.out0` |
| 50 | `SeamlessNormalFromHeight` | `bump_strength`, `directx` | `40.out0` |
| 60 | `SeamlessAOFromHeight` | `radius`, `scales`, `strength`, `gamma`, `smooth` | `40.out0` |
| 61 | `SeamlessRoughnessFromHeight` | `window`, `min_roughness`, `max_roughness`, `gamma`, `invert` | `40.out0` |
| 62 | `PackPBRChannels` | `metalness_value` | `60.out0` → **in0**, `61.out0` → **in1** |
| 70 | `SeamlessDelightAlbedo` | `radius`, `amount`, `preserve_mean` | `LOAD.out0` |
| 41 | `SaveImage` | `filename_prefix` | `40.out0` → height |
| 54 | `SaveImage` | `filename_prefix` | `50.out0` → normal |
| 63 | `SaveImage` | `filename_prefix` | `60.out0` → ao |
| 64 | `SaveImage` | `filename_prefix` | `61.out0` → roughness |
| 65 | `SaveImage` | `filename_prefix` | `62.out0` → orm |
| 71 | `SaveImage` | `filename_prefix` | `70.out0` → albedo |

⚠️ **`PackPBRChannels` 的输入顺序：`in0 = AO`，`in1 = Roughness`。** 与"ORM = R:AO / G:Rough / B:Metal"的常见命名顺序**不一致**，接反会导致 ORM 的红绿互换。

---

## 5. 裁剪清单（20 节点 · 反向可达性推导）

```
10  MarkdownNote                 UI-only
21  TilePreview                  UE 侧本地拼接 2×2 替代
30  LoadMoGeModel                ┐
31  SeamlessMoGeInference        │
32  MoGeRender (normal_opengl)   │ MoGe 对照支路
33  MoGeRender (depth)           │ 与 6 个交付输出零连接
34  SaveImage (moge_depth)       │
37  SaveImage (normal_moge_raw)  │
38  SeamlessFlattenNormal        │
39  SaveImage (normal_moge)      ┘
42  TilePreview      43 SaveImage (height_tilecheck)
44  SaveImage (normal_dsine)     ← 决策 Q2：不保留调试输出
55  TilePreview      56 SaveImage (normal_tilecheck)
66  TilePreview      67 SaveImage (orm_tilecheck)
72  TilePreview      73 SaveImage (albedo_tilecheck)
74  PreviewImage     2×2 自检 · 渲染图
```

**推导方式**（`Tools/derive_keepset.py`，T5 固化）：

```
阶段A keep = 反向可达( {20} )                                    → {1,2,3,4,5,6,7,8,20}
阶段B keep = (反向可达( {41,54,63,64,65,71} ) \ {1..8}) ∪ {LOAD}
           → {LOAD,35,36,40,50,60,61,62,70,41,54,63,64,65,71}
裁掉      = 全图节点 \ (A keep ∪ B keep)                          → 20 个
```

> **约束**：任何对源图的改动（新增/删除节点、改接线）后，必须**重跑该推导**并 diff 结果，不允许手写 id 列表。
> 未通过 §7 三层验证的图不得进入 `Config/Workflows/`。

**数目自洽**：`原图 43 = 阶段A 9 + 阶段B 沿用 14 + 裁掉 20`。
阶段 B **图文件**是 15 节点 = 沿用 14 + **新增 1 个 `LoadImage`**（它不在原图 43 个里）。两式都成立，别混用。

---

## 6. 槽位全表（81 个，按节点分组）

> 地址格式 `<node_id>.<widget_name>`，与 `comfy workflow slots` 的 `address` 一致。
> `⚙` 标记 = PRD 面板需要暴露的参数；`—` = 裁掉后不存在。

### 阶段 A

| 地址 | 类型 | 默认值 | 面板 |
|---|---|---|---|
| `1.ckpt_name` | COMBO | `sd_xl_base_1.0.safetensors` | ⚙ |
| `2.lora_name` | COMBO | `sdxl-texture-sand-lora-v2.safetensors` | ⚙ |
| `2.strength_model` | FLOAT | `1` | ⚙ |
| `2.strength_clip` | FLOAT | `1` | ⚙ |
| `3.text` | STRING | 见下 | ⚙ |
| `4.text` | STRING | 见下 | ⚙（折叠） |
| `5.width` / `5.height` | INT | `1024` / `1024` | ⚙ |
| `5.batch_size` | INT | `1` | P2 |
| `6.mode` | COMBO | `circular` | 只读（勿改） |
| `7.seed` | INT | `20261013` | ⚙ |
| `7.steps` | INT | `30` | ⚙ |
| `7.cfg` | FLOAT | `6.5` | ⚙ |
| `7.sampler_name` | COMBO(46 项) | `dpmpp_2m` | ⚙ |
| `7.scheduler` | COMBO(9 项) | `karras` | ⚙ |
| `7.denoise` | FLOAT | `1` | 只读 |
| `8.margin` | INT | `8` | 高级 |
| `20.filename_prefix` | STRING | `textures/pbr_v2/sand_v2_lit` | 自动 |

**`1.ckpt_name` 枚举**：`sam3.1_multiplex_fp16.safetensors` / `sd_xl_base_1.0.safetensors` / `v1-5-pruned-emaonly-fp16.safetensors`

**`2.lora_name` 枚举**（2026-10-12 实测值）：`sdxl-texture-sand-lora-v1-000004.safetensors` / `sdxl-texture-sand-lora-v2-000010.safetensors` / `sdxl-texture-sand-lora-v2-000013.safetensors` / `sdxl-texture-sand-lora-v2.safetensors`

> **Q7**：这个下拉**不硬编码**，运行时从 `GET /object_info` 的 `LoraLoader.input.required.lora_name` 实时拉取 ⇒ 换任意 LoRA（不只沙地）都能直接选到。
> ⚠️ 换 LoRA 时 `3.text` 里那两段固定 tag（`mtrlx, seamless tileable PBR material, ` 前缀 与 `, square top-down surface, ...` 后缀）是**跟着 LoRA 训练集走的**，需一并替换，否则新 LoRA 不生效。

**`3.text` 默认值**（9 个绑死 tag + 材质描述，**前后固定段不可改**）：
```
mtrlx, seamless tileable PBR material, coarse pebbly sand, beige and grey small stones,
dense gravel aggregate, square top-down surface, directional sunlight from upper left,
visible bump relief with soft cast shadows, raking light, crisp fine detail,
high frequency texture, small scale pattern
```

**`4.text` 默认值**：
```
blurry, lowres, out of focus, jpeg artifacts, watermark, text, signature, logo,
perspective view, side view, overcast, flat lighting, front lighting, lighting gradient,
vignette, depth of field, bokeh, flat solid color, cartoon, painting, tiling seam,
visible grid, tile border, depth map, normal map, displacement map, uv checker
```
> 后半段（`tiling seam` 起的 5 项）是压制接缝类伪影的关键，**不要删**。

**`7.sampler_name` 枚举（46 项，节选常用）**：`euler`, `euler_ancestral`, `dpmpp_2m`, `dpmpp_2m_sde`, `dpmpp_3m_sde`, `ddim`, `uni_pc`, `res_multistep` …
**`7.scheduler` 枚举（9 项）**：`simple`, `sgm_uniform`, `karras`, `exponential`, `ddim_uniform`, `beta`, `normal`, `linear_quadratic`, `kl_optimal`

### 阶段 B

| 地址 | 类型 | 默认值 | 取值范围 | 面板 |
|---|---|---|---|---|
| `LOAD.image` | STRING | `cts_<GUID>.png` | — | 自动 |
| `35.fov` | FLOAT | `3` | `0.5 – 120`, step `0.5` | ⚙（标注"勿随意调大"） |
| `35.pad` | INT | `256` | `0 – 1024`, step `32` | ⚙ |
| `35.iterations` | INT | `5` | `1 – 20` | 高级 |
| `35.flip_red` | BOOLEAN | `true` | — | 高级（勿改） |
| `35.use_fp16` | BOOLEAN | `false` | — | 高级 |
| `36.relief_scale` | FLOAT | `1` | — | ⚙ |
| `36.flatten` | BOOLEAN | `true` | — | ⚙ |
| `40.low_freq_radius` | INT | `96` | — | ⚙ |
| `40.invert` | BOOLEAN | `false` | — | ⚙ |
| `40.headroom` | FLOAT | `0.15` | — | ⚙（标注"勿设 0"） |
| `50.bump_strength` | FLOAT | `31.4` | — | ⚙ |
| `50.directx` | BOOLEAN | `false` | — | 高级（保持 false，交给 UE 翻） |
| `60.radius` | INT | `24` | — | ⚙ |
| `60.scales` | INT | `4` | — | ⚙ |
| `60.strength` | FLOAT | `1` | — | ⚙ |
| `60.gamma` | FLOAT | `1` | — | 高级 |
| `60.smooth` | INT | `2` | — | 高级 |
| `61.window` | INT | `12` | — | ⚙ |
| `61.min_roughness` | FLOAT | `0.55` | — | ⚙ |
| `61.max_roughness` | FLOAT | `0.95` | — | ⚙ |
| `61.gamma` | FLOAT | `1` | — | 高级 |
| `61.invert` | BOOLEAN | `false` | — | ⚙ |
| `62.metalness_value` | FLOAT | `0` | — | ⚙（砂石=0，电介质） |
| `70.radius` | INT | `128` | — | ⚙ |
| `70.amount` | FLOAT | `1` | — | ⚙ |
| `70.preserve_mean` | BOOLEAN | `true` | — | ⚙ |
| `41/54/63/64/65/71.filename_prefix` | STRING | 见下 | — | 自动 |

**输出前缀默认值**（运行时替换为 `cts/<任务GUID>/<name>` 形式）：

| 地址 | 默认值 | 产物 |
|---|---|---|
| `41.filename_prefix` | `textures/pbr_v2/sand_v2_height` | height |
| `54.filename_prefix` | `textures/pbr_v2/sand_v2_normal` | normal ★ |
| `63.filename_prefix` | `textures/pbr_v2/sand_v2_ao` | ao（默认不导入，见决策 Q3） |
| `64.filename_prefix` | `textures/pbr_v2/sand_v2_roughness` | roughness（默认不导入，见决策 Q3） |
| `65.filename_prefix` | `textures/pbr_v2/sand_v2_orm` | orm ★ |
| `71.filename_prefix` | `textures/pbr_v2/sand_v2_albedo` | albedo ★ |

> 6 个交付产物：`albedo` / `normal` / `height` / `orm`（+ `ao` / `roughness`，决策 Q3 定为**不接进材质**，仅按需导入）。

> `SaveImage` 实际落盘文件名会在 prefix 后追加 `_00001_` 之类的序号。**必须从 `/history` 的 `outputs` 读取真实 `filename`/`subfolder`，不要自己拼。**

### 被裁节点的槽位（保留备查，插件不加载）

| 地址 | 默认值 |
|---|---|
| `31.pad` | `256` |
| `31.circular` | `true` |
| `31.resolution_level` | `9` ← MoGe 真分辨率旋钮（0–9） |
| `31.num_tokens` | `0` |
| `31.fov_x_degrees` | `0` |
| `31.batch_size` | `1` |
| `31.force_projection` | `true` |
| `31.apply_mask` | `false` |
| `30.model_name` | `moge_2_vitl_normal_fp16.safetensors` |
| `32.output` | `normal_opengl` |
| `33.output` | `depth` |
| `38.relief_scale` / `38.flatten` | `1` / `true` |
| `21/42/55/66/72.tiles_x` / `.tiles_y` | `2` / `2` |

`MoGeRender.output` 枚举：`depth`, `depth_colored`, `normal_opengl`, `normal_directx`, `mask`
> `normal_directx` 是 ComfyUI 侧翻绿通道的开关。既然选了 D3（在 UE 侧翻），这条留着以备将来改主意。

---

## 7. 进 `Config/Workflows/` 前的三层验证

对齐既有硬标准，**全绿才允许入库**。**2026-10-12 实测结果见"实测"列。**

| 层 | 动作 | 通过标准 | 实测 |
|---|---|---|---|
| **结构** | `Tools/derive_keepset.py --diff --strict` 重算 keep/drop 并与 manifest 声明 diff | 差集为空 | ✅ 阶段A / 阶段B / 裁剪集三集合一致，退出码 0 |
| **schema** | `comfy validate --workflow <path>` | `valid: true`，`errors` 为空 | ✅ 两张图 `valid=true`，0 error / 0 warning |
| **live** | `POST /prompt`，轮询 `/history/{id}` | `status_str == "success"`，`outputs` 含全部预期节点 | ✅ 阶段A 出 1 张；阶段B 出 6/6 |

**产出**：`Config/Workflows/stage_a_base_texture.json`（9 节点）、`stage_b_pbr.json`（15 节点，
含动态 id 的 `LoadImage`）。

> ⚠️ **`comfy validate` 的盲区已实测确认**：阶段 B 带着不存在的占位输入
> `__CTS_RUNTIME_INPUT__.png` 依然报 `valid=true`（它不检查 COMBO 的文件存在性）。
> 其他已知盲区：不检查缺失的 required 输入、不查 `COMFY_DYNAMICCOMBO_V3` 子输入、不估算显存分配。
> ⇒ **schema 层不能替代 live 层**；插件的提交前预检必须自己确认输入文件已上传。

---

## 8. manifest schema

`Config/Workflows/workflows.manifest.json` 的字段定义：

| 字段 | 类型 | 说明 |
|---|---|---|
| `schemaVersion` | int | 本 manifest 格式版本，当前 `1` |
| `generatedAt` | string | ISO 8601 |
| `sourceWorkflow` | string | 源前端格式图的路径（便于追溯） |
| `sourceSha256` | string | 源图哈希，用于检测源图被改动 |
| `comfyuiVersion` | string | 导出时的 ComfyUI 版本（如 `0.38.2`） |
| `comfyCliVersion` | string | `1.22.0` |
| `requiredCustomNodePacks` | string[] | `["comfyui_seamless_texture"]` |
| `stages` | object | `stageA` / `stageB` 两键 |
| `stages.<s>.workflowFile` | string | API 图相对路径 |
| `stages.<s>.outputNodeIds` | object | 语义产物名 → SaveImage 节点 id |
| `stages.<s>.slots` | object | 语义键 → `{address, type, default, enum?, ui:{visible,group,label,advanced}}` |

`ui` 子对象驱动面板自动生成，避免"参数改了 UI 忘了改"的漂移。

---

## 9. 已知坑位速查

| # | 坑 | 后果 | 规避 |
|---|---|---|---|
| 1 | `8→35` / `8→70` 拆分遗漏 | 阶段 B 报缺输入 | 本文 §2 |
| 2 | `PackPBRChannels` in0=AO / in1=Roughness 接反 | ORM 红绿互换 | 本文 §4 |
| 3 | 自作聪明拼 `SaveImage` 文件名 | 下载 404 | 从 `/history` 读 `outputs` |
| 4 | 删掉负向提示里的 `tiling seam` 等 5 项 | 接缝伪影回来 | 本文 §6 |
| 5 | 改 `6.mode` 从 `circular` 移开 | 失去四方连续，整个链路前提崩塌 | UI 设为只读 |
| 6 | 反相理解法线约定 | 凹凸方向反 | UE 侧 `FlipGreenChannel=true` |
| 7 | 静态文本里的 `node id` 硬编码进 C++ | 源图一改就崩 | 走 manifest 语义键 |
| 8 | widget 顺序靠猜 | 参数错位、静默出错 | 只用 `/object_info` 的 `input_order` |
| 9 | 假设 ComfyUI 是 8188 端口 | 连不上 | 本机实测为 **8000** |
| 10 | 把 OOM 崩溃当超时处理 | 引导用户加超时而非降配置 | 区分"连接中断"与"节点错误" |

---

## 10. 已确认决策（PRD v1.1）

原 PRD §12.2 的 8 个待决项已全部确认，**不再有未决项**：

| # | 问题 | 决策 | 对本文件的影响 |
|---|---|---|---|
| Q1 | 插件名 | `ComfyTextureStudio` 定案 | — |
| Q2 | 是否保留 `44`（normal_dsine 调试输出） | **不保留** | §4 阶段 B 由 16 → **15 节点**；§5 裁剪集由 19 → **20 节点** |
| Q3 | ORM 合并 vs AO/Roughness 分开 | **合并图** | §6 产物表：`ao` / `roughness` 标为默认不导入、不接材质 |
| Q4 | 高度图在材质里的用法 | **BumpOffset 视差** | 本文件不受影响（UE 侧） |
| Q5 | 是否支持排队 | **暂不支持**，单任务 | — |
| Q6 | 命名规则 | **模板 + 手填** 两种都提供 | 本文件不受影响（UE 侧） |
| Q7 | 是否预留换 LoRA 通道 | **预留** | §6 阶段 A：`2.lora_name` 与两个强度保留，枚举从 `/object_info` 实时拉取 |
| Q8 | 节点包位置 | **放 `Resources/`**，README 记录 | §1 已更新；新增 `Resources/README.md` |
