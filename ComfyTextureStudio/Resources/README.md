# Resources

随插件分发的**外部资源**。目前只有一项：ComfyUI 自定义节点包 `comfyui_seamless_texture`。

---

## 1. `comfyui_seamless_texture/`

本插件依赖的 ComfyUI 自定义节点包。**源码随仓库分发**，避免"换了台机器发现节点全缺"。

| 项 | 值 |
|---|---|
| 版本 | **v1.3.0** |
| 许可 | MIT |
| pip 依赖 | **无**（`dependencies = []`） |
| 节点数 | 18 |
| 源码体积 | 133 KB（8 个文件） |
| 源位置 | `D:\PersonalDocuments\hongxuan_document\comfyUI_tools\custom_nodes\comfyui_seamless_texture` |
| 纳管时间 | 2026-10-12 |

### 1.1 文件

| 文件 | 内容 |
|---|---|
| `__init__.py` | `SeamlessTextureExtension`，注册 18 个节点 |
| `nodes.py` | `SeamlessModelPadding` / `SeamlessVAEDecode` / `SeamlessVAEEncode` / `TilePreview` |
| `geometry.py` | `SeamlessDSINENormal` / `SeamlessFlattenNormal` / `SeamlessHeightFromNormal` / `SeamlessDelightAlbedo` / `SeamlessRoughnessFromHeight` / `SeamlessMoGeInference` |
| `pbr.py` | `PackPBRChannels` / `SeamlessAOFromHeight` / `SeamlessNormalFromHeight` / `SeamlessHeightFromAlbedo` / `SeamlessRoughnessFromAlbedo` |
| `materials.py` | `SeamlessMaterialsFromAlbedo`（StableMaterials 路线，本流程**不使用**） |
| `upscale.py` | `SeamlessResize` / `SeamlessUpscale` |
| `README.md` | 包自带的方法论与实测记录 |
| `pyproject.toml` | 版本与元数据 |

本流程实际用到其中 **10 个**：

```
SeamlessModelPadding · SeamlessVAEDecode
SeamlessDSINENormal · SeamlessFlattenNormal · SeamlessHeightFromNormal
SeamlessNormalFromHeight · SeamlessAOFromHeight · SeamlessRoughnessFromHeight
SeamlessDelightAlbedo · PackPBRChannels
```

### 1.2 安装到 ComfyUI

把 `comfyui_seamless_texture/` 整个目录放到 ComfyUI 的 `custom_nodes/` 下，然后重启 ComfyUI。

本机实测的 ComfyUI 用户数据目录（ComfyUI Desktop）：

```
C:\Users\1142\AppData\Local\Programs\ComfyUI-data\custom_nodes\
```

> ⚠️ **不是** `Comfy-Desktop\ComfyUI-Installs\ComfyUI\ComfyUI\custom_nodes\`——Desktop 版把 `custom_nodes` / `models` / `input` / `output` 放在 `ComfyUI-data` 下，前者只有示例文件。
>
> 本机原本是软链接指向 `comfyUI_tools` 项目；插件分发时用**实体拷贝**，两者互不影响。

```bash
# 复制到 custom_nodes（本机路径示例）
cp -r Resources/comfyui_seamless_texture \
      "C:/Users/1142/AppData/Local/Programs/ComfyUI-data/custom_nodes/"
```

---

## 2. 节点包的**外部**依赖（不在本仓库内）

`comfyui_seamless_texture` 自身零 pip 依赖，但 `SeamlessDSINENormal` 需要额外的模型与代码：

| 依赖 | 必需性 | 位置 / 获取方式 |
|---|---|---|
| **`comfyui_controlnet_aux`** 节点包 | **必需** | 提供 `custom_controlnet_aux.dsine` 模块（`geometry.py:348` 从其导入 `load_checkpoint` 与 `DSINE` 架构）。从 ComfyUI Manager 安装 |
| **`dsine.pt`** 权重 | **必需** | 精确 **291,298,581 B**（≈277.8 MiB）。默认查找 `<custom_nodes>/comfyui_controlnet_aux/ckpts/hr16/Diffusion-Edge/dsine.pt` |
| `Pillow` | 可选 | 仅 `materials.py` 的 `SeamlessMaterialsFromAlbedo` 用（本流程不使用该节点） |
| MoGe-2 权重 | **不需要** | `moge_2_vitl_normal_fp16.safetensors`。MoGe 对照支路已被裁掉（PRD D2），本流程不再引用 |

`dsine.pt` 的查找顺序（`geometry.py::_dsine_weight_path`）：

```
1. 环境变量 SEAMLESS_DSINE_WEIGHTS
2. <custom_nodes>/comfyui_controlnet_aux/ckpts/hr16/Diffusion-Edge/dsine.pt
3. folder_paths 的 dsine / geometry_estimation / checkpoints 三个目录
```

**该包不会自动下载权重**（源码注释原文：*"Locate dsine.pt without downloading anything behind the user's back"*），且带截断检查（镜像曾送出过 29 MB 的残缺文件）。下载：

```bash
curl -skL -m 900 \
  "https://hf-mirror.com/hr16/Diffusion-Edge/resolve/main/dsine.pt" \
  -o "<custom_nodes>/comfyui_controlnet_aux/ckpts/hr16/Diffusion-Edge/dsine.pt"

# 校验字节数必须是 291298581
stat -c%s "<上述路径>"
```

> 用 `hf-mirror.com` 而非 `huggingface.co`，是因为国内网络；`-k` 是因为本机 Python 不信任本地代理的 CA（此处用 curl 绕开）。这是包自带 README 给出的官方建议方式。

---

## 3. 本机实测状态（2026-10-12）

| 项 | 状态 |
|---|---|
| ComfyUI Desktop 版本 | `v0.38.2` (commit `daeb5e53`) |
| 用户数据目录 | `C:\Users\1142\AppData\Local\Programs\ComfyUI-data\` |
| 服务地址 | `http://127.0.0.1:8000`（**不是 8188**） |
| `comfyui_seamless_texture` | ✅ 已装（软链接 → `comfyUI_tools`） |
| `comfyui_controlnet_aux` | ✅ 已装 |
| `dsine.pt` | ✅ 291,298,581 B（字节数吻合） |
| `SEAMLESS_DSINE_WEIGHTS` | 未设置（走默认路径查找） |
| GPU | RTX 5090 D v2 · 25,642,926,056 B VRAM |

---

## 4. 更新策略

本仓库的副本是**快照**。上游（`comfyUI_tools` 项目）改动后需要手动同步：

```bash
SRC="D:/PersonalDocuments/hongxuan_document/comfyUI_tools/custom_nodes/comfyui_seamless_texture"
DST="D:/PersonalDocuments/hongxuan_document/UE_tools/ComfyTextureStudio/Resources/comfyui_seamless_texture"

# 同步前先 diff，确认只同步预期改动（遵循"先验证再删除"）
diff -r --exclude=__pycache__ "$SRC" "$DST"

# 确认后再 cp -p 覆盖，并更新本文件 §3 的纳管时间与版本
```

> 版本变了要同步更新：本文件 §1 表格的**版本号**、`Config/Workflows/workflows.manifest.json` 的 `requiredCustomNodePacks[0].version`。
