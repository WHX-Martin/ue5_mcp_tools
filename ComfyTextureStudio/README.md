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
| [`docs/M1-report.md`](docs/M1-report.md) | **M1 报告**：插件骨架 + 后端打通；编译链、commandlet 用法、7 个实现坑 |
| [`docs/M2-report.md`](docs/M2-report.md) | **M2 报告**：面板 + 导入 + 材质；三层验证结果、2 个自引入 bug（含一个崩溃）、未验证项清单 |
| [`docs/M2.1-sampler-fix.md`](docs/M2.1-sampler-fix.md) | **M2.1 补丁**：SamplerType 编译失败的三条根因（规范写错 / 自检盲区 / 编译假绿灯）、修复与负向回归测试 |
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

## 部署（M2 之后）

```ini
; 方式一：拷贝到插件目录
<UE项目>/Plugins/ComfyTextureStudio/

; 方式二：开发期在 .uproject 里挂外部目录（推荐，改代码不用来回拷）
"AdditionalPluginDirectories": [ "D:/PersonalDocuments/hongxuan_document/UE_tools" ]
```

启用后：**Window → Comfy Texture Studio**（关卡编辑器工具栏也有入口）。
面板是可自由停靠/浮出的 Nomad Tab；打开时会自动探测 ComfyUI 连接并从
`/object_info` 拉取 LoRA / 采样器 / 调度枚举。

面板里的**缩略图最大只有 150 px**，判接缝/噪点不够用，所以**所有图片都可以点开**：
单击弹出独立查看器窗口（滚轮缩放 · 拖拽平移 · 双击复位 · 2×2 平铺 · 转系统看图器）。
每次点击新建一个窗口，方便把 albedo 与 normal 并排对比。

## 当前状态

- [x] **M0-前置**：工作流结构实测解析（43 节点 / 46 link / 81 槽位）
- [x] **M0-前置**：拆分点定位 + 反向可达性 keep/drop 推导 + 槽位清单落盘
- [x] **M0-前置**：需求评审完成，8 项决策全部确认（PRD v1.1）
- [x] **M0-前置**：节点包纳管至 `Resources/`（字节级校验通过）+ 依赖链记录
- [x] **M0**：接口验证全绿 —— 两张 API 图导出并通过三层验证，端到端实跑成功
- [x] **M1**：Runtime 模块（HTTP 客户端 / 工作流装载 / 槽位注入 / 任务状态机）+ 无 UI 命令行触发
- [x] **M2**：Editor 模块 —— Slate 三态面板、贴图导入（§6.3 属性表逐项自检）、材质图 + BumpOffset 视差 + 材质实例
- [x] **M2.1**：修复 SamplerType 与贴图压缩格式不符（材质在编辑器里编译报红字）+ 揭穿"编译 0 错误"假绿灯
- [x] **M2.2**：图片查看器 —— 面板里所有图点击即弹独立窗口（滚轮缩放 / 拖拽平移 / 2×2 平铺 / 转系统看图器）
- [ ] **M3**：WebSocket 进度 / 取消过渡态 / 参数预设 / 资产浏览器右键入口
- [ ] **M4**：批量候选 / 任意图→PBR / Nanite 位移 / Upscale 接入

**M0 实测结论：端到端热 12.6 s / 冷 16.7 s**（阶段 A 7.3 s + 阶段 B 5.1 s），
`batch_size=8` 亦不 OOM。详见 [`docs/M0-baseline.md`](docs/M0-baseline.md)。

**M1 实测结论：一次 commandlet 调用产出 6/6 通道，端到端 18.34 s**
（阶段 A 12.33 s + 阶段 B 4.58 s + UE 启动开销）。详见 [`docs/M1-report.md`](docs/M1-report.md)。

**M2 实测结论：导入 + 建材质全链路通过，§6.3 属性表 37 项核对 0 项不符**
（新进程从磁盘冷加载核对），视差驱动 **3/3** 张交付贴图。
详见 [`docs/M2-report.md`](docs/M2-report.md)。

> ⚠️ M2 报告里那句"材质编译 0 错误"**不成立**：无头环境下编译根本没跑
> （`RecompileMaterial` 拿不到 `MaterialResource` 就静默返回空数组），
> 这个假绿灯同时掩盖了下面的 sampler type 错误。见 M2.1。

**M2.1 实测结论：SamplerType 自检 4/4、冷加载核对 41 项 0 项不符、负向回归 8/8。**
详见 [`docs/M2.1-sampler-fix.md`](docs/M2.1-sampler-fix.md)。

> ⚠️ **M2 的 UI 部分只做到"编译通过 + 逻辑接线"**：当前沙箱无法创建 GUI 窗口，
> 无头环境下 Slate 未初始化（面板自检会优雅跳过）。面板的视觉与交互需要人工确认，
> 清单见 M2-report §6。另外**法线单光源渲染验证仍未做**。

---

## 构建与运行（M1 起可用）

需要一个宿主 UE 工程把插件编进来。仓库根下已备好最小宿主 `ComfyTextureStudioHost`：

```bat
:: 1) 编译（-NoUBA 是必需的，见注解）
"<UE_5.8>\Engine\Build\BatchFiles\Build.bat" ComfyTextureStudioHostEditor Win64 Development ^
  -Project="D:/PersonalDocuments/hongxuan_document/UE_tools/ComfyTextureStudioHost/ComfyTextureStudioHost.uproject" ^
  -WaitMutex -NoUBA

:: 2) 无 UI 跑完整两阶段（产出 6 张 PBR 贴图）
"<UE_5.8>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "D:/PersonalDocuments/hongxuan_document/UE_tools/ComfyTextureStudioHost/ComfyTextureStudioHost.uproject" ^
  -run=ComfyTextureStudio -unattended -nopause -nosplash -stdout
```

```bat
:: 常用参数
-Seed=random          避开 ComfyUI 的节点级缓存（同图同种子重提交会直接命中，看起来"很快"其实没算）
-Set=dsineFov=2.5,aoRadius=32     任意语义键覆盖
-Lora=<文件名>        换 LoRA（Q7 的预留通道）
-OutDir=<绝对路径>    覆盖产物根目录
```

### M2 起：导入 UE 并建材质

```bat
:: 跑完两阶段后直接导入 UE + 建材质（无 UI）
"<UE_5.8>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<...>.uproject" ^
  -run=ComfyTextureStudio -unattended -nopause -nosplash -stdout ^
  -Import -Seed=random -BaseName=cts_m2_verify2
:: 可选：-DestPath=/Game/... -NoFlipGreen -NoBumpOffset -ImportOrphans -NoMaterialInstance -BumpDepth=0.02

:: 冷加载核对（**不需要 ComfyUI**）：新进程从磁盘重新加载资产，逐项核 §6.3 与材质图结构
:: ⚠ /Game 路径前必须关掉 Git Bash 的 MSYS 路径转换，否则参数会被改写成 Windows 本地路径
MSYS_NO_PATHCONV=1 "<UE_5.8>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<...>.uproject" ^
  -run=ComfyTextureStudio -unattended -nopause -nosplash -stdout ^
  -VerifyOnly=/Game/ComfyTextures/cts_m2_verify2

:: HTTP 存活令牌回归测试（复现"请求飞行中销毁客户端"的时序）
"<UE_5.8>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<...>.uproject" ^
  -run=ComfyTextureStudio -unattended -nopause -nosplash -stdout -TestLiveness

:: SamplerType 校验器负向回归测试（**不需要 ComfyUI，也不需要任何资产**）
:: 断言错误组合被拒、正确组合通过 —— 防止"永远返回 true 的校验器"混过去
"<UE_5.8>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<...>.uproject" ^
  -run=ComfyTextureStudio -unattended -nopause -nosplash -stdout -TestSamplerTypeCheck
```

产物落在 `<宿主工程>/Saved/ComfyTextureStudio/Cache/<JobId>/`，含 `stage_a/`、`stage_b/`、
`report.json` 与 `report_import.json`；同时会把**实际提交的 API 图**存成
`stage_a_api.json` / `stage_b_api.json`，出问题可以直接拖进 ComfyUI 前端复现。
导入的资产落在宿主工程的 `Content/ComfyTextures/<基名>/`。

> ⚠️ **`-NoUBA` 不能省。** 不关 Unreal Build Accelerator 时，它生成 PCH 会被文件系统权限拒绝，
> 报 `fatal error C1083: 无法打开预编译头文件 … 拒绝访问`，看起来像代码错误，其实是构建加速器。

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

需求已全部关闭，无待决策项。**下一步是 M3**：WebSocket 实时进度、取消过渡态、参数预设、
资产浏览器右键入口，以及人工确认 M2 的 UI（清单见 M2-report §6）。
