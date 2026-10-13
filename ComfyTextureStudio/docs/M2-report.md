# M2 报告 · Slate 面板 + 两阶段向导 + 导入与材质构建

| 项 | 值 |
|---|---|
| 日期 | 2026-10-13 |
| 目标 | PRD §11 的 M2：完整 UI、状态门控、缩略图、2×2 自检、错误映射；**不打开 ComfyUI 前端**完成「提示词 → 审阅 → PBR → 导入 → 材质」 |
| 引擎 | UE 5.8.3（`++UE5+Release-5.8-CL-58210709`） |
| 编译器 | Visual Studio 2026 工具链 14.44.35221，`-NoUBA` |
| 结论 | **后端链路（导入 + 材质 + 属性表）已达成并程序化验证；UI 已编译但视觉/交互待人工确认**（见 §6） |

---

## 1. 结论先行

M2 新增一个 Editor 模块，把 M1 的「跑出 6 张 PNG」接到「进 UE、建材质、能在关卡里用」。
其中最容易被做错、也最难靠肉眼发现的两处，都做了**回读自检**并实测通过：

| 关键项 | 结果 |
|---|---|
| PRD §6.3 导入属性表（压缩 / sRGB / 绿通道 / 寻址 / LOD 组） | **37 项核对，0 项不符**（新进程从磁盘冷加载后核对） |
| 视差是否作用到**全部三张**交付贴图（Q4 的核心） | **3 / 3** —— 只接 BaseColor 的经典错误已被排除 |
| 材质编译 | 0 错误 0 警告 |
| 材质实例 | 已创建，父材质正确，3 个标量参数都在实例上 |
| `ao` / `roughness`（Q3 orphan） | 未导入、未接材质 ✓ |

过程中**自己引入并修掉两个真 bug**（§4），其中一个会导致编辑器崩溃。

---

## 2. 建了什么

### 2.1 新模块 `ComfyTextureStudioEditor`（Type=Editor, PostEngineInit）

```
Source/ComfyTextureStudioEditor/
├── ComfyTextureStudioEditor.Build.cs
├── Public/
│   ├── ComfyTextureStudioEditor.h        模块入口
│   ├── ComfyAssetNaming.h                命名双通道（Q6）
│   ├── ComfyTextureImportService.h       导入 + 属性表 + 自检（§6.1/§6.3）
│   ├── ComfyMaterialBuilder.h            材质图构建（§6.2）
│   └── SComfyTextureStudioPanel.h        三态面板（§7.2）
└── Private/
    ├── ComfyTextureStudioEditor.cpp      服务注册 / Tab / 菜单 / 工具栏 / 面板自检
    ├── ComfyAssetNaming.cpp
    ├── ComfyTextureImportService.cpp
    ├── ComfyMaterialBuilder.cpp
    └── SComfyTextureStudioPanel.cpp
```

### 2.2 模块解耦：接口 + 注册表

Runtime 模块的 `ComfyTextureStudio.Build.cs` 明确写着「不碰编辑器专用 API，这样它才能被
commandlet 复用」。但 M2 的导入与建材质**必须**用 `UnrealEd` / `MaterialEditor`，而 commandlet
（在 Runtime 模块里）又要能调用它们做程序化验收。

⇒ 在 Runtime 侧只定义**纯数据结构 + 抽象接口 + 注册表**（`ComfyImportTypes.h`），
Editor 模块在 `StartupModule` 注册实现：

```
Runtime  ComfyImportTypes.h   FComfyImportRequest / FComfyImportResult / FComfyImportReport
                              IComfyAssetImportProvider + FComfyImportRegistry
                                   ▲
                                   │ 注册实现（StartupModule）
Editor   FComfyTextureImportService : IComfyAssetImportProvider
```

两边都只依赖这一层，**没有模块循环依赖**，Runtime 也仍然是"干净"的。

### 2.3 属性表的单一来源

PRD §6.3 那张表**不在 C++ 里再抄一遍**：`FComfyTextureImportService::LoadStageBSpecs()`
从 `workflows.manifest.json` 的 `stageB.outputAssets[].ue` 读
（`compressionSettings` / `srgb` / `flipGreenChannel` / `lodGroup` / `addressMode` /
`importByDefault` / `orphanReason`），C++ 只负责**字符串 → 引擎枚举**的映射。

manifest 改了，导入行为跟着改，代码不用动。

> 映射表刻意**不做兜底**：出现没见过的值时**报错**而不是退回默认值。
> 静默退化会让"导入成功但属性不对"变成不可见的故障。

### 2.4 UI（PRD §7.2 三态）

单页左右分栏：左栏 ① 生成基础贴图（材质描述 + 完整提示词折叠编辑 + 种子/步数/CFG/
采样器/调度/尺寸/LoRA 强度）、右栏预览（原图 + **2×2 平铺自检** + 历史候选 + 换种子重出/
丢弃/用这张继续）、下方 ② 生成 PBR 全套（6 个通道缩略图，点击放大做平铺自检）、
③ 导入 UE（命名双通道 + 实时路径预览 + 4 个导入开关 + 报告区 + 复制/定位）。

按钮门控严格照 §7.3 的表。枚举（LoRA / 采样器 / 调度）从 `/object_info` **实时拉取**，
不硬编码（Q7：换 LoRA 的通道）。

### 2.5 Runtime 侧的一处必要扩展

M1 的 `FComfyTextureJob` 是「一条线跑完两阶段」。M2 的向导要求**阶段 A 跑完停下来等确认**
（G2：不满意时可以只跑阶段 A 就关掉面板）。所以加了：

- `FComfyTextureJobParams::bPauseAfterStageA`
- 新状态 `EComfyJobState::StageAReady` + 新步骤 `PauseForReview`
- `ContinueToStageB()`：复用已上传的输入继续，并**重新起算超时窗口**

> 顺带修了一处逻辑漏洞：原来整任务超时检查在 `bWaiting` 之前，如果不改，用户停在审阅界面
> 超过 `JobTimeoutSeconds`（默认 900s）就会被判"超时失败"。现在暂停期间不计时。

---

## 3. 验收过程与结果

三层验证，全绿：

| 层 | 动作 | 结果 |
|---|---|---|
| **编译** | `Build.bat ... -NoUBA` | `Result: Succeeded` |
| **live 导入** | `-run=ComfyTextureStudio -Import -Seed=random` | 退出码 **0**，4 张贴图 + 材质 + MI，编译 0 错误 |
| **冷加载核对** | `-VerifyOnly=/Game/ComfyTextures/cts_m2_verify2`（**新进程**） | 退出码 **0**，**37 项核对 0 项不符** |

### 3.1 导入报告（`-Import` 实测输出）

```
导入报告 · cts_m2_verify2 · 2026-10-13 11:43:26
──────────────────────────────────────────────
✓ T_cts_m2_verify2_BC      TC_Default   sRGB=on        TA_Wrap → BaseColor
✓ T_cts_m2_verify2_N       TC_Normalmap sRGB=off FlipG TA_Wrap → Normal
✓ T_cts_m2_verify2_H       TC_Grayscale sRGB=off       TA_Wrap → BumpOffset 视差高度
✓ T_cts_m2_verify2_ORM     TC_Masks     sRGB=off       TA_Wrap → R=AO, G=Roughness, B=Metallic
— 环境光遮蔽（可选）  未导入（ORM 合并图已含 AO，Q3）
— 粗糙度（可选）      未导入（ORM 合并图已含 Roughness，Q3）
──────────────────────────────────────────────
✓ M_cts_m2_verify2         已编译，0 错误 0 警告
✓ 视差已接入 3 / 3 张交付贴图
✓ MI_cts_m2_verify2        已创建
    · Tiling = 1.0000
    · NormalStrength = 1.0000
    · BumpDepth = 0.0200
```

### 3.2 冷加载核对（为什么它比导入自检更硬）

导入时的自检读的是**同一个进程里刚写过的内存对象** —— 严格说是"自己检查自己"。
`-VerifyOnly` 在**全新进程**里重新 `LoadObject`，从磁盘读回来再核一遍，能证明属性
**真的被序列化保存了**（§6.3 是 G3 的验收依据，必须经得起这一点）。

```
════ M2 · 冷加载核对 ════
  [贴图] 从磁盘重新加载
    ✓ T_cts_m2_verify2_BC 加载成功
    ✓ _BC.CompressionSettings = TC_Default
    ✓ _BC.SRGB = true
    ✓ _BC.FlipGreenChannel = false
    ✓ _BC.LODGroup = TEXTUREGROUP_World
    ✓ _BC.AddressX/Y = TA_Wrap / TA_Wrap
    …（_N / _H / _ORM 各 5 项，全通过；_N.FlipGreenChannel = true、其余为 false）
    — ao 未导入（orphan，Q3）    — roughness 未导入（orphan，Q3）
  [材质] 从磁盘重新加载
    ✓ MaterialDomain = 0 (MD_Surface)      ✓ BlendMode = 0 (BLEND_Opaque)
    ✓ 找到 BumpOffset 节点
    ✓ BumpOffset.Height       = MaterialExpressionTextureSampleParameter2D
    ✓ BumpOffset.Coordinate   = MaterialExpressionMultiply
    ✓ BumpOffset.HeightRatio  = MaterialExpressionScalarParameter
    ✓ 视差驱动的采样数 = 3
  [材质实例] 从磁盘重新加载
    ✓ MI 的父材质 = M_cts_m2_verify2
    ✓ MI 覆盖参数 Tiling / NormalStrength / BumpDepth
  核对结果  37 项，0 项不符 → 通过
```

### 3.3 落盘资产

```
Content/ComfyTextures/cts_m2_verify2/
  T_cts_m2_verify2_BC.uasset    2,465,073 B   (BC7)
  T_cts_m2_verify2_N.uasset     1,789,248 B   (BC5)
  T_cts_m2_verify2_ORM.uasset     912,895 B   (Masks)
  T_cts_m2_verify2_H.uasset       453,479 B   (BC4)
  M_cts_m2_verify2.uasset          12,395 B
  MI_cts_m2_verify2.uasset          4,439 B
```

---

## 4. 自己引入并修掉的两个 bug

### 4.1 绿通道翻转被全局应用（属性表自检抓出来的）

第一版写成"请求里一个布尔值，应用到所有贴图"：

```cpp
Texture->bFlipGreenChannel = Request.bFlipGreenChannel;   // ✗ 错
```

于是 albedo / ORM / height 也被标上了 `FlipG`。**是第一版报告里那行
`✓ T_..._H  TC_Grayscale sRGB=off FlipG TA_Wrap` 把它照出来的** —— 一个 height 贴图不该翻绿通道。

正确分工：manifest 的 `ue.flipGreenChannel` 决定**哪些通道**该翻（只有 normal 为 true），
请求里的开关是 **D3 这条策略的总开关**，两者都成立才翻：

```cpp
const bool bEffectiveFlipGreen = Spec.bFlipGreenChannel && Request.bFlipGreenChannel;  // ✓
```

同时**加固自检**：现在非 normal 通道若带了 `FlipG` 会直接判不符；自检的期望值也从
"请求的开关"改成"应当生效的值"——原来那个写法是在检查**我自己的意图**，而不是 PRD 的表，
属于"自证清白"。

修复后：`_N` 带 `FlipG`，`_BC` / `_H` / `_ORM` 都干净。

### 4.2 HTTP 回调在对象销毁后访问野指针（编辑器崩溃）

**这是 M2 里最值得记的一个。**

为验证 UI，我用 `-CtsTestPanel` 让编辑器启动时组装一次面板再销毁。结果编辑器**启动即崩**：

```
LogComfyTextureStudio: object_info：2269 个节点类
LogWindows: Error: Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0xffffffffffffffff
```

崩溃点就在 `/object_info` 返回之后。根因链条：

1. 面板 `Construct` 里发起了 `/object_info`（探测连接 + 拉枚举）
2. 面板被销毁（`-CtsTestPanel` 里立刻销毁；现实里是用户点了「重新探测」后马上关面板）
3. HTTP 完成回调触发，**裸捕获的 `this` 已经是野指针**
4. `EXCEPTION_ACCESS_VIOLATION`，堆栈完全落在引擎的 HTTP 代码里，指不到业务代码

**影响面比"面板"更大**：`FComfyTextureJob` 持有的 `FComfyHttpClient` 同样如此 ——
任务被丢弃时若有请求在飞，一样会崩。

**修法**：给 `FComfyHttpClient` 加**存活令牌**，9 个端点的回调全部先确认它还在：

```cpp
struct FLiveness { bool bAlive = true; };
TSharedPtr<FLiveness> Liveness;

Request->OnProcessRequestComplete().BindLambda(
    [this, Liveness = Liveness, OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
    {
        if (!Liveness.IsValid() || !Liveness->bAlive) { return; }   // 宿主没了，安静退出
        …
    });
```

- 令牌用 `TSharedPtr` 持有：客户端没了，令牌还活着，回调因此能**安全地读到 `bAlive == false`**
  （如果令牌跟对象一起死，回调连读都读不了）
- 析构时先作废令牌，再 `CancelRequest()` 掉飞行中的请求（免得关掉面板后还在后台等超时）
- 显式实现移动构造：被移走的对象析构时**不能**把新对象正在用的令牌标记为已死
  （否则新对象的回调会被全部静默丢弃 —— 这种 bug 比崩溃更难查）
- 4 个不捕获 `this` 的回调也加了守卫，因为它们调用的 `OnDone` 由**调用方**捕获，风险只是转移了

**回归测试**：`-TestLiveness` 复现该时序（发请求 → 立刻销毁客户端 → 泵 2 秒让回调有机会触发）：

```
════ 存活令牌回归测试 ════
  服务  http://127.0.0.1:8000
  已发起 /object_info 并**立刻销毁客户端**
  ✓ 回调被令牌挡下，未触碰已析构对象
  测试通过
```

---

## 5. UE 5.8 的 API 适配（编译期踩到的）

| 症状 | 真因 | 处理 |
|---|---|---|
| `error C2248: UMaterial::ShadingModel 是 private` | 5.8 把 `ShadingModel` 收成了私有 | 改用 public 的 `SetShadingModel(MSM_DefaultLit)`（它会一并维护 `ShadingModels` 位域） |
| `error C2027: 使用了未定义类型 UAssetImportTask` | 漏 include | 补 `#include "AssetImportTask.h"` |
| `error C2027: 使用了未定义类型 FHttpManager` | `HttpModule.h` 只有前置声明 | 补 `#include "HttpManager.h"` |
| `error C4099: FSlateDynamicImageBrush 前向声明是 class` | 是 `struct` | 前置声明改 `struct` |
| `error C2661: FSlateColor 没有三参数构造` | 只有 `FLinearColor` / `FColor` 构造 | `FSlateColor(FLinearColor(r,g,b))` |
| `error C7595: TCheckedFormatStringPrivate` | `FString::Printf` 的三元两个分支类型不同（`const TCHAR*` vs `FString`） | 统一成 `FString`。**与 M1 那次 `%s` 缺解引用是同一类问题**：5.8 新增了格式化字符串检查 |
| `error LNK2019: FComfyImportReport::ToText 未解析` | 跨模块的结构体成员函数没导出 | 结构体加 `COMFYTEXTURESTUDIO_API` |
| `error LNK2019: FWindowsPlatformApplicationMisc::ClipboardCopy 未解析` | 缺模块依赖 | Build.cs 加 `ApplicationCore` |
| `UToolMenus::UnRegisterOwner` 不存在 | 实际拼写是 `UnregisterOwner` | 修正 |

另外两处**非编译期**的坑：

- **Git Bash 的 MSYS 路径转换**会把命令行里的 `/Game/ComfyTextures/xxx` 改写成
  `C:/Users/.../PortableGit/Game/ComfyTextures/xxx`，导致 `-VerifyOnly` 找不到资产。
  跑 `-VerifyOnly` / 其它带 `/Game` 路径的参数时要用 `MSYS_NO_PATHCONV=1`。
- **`FCommands` 的 `-NoUBA`**（M1 已记录，M2 每次编译仍然必须带）。

---

## 6. 未验证项（必须人工确认）

**UI 的视觉与交互没有做运行期验证**，原因是环境限制而不是省事：

- 无头 commandlet 里 `FSlateApplication::IsInitialized()` 为 false ⇒ 面板自检自动跳过（已实测走到降级路径，未崩）
- GUI 版 `UnrealEditor.exe` 在当前沙箱里**无法创建窗口**（退出码 3、日志 0 字节）

所以下面这些只能人工打开面板确认：

| # | 确认项 | 预期 |
|---|---|---|
| 1 | `Window → Comfy Texture Studio` 菜单项存在；关卡编辑器工具栏有入口；点开出现可停靠面板 | 面板正常显示 |
| 2 | 启动日志里有 `面板自检通过`（带上 `-CtsTestPanel` 启动时才会打） | 无 warning |
| 3 | 缩略图能显示（原图 + 2×2 平铺自检） | 不是白块 |
| 4 | 布局在窄窗口下不挤（左右分栏可拖动） | 可读 |
| 5 | 面板关闭后**再打开**，状态正常（无残留 job / 无崩溃） | 正常 |
| 6 | 点「重新探测」后**立刻关面板**（这是 §4.2 崩溃的场景） | **不崩** |

第 6 项是本轮修复的直接受益者，建议重点试。

### 手工走一遍完整流程

1. 打开宿主机工程 `ComfyTextureStudioHost.uproject`（或把它作为插件装进自己的工程）
2. 确认 ComfyUI 在跑（8000 端口），面板右上角显示「● 已连接」
3. ① 填材质描述 → 生成基础贴图 → 看 2×2 有无接缝 → 不满意就「换种子重出」
4. 满意 →「✓ 用这张继续」→ ② 生成 PBR 全套（约 5 秒）
5. ③ 选命名（模板/手填）→ 勾选项 → 导入并创建材质
6. 到 `/Game/ComfyTextures/<名字>/` 看资产，把 `MI_*` 拖到平面上，平铺 4×4 看接缝与视差

> **单光源法线验证**（PRD §6.4 / G4 的前置）仍未做：需要把 `T_*_N` 贴到已知朝向的平面上、
> 配一盏从左上方来的平行光，与 albedo 的明暗方向比对。M0 的 `directx` 开关对照已经证明
> 产物是 OpenGL 约定、`FlipGreenChannel=true` 成立，但**渲染侧的最终确认仍待人工**。

---

## 7. M3 的输入

M2 明确没做、留给 M3 的：

1. **WebSocket 实时进度** —— 现在进度条是不确定态。`11.x` 里的节点级进度需要 WS 的 `progress` 消息。
2. **取消按钮的完整闭环** —— `/interrupt` + `/queue` 两条路径在 M1 已实现，但 UI 上没有「取消中…」的过渡态。
3. **参数预设**（导出/导入 JSON）—— 现在参数只在会话内存里。
4. **Content Browser 右键入口**（PRD §7.1 的 P1 项）。
5. **阶段 B 重跑** —— 现在 `Ready` 之后不能只重跑阶段 B（要改阶段 B 参数再跑一次）。
6. **导入报告的图形化** —— 现在是等宽文本（已在用 `SMultiLineEditableTextBox` 只读展示），
   可以做成带对勾/叉的列表。

另外两条要带进 M3 的经验：

- 任何新增的异步回调都**必须**走存活令牌（`FComfyHttpClient` 已经统一处理，
  但 WebSocket 是另一条路径，要单独做同样的事）。
- 属性表自检的**期望值必须来自 manifest**，不能来自"我们刚设了什么"（§4.1 的教训）。

---

## 8. 命令速查

```bash
# 编译（必须 -NoUBA）
"$UE/Engine/Build/BatchFiles/Build.bat" ComfyTextureStudioHostEditor Win64 Development \
  -Project="<...>/ComfyTextureStudioHost.uproject" -WaitMutex -NoUBA

# 跑通两阶段 + 导入 UE + 建材质（需要 ComfyUI 在跑）
UnrealEditor-Cmd.exe "<...>.uproject" -run=ComfyTextureStudio \
  -unattended -nopause -nosplash -stdout -Import -Seed=random -BaseName=cts_m2_verify2

# 冷加载核对（不需要 ComfyUI；/Game 路径必须关掉 MSYS 转换）
MSYS_NO_PATHCONV=1 UnrealEditor-Cmd.exe "<...>.uproject" -run=ComfyTextureStudio \
  -unattended -nopause -nosplash -stdout -VerifyOnly=/Game/ComfyTextures/cts_m2_verify2

# HTTP 存活令牌回归测试
UnrealEditor-Cmd.exe "<...>.uproject" -run=ComfyTextureStudio \
  -unattended -nopause -nosplash -stdout -TestLiveness

# 编辑器内组装一次面板并销毁（需 Slate；无头环境会优雅跳过）
UnrealEditor.exe "<...>.uproject" -CtsTestPanel ...
```
