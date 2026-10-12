# ComfyTextureStudio

在 UE5 编辑器里直接"提示词 → 材质贴图 → 可渲染材质"的原生插件。

```
提示词 ──> ComfyUI 阶段A (SDXL + 沙地 LoRA) ──> 受光渲染图 (1024², 严格四方连续)
                                                      │
                                        在面板里审阅 + 2×2 平铺自检
                                                      │
                                        满意 ──> ComfyUI 阶段B (几何反演)
                                                      │
                            albedo / normal / height / orm   (+ ao / roughness 不接入材质)
                                                      │
                                      自动导入 UTexture2D + 生成 M_/MI_ 材质（含 BumpOffset 视差）
```

## 文档

| 文件 | 内容 |
|---|---|
| **[`docs/PRD.md`](docs/PRD.md)** | **主文档。** 需求、架构、接口契约、UE 侧实现设计、UI、里程碑、风险、已确认决策 |
| [`docs/workflow-interface.md`](docs/workflow-interface.md) | 工作流拆分契约、81 个槽位全表、裁剪清单、坑位速查 |
| [`docs/M0-baseline.md`](docs/M0-baseline.md) | **实测基线**（耗时 / 显存 / IO / 几何正确性）+ 测量方法论 |
| [`Config/Workflows/workflows.manifest.json`](Config/Workflows/workflows.manifest.json) | 语义槽位映射（机器可读），C++ 侧按语义键读写，**不硬编码节点 id** |
| **[`Resources/README.md`](Resources/README.md)** | **节点包纳管说明** · 依赖链 · 安装与同步流程 |

## 工具

| 脚本 | 用途 |
|---|---|
| `Tools/export_api_workflows.py` | 前端格式图 → 两张 API 图（含 live `/object_info` 校验 + manifest 指纹回写） |
| `Tools/derive_keepset.py` | 反向可达性推导 keep/drop 集合，`--diff --strict` 可作门禁 |
| `Tools/run_live_check.py` | M0 实跑器；**即 M1 的最小 REST 客户端参考实现**（6 个端点） |
| `Tools/check_normal_convention.py` | 几何自洽性 + 无缝 z 检验 + `directx` 开关语义（需 numpy + Pillow） |

## 目标环境

| 项 | 值 |
|---|---|
| 引擎 | UE **5.8.3**（主）/ 5.6.1（兼容） |
| ComfyUI | Desktop `v0.38.2` @ `http://127.0.0.1:8000`（**不是 8188**） |
| ComfyUI 用户数据目录 | `C:\Users\1142\AppData\Local\Programs\ComfyUI-data\` |
| 自定义节点包 | `comfyui_seamless_texture` **v1.3.0**（已纳管至 `Resources/`） |
| 节点包外部依赖 | `comfyui_controlnet_aux` · `dsine.pt` **291,298,581 B**（**不自动下载**） |
| GPU | RTX 5090 D v2 · 24 GiB VRAM |

---

## ⚠️ 依赖的 ComfyUI 节点包

本插件的阶段 B（几何反演）**完全依赖** `comfyui_seamless_texture`。该包**已随本仓库分发**：

```
Resources/comfyui_seamless_texture/          v1.3.0 · MIT · 18 节点 · 133 KB · pip 依赖 0
```

### 安装到 ComfyUI

```bash
# 1) 复制节点包
cp -r Resources/comfyui_seamless_texture \
      "C:/Users/1142/AppData/Local/Programs/ComfyUI-data/custom_nodes/"

# 2) 装它自己的依赖（SeamlessDSINENormal 需要）
#    a. comfyui_controlnet_aux —— 用 ComfyUI Manager 安装
#    b. dsine.pt 权重 —— 精确 291,298,581 B，不会被自动下载
curl -skL -m 900 \
  "https://hf-mirror.com/hr16/Diffusion-Edge/resolve/main/dsine.pt" \
  -o "C:/Users/1142/AppData/Local/Programs/ComfyUI-data/custom_nodes/comfyui_controlnet_aux/ckpts/hr16/Diffusion-Edge/dsine.pt"

# 3) 重启 ComfyUI
```

> ⚠️ **注意 `custom_nodes` 是哪个**：ComfyUI Desktop 的实际用户数据目录是
> `Programs\ComfyUI-data\custom_nodes\`。`comfy-cli` 报的 `default_workspace`
> （`Comfy-Desktop\ComfyUI-Installs\ComfyUI\ComfyUI`）下面的 `custom_nodes` **只有示例文件**，
> 复制到那里等于没装。
>
> 插件运行时的预检会提供"一键复制"按钮，并把目标目录写死成上面那个正确路径，
> 复制后**回读 `/object_info` 验证**节点真的出现了。

完整说明（含依赖链、字节数校验、上游同步流程）见 [`Resources/README.md`](Resources/README.md)。

---

## 三个核心设计决策

| # | 决策 | 结论 |
|---|---|---|
| **D1** | 两阶段如何拆分 | 拆成两张独立 API 图 —— 阶段 B 用 `LoadImage` 取代 `node 8`，并**同时**接到 `35`(DSINE) 与 `70`(Delight)。这两条边是唯一的耦合点 |
| **D2** | 是否保留 MoGe 对照支路 | **裁掉** —— 反向可达性证明它与交付输出零连接，省一次 ViT-L 推理 |
| **D3** | 法线绿通道在哪儿翻 | UE 侧 `bFlipGreenChannel=true`；ComfyUI 保持 OpenGL 输出（`50.directx=false`） |

拆分结果：阶段 A **9 节点** / 阶段 B **15 节点**（沿用原图 14 + 新增 `LoadImage` 1）/ 裁掉 **20 节点**。
自洽关系：`原图 43 = 阶段A 9 + 阶段B 沿用 14 + 裁掉 20`。

## 已确认的 8 项决策

| # | 问题 | 决策 |
|---|---|---|
| Q1 | 插件名 | `ComfyTextureStudio` |
| Q2 | 保留 `44`（normal_dsine 调试输出）？ | **不保留** |
| Q3 | ORM 合并 vs AO/Roughness 分开 | **合并图**（AO/Roughness 不导入、不接材质） |
| Q4 | 高度图用法 | **BumpOffset 视差** |
| Q5 | 是否支持排队 | **暂不支持**，单任务 |
| Q6 | 命名规则 | **模板 + 手填** 双通道（手填冲突报错，不静默覆盖） |
| Q7 | 预留换 LoRA 通道 | **预留**，LoRA 枚举从 `/object_info` 实时拉取 |
| Q8 | 节点包位置 | **放 `Resources/`** + README 记录 |

## 部署（M1 之后）

```ini
; 方式一：拷贝到插件目录
<UE项目>/Plugins/ComfyTextureStudio/

; 方式二：开发期在 .uproject 里挂外部目录（推荐，改代码不用来回拷）
"AdditionalPluginDirectories": [ "D:/PersonalDocuments/hongxuan_document/UE_tools" ]
```

## 当前状态

- [x] **M0-前置**：工作流结构实测解析（43 节点 / 46 link / 81 槽位）
- [x] **M0-前置**：拆分点定位 + 反向可达性 keep/drop 推导 + 槽位清单落盘
- [x] **M0-前置**：需求评审完成，8 项决策全部确认（PRD v1.1）
- [x] **M0-前置**：节点包纳管至 `Resources/`（字节级校验通过）+ 依赖链记录
- [x] **M0**：接口验证全绿 —— 两张 API 图导出并通过三层验证，端到端实跑成功
- [ ] **M1**：Runtime 模块 + 任务状态机
- [ ] **M2**：Slate 两阶段面板 + 导入 + 材质构建（含 BumpOffset）
- [ ] **M3**：WebSocket 进度 / 取消 / 导入报告
- [ ] **M4**：批量候选 / 参数预设 / Nanite 位移 / Upscale 接入

**M0 实测结论：端到端热 12.6 s / 冷 16.7 s**（阶段 A 7.3 s + 阶段 B 5.1 s），
`batch_size=8` 亦不 OOM。详见 [`docs/M0-baseline.md`](docs/M0-baseline.md)。

## 快速验证（不需要 ComfyUI 之外的东西）

```bash
PY=C:/Users/1142/.workbuddy/binaries/python/versions/3.14.3/python.exe

# 1) 结构门禁：keep/drop 必须与 manifest 一致
$PY Tools/derive_keepset.py --diff --strict

# 2) 重新导出两张 API 图（需要 ComfyUI 在跑，读 /object_info）
$PY Tools/export_api_workflows.py

# 3) 端到端实跑（冷/热各若干次 + 阶段 B）
$PY Tools/run_live_check.py --stage both --repeat 3
```

需求已全部关闭，无待决策项。下一步就是 **M1**。
