# M1 报告 · 插件骨架 + 后端打通

> 日期：2026-10-13 · 对应 PRD §11 的 M1 里程碑 · 状态：**✅ 通过**

## 0. 结论先行

**M1 验收标准（PRD §11）：一次调用产出 6 个 PNG 到 `Saved/.../Cache/<GUID>/` —— 已达成。**

在 UE 5.8 里从零建了 Runtime 模块 + 无 UI 命令行入口，用 `UnrealEditor-Cmd.exe -run=ComfyTextureStudio`
跑通完整两阶段：SDXL+LoRA 出基础贴图 → 上传 → 几何反演+去光照 → 6 张 PBR 贴图全部落盘。

| 项 | 实测 |
|---|---|
| 阶段 A（真算，无缓存命中） | **12.33 s** |
| 阶段 A（**命中节点级缓存**） | **1.55 s** ← 见 §4.1，这是坑不是优点 |
| 阶段 B | **4.58 s** |
| commandlet 端到端（含 UE 启动） | **18.34 s** |
| 通道产出 | **6/6**（4 交付 + 2 可选） |
| 产物体积 | 6 张 PNG 合计 ≈ 5.8 MB |
| 增量编译 | ~6–10 s |

比 M0 的纯 HTTP 实测只多了 UE 自身的启动开销（约 5 s），插件层没有引入额外延迟。

## 1. 交付物

### 插件本体（`ComfyTextureStudio/`）

```
ComfyTextureStudio.uplugin                        ← Runtime 模块，LoadingPhase 默认
Source/ComfyTextureStudio/
  ComfyTextureStudio.Build.cs
  Public/
    ComfyTextureStudio.h                          模块入口 + LogComfyTextureStudio
    ComfyTypes.h                                  状态机 / 13 类失败分类 / 中文名映射（纯 C++，无 UHT）
    ComfyTextureStudioSettings.h/.cpp             UDeveloperSettings，PRD §8 的全部配置键
    ComfyHttpClient.h                             8 个端点的异步接口 + Pump
    ComfyWorkflowGraph.h                          工作流装载 + 语义槽位注入
    ComfyTextureJob.h                             两阶段任务状态机
    ComfyTextureStudioCommandlet.h                无 UI 触发入口
  Private/
    ComfyTextureStudio.cpp  ComfyHttpClient.cpp  ComfyWorkflowGraph.cpp
    ComfyTextureJob.cpp     ComfyTextureStudioSettings.cpp  ComfyTextureStudioCommandlet.cpp
```

> `ComfyTypes.h` 刻意**不带 UHT 反射**：这些结构要在后台线程和 commandlet 里用，不参与 GC，
> 也不允许持有 UObject 引用（PRD §3.3 红线）。唯一需要反射的是配置类与 commandlet。
>
> 附录 B 的文件树里没有列 commandlet 两个文件 —— 它们是 M1 的实现需要（PRD 只说了「命令行式触发」）。
> Editor 模块（`SComfyTextureStudioPanel` 等）按计划留到 M2 再建。

### 开发宿主工程（`ComfyTextureStudioHost/`）

仓库根下新增一个最小 UE 5.8 工程，**唯一用途**是把插件编起来并跑 commandlet：

```
ComfyTextureStudioHost.uproject      ← AdditionalPluginDirectories 指向 UE_tools
Source/*.Target.cs                   ← Game + Editor 两个 target
Source/ComfyTextureStudioHost/       ← 一个空主模块，依赖 ComfyTextureStudio
Config/DefaultEngine.ini
```

它的 `Saved/` 就是 M1 产物的落点，且已被根 `.gitignore` 覆盖（`Binaries/ Intermediate/ Saved/`）。

## 2. 编译链（可复现）

```
UE 5.8（C:/Program Files/Epic Games/UE_5.8）
Visual Studio Community 2026 · 工具链 14.44.35221 · Windows 10.0.26100 SDK
24 物理核 / 128 GB
```

```bat
Build.bat ComfyTextureStudioHostEditor Win64 Development ^
  -Project="<宿主工程>.uproject" -WaitMutex -NoUBA
```

> **`-NoUBA` 是必需的。** 不关 Unreal Build Accelerator 时，它的 storage server 会在
> 生成 PCH 时被文件系统权限拒绝（`SetFileInformationByHandle ... Access is denied`），
> 报错是 `fatal error C1083: 无法打开预编译头文件 ... 拒绝访问` —— 看起来像代码问题，
> 其实是构建加速器的权限问题。关掉后同一份代码立刻编过。

## 3. commandlet 用法

```bat
UnrealEditor-Cmd.exe <宿主工程>.uproject ^
  -run=ComfyTextureStudio -unattended -nopause -nosplash -stdout
```

| 参数 | 说明 |
|---|---|
| `-JobId=<名>` | 产物子目录名，默认时间戳 + GUID 短码 |
| `-Prompt=` / `-Negative=` | 提示词 |
| `-Seed=<int>` / `-Seed=random` | 种子；`random` 用于避开缓存（见 §4.1） |
| `-Steps= -Width= -Height= -BatchSize=` | 采样与尺寸 |
| `-Lora=<文件名>` | 换 LoRA（Q7 的预留通道） |
| `-Set=key=value,...` | 任意语义键覆盖，如 `-Set=dsineFov=2.5,aoRadius=32` |
| `-ServerUrl=` / `-OutDir=` | 覆盖配置里的服务地址 / 产物根 |

产物结构：

```
<宿主工程>/Saved/ComfyTextureStudio/Cache/<JobId>/
├── stage_a_api.json        实际提交的图（可直接拖进 ComfyUI 前端复现）
├── stage_a/stage_a_base_00001_.png
├── stage_b_api.json
├── stage_b/{albedo,normal,height,orm,ao,roughness}_00001_.png
└── report.json             机器可读结果：通道路径 / 耗时 / 错误分类
```

退出码 0 = 成功，1 = 失败。日志走 `LogComfyTextureStudio`；`Saved/Logs/*.log` 是 UTF-8，
**控制台 stdout 是 GBK**，用 grep 过滤中文会失败（会被判成二进制）—— 排查时读日志文件。

## 4. 实现中挖出的坑

### 4.1 节点级结果缓存会被误读成「工具很快」（M0 已预言，现已在 C++ 路径复现）

同一张图、同一个种子重复提交，阶段 A 从 **12.33 s 掉到 1.55 s** —— ComfyUI 直接命中缓存，
根本没重算。用户点两次「生成」会以为工具坏了（拿到一模一样的图）或者很快（其实没算）。

应对：commandlet 提供 `-Seed=random`；**M2 的 UI 必须把「换种子/失效缓存」做成显式选项**，
不能默认静默复用。

### 4.2 Windows 上 `history` 报的 `subfolder` 是**反斜杠**

```
outputs[20].images[0] = { filename: 'stage_a_base_00001_.png',
                          subfolder: 'cts\\20261013-105755_600AEC',   ← 反斜杠
                          type: 'output' }
```

ComfyUI 用 `os.sep` 拼路径，Windows 下就是 `\`。两种写法 `/view` 都认（都实测 200），
但**百分号编码必须完整**。

### 4.3 ⚠ 我自己引入并修掉的一个 bug：URL 编码器漏了 `%`

第一版 `ComfyUrlEncode` 写成了"输出两位十六进制"，**忘了先输出 `%`**：

```
'\' (0x5C) →  "5C"      ← 错
'\' (0x5C) →  "%5C"     ← 对
```

于是 `/view?subfolder=cts5C20261013-...`，ComfyUI 去找一个叫 `cts5Cxxx` 的目录，回 **404**。
而报错姿势极具误导性：product 明明好好躺在 `outputs/cts/20261013-.../` 下，
"404 + 建议检查 output 目录可读"会把人引向权限问题。

**这是 M1 唯一的真实故障**，也是这次唯一改了一轮的代码。已在 `ToQueryString()` 里加了
反斜杠→正斜杠归一，并把 404 的错误提示改成指向「转义错误 / 产物被清理」。

### 4.4 commandlet 里没有 tick 循环，HTTP 必须手工泵

`FHttpManager` 继承 `FTSTickerObjectBase`，编辑器里靠引擎 tick 驱动；**commandlet 没有 tick**，
不显式调用 `FHttpManager::Tick()` 的话请求会一直"在飞"、完成回调永不触发（表现为无限挂起）。
`FComfyHttpClient::Pump(Seconds)` 就是干这个的，`FComfyTextureJob::Tick()` 内部自动调用，
所以 M2 用 `FTSTicker` 驱动同一份代码时不用改。

### 4.5 UE 5.8 的两个 API 变更

| 项 | 变更 | 处理 |
|---|---|---|
| `FJsonObject::Values` 的键类型 | 5.8 是 `UE::FSharedString`（旧版是 `FString`），`Values.Find(FString)` / `GetKeys(TArray<FString>)` 都编不过 | 加 `ComfyJsonKeyToString()` 兼容层；单个字段一律走 `HasField` / `TryGetField`（收 `FStringView`，不受影响） |
| `FString::Printf` | 新增格式化字符串检查，`%s` 传未解引用的 `FString` 直接报错 | 统一 `*Str` |

### 4.6 commandlet 能被找到，靠的是模块加载时机

`FEngineLoop` 里 `LoadStartupModules()`（加载 PreDefault/Default/PostDefault 三个相位的插件模块）
在 commandlet 解析（`FindFirstObject<UClass>(...Commandlet)`）**之前**执行，
所以插件模块用**默认 LoadingPhase** 即可被 `-run=ComfyTextureStudio` 找到，不需要 `PostConfigInit`。

### 4.7 环境：`http_proxy` 会劫持 localhost

本机环境变量里有 `http_proxy=http://127.0.0.1:7852`（Clash），Python 的 `urllib` 会把发往
`127.0.0.1:8000` 的请求也塞给代理，拿回 **502 Bad Gateway** —— 看起来像"ComfyUI 挂了"。
已给 `Tools/run_live_check.py` 与 `Tools/export_api_workflows.py` 换成 `ProxyHandler({})` 的 opener。
（引擎侧的 `FHttpModule` 不走环境变量代理，不受影响。）

### 4.8 ComfyUI 的真实 output 目录不在 `ComfyUI-data` 下

```
custom_nodes : C:/Users/1142/AppData/Local/Programs/ComfyUI-data/custom_nodes   ← M0 已确认
output       : D:/Documents/comfyUI_documents/outputs                          ← M1 新发现
```

两者由 ComfyUI Desktop 分别配置。插件下载走 `/view` API，不碰文件系统，所以不影响功能；
但**预检里"检查 output 目录"这类动作不能靠猜路径**。

## 5. 已验证的契约点

从产物反查，以下几点在真跑中确认无误：

| 检查项 | 结果 |
|---|---|
| 阶段 B 的输入占位符替换 | `LoadImage.image = 'cts_<jobId>.png'`，占位符字符串已不存在（命中次数断言 = 1 通过） |
| 语义槽位注入（不硬编码节点 id） | 6 个输出前缀按 manifest 的 `runtimeValuePattern` 展开为 `cts/<jobId>/<通道>` |
| D3 法线约定 | 提交图里 `50.directx = false`，交 UE 翻绿通道 |
| Q3 ORM 合并 | `62.metalness_value = 0`（砂石是电介质） |
| 阶段 B 输入确已上传 | `input/cts_<jobId>.png`，且阶段 B 真读到了它（产物非空、结构正确） |
| 产物正确性 | albedo 是干净去光照砾石；ORM 的 B=0（非金属）、A 通道砾石顶更亮 |

## 6. 给 M2 的输入

1. **UI 必须处理缓存语义**：提供「换随机种子 / 复用上次种子」的显式选择，并把 `stageASeconds`
   展示出来（1.5 s vs 12 s 是用户能看懂的信号）。
2. **线程模型已铺好**：`FComfyTextureJob::Tick(DeltaSeconds)` 在编辑器里由 `FTSTicker` 驱动即可，
   不需要改状态机；HTTP 回调全部在游戏线程触发（我们手工泵的），所以 UI 回调里可以安全碰 UObject。
3. **失败分类已经落地**：13 类 `EComfyErrorKind` + 每类的中文说明与建议动作，UI 直接映射即可。
4. **Editor 模块尚未创建**：`.uplugin` 目前只声明 Runtime 模块；M2 加入 `ComfyTextureStudioEditor`
   时要同步补 `Type: Editor` 的模块项。
5. **导入与建材质尚未开始**（M2 主体）：`FComfyTextureImportService` / `FComfyMaterialBuilder`，
   参数表见 PRD §6.3（逐项：`CompressionSettings` / `sRGB` / `FlipGreenChannel` / `Address`）。
