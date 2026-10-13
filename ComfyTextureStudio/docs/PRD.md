# ComfyTextureStudio — UE5 材质生成插件 PRD

| 项 | 值 |
|---|---|
| 文档版本 | **v1.2**（M0 接口验证已完成） |
| 日期 | 2026-10-12 |
| 插件名 | `ComfyTextureStudio`（短名 `CTS`，模块 `ComfyTextureStudio` + `ComfyTextureStudioEditor`）· **Q1 已定案** |
| 目标引擎 | **UE 5.8.3**（主）/ UE 5.6.1（兼容） |
| 目标宿主 | ComfyUI Desktop **v0.38.2** @ `http://127.0.0.1:8000` |
| 依赖工作流 | `sdxl_sand_pbr_v2_geom.json`（43 节点，实测已跑通） |
| 依赖节点包 | `comfyui_seamless_texture` **v1.3.0**（Q8：已纳管至 `Resources/`） |
| 状态 | **已评审 · 8 项决策确认 · M0 接口验证全绿** |

---

## 0. 结论先行

**做什么**：一个原生 UE5 编辑器插件（C++ + Slate），在引擎内提供一个两阶段材质生成工作台：

```
①  提示词 ──POST /prompt──> ComfyUI 阶段A(SDXL+LoRA) ──> 受光渲染图(1024², 严格四方连续)
                                                            │
                                          用户在面板里审阅 / 2×2 平铺自检
                                                            │
                                          满意 ──POST /upload/image──> ComfyUI input/
                                                            │
②                                    ComfyUI 阶段B(几何反演) ──> albedo / normal / height / ao / roughness / orm
                                                            │
③  自动下载 PNG ──UAssetImportTask──> UTexture2D(设好压缩/sRGB/法线翻转)
                                    └──UMaterialEditingLibrary──> M_<Name> + MI_<Name>
```

**三个必须先定下来的技术决策**（本文档的核心结论）：

| # | 决策 | 结论 | 理由 |
|---|---|---|---|
| **D1** | 两阶段如何拆分 | **拆成两张独立 API 图**，阶段 B 用 `LoadImage` 取代 `node 8` | 现图中阶段 B 通过 `SeamlessVAEDecode(8)` 与阶段 A 耦合（`8→35`、`8→70` 两条边）。不拆则"用户满意后再生成"无法实现——只能整图重跑 |
| **D2** | 阶段 B 是否保留 MoGe 对照支路 | **裁掉**（`30/31/32/33/34/37/38/39`，8 个节点） | 反向可达性证明该支路与 6 个交付输出**完全无连接**，纯对照产物。**实测收益：稳态省 ≈1.03 s/轮（≈阶段B 的 20%）+ 一次性省 ~6 s 权重加载 + ~1.4 GiB 显存 + 去掉 MoGe 权重依赖**（见 §5.3） |
| **D3** | 法线绿通道在哪儿翻转 | **UE 侧**（`bFlipGreenChannel = true`），ComfyUI 侧保持 OpenGL 输出 | 保持源 PNG 可移植；翻转是 UE 资产的每资产可逆开关，比改工作流更可控 |

**M0 接口验证：✅ 已完成（2026-10-12）** —— 两张 API 图已导出并通过三层验证，端到端实跑成功，
**热 12.6 s / 冷 16.7 s**。详见 §5.3 与 `docs/M0-baseline.md`。下一步是 M1（Runtime 模块）。

**已确认的 8 项产品决策（v1.1 评审结论）**：

| # | 问题 | 决策 | 影响 |
|---|---|---|---|
| Q1 | 插件名 | `ComfyTextureStudio` | 定案 |
| Q2 | 保留 `44`（normal_dsine 调试输出）？ | **不保留** | 阶段 B 由 16 → **15 节点**；裁剪集由 19 → **20 节点** |
| Q3 | ORM 合并 or AO/Roughness 分开 | **合并图** | 材质只采 ORM；AO/Roughness 默认不导入 |
| Q4 | 高度图用法 | **BumpOffset 视差** | 材质图中 Height→BumpOffset→各采样的 UV 偏移；Nanite 真位移列为 M4 可选项 |
| Q5 | 是否支持排队 | **暂不支持** | 单任务串行，无队列管理器 |
| Q6 | 命名规则 | **模板 + 手填** 双通道 | 详见 §7.2 / §8 |
| Q7 | 预留换 LoRA 通道 | **预留** | LoRA 枚举从 `/object_info` 实时拉取，不硬编码 |
| Q8 | 节点包位置 | **放 `Resources/`** | 已纳管 v1.3.0 + `Resources/README.md` 记录依赖链 |

**MVP 范围**：D1 两段式 + D2 裁剪 + D3 + 4 张交付贴图导入（albedo/normal/height/orm）+ 材质图自动搭建（含 BumpOffset 视差）。不含 WebSocket 进度、任务队列、Nanite 位移。

---

## 1. 背景与目标

### 1.1 现状

已有一条在 ComfyUI 里跑通的材质生成链路 `sdxl_sand_pbr_v2_geom`，它解决了一个非常具体的问题：**v2 LoRA 训练集是"受光渲染图"**（caption 绑死了 `directional sunlight from upper left` / `raking light` / `visible bump relief with soft cast shadows`），所以它画出来的亮度里已含起伏信息。链路不是"凭空生成其他通道"，而是**把光照反演掉**：

```
lit render（环面 padding，严格四方连续）
   ├─ DSINE 法线（纯卷积 · fov 3° · pad 256）→ 整平 ⇒ 主几何源
   │        └─ 周期泊松积分（FFT）+ 低频剔除 ──> height
   │              ├─ 圓環中心差分 ──> normal（主法线，严格四方连续）
   │              ├─ 空腔法 ───────> AO
   │              └─ 微起伏能量 ───> roughness
   ├─ MoGe-2 法线头（周期补边 · 对照支路）
   ├─ 平场除法（低频光照）──> albedo
   └──────────────────────> ORM 打包
```

### 1.2 痛点

| 痛点 | 具体表现 |
|---|---|
| 工具链断层 | 在 ComfyUI 里出图 → 手动找输出目录 → 拖进 UE → 一张张改 sRGB/压缩设置 → 手搭材质图。一次 6~12 个资产操作 |
| 易错点全在手工环节 | 法线要翻绿通道；ORM 必须关 sRGB；AO/Roughness/Metal 要从 R/G/B 分别引线。任何一步漏了都是"看起来对但渲染不对" |
| 无法迭代 | "先生成看看、满意了再做全套"这个自然工作流，在现有一体化图里做不到——前半段跑完必然把后半段一起跑完 |
| 参数不可复用 | 每次调 fov/pad/bump_strength 都要打开 ComfyUI 找节点改数值 |

### 1.3 目标（可验证）

| ID | 目标 | 验收方式 |
|---|---|---|
| G1 | 插件内一句话产出可用基础贴图 | 输入提示词 → 面板出现图，`GET /history` 有产物，耗时被记录 |
| G2 | 满意后才跑第二阶段 | 阶段 A 结束后可 **不** 触发生成，可换种子/改词重跑；阶段 B 只在用户点按钮后启动 |
| G3 | 全程零手工导入 | 4 张交付贴图（+ 2 张 orphan 按需）落到 `/Game/ComfyTextures/<Name>/`，且 sRGB/压缩/绿通道/寻址模式**全部正确**（对照 §6.3 表逐项核对） |
| G4 | 材质开箱可用 | 生成的 `M_<Name>` + `MI_<Name>` 可直接贴到 StaticMesh 上，无黄色警告，平铺不露接缝 |
| G5 | 可脱离 ComfyUI 前端使用 | 插件启动 → 出图 → 导入 → 材质，全程不打开 ComfyUI 窗口 |

### 1.4 非目标（明确不做）

- ❌ 不做 ComfyUI 工作流可视化编辑器（工作流仍是 ComfyUI 里编辑，插件只消费 API 图）
- ❌ 不做 LOD / 光照烘焙 / 材质函数库
- ❌ 不做云端 ComfyUI（v1 只支持本机或局域网 HTTP）
- ❌ 不做运行时（打包后）生成——纯 Editor 插件，不进 Shipping 包
- ❌ 不做贴图后处理（去噪/锐化/超分，ComfyUI 侧已有 `SeamlessUpscale` 可后续接）
- ❌ **不做任务队列**（Q5）：一次只跑一个任务；排队、并发、多材质批量均为 v1 之外
- ❌ **不做 Nanite Tessellation 真位移**（Q4）：v1 的高度图只走 BumpOffset 视差，真位移列为 M4 可选项
- ❌ 不做 AO / Roughness 单独接入材质（Q3）：只采 ORM 合并图

---

## 2. 需求

### 2.1 用户故事

| # | 故事 | 优先级 |
|---|---|---|
| US-1 | 作为美术，我想在 UE 里输入"粗糙鹅卵石海滩沙地"就得到一张四方连续的沙地贴图，好让我不用切换软件 | **P0** |
| US-2 | 作为美术，我想先看到 2×2 平铺效果确认没有接缝，再决定是否继续 | **P0** |
| US-3 | 作为美术，我对图不满意时想只改种子重出，不想重跑全套 | **P0** |
| US-4 | 作为美术，我满意后点一个按钮就拿到整套 PBR 贴图并自动导入 UE | **P0** |
| US-5 | 作为美术，我想要材质节点已经连好，而不是给我 4 张贴图让我自己搭 | **P0** |
| US-6 | 作为技术美术，我想调 DSINE fov / pad / bump_strength 等几何反演参数并保存成预设 | **P1** |
| US-7 | 作为技术美术，我想知道任务卡在哪个节点、报了什么错 | **P1** |
| US-8 | 作为技术美术，我想中断一个跑偏的任务 | **P1** |
| US-9 | 作为美术，我想一次生成 4 个候选，挑一张继续 | **P2** |
| US-10 | 作为美术，我想把外部已有的一张沙地照片丢进来直接生成 PBR 全套 | **P2** |

### 2.2 功能清单

**F1 · 连接与自检（P0）**
- `GET /system_stats` 探活，面板顶部显示连接状态灯（绿/红/黄）
- `GET /object_info` 做**预检**：校验图内全部 `class_type` 存在、校验模型文件存在
- 预检失败时给出**中文可执行清单**，而不是抛异常。例：
  - `缺少自定义节点包 comfyui_seamless_texture` → 提示"该包已随插件分发在 `Resources/comfyui_seamless_texture/`，复制到 ComfyUI 的 `custom_nodes/` 后重启"，并提供**一键复制**按钮
  - `缺少 comfyui_controlnet_aux` → 提示"`SeamlessDSINENormal` 依赖该包提供 DSINE 架构，请用 ComfyUI Manager 安装"
  - `缺少 dsine.pt 权重` → 提示预期路径与**精确字节数 291,298,581**，并附下载命令（该权重**不会被自动下载**，源码有意如此）
  - `dsine.pt 字节数不符` → 单独报错（镜像曾送出 29 MB 的截断文件，包内有截断检查）
  - `缺少模型 sd_xl_base_1.0.safetensors` → 显示 `GET /object_info` 里的可用枚举值供选择
- **预检只校验当前图内实际引用的依赖**。裁掉 MoGe 支路后 `moge_2_vitl_normal_fp16.safetensors` 不再是必需项，不应出现在缺失清单里
- ComfyUI 未启动 → 提示 + 可选"尝试启动"（配置了 `comfy-cli` 路径时可用）

**F2 · 阶段 A：基础贴图生成（P0）**
- 提示词输入（多行）。**默认值 = 工作流实测的 9 个绑死 tag 模板**，末尾留材质描述位：
  ```
  mtrlx, seamless tileable PBR material, <用户材质描述>,
  square top-down surface, directional sunlight from upper left,
  visible bump relief with soft cast shadows, raking light,
  crisp fine detail, high frequency texture, small scale pattern
  ```
  > ⚠️ 前 9 个 tag 是 LoRA 训练时的固定触发词（受光渲染版），**必须保留**，否则 LoRA 不生效或出图不带起伏。
- 负向提示（默认折叠）：工作流原值，含 `tiling seam, visible grid, tile border, depth map, normal map, displacement map, uv checker`——**不要删这些**，它们是把接缝类伪影压下去的关键。
- 出图参数：seed（含 🎲 随机 / 🔒 锁种子）、steps、cfg、sampler、scheduler、尺寸
- LoRA 选择 + `strength_model` / `strength_clip`
- 提交后进入"生成中"态：轮询 `GET /history/{prompt_id}`，显示已耗时
- 完成后：下载 PNG → 面板显示原图 + **2×2 平铺预览**（在 UE 侧本地拼接，不额外占用 ComfyUI 一次往返）

**F3 · 审阅与迭代（P0）**
- 按钮组：`重新生成（换种子）` / `用当前参数重跑` / `换种子重出 N 张`（P2）/ `✓ 用这张继续` / `✗ 丢弃`
- 每次生成落一条历史记录条目（缩略图 + 参数快照 + prompt_id），可回选

**F4 · 阶段 B：PBR 全套生成（P0）**
- 触发时：把阶段 A 的 PNG `POST /upload/image` 到 ComfyUI `input/`，同时写入一个**唯一文件名**（`cts_<任务GUID>.png`）避免与用户已有文件撞名
- 提交裁剪版阶段 B API 图（`LoadImage` → `35/36/40/50/60/61/62/70`）
- 默认参数取工作流实测值（见 §5.4），P1 起在面板暴露
- 完成后从 `/history` 的 `outputs` 精确取 7 个产物（6 交付 + 1 可选调试），**不猜文件名**

**F5 · 导入 UE + 建材质（P0）**
- 导入 **4 张交付贴图**：`albedo` / `normal` / `height` / `orm`，并**逐项设置**导入属性（§6.3）
- `ao` / `roughness` 是 **orphan 产物**（Q3：ORM 已含），默认**不导入、不接材质**；仅在用户显式勾选"额外导入 AO/Roughness 单图"时导入
- 用 `UMaterialEditingLibrary` 建材质图（§6.2），**含 BumpOffset 视差**（Q4）
- 建 `MI_<Name>` 材质实例，把标量参数挂上
- 完成后在 Content Browser 里定位并高亮新资产
- 输出一份导入报告（成功/失败/警告逐项），可复制为文本

**F5b · 材质命名（P0 · Q6）**
- **双通道**：模板自动生成 + 手填覆盖
- 模板 token：`{material}`（材质描述关键词）· `{grain}`（粒度）· `{index}`（序号，自动避让已有资产）· `{date}`
- 默认模板与资产命名规则（可在设置里改）：

  | 模板 | 产出 |
  |---|---|
  | 材质模板 `M_{material}_{grain}_{index}` | `M_Sand_Coarse_01` |
  | 贴图模板 `T_{material}_{grain}_{index}_{suffix}` | `T_Sand_Coarse_01_BC` / `_N` / `_ORM` / `_H` |
  | 文件夹模板 `{assetRoot}/{material}_{grain}_{index}` | `/Game/ComfyTextures/Sand_Coarse_01/` |

- 手填模式下 `{index}` 冲突时**必须报错而不是静默覆盖**（`bReplaceExisting` 只在用户显式确认覆盖时置 true）
- 命名框旁边实时预览最终资产路径

**F6 · 取消与错误（P1）**
- 取消：`POST /interrupt` + 清空队列，UI 立刻回到可操作态
- 错误映射：`/history` 里 `status.messages` 携带 `node_id` 与异常 → 按 §5.5 映射表翻成中文节点名 + 建议动作
- 区分三类失败：`节点执行报错` / `ComfyUI 进程崩溃（OOM，HTTP 连接中断）` / `超时`

**F7 · 参数预设（P2）**
- 阶段 A / 阶段 B 参数各存为命名预设（JSON，落在 `Saved/ComfyTextureStudio/Presets/`）
- 预设可导出/导入，便于换机器或团队共享

**F8 · 批量候选（P2）**
- 走 ComfyUI 的 `5.batch_size` **单次提交**产出 N 张（**不是任务队列**，与 Q5 不冲突），网格展示后选一张进阶段 B
- N 次不同 seed 的连续提交属于排队行为，**v1 不做**

**F9 · 任意图 → PBR（P2）**
- 阶段 B 可直接吃用户从磁盘拖入的 PNG（不经过阶段 A）

---

## 3. 系统架构

### 3.1 模块划分

| 模块 | 类型 | 职责 | 关键依赖（Build.cs） |
|---|---|---|---|
| `ComfyTextureStudio` | Runtime | ComfyUI HTTP 客户端、工作流 JSON 装载与槽位注入、任务状态机、数据结构、设置 | `Core`, `CoreUObject`, `Engine`, `HTTP`, `Json`, `JsonUtilities`, `DeveloperSettings` |
| `ComfyTextureStudioEditor` | Editor | Slate 面板与两阶段向导、贴图导入、材质图构建、资产浏览器交互 | 上面全部 + `UnrealEd`, `Slate`, `SlateCore`, `ToolMenus`, `MaterialEditor`, `AssetTools`, `ImageWrapper`, `RenderCore`, `EditorScriptingUtilities`, `WebSockets`(P1) |

> 为什么要拆两个模块：Runtime 模块可被其他工具（如未来的批处理命令行）复用；Editor 模块能带上 `UnrealEd`/`MaterialEditor` 这类只在编辑器存在的重依赖，不污染运行时。

### 3.2 组件图

```
┌──────────────────────────── ComfyTextureStudioEditor ────────────────────────────┐
│                                                                                   │
│  SComfyTextureStudioPanel (SDockTab · Nomad)                                      │
│    ├─ SFaseA_Wizard            参数表单 + ProgressBar + 预览缩略图                 │
│    └─ SFaseB_Review            6 通道预览 + 导入选项 + 报告                         │
│                                     │                                             │
│         FComfyTextureImportService  │  FComfyMaterialBuilder                      │
│           UAssetImportTask          │   UMaterialEditingLibrary                    │
│           设置压缩/sRGB/翻转         │   建表达式+连线+编译                          │
│                                     │                                             │
└─────────────────────────────────────┼─────────────────────────────────────────────┘
                                      │
┌─────────────────────────────────────┼──────── ComfyTextureStudio (Runtime) ────────┐
│                                     ▼                                             │
│  FComfyTextureJob (状态机)   ── 驱动 ──>  FComfyHttpClient                          │
│    Idle→Preflight→Submitting→              POST /prompt                            │
│    Running→Downloading→Ready               GET  /history/{id}   (轮询 1.5s)        │
│    →Importing→Done | Failed | Cancelled    GET  /view?...      (下载)              │
│                                            POST /upload/image                      │
│  FComfyWorkflowGraph  <── 装载 ──  Config/Workflows/*.json                         │
│    SetSlot(Address, Value)  ← slots manifest                                      │
│    ToJsonString()                                                                  │
└────────────────────────────────────────────────────────────────────────────────────┘
```

### 3.3 线程模型（硬约束）

| 操作 | 线程 | 说明 |
|---|---|---|
| HTTP 请求 / 轮询 / 下载 | **任意后台线程**（`FHttpModule` 回调本就非游戏线程） | 绝不在回调里碰 UObject |
| 轮询循环 | `FRunnable` 后台线程 或 基于 `FTSTicker` 的定时器 | 推荐 Ticker：实现简单且天然回到游戏线程；1.5s 间隔对本地服务足够 |
| 下载得到的 PNG 字节 | 后台线程持有 `TArray<uint8>` | 无 UObject 依赖，安全 |
| `UTexture2D` 创建 / 导入任务 / 材质构建 / 写盘 | **必须游戏线程** | 经 `AsyncTask(ENamedThreads::GameThread, ...)` 回投 |
| Slate 刷新 | 游戏线程 | 状态变更走 `TAttribute` + `Invalidate` |

> **红线**：不得在 HTTP 回调里直接 `NewObject` 或调用 `UMaterialEditingLibrary`。这是 UE 插件最常见的崩溃来源。

### 3.4 状态持久化

| 内容 | 位置 | 机制 |
|---|---|---|
| 插件设置（URL、超时、资产根路径、默认参数） | `DefaultGame.ini` → `[/Script/ComfyTextureStudio.ComfyTextureStudioSettings]` | `UDeveloperSettings` 子类 + `config=Game, defaultconfig` |
| 本次会话任务历史（缩略图、prompt_id、参数） | 内存 + `Saved/ComfyTextureStudio/Session.json` | 退出可选保留 |
| 参数预设（P2） | `Saved/ComfyTextureStudio/Presets/*.json` | 手写读写 |
| 工作流 API 图 + 槽位清单 | 插件 `Config/Workflows/` | 随插件分发 |

---

## 4. ComfyUI 接口契约

### 4.1 REST 端点（v0.38.2 实测可用）

| 用途 | 方法 | 路径 | 请求/响应要点 |
|---|---|---|---|
| 探活 | GET | `/system_stats` | 返回 `devices[].vram_free/total`、`system.ram_*`。**启动时先查 VRAM**，不足则提示 |
| 预检 | GET | `/object_info` | 巨型 JSON。取 `[class_type].input.required` 的键序与模型枚举做校验 |
| 提交 | POST | `/prompt` | body `{"prompt": <API图>, "client_id": "<GUID>"}`；成功返回 `{"prompt_id","number","node_errors"}`。**`node_errors` 非空 = 校验失败，未入队** |
| 查询 | GET | `/history/{prompt_id}` | 键为 prompt_id；`outputs[<node_id>]["images"][].{filename,subfolder,type}`；`status.status_str` ∈ `success`/`error`，`status.messages` 带节点级错误 |
| 下载 | GET | `/view?filename=&subfolder=&type=output` | 二进制 PNG。**参数必须取自 `/history` 的 outputs，不能自己拼** |
| 上传 | POST | `/upload/image` | multipart：`image`=文件、`overwrite`=`true`、`type`=`input`、`subfolder`=可选。返回 `{name,subfolder,type}` |
| 中断 | POST | `/interrupt` | 取消当前执行 |
| 队列 | GET | `/queue` | 查看 running/pending；取消前确认排队位置 |
| 进度(P1) | WS | `/ws?clientId=<GUID>` | 事件 `status` / `executing`(带 node) / `progress`(value,max) / `executed` |

> **CORS 不需要**：UE 的 `FHttpModule` 是原生 HTTP 客户端，不受浏览器同源策略约束，无需给 ComfyUI 加 `--enable-cors-header`。
> **跨机访问**：默认 Desktop 只监听 `127.0.0.1:8000`。若 ComfyUI 在另一台机器上，需要以 `--listen 0.0.0.0` 启动，并把插件 URL 指向对方 IP。
> **⚠ `/view` 的下载参数（M1 实测踩中）**：`subfolder` 在 **Windows 上由 ComfyUI 用 `os.sep` 拼**，
> `/history` 报回来的是**反斜杠**（`cts\20261013-110215_EB98BF`）。两种写法 `/view` 都认，但**百分号编码必须完整**：
> 漏掉 `%` 会把 `\`(0x5C) 编成字面量 `5C`，ComfyUI 于是去找一个叫 `cts5Cxxx` 的目录并回 **404** ——
> 而产物其实好好躺在 output 目录里，报错姿势极具误导性。实现上统一把 `\` 归一成 `/` 再编码。

### 4.2 工作流拆分方案（D1 落地）

**现状耦合（实测）**：

```
8:SeamlessVAEDecode.out0 ──┬──> 20:SaveImage            （阶段A 产物）
                           ├──> 21:TilePreview          （仅预览）
                           ├──> 31:SeamlessMoGeInference （对照支路）
                           ├──> 35:SeamlessDSINENormal   ★ 阶段B 使用
                           └──> 70:SeamlessDelightAlbedo ★ 阶段B 使用
```

**⇒ 拆分点就是 `8→35` 与 `8→70` 这两条边。** 把 `node 8` 换成 `LoadImage`，并把它的 `IMAGE` 输出同时接到 `35.in0` 与 `70.in0`，阶段 B 即成为可独立提交的图。

**拆分结果**：

| 图 | 节点数 | 保留节点 id |
|---|---|---|
| `stage_a_base_texture.json` | **9** | `1, 2, 3, 4, 5, 6, 7, 8, 20` |
| `stage_b_pbr.json` | **15** | `LOAD_IMAGE`, `35, 36, 40, 50, 60, 61, 62, 70`, SaveImage `41, 54, 63, 64, 65, 71` |
| 合计裁剪掉 | **20** | `10, 21, 30, 31, 32, 33, 34, 37, 38, 39, 42, 43, 44, 55, 56, 66, 67, 72, 73, 74` |

裁剪依据（**程序化反向可达性推导，非手写**）：
- 阶段 A：从 `20:SaveImage` 反向可达 ⇒ `{1,2,3,4,5,6,7,8,20}`
- 阶段 B：从 6 个交付 `SaveImage` 反向可达 ⇒ 含 `1..8`（因耦合）；**替换 `8` 为 `LoadImage` 后** ⇒ `{LoadImage,35,36,40,50,60,61,62,70,41,54,63,64,65,71}`
- `30/31/32/33/34/37/38/39`（MoGe 对照支路）在交付输出的反向可达集里**不出现** ⇒ 与交付物零耦合，可裁（D2）
- `44`（`normal_dsine` 调试输出）与交付无关，且 **Q2 已决定不保留** ⇒ 一并裁掉
- **数目自洽**：全图 43 = 阶段A 9 + 阶段B 沿用原图 14 + 裁掉 20。
  阶段 B **图文件**是 15 节点，因为其中有 **1 个新增的 `LoadImage`**（不在原图 43 个里）——`43 = 9 + 14 + 20` 与 `阶段B图 = 14 + 1` 两者都对，别把 15 直接代进 43 的拆分式

**阶段 B 保留部分的精确连线（实现时照抄）**：

```
LoadImage.out0 ──┬──> 35:SeamlessDSINENormal.in0
                 └──> 70:SeamlessDelightAlbedo.in0

35 ──> 36:SeamlessFlattenNormal ──> 40:SeamlessHeightFromNormal
                                       │
                                       ├──> 41:SaveImage(height)              ✅ 交付
                                       ├──> 50:SeamlessNormalFromHeight
                                       │        └──> 54:SaveImage(normal)     ✅ 交付
                                       ├──> 60:SeamlessAOFromHeight
                                       │        ├──> 63:SaveImage(ao)         ⃠ orphan（Q3 不导入）
                                       │        └──> 62:PackPBRChannels.in0
                                       └──> 61:SeamlessRoughnessFromHeight
                                                ├──> 64:SaveImage(roughness)  ⃠ orphan（Q3 不导入）
                                                └──> 62:PackPBRChannels.in1
                                                         └──> 65:SaveImage(orm)  ✅ 交付

70 ──> 71:SaveImage(albedo)                                                   ✅ 交付
```

> `62:PackPBRChannels` 的输入顺序是 **in0 = AO，in1 = Roughness**（实测），不是文档里常见的 ORM 顺序，接线时别弄反。

### 4.3 槽位映射（语义键 → 节点地址）

插件不硬编码节点 id，而是读 `Config/Workflows/workflows.manifest.json`。地址格式与 `comfy workflow slots` 一致（`<node_id>.<widget_name>`）。完整清单见 `docs/workflow-interface.md`，此处列默认值：

**阶段 A**

| 语义键 | 地址 | 默认值 |
|---|---|---|
| `model` | `1.ckpt_name` | `sd_xl_base_1.0.safetensors` |
| `lora` | `2.lora_name` | `sdxl-texture-sand-lora-v2.safetensors`（枚举从 `/object_info` 实时拉取，**不硬编码** —— Q7） |
| `lora_model_strength` | `2.strength_model` | `1.0` |
| `lora_clip_strength` | `2.strength_clip` | `1.0` |
| `positive` | `3.text` | 见 §2.2 F2 模板 |
| `negative` | `4.text` | 工作流原值 |
| `width` / `height` | `5.width` / `5.height` | `1024` / `1024` |
| `batch_size` | `5.batch_size` | `1` |
| `padding_mode` | `6.mode` | `circular` ← **勿改，四方连续的来源** |
| `seed` | `7.seed` | `20261013`（实测值） |
| `steps` | `7.steps` | `30` |
| `cfg` | `7.cfg` | `6.5` |
| `sampler` | `7.sampler_name` | `dpmpp_2m` |
| `scheduler` | `7.scheduler` | `karras` |
| `denoise` | `7.denoise` | `1.0` |
| `vae_margin` | `8.margin` | `8` |
| `out_prefix` | `20.filename_prefix` | `cts/stage_a/base` |

**阶段 B**（默认值全部取自工作流实测，见 §5.4）

| 语义键 | 地址 | 默认值 |
|---|---|---|
| `input_image` | `LoadImage.image` | 运行时写入 `cts_<GUID>.png` |
| `dsine_fov` | `35.fov` | `3.0` ← 压到近正交是补边能生效的关键 |
| `dsine_pad` | `35.pad` | `256` |
| `dsine_iterations` | `35.iterations` | `5` |
| `dsine_flip_red` | `35.flip_red` | `true`（相机系→OpenGL 的红通道翻转，实测标定） |
| `dsine_fp16` | `35.use_fp16` | `false` |
| `flatten` / `relief_scale` | `36.flatten` / `36.relief_scale` | `true` / `1.0` |
| `height_low_freq_radius` | `40.low_freq_radius` | `96` ← 不做低频剔除会出现"碗形"偏置 |
| `height_headroom` | `40.headroom` | `0.15` ← 分段线性映射，**不可用硬截断**（会产生零梯度死平面） |
| `height_invert` | `40.invert` | `false` |
| `bump_strength` | `50.bump_strength` | `31.4`（headroom=0.15 下的等价幅度；旧硬截断版本是 22） |
| `normal_directx` | `50.directx` | `false` ← 保持 false，交给 UE 翻绿通道（D3） |
| `ao_radius` / `ao_scales` | `60.radius` / `60.scales` | `24` / `4` |
| `ao_strength` / `ao_gamma` / `ao_smooth` | `60.strength` / `60.gamma` / `60.smooth` | `1.0` / `1.0` / `2` |
| `rough_window` | `61.window` | `12` |
| `rough_min` / `rough_max` | `61.min_roughness` / `61.max_roughness` | `0.55` / `0.95` |
| `rough_gamma` / `rough_invert` | `61.gamma` / `61.invert` | `1.0` / `false` |
| `metalness` | `62.metalness_value` | `0.0` ← 砂石是电介质；换金属材质才改 |
| `delight_radius` | `70.radius` | `128` |
| `delight_amount` | `70.amount` | `1.0` |
| `delight_preserve_mean` | `70.preserve_mean` | `true` |
| 6 个输出前缀 | `41/54/63/64/65/71.filename_prefix` | `cts/<任务GUID>/height` 等，运行时自动替换 |

### 4.4 API 格式导出的坑（实现任务 T4 的输入）

前端格式（本仓库的 `sdxl_sand_pbr_v2_geom.json`，43 节点/46 link）与 API 格式（`{"<id>":{"class_type","inputs"}}`）之间需要转换。转换必须用 `GET /object_info` 的 schema，**不能靠启发式猜 widget 顺序**：

| # | 坑 | 处理 |
|---|---|---|
| 1 | `widgets_values` 是**数组**，要靠 `/object_info` 的 `input_order.required` 把它映射到具名输入 | 严格按 `input_order.required` 顺序逐个赋值 |
| 2 | `mode: 2`（muted）的节点应丢弃 | 转换时过滤 |
| 3 | `mode: 4`（bypass）需要改接线，不能简单丢弃 | 本图无 bypass，转换器检测到则报错退出 |
| 4 | 前端 link 是 `[id, src_node, src_slot, dst_node, dst_slot, type]`，API 是 `[str(src_node), src_slot]` | 建 id→link 表 |
| 5 | `MarkdownNote` / `Note` 无 API 语义 | 丢弃 |
| 6 | 前端有 `groups`/`extra.ds` 等 UI 字段 | API 格式不带 |
| 7 | 枚举值（模型名/sampler）在 API 格式里就是字符串，不校验 | 由插件预检阶段用 `/object_info` 兜底 |
| 8 | **ComfyUI 升级可能改节点 schema** | 转换器 + 预检都要跑；把 `comfy-cli` 版本记进 manifest |

**交付形态**：仓库保留前端格式图作为**唯一可编辑源**（能在 ComfyUI 里直接打开调试），`Tools/export_api_workflows.py` 负责导出两张 API 图 + manifest 到 `Config/Workflows/`。运行期插件只读 API 图。

### 4.5 节点 ID → 中文名映射（错误提示用）

| id | 节点 | 面向用户的名称 |
|---|---|---|
| 1 | `CheckpointLoaderSimple` | 加载基础模型 |
| 2 | `LoraLoader` | 加载沙地 LoRA（受光渲染版） |
| 3/4 | `CLIPTextEncode` | 正向/负向提示词 |
| 5 | `EmptyLatentImage` | 空潜空间（出图尺寸） |
| 6 | `SeamlessModelPadding` | UNet 环面 padding（四方连续） |
| 7 | `KSampler` | 采样器 |
| 8 | `SeamlessVAEDecode` | VAE 解码（环形 padding） |
| 20 | `SaveImage` | 保存基础贴图 |
| 35 | `SeamlessDSINENormal` | DSINE 法线估计 |
| 36 | `SeamlessFlattenNormal` | 法线基面整平 |
| 40 | `SeamlessHeightFromNormal` | 泊松积分求高度 |
| 50 | `SeamlessNormalFromHeight` | 由高度重导法线 |
| 60 | `SeamlessAOFromHeight` | 由高度求 AO |
| 61 | `SeamlessRoughnessFromHeight` | 由高度求粗糙度 |
| 62 | `PackPBRChannels` | ORM 通道打包 |
| 70 | `SeamlessDelightAlbedo` | 去光照（求 albedo） |

---

## 5. 参数与默认值

### 5.1 环境基线（实测）

| 项 | 值 |
|---|---|
| ComfyUI | Desktop，`v0.38.2`（commit `daeb5e53`） |
| ComfyUI **用户数据目录** | `C:\Users\1142\AppData\Local\Programs\ComfyUI-data\`（`custom_nodes` / `models` / `input` / `output` / `user` 都在这里） |
| comfy-cli 的 default_workspace | `C:\Users\1142\AppData\Local\Comfy-Desktop\ComfyUI-Installs\ComfyUI\ComfyUI`（⚠️ 见下方说明） |
| 服务地址 | `http://127.0.0.1:8000`（**非默认 8188**） |
| GPU | NVIDIA RTX 5090 D v2，VRAM 25,642,926,056 B（≈23.9 GiB） |
| RAM | 137,046,908,928 B（≈127.6 GiB） |
| comfy-cli | `1.22.0` |
| 自定义节点包 | **`comfyui_seamless_texture` v1.3.0**（MIT · 18 节点，含本图用到的全部 `Seamless*` / `PackPBRChannels` / `TilePreview`；**已纳管至 `Resources/`**） |
| 节点包外部依赖 | `comfyui_controlnet_aux`（必需）· `dsine.pt` **291,298,581 B**（必需，**不自动下载**）· Pillow（可选） |
| 必需模型 | `sd_xl_base_1.0.safetensors`、`sdxl-texture-sand-lora-v2.safetensors`（⚠️ MoGe 权重见下） |

> ⚠️ **两个目录不是同一个**：`comfy-cli` 报的 `default_workspace` 是 `ComfyUI-Installs\ComfyUI\ComfyUI`，但该目录下的 `custom_nodes/` **只有示例文件**；正在运行的服务实际加载的是 `Programs\ComfyUI-data\custom_nodes\`（本机那里有 20+ 个节点包，`comfyui_seamless_texture` 是以软链接形式挂进去的）。
> **预检与"一键复制节点包"必须写用户数据目录，不能信 `comfy-cli` 的 default_workspace**，否则会把包复制到一个引擎根本不读的地方，表现为"复制成功了但节点还是缺"。

> ⚠️ **裁剪 MoGe 后 `moge_2_vitl_normal_fp16.safetensors` 不再是运行必需**，但 `LoadMoGeModel(30)` 若被误留在图里就会变成隐式必需项。预检逻辑应只校验**当前图内实际引用**的模型——这也是裁剪带来的一项隐性收益。
> 另注：`GET /object_info` 的 checkpoints 枚举只有 `sam3.1_multiplex_fp16` / `sd_xl_base_1.0` / `v1-5-pruned-emaonly-fp16`，LoRA 枚举有 4 个沙地 LoRA。DSINE 与 MoGe 权重不在 `ckpt_name` 枚举里，需通过各自节点的枚举校验。

### 5.2 ComfyUI 版本风险

`server_info` 报告 **core 已过期**：本地 `v0.38.2` → 最新 `v0.39.2`。

**处置**：M0 期间**不升级**。先把链路在 `v0.38.2` 上跑通并锁定 API 图。若后续要升级，必须先重跑 §4.4 的导出与预检，并在 manifest 里更新 `comfyui_version` 字段。理由：工作流 note 里已记录 `MoGeInference` 在 `0.38.2` 下 `refine_steps` 被真实透传但被模型侧忽略（`version=="v3"` 才下发）——**节点行为随版本漂移是已验证发生过的事**。

### 5.3 实测基线（M0 已完成 · 2026-10-12）

✅ **已实测回填。** 完整数据、方法论与复现命令见 **`docs/M0-baseline.md`**。

| 项 | 阶段 | 实测 |
|---|---|---|
| 单图耗时（1024², 30 steps, dpmpp_2m/karras） | A | **冷启 11.4 s** / **热跑 7.3 s** 中位 |
| 阶段 B 耗时（裁剪版，无 MoGe） | B | **5.07 s** 中位（互异输入 ×3） |
| 阶段 B 耗时（含 MoGe 对照） | B | MoGe 支路单独 **1.03 s**（稳态）/ 7.1 s（含首次权重加载） |
| 峰值 VRAM 增量（阶段 A / B） | A+B | A **7.00 GiB**（冷启，含 SDXL 加载）/ B **0.45 GiB** |
| `batch_size=4` 是否 OOM | A | **不 OOM**（19.2 s，增量 9.66 GiB）；batch 8 亦不 OOM（37.5 s） |
| 上传+下载往返开销 | 拆分点 | 上传 46–92 ms（2 MB）/ 下载 6 张 ≈ 6 MB；合计 **< 0.2 s** |

**关键结论**

- **端到端十秒量级**：热 ≈ **12.6 s**，冷 ≈ **16.7 s**。⇒ UI 不需要长任务式进度条，
  1.0 s 轮询 + 简单进度指示就够。
- **D2 的收益要修正**：实测稳态只省 **≈ 1.03 s/轮（≈ 阶段 B 的 20%）**，
  外加一次性省 ~6 s 权重加载、~1.4 GiB 显存、以及去掉 MoGe 权重依赖。
  早先"省一次 ViT-L 推理"的说法夸大了提速收益；**主要价值在去依赖与简化图**。
- **批处理效率良好**：单张摊薄 7.3 s（batch1）→ 4.8 s（batch4）→ 4.7 s（batch8）。
- **OOM 预检阈值可放宽**：23.88 GiB 上 batch 8 峰值常驻 14.84 GiB。但冷启需 ~7 GiB 余量，
  提交前仍应查 `/system_stats` 的 `vram_free`。

> ⚠️ **测量方法论**（照做，否则数字不可比）：ComfyUI 有**节点级结果缓存**，图不变时重提交会直接命中，
> 阶段 B 实测会从 5.1 s 掉到 1.0 s；VRAM 要区分**增量**（`free_before - min_free`）与**常驻**
> （`total - min_free`，含 ComfyUI 缓存的其它模型）；显存采样必须独立于 `/history` 轮询且 ≥5 Hz。
> 详见 `docs/M0-baseline.md` §6。

---

## 6. UE 侧实现设计

### 6.1 贴图导入

**方案**：传统 `UAssetImportTask` 路径（PNG → `UTextureFactory`）。

```cpp
UAssetImportTask* Task = NewObject<UAssetImportTask>();
Task->Filename            = AbsolutePngPath;      // 先落盘到 Saved/ComfyTextureStudio/Cache/<GUID>/
Task->DestinationPath     = TEXT("/Game/ComfyTextures/Sand_Coarse_01");
Task->DestinationName     = TEXT("T_Sand_Coarse_01_BC");
Task->bAutomated          = true;
Task->bReplaceExisting    = true;
Task->bSave               = true;                 // 导入后直接落盘
Task->Options             = nullptr;              // 走工厂默认，不手搓 Options 对象
IAssetTools::Get().ImportAssetTasks({Task});
```

导入后取 `Task->GetObjects()` 里的 `UTexture2D*`，设置属性，再 `MarkPackageDirty` + 保存。

> **为什么不用 Interchange**：Interchange 对 PNG 的支持路径在 5.6/5.8 间仍有差异，且默认属性覆盖需要额外写 Pipeline Stack。本插件的属性设置是**固定且少量**的，`UAssetImportTask` + 直接写 `UTexture2D` 字段更短、更可控、跨 5.6/5.8 一致。Interchange 留作后续可选路径。

### 6.2 材质图构建

使用 `UMaterialEditingLibrary`（`MaterialEditor` 模块）。生成结构与落点：

```
M_Sand_Coarse_01  (Default Lit / Opaque / TwoSided=false)
  │
  TexCoord(0) ──Multiply(Tiling)──┬─────────────────────────────┐
                                  │                             │
                                  ▼                             ▼
                      T_.._H (Grayscale)              BumpOffset.Coordinate
                            │                                │
                            └──> BumpOffset.Height           │
                                 HeightRatio = BumpDepth     │
                                                             │
                        BumpOffset (UV out) ──┬──────────────┼──────────────┐
                                              ▼              ▼              ▼
                                        T_.._BC         T_.._N          T_.._ORM
                                       (Color,sRGB)  (Normal smp)   (LinearColor,非sRGB)
                                              │              │          ├ R ──> AmbientOcclusion
                                              │              │          ├ G ──> Roughness
                                              │              │          └ B ──> Metallic
                                              ▼              ▼
                                          BaseColor      NormalStrength
                                                              │
                                                              ▼
                                                            Normal
```

**Parallax 的接线顺序是本设计里最容易搞错的一环**：`BumpOffset` 的输出是 **UV**，要接到**所有**贴图采样的 `Coordinates`（含 height 自己以外的三张）。如果只有 BaseColor 接了 BumpOffset 而 normal/ORM 用的是原始 UV，视差会在三者之间错位，表现为"贴图看着有深度但法线不跟着动"。

**实现要点**：

| 项 | 做法 |
|---|---|
| 节点创建 | `UMaterialEditingLibrary::CreateMaterialExpression(M, Class, X, Y)` |
| 贴图节点类型 | 用 `TextureSampleParameter2D`（**参数化**）而非 `TextureSample`，这样材质实例能换贴图 |
| SamplerType | albedo→`SAMPLERTYPE_Color`；ORM→`SAMPLERTYPE_LinearColor`；normal→`SAMPLERTYPE_Normal`；height→`SAMPLERTYPE_Grayscale` |
| 通道引线 | `ConnectMaterialProperty(SampleExpr, TEXT("R"/"G"/"B"), MP_AmbientOcclusion / MP_Roughness / MP_Metallic)` |
| 主输出 | `ConnectMaterialProperty(Albedo, TEXT("RGB"), MP_BaseColor)`、`(Normal, TEXT("RGB"), MP_Normal)` |
| 标量参数 | `RoughnessScale` / `NormalStrength` / `BumpDepth` / `Tiling`，全部建为 ScalarParameter 并连线 |
| 材质实例 | `UMaterialInstanceConstantFactoryNew` → `MI_<Name>`；只设可调标量，不改贴图 |
| 编译 | `UMaterialEditingLibrary::RecompileMaterial(M)`，然后检查 `M->GetCompileErrors()` 为空 |
| UV 平铺 | `TextureCoordinate(UTiling=0)` → `Multiply(Tiling)` → 进 `BumpOffset.Coordinate`（再由 BumpOffset 分发到各采样） |
| 视差 | `UMaterialExpressionBumpOffset`：`Coordinate`=Tiling 后的 UV，`Height`=height 采样，`HeightRatioInput`=`BumpDepth` 标量参数（默认建议 `0.02`，需实测手感）；其 UV 输出接三张交付贴图的 `Coordinates` |
| Nanite 真位移 | **v1 不做**（Q4）。列为 M4 可选项，需要材质勾选 Enable Nanite Tessellation 并把高度接 World Displacement |

> ⚠️ `UMaterialEditingLibrary` 的连线 API 对**输出/输入名称大小写敏感**（`"R"` 不是 `"r"`）。若连不上，回退到直接操作 `Material->GetEditorOnlyData()->ExpressionCollection` 并手工填 `FExpressionInput`——PRD 把它列为实现期需以引擎源码核实的点。

### 6.3 导入属性表（**逐项照做，这是 G3 的验收依据**）

| ComfyUI 产物 | UE 资产名 | 默认导入 | `CompressionSettings` | `SRGB` | `FlipGreenChannel` | `LODGroup` | `AddressX/Y` | `MipGenSettings` |
|---|---|---|---|---|---|---|---|---|
| `albedo` | `T_<N>_BC` | ✅ | `TC_Default` (BC7) | **true** | false | `TEXTUREGROUP_World` | `TA_WRAP` | `TMGS_FromTextureGroup` |
| `normal` | `T_<N>_N` | ✅ | `TC_Normalmap` (BC5) | **false** | **true** ← D3 | `TEXTUREGROUP_WorldNormalMap` | `TA_WRAP` | `TMGS_FromTextureGroup` |
| `orm` | `T_<N>_ORM` | ✅ | `TC_Masks` | **false** | false | `TEXTUREGROUP_World` | `TA_WRAP` | `TMGS_FromTextureGroup` |
| `height` | `T_<N>_H` | ✅ | `TC_Grayscale` (BC4) | false | false | `TEXTUREGROUP_World` | `TA_WRAP` | `TMGS_FromTextureGroup` |
| `ao` | `T_<N>_AO` | ❌ orphan | `TC_Grayscale` | false | false | `TEXTUREGROUP_World` | `TA_WRAP` | `TMGS_FromTextureGroup` |
| `roughness` | `T_<N>_R` | ❌ orphan | `TC_Grayscale` | false | false | `TEXTUREGROUP_World` | `TA_WRAP` | `TMGS_FromTextureGroup` |

> **orphan 的含义（Q3）**：`ao` / `roughness` 仍会被 ComfyUI 产出、插件也会下载（它们在 `/history` 的 outputs 里），但**默认不导入 UE、不接材质**——ORM 合并图已含这两个通道。仅在用户显式勾选"额外导入 AO/Roughness 单图"时才导入，且仍不接入材质节点。
> manifest 里对应 `importByDefault: false` + `orphanReason`。

三个最常出错的点，必须写进导入后自检：

1. **`SRGB=false`**：ORM 若被当 sRGB 读，AO/Roughness/Metal 会被 gamma 曲线整体拉偏——渲染结果"说不上哪儿不对"。
2. **`FlipGreenChannel=true`**：工作流输出的 `normal` 是 **OpenGL 约定**（工作流 note 明确写了"OpenGL；UE 用 directx"）。不翻绿通道的表现是光照方向反了，凹凸看起来像凹坑。
3. **`AddressX/Y = TA_WRAP`**：贴图是严格四方连续的，一旦被设成 `TA_Clamp`，平铺立刻露边。

> **建议顺带**：把 `bFlipGreenChannel` 做成面板可勾选（默认开）。这样如果哪天把 ComfyUI 的 `50.directx` 打开、输出改成 DirectX，UE 侧翻回来即可，不用重新导入。

### 6.4 法线约定核对（D3 的执行与验证）

工作流 note 记录：通道符号是**实测标定**的（DSINE 相机系 `(right, down, front)` → OpenGL 需**只翻转红通道**，`35.flip_red=true`），并且明确警告 **`std` / `tilt` / `|n_xy|` 这类标量统计量对通道翻转完全不变**——所以"看起来差不多"不能证明符号对。

**⇒ 插件的验收不能靠肉眼**。M0 期间需要一次**单光源对照**：把生成的 `T_<N>_N` 贴到一个已知朝向的平面上，配一盏从左上方来的平行光，与 albedo 里"左上受光"的明暗方向比对；方向一致才算通过。这一条写进 M2 的验收清单。

---

## 7. UI / UX 设计

### 7.1 入口

- 主菜单：`Window → Comfy Texture Studio`
- 工具栏（`LevelEditor.LevelEditorToolBar.PlayToolBar`）加一个图标按钮
- Nomad Tab（可自由停靠/浮出），`FGlobalTabmanager::RegisterNomadTabSpawner`
- 另外提供一个**右键菜单入口**：Content Browser 文件夹右键 → "从此处生成材质贴图"（自动把该路径作为资产落点）— P1

### 7.2 面板布局（单页 · 左右分栏 · 三态推进）

```
┌ Comfy Texture Studio ─────────────────────────────── ● 已连接 127.0.0.1:8000 ── ⚙ ─┐
├──────────────────────────────────┬────────────────────────────────────────────────┤
│ ① 生成基础贴图                    │  预览区                                        │
│                                  │  ┌────────────────────┐ ┌────────────────────┐ │
│ 材质描述                          │  │                    │ │                    │ │
│ ┌──────────────────────────────┐ │  │      原图           │ │   2×2 平铺自检      │ │
│ │ coarse pebbly sand, beige... │ │  │                    │ │                    │ │
│ └──────────────────────────────┘ │  └────────────────────┘ └────────────────────┘ │
│ ⓘ 前后固定 tag 来自 LoRA 训练集，  │  1024×1024 · 30 steps · seed 20261013          │
│   请勿修改                        │  ┌──────┐┌──────┐┌──────┐┌──────┐             │
│                                  │  │ #1   ││ #2   ││ #3   ││ #4   │  历史候选    │
│ ▸ 正向提示词（完整，只读切换）      │  └──────┘└──────┘└──────┘└──────┘             │
│ ▸ 负向提示词（折叠）               │                                                │
│                                  │  [ 换种子重出 ]  [ 丢弃 ]  [ ✓ 用这张继续 ]      │
│ 种子  [20261013] [🎲] [🔒]        │                                                │
│ 步数  [30]   CFG [6.5]           │                                                │
│ 采样器 [dpmpp_2m ▾] 调度 [karras▾]│                                                │
│ 尺寸  [1024]×[1024]  ☑ 锁定 1:1   │                                                │
│ LoRA  [sdxl-texture-sand-v2 ▾]   │                                                │
│ 强度  [1.00] / [1.00]             │                                                │
│                                  │                                                │
│ [        ⚡ 生成基础贴图        ]  │                                                │
│  ━━━━━━━━━━━━░░░░░ 采样中 18/30   │                                                │
├──────────────────────────────────┴────────────────────────────────────────────────┤
│ ② 生成 PBR 全套                                               ▸ 参数（按需展开）│
│ 输入：阶段A 产物 (cts_a3f9....png)  [ 生成 PBR 全套 ]  几何反演 · 无 MoGe 支路     │
│ ━━━━━━━━━━━━━━━━━━━━━━━━━━ 完成                                                   │
│  ┌────────┐┌────────┐┌────────┐┌────────┐┌────────┐┌────────┐                    │
│  │ albedo ││ normal ││ height ││  orm   ││  ao    ││roughness│  ← 每张可点开 2×2  │
│  │  ✅导入 ││  ✅导入 ││  ✅导入 ││ ✅导入  ││ 灰·不导入││ 灰·不导入│                 │
│  └────────┘└────────┘└────────┘└────────┘└────────┘└────────┘                    │
├───────────────────────────────────────────────────────────────────────────────────┤
│ ③ 导入 UE                                                                          │
│ 命名  ○ 模板  M_{material}_{grain}_{index}  [Sand] [Coarse] → M_Sand_Coarse_01     │
│       ● 手填  [Sand_Coarse_01]                                                     │
│ 资产路径 [/Game/ComfyTextures/Sand_Coarse_01]   ⓘ 命名冲突会报错，不会静默覆盖       │
│ ☑ 生成材质实例(MI)  ☑ 翻转法线绿通道  ☑ 高度接入 BumpOffset  ☐ 额外导入 AO/Roughness │
│ [                        📦 导入并创建材质                        ]                 │
│ ────────────────────────────────────────────────────────────────────────────────── │
│ ✓ 4 张贴图已导入  ✓ M_ 已编译(0 错误)  ✓ MI_ 已创建  ⚠ AO/Rough 未导入(ORM 已含)   │
└───────────────────────────────────────────────────────────────────────────────────┘
```

> Q6 的命名双通道在 UI 上就是那组单选：**模板模式**由材质描述自动拼出 `{material}_{grain}_{index}`（`{index}` 自动避让已有资产），**手填模式**直接覆盖。两种模式下方都实时预览最终资产路径。Q5 的"不支持排队"体现在：面板同一时间只有一个任务，没有队列列表。

### 7.3 状态机与 UI 门控

| 状态 | 可点 | 禁用 | 文案 |
|---|---|---|---|
| `Disconnected` | 重试连接 | 全部生成按钮 | "无法连接 ComfyUI（127.0.0.1:8000）。请确认已启动。" |
| `PreflightFailed` | 重新预检 | 生成按钮 | 缺失项清单（§2.2 F1） |
| `Idle` | ① 生成 | ②③ | "就绪" |
| `StageA_Running` | 取消 | 全部其他 | 进度 + 已耗时 |
| `StageA_Ready` | 换种子重出 / ✓继续 / 丢弃 | ② | "满意就继续，不满意换种子重出" |
| `StageB_Running` | 取消 | — | 节点级进度（P1） |
| `StageB_Ready` | ③ 导入 | ②（可重跑） | — |
| `Importing` | — | — | — |
| `Done` | 全部 | — | 导入报告（§7.4） |
| `Failed` | 重试 / 查看日志 | — | 中文错误 + 建议动作 |

### 7.4 导入报告示例

```
导入报告 · Sand_Coarse_01 · 2026-10-12 21:04
──────────────────────────────────────────────
✓ T_Sand_Coarse_01_BC     BC7    sRGB=on    Wrap   → BaseColor
✓ T_Sand_Coarse_01_N      BC5    sRGB=off   FlipG  Wrap   → Normal
✓ T_Sand_Coarse_01_ORM    Masks  sRGB=off          Wrap   → AO / Roughness / Metallic
✓ T_Sand_Coarse_01_H      BC4    sRGB=off          Wrap   → BumpOffset 视差
✓ M_Sand_Coarse_01        已编译，0 错误 0 警告
✓ MI_Sand_Coarse_01       已创建
─ 未导入单独 AO / Roughness 图（Q3：ORM 合并图已含）
──────────────────────────────────────────────
[ 复制报告 ]  [ 在 Content Browser 中显示 ]
```

---

## 8. 配置项

`UComfyTextureStudioSettings`（`DefaultGame.ini`）：

| 键 | 默认 | 说明 |
|---|---|---|
| `ServerUrl` | `http://127.0.0.1:8000` | **注意不是 8188** |
| `RequestTimeoutSeconds` | `60` | 单次 HTTP 超时；轮询间隔另设 |
| `PollIntervalSeconds` | `1.5` | `/history` 轮询间隔 |
| `JobTimeoutSeconds` | `900` | 整任务上限，超过判失败 |
| `AssetRootPath` | `/Game/ComfyTextures` | 资产落点根 |
| `CacheDir` | `Saved/ComfyTextureStudio/Cache` | 下载的 PNG 中转目录 |
| `StageAWorkflowFile` | `Config/Workflows/stage_a_base_texture.json` | |
| `StageBWorkflowFile` | `Config/Workflows/stage_b_pbr.json` | |
| `WorkflowManifestFile` | `Config/Workflows/workflows.manifest.json` | |
| `DefaultFlipGreenChannel` | `true` | D3 |
| `DefaultImportHeight` | `true` | 高度图参与 BumpOffset（Q4） |
| `DefaultImportOrphanMaps` | `false` | Q3：AO/Roughness 默认不导入 |
| `DefaultCreateMaterialInstance` | `true` | |
| `DefaultBumpDepth` | `0.02` | BumpOffset 的 `HeightRatio` 初值，需实测手感后固化 |
| `PreserveDownloadedPng` | `false` | 关则导入后清缓存 |
| `VerboseHttpLog` | `false` | 打印完整请求/响应体，调试用 |

**命名相关（Q6）**：

| 键 | 默认 | 说明 |
|---|---|---|
| `NamingMode` | `Template` | `Template` / `Manual` |
| `MaterialNameTemplate` | `M_{material}_{grain}_{index}` | 材质资产名 |
| `TextureNameTemplate` | `T_{material}_{grain}_{index}_{suffix}` | 贴图资产名 |
| `FolderNameTemplate` | `{assetRoot}/{material}_{grain}_{index}` | 资产文件夹 |
| `IndexPadding` | `2` | `{index}` 补零位数 → `01` |
| `IndexConflictPolicy` | `Error` | `Error` / `AutoIncrement`（手填模式下冲突时的行为，**不提供静默覆盖**） |

**ComfyUI 侧定位（Q8 · 预检与一键复制用）**：

| 键 | 默认 | 说明 |
|---|---|---|
| `ComfyUserDataDir` | `C:/Users/1142/AppData/Local/Programs/ComfyUI-data` | **必须是这个，不是 comfy-cli 的 default_workspace**（§5.1 的警告） |
| `ComfyCustomNodesDir` | 由上一个推导 | 一键复制节点包的目标目录 |
| `ComfyCliPath` | 空 | 填了才能用"尝试启动 ComfyUI" |
| `DsineWeightExpectedBytes` | `291298581` | 预检做截断校验用 |

---

## 9. 错误处理与可观测性

### 9.1 失败分类与处置

| 类别 | 判定 | 面向用户的话 | 建议动作 |
|---|---|---|---|
| 未连接 | `/system_stats` 失败 | "ComfyUI 未在 127.0.0.1:8000 响应" | 启动 ComfyUI；检查端口（**默认桌面版是 8000 不是 8188**） |
| 缺节点包 | `/object_info` 无 `SeamlessDSINENormal` | "缺少自定义节点包 comfyui_seamless_texture" | 从插件 `Resources/comfyui_seamless_texture/` **一键复制**到 `ComfyUI-data/custom_nodes/` 后重启 |
| 缺 controlnet_aux | `/object_info` 无 `DSINE_Normal_Map_Preprocessor` 等 | "缺少 comfyui_controlnet_aux" | ComfyUI Manager 安装（`SeamlessDSINENormal` 依赖它提供 DSINE 架构） |
| 缺 dsine.pt | 权重文件不存在 | "缺少 DSINE 权重，预期 <路径>" + 下载命令 | 按提示下载，**必须 291,298,581 B** |
| dsine.pt 截断 | 字节数 ≠ 291,298,581 | "DSINE 权重文件不完整（<实际> B，应为 291,298,581 B）" | 重新下载（镜像曾送出 29 MB 残件） |
| 缺模型 | 枚举中无目标文件名 | "缺少模型 <名>，当前可用：<枚举>" | 下载到 ComfyUI `models/` 对应目录 |
| 提交校验失败 | `/prompt` 返回 `node_errors` 非空 | 逐节点列出 | 通常是参数超范围或模型缺失 |
| 节点执行失败 | `/history` 的 `status.status_str=="error"` | "节点 [35 泊松积分求高度] 报错：<异常>" | 附 traceback 尾部（可复制） |
| **ComfyUI 崩溃（OOM）** | HTTP **连接中断**/超时（不是节点错误） | "ComfyUI 进程疑似崩溃（显存不足）" | 降 batch_size / 降尺寸；提示 `/free` |
| 下载失败 | `/view` 非 200 | 展示 HTTP 码 + URL | 重试 |
| 导入失败 | `ImportAssetTasks` 未产出资产 | "导入失败：<原因>" | 检查资产路径是否只读/重名 |
| 材质编译失败 | `GetCompileErrors()` 非空 | 逐条列出 | 附错误行 |

> OOM 那一行是**必须区分的**：工作流 note 已记录"大分配可能通过校验、然后在执行期把 ComfyUI 整个进程干掉"，表现为连接中断而非节点错误——若归类为"超时"会误导用户去加超时而不是降配置。

### 9.2 日志

| 级别 | 内容 |
|---|---|
| `Log` | 状态迁移、prompt_id、文件路径、耗时 |
| `Warning` | 预检缺项、导入属性覆盖、可选产物缺失 |
| `Error` | HTTP 失败、节点错误、导入/编译失败 |
| `Verbose`（开关） | 完整请求/响应 JSON（含工作流图），用于对照 ComfyUI 日志 |

日志类别：`LogComfyTextureStudio`，可 `LogComfyTextureStudio Verbose` 开。

### 9.3 取消

`POST /interrupt` 后**仍需**把本地状态机切到 `Cancelled` 并释放 UI；ComfyUI 的 `interrupt` 只作用于当前执行节点，若任务还在 pending 队列里则需要 `/queue` 取出后删除条目。两条路径都要走。

---

## 10. 非功能需求

| 项 | 要求 |
|---|---|
| 引擎兼容 | 主 `5.8.3`（`CompatibleChangelist 55116800`）；`5.6.1` 通过 `BuildId`/`EngineVersion` 与 `#if ENGINE_MAJOR_VERSION` 守卫 |
| 平台 | Win64（开发机）。代码不写平台特定路径，PNG 中转用 `FPaths::ProjectSavedDir()` |
| 外部依赖 | **零第三方库**。HTTP 用 `FHttpModule`，JSON 用 `FJsonSerializer`，WebSocket 用 `FWebSocketsModule`，图片解码用 `FImageUtils` / `ImageWrapper` |
| 打包 | `Editor` 模块 `Type: Editor`，不进 Shipping。Runtime 模块 `Type: Runtime` 但插件 `"RenderOnlyWhenEditor"` 语义上不适用——设置为 `"EnabledByDefault": true` 且 Editor 模块 `"Type": "Editor"` |
| UI 线程 | 任何 UE 对象操作都在游戏线程（§3.3） |
| 内存 | 下载的 PNG 与中间纹理及时释放；导入后清缓存（可配保留） |
| 资产安全 | `bReplaceExisting=true` + 明确的路径归属（`AssetRootPath` 下），且在覆盖已有资产前弹确认 |
| 国际化 | v1 只做中文文案，但全部走 `LOCTEXT`，便于后续英文化 |

---

## 11. 里程碑与验收

### M0 · 接口验证（先跑通，再写插件） — ✅ 已完成 2026-10-12

| # | 任务 | 通过标准 | 结果 |
|---|---|---|---|
| T1 | 用 `POST /prompt` 手搓提交阶段 A 的 API 图 | 出图，`/history` 取到 `sand_v2_lit_*.png` | ✅ 冷 11.4s / 热 7.3s |
| T2 | `POST /upload/image` 上传该图 + 提交裁剪版阶段 B 图 | 出 6 张 PBR 图 | ✅ 6/6 齐备，5.07s |
| T3 | 实测 §5.3 全部耗时/显存项 | 表回填完毕 | ✅ 见 §5.3 + `docs/M0-baseline.md` |
| T4 | 写 `Tools/export_api_workflows.py`：前端图 → 两张 API 图 + manifest | 两张图均通过 `comfy validate`（结构→schema→live POST 三层全绿） | ✅ 9 / 15 节点，`valid=true`，live 跑通 |
| T5 | 反向可达性脚本固化为 `Tools/derive_keepset.py` | 任意改动后能重算 keep/drop 集合并 diff | ✅ `--diff --strict` 三集合全绿，退出码 0 |
| T6 | 法线约定对照 | 光向一致 | ⚠️ **改为数值判定**：几何自洽 + `directx` 开关取反关系已确证；**渲染器对照顺延到 M2** |

**M0 额外产出**

- `Tools/run_live_check.py` —— 最小 REST 客户端（6 个端点），**即 M1 的参考实现**；
  含冷/热分离、批量覆盖、显存高频采样
- `Tools/check_normal_convention.py` —— 几何自洽性 + 无缝判据（z 检验）+ `directx` 开关语义
- `docs/M0-baseline.md` —— 全部实测数据与测量方法论（含 4 个已踩的坑）
- 两张可用 API 图：`Config/Workflows/stage_a_base_texture.json`（9 节点）、
  `stage_b_pbr.json`（15 节点，含动态 `LoadImage` #75）
- manifest 新增 `apiWorkflowFile` / `inputNodeId` / `inputNodeToken` / `apiNodeCount` 字段

**M0 发现的两个必须带进 M1 的实现要点**

1. **ComfyUI 有节点级结果缓存** —— 图不变时重提交会直接命中。插件里"重跑"必须换输入文件名或改至少一个上游参数，
   否则用户会以为"跑得很快"而实际没重算。
2. **`comfy validate` 不检查文件存在性** —— 阶段 B 带占位符 `__CTS_RUNTIME_INPUT__.png` 也能 `valid=true`。
   所以**schema 层不能替代 live 层**，插件提交前的预检必须自己确认输入文件已上传。

> T4/T5 遵循既有硬标准：**keep/drop 集合一律由反向可达性程序化推导，绝不手写 id 列表**；工作流改动走 **结构 → schema → live POST /prompt** 三层验证，全绿才算通过。

### M1 · 插件骨架 + 后端打通（P0 最小闭环） — ✅ 已完成 2026-10-13

| # | 任务 | 通过标准 | 结果 |
|---|---|---|---|
| — | `ComfyTextureStudio` Runtime 模块：HTTP 客户端、工作流装载、槽位注入、任务状态机 | 能编译进 UE 5.8 | ✅ `UnrealEditor-ComfyTextureStudio.dll` 编译链接通过 |
| — | 命令行式触发（不做 UI） | 从命令行跑完整两阶段 + 下载 PNG | ✅ `-run=ComfyTextureStudio`（默认 LoadingPhase 即可被找到） |
| — | **验收**：一次调用产出 6 个 PNG 到 `Saved/.../Cache/<GUID>/` | 6/6 齐备 | ✅ **6/6**（4 交付 + 2 可选），产物 ≥ 336 KB/张，目视结构正确 |

**M1 实测**：阶段 A **12.33 s**（真算）/ **1.55 s**（命中节点级缓存）· 阶段 B **4.58 s** ·
commandlet 端到端（含 UE 启动）**18.34 s** · 增量编译 ~6–10 s。

**M1 产出的可复用经验**（完整版见 [`docs/M1-report.md`](M1-report.md)）：

1. **`-NoUBA` 是必需的**：不关 Unreal Build Accelerator 时，它在生成 PCH 阶段被文件系统权限拒绝，
   报 `fatal error C1083: 无法打开预编译头文件 ... 拒绝访问` —— 像代码问题，其实是构建加速器。
2. **commandlet 里没有 tick 循环**：`FHttpManager` 继承 `FTSTickerObjectBase`，必须手工
   `FHttpManager::Tick()`（`FComfyHttpClient::Pump()`），否则请求永远不完成。
3. **`FJsonObject::Values` 的键类型在 5.8 是 `UE::FSharedString`**：`Find(FString)`/`GetKeys(TArray<FString>)`
   都编不过；单个字段改走 `HasField`/`TryGetField`。
4. **`/view` 404 多半是转义问题**：见 §4.1 的 ⚠ 条。
5. **缓存命中会被误读成"工具很快"**：同图同种子重提交，阶段 A 从 12.3 s 掉到 1.5 s。
   commandlet 已提供 `-Seed=random`；**M2 的 UI 必须把"换种子/失效缓存"做成显式选项**。
6. **ComfyUI 的真实 output 目录不在 `ComfyUI-data` 下**（本机是 `D:/Documents/comfyUI_documents/outputs`），
   与 `custom_nodes` 是分别配置的 —— 预检里"检查 output 目录"这类动作不能靠猜路径。

### M2 · Slate 面板 + 两阶段向导（P0）

- 完整 UI（§7.2）、状态门控、缩略图、2×2 自检、错误映射
- **验收**：不打开 ComfyUI 前端，面板内完成 提示词→审阅→PBR→导入→材质；§6.3 属性表逐项核对通过；法线单光源验证通过

### M3 · 体验完善（P1）

- WebSocket 实时进度、取消、导入报告、参数面板、Content Browser 右键入口

### M4 · 增强（P2）

- 批量候选（走 `batch_size` 单次提交）· 参数预设导出/导入 · 任意图→PBR · Nanite Tessellation 真位移（Q4 的可选项）· `SeamlessUpscale` 接入
- 仍然**不含任务队列**（Q5）：并发 / 排队 / 多材质批量是 v1 之外的事

### 全局验收清单

- [ ] 单提示词 → 4 张交付贴图 → 可渲染材质，全程零手工文件操作
- [ ] 阶段 A 跑完后**不**触发阶段 B 即可关闭面板（G2）
- [ ] 全部导入贴图的压缩 / `sRGB` / 绿通道 / 寻址 **全部**符合 §6.3
- [ ] `ao` / `roughness` **未**被导入、未接材质（Q3）
- [ ] 高度图**确实**通过 `BumpOffset` 影响了三张交付贴图的 UV（不是只接了 BaseColor）（Q4）
- [ ] 命名模板模式与手填模式都能产出正确资产名；手填冲突时**报错而非覆盖**（Q6）
- [ ] 换一个非沙地 LoRA 能直接在下拉里选到并生效（Q7）
- [ ] 预检的"一键复制节点包"写到了 `ComfyUI-data/custom_nodes/`，复制后节点真的可用（Q8）
- [ ] 材质编译 0 错误 0 警告，平铺 4×4 无可见接缝
- [ ] 错误分类能区分"节点报错"与"进程崩溃"（§9.1）
- [ ] 已在 5.6 与 5.8 上各编译一次

---

## 12. 风险与已确认决策

### 12.1 风险

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| R1 | 前端→API 格式转换出错（widget 顺序/schema 漂移） | 静默产出错误参数 | 严格走 `/object_info`；`comfy validate` 拦截；manifest 记版本 |
| R2 | ComfyUI 升级改节点行为 | 链路失效 | M0 锁 `v0.38.2`；升级前重跑 T1/T2/T4 |
| R3 | 高度/粗糙度走 **8-bit PNG** 有精度损失 | 置换/粗糙度出现 banding | M0 实测；必要时改用 16-bit 输出节点，或接受（材质量级 1024² 通常够） |
| R4 | 法线绿通道约定判断错 | 凹凸方向反，且**肉眼不易察觉** | §6.4 单光源对照，写进 M2 验收 |
| R5 | `UMaterialEditingLibrary` 连线 API 名称/签名在 5.6↔5.8 有差异 | 编译失败或连线失败 | 实现期以引擎源码核实；准备手工填 `FExpressionInput` 的回退路径 |
| R6 | 阶段 B 独立成图后，`LoadImage` 走 8-bit PNG 再进 DSINE | 相对原图直连多一次量化 | M0 对比：拆分版 vs 原图整跑的 `normal` 差异；若显著则改为同会话复用 latent（代价是失去"先审阅"能力，不推荐） |
| R7 | 长任务（>15min）期间用户关闭面板/切关卡 | 回调访问已销毁对象 | 状态机持有 `TWeakPtr`，回调先校验 |
| R8 | 大 batch + 高分辨率 OOM | ComfyUI 崩溃，表现为连接中断 | 提交前查 `/system_stats` 的 `vram_free` 给提示 |
| R9 | `BumpOffset` 的 `BumpDepth` 没有物理依据，纯手感 | 视差过强/过弱，或与法线打架 | `0.02` 只是初值，M2 期间用实际平面实测几个量级后固化进 `DefaultBumpDepth` |
| R10 | `Resources/` 里的节点包副本与上游 `comfyUI_tools` 漂移 | 用了旧节点行为，排查时误判 | `Resources/README.md` §4 有 diff→同步流程；同步后要更新版本号与 manifest |
| R11 | "一键复制节点包"写到错误的 `custom_nodes` | 复制成功但节点仍缺，用户困惑 | 默认值写死在 `ComfyUI-data`，并在复制后**回读 `/object_info` 验证**节点真的出现了 |

### 12.2 已确认决策（原"未决问题"现已全部关闭）

| # | 问题 | **决策** | 落地位置 |
|---|---|---|---|
| Q1 | 插件名 | `ComfyTextureStudio` | 全文 |
| Q2 | 保留 `44`（`normal_dsine` 调试输出） | **不保留** | 阶段 B 15 节点；裁剪 20 节点 |
| Q3 | ORM 合并 vs AO/Roughness 分开 | **合并图** | §6.3 orphan 行；§6.2 材质图；F5 |
| Q4 | 高度图用法 | **BumpOffset 视差** | §6.2 视差接线；§8 `DefaultBumpDepth` |
| Q5 | 多材质排队 | **不支持**，单任务 | §1.4 非目标；F8 限定为 `batch_size` 单次提交 |
| Q6 | 命名规则 | **模板 + 手填 双通道** | 新增 F5b；§7.2 命名区；§8 命名配置 |
| Q7 | 换 LoRA 通道 | **预留**，枚举从 `/object_info` 实时拉取 | §4.3 `lora` 行；F2 LoRA 选择 |
| Q8 | 节点包分发 | **随仓库放 `Resources/`** + README 记录依赖链 | `Resources/README.md`；§2.2 F1 一键复制 |

---

## 附录 A · 工作流全量节点清单（43 节点）

分组取自原图 `groups`。`归属` 列 = 拆分后归属：

| id | 类型 | 标题 | 原图分组 | 拆分归属 |
|---|---|---|---|---|
| 1 | `CheckpointLoaderSimple` | — | A · 生成受光渲染图 | A |
| 2 | `LoraLoader` | v2 LoRA（受光渲染版） | A | A |
| 3 | `CLIPTextEncode` | Positive · 9 个绑死 tag + 材质描述 | A | A |
| 4 | `CLIPTextEncode` | Negative | A | A |
| 5 | `EmptyLatentImage` | 1024 = SDXL 原生 + MoGe 档位 9 的甜点 | A | A |
| 6 | `SeamlessModelPadding` | 关键：UNet 卷积改 circular → 环面拓扑 | A | A |
| 7 | `KSampler` | — | A | A |
| 8 | `SeamlessVAEDecode` | Seamless VAE Decode | （未分组） | A（B 中以 `LoadImage` 替代） |
| 10 | `MarkdownNote` | 读我 · 方法与实测依据 | （未分组） | 弃（UI-only） |
| 20 | `SaveImage` | 受光渲染图（中间产物，不是 albedo） | （未分组） | **A 输出** |
| 21 | `TilePreview` | Tile Preview · 2×2 | （未分组） | 弃（UE 侧自做） |
| 30 | `LoadMoGeModel` | MoGe-2（自带 normal 头） | B · 几何反演 | 弃（对照支路） |
| 31 | `SeamlessMoGeInference` | 周期补边 MoGe（pad 256 · token 按面积缩放） | B | 弃 |
| 32 | `MoGeRender` | 法线（OpenGL；UE 用 directx） | B | 弃 |
| 33 | `MoGeRender` | depth（仅参考，HF 只有 4–12%） | B | 弃 |
| 34 | `SaveImage` | 参考：MoGe depth | B | 弃 |
| 35 | `SeamlessDSINENormal` | ★ 主几何源：DSINE（纯卷积）· fov 3° · pad 256 | （未分组） | **B** |
| 36 | `SeamlessFlattenNormal` | 整平全局倾斜（坡度交给 bump_strength 一个旋钮） | （未分组） | **B** |
| 37 | `SaveImage` | 对照：MoGe 直出法线（未整平，环绕 2.70/3.07） | B | 弃 |
| 38 | `SeamlessFlattenNormal` | 对照支路：MoGe 同样整平 | B | 弃 |
| 39 | `SaveImage` | 对照：MoGe 整平后法线 | B | 弃 |
| 40 | `SeamlessHeightFromNormal` | 周期泊松积分 + 低频剔除 → height | B | **B** |
| 41 | `SaveImage` | height | 输出 | **B 输出** |
| 42 | `TilePreview` | — | 2×2自检 | 弃 |
| 43 | `SaveImage` | 2×2 自检 · height | 2×2自检 | 弃 |
| 44 | `SaveImage` | 主几何：DSINE 整平后法线（环绕 1.12/1.64） | 输出 | 弃（Q2：不保留调试输出） |
| 50 | `SeamlessNormalFromHeight` | 主法线：从 height 重导 | B | **B** |
| 54 | `SaveImage` | normal（主输出 = 严格四方连续） | 输出 | **B 输出** |
| 55 | `TilePreview` | — | 2×2自检 | 弃 |
| 56 | `SaveImage` | 2×2 自检 · normal | 2×2自检 | 弃 |
| 60 | `SeamlessAOFromHeight` | AO（多尺度空腔） | B | **B** |
| 61 | `SeamlessRoughnessFromHeight` | roughness（高度微起伏代理量） | B | **B** |
| 62 | `PackPBRChannels` | ORM（砂石是电介质 → metal = 0） | B | **B** |
| 63 | `SaveImage` | ao / roughness / orm | 输出 | **B 输出** |
| 64 | `SaveImage` | — | 输出 | **B 输出** |
| 65 | `SaveImage` | — | 输出 | **B 输出** |
| 66 | `TilePreview` | — | 2×2自检 | 弃 |
| 67 | `SaveImage` | 2×2 自检 · orm | 2×2自检 | 弃 |
| 70 | `SeamlessDelightAlbedo` | 去光照 albedo（低频平场除法） | C · 去光照 albedo | **B** |
| 71 | `SaveImage` | albedo | 输出 | **B 输出** |
| 72 | `TilePreview` | — | 2×2自检 | 弃 |
| 73 | `SaveImage` | 2×2 自检 · albedo | 2×2自检 | 弃 |
| 74 | `PreviewImage` | 2×2 自检 · 渲染图 | （未分组） | 弃 |

---

## 附录 B · 目标目录结构

```
UE_tools/ComfyTextureStudio/                    ← 插件仓库根（本文件夹）
├── README.md                                   ← 快速上手 + 部署到 UE 项目
├── ComfyTextureStudio.uplugin                  ← ✅ M1 创建（Runtime 模块，默认 LoadingPhase）
├── docs/
│   ├── PRD.md                                  ← 本文件
│   ├── workflow-interface.md                   ← 工作流拆分契约 + 完整槽位表
│   ├── M0-baseline.md                          ← ✅ 实测基线（耗时/显存/IO）+ 测量方法论
│   └── M1-report.md                            ← ✅ 插件骨架 + 后端打通：实测、编译链、7 个坑
├── Config/
│   └── Workflows/
│       ├── stage_a_base_texture.json           ← ✅ 9 节点 API 图（T4 产出，valid=true）
│       ├── stage_b_pbr.json                    ← ✅ 15 节点 API 图（T4 产出，valid=true）
│       ├── workflows.manifest.json             ← 语义槽位映射 + 版本元数据 + source 指纹
│       └── _source/
│           └── sdxl_sand_pbr_v2_geom.json      ← 前端格式源图（只读副本，便于对照）
├── Tools/
│   ├── export_api_workflows.py                 ← ✅ 前端图 → API 图 + manifest（T4）
│   ├── derive_keepset.py                       ← ✅ 反向可达性 keep/drop 推导（T5）
│   ├── run_live_check.py                       ← ✅ M0 live 实跑器；**即 M1 的最小 REST 客户端参考实现**
│   └── check_normal_convention.py              ← ✅ 几何自洽性 + 无缝 z 检验 + directx 开关语义（T6）
├── Saved/
│   ├── M0/<runId>/                             ← M0 实跑产物 + report.json（可删）
│   └── ComfyTextureStudio/Cache/<jobId>/       ← M1 起：运行的产物中转（由 CacheDir 配置决定）
├── Resources/
│   ├── README.md                               ← 节点包纳管说明 + 依赖链 + 安装/同步流程（Q8）
│   ├── Icon128.png
│   └── comfyui_seamless_texture/               ← 已纳管 v1.3.0（8 文件 · 133 KB · 字节级校验通过）
│       ├── __init__.py  nodes.py  geometry.py
│       ├── pbr.py  materials.py  upscale.py
│       ├── pyproject.toml
│       └── README.md
└── Source/
    ├── ComfyTextureStudio/                     ← ✅ Runtime 模块（M1 完成）
    │   ├── ComfyTextureStudio.Build.cs
    │   ├── Public/
    │   │   ├── ComfyTextureStudio.h                          模块入口 + 日志类别
    │   │   ├── ComfyTypes.h                                  状态机/失败分类/中文名（纯 C++）
    │   │   ├── ComfyHttpClient.h                             8 个 REST 端点
    │   │   ├── ComfyWorkflowGraph.h                          装载 + 语义槽位注入
    │   │   ├── ComfyTextureJob.h                             两阶段状态机
    │   │   ├── ComfyTextureStudioSettings.h                  UDeveloperSettings（PRD §8）
    │   │   └── ComfyTextureStudioCommandlet.h                无 UI 触发入口
    │   └── Private/
    │       ├── ComfyTextureStudio.cpp
    │       ├── ComfyHttpClient.cpp
    │       ├── ComfyWorkflowGraph.cpp
    │       ├── ComfyTextureJob.cpp
    │       ├── ComfyTextureStudioSettings.cpp
    │       └── ComfyTextureStudioCommandlet.cpp
    └── ComfyTextureStudioEditor/               ← Editor 模块（M2 创建）
        ├── ComfyTextureStudioEditor.Build.cs
        ├── Public/
        │   ├── ComfyTextureStudioEditor.h
        │   ├── SComfyTextureStudioPanel.h
        │   ├── FComfyTextureImportService.h
        │   ├── FComfyMaterialBuilder.h
        │   └── SComfyStagePreview.h
        └── Private/
            ├── ComfyTextureStudioEditor.cpp
            ├── SComfyTextureStudioPanel.cpp
            ├── FComfyTextureImportService.cpp
            ├── FComfyMaterialBuilder.cpp
            └── SComfyStagePreview.cpp
```

**开发宿主工程（仓库根的兄弟目录）**：

```
UE_tools/ComfyTextureStudioHost/                ← ✅ M1 创建，只为编译与跑 commandlet，不含游戏内容
├── ComfyTextureStudioHost.uproject             ← AdditionalPluginDirectories 指向 UE_tools
├── Config/DefaultEngine.ini
└── Source/
    ├── ComfyTextureStudioHost.Target.cs
    ├── ComfyTextureStudioHostEditor.Target.cs
    └── ComfyTextureStudioHost/                 ← 空主模块，依赖 ComfyTextureStudio
```

它的 `Saved/ComfyTextureStudio/Cache/` 就是无 UI 验证时产物的落点。

**部署到 UE 项目**（两种）：

```ini
# 方式一：直接拷贝
<UE项目>/Plugins/ComfyTextureStudio/          ← 把本文件夹整体复制进去

# 方式二：.uproject 里加外部插件目录（推荐开发期，改代码不用来回拷）
"AdditionalPluginDirectories": [ "D:/PersonalDocuments/hongxuan_document/UE_tools" ]
```

---

## 附录 C · 与工作流原始 note 的对应关系

工作流自带的 `MarkdownNote`（节点 10）记录了这条链路全部的方法论依据与实测数据。PRD 中凡是"实测值"都源自它，实现期遇到参数疑问应优先查该 note。其中最需要在插件里落地的几条：

| note 中的结论 | 在插件里的落地 |
|---|---|
| 通道符号是实测标定的，且**标量统计量对翻转不敏感** | §6.3 强制 `FlipGreenChannel=true` + §6.4 单光源验收 |
| `height` 归一化**不可用硬截断**（会产生零梯度死平面） | `40.headroom=0.15` 保留；若暴露给用户，加警告文案 |
| `height` 必须做低频剔除，否则出现"碗形"偏置、梯度被压 35–50× | `40.low_freq_radius=96` 保留为默认 |
| DSINE 的 fov 必须压小补边才生效（fov 60 时补边完全无效甚至更差） | `35.fov=3` 保留为默认，并在 UI 上标注"勿随意调大" |
| MoGe 是 ViT，补边**修不干净**（永远到不了 1.0） | 支持 D2 裁剪决策 |
| `refine_steps` 对 MoGe-2 无效（仅 v3 下发） | 不暴露该参数 |
| 真分辨率旋钮是 `resolution_level`（0–9），节点上没有 `num_tokens` 输入 | 裁掉 MoGe 后此条不适用；若恢复对照支路则按此设置 |
| StableMaterials 只做全局条件、无空间对应 | 不使用该路线（`SeamlessMaterialsFromAlbedo` 不在图内） |

---

*本 PRD 中所有工作流节点 id、参数默认值、连线关系、节点属性值均来自对 `sdxl_sand_pbr_v2_geom.json`（73,211 B，2026-10-12 15:22）的实测解析，以及 `comfy workflow slots`（81 个槽位）与 `comfy node` 的实时查询结果，未做推测。*
