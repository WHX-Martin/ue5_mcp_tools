# ue-suntime

输入一个地点和时间，把 UE 场景灯光还原成该地点该时刻的真实物理环境光。

## 它做什么

```
"明天下午五点的上海外滩"
        │
        ├─ 大模型 ──> 结构化参数（经纬度 / 时刻 / UTC 偏移）      ← 只做"理解"
        │
        ├─ 天文算法 ─> 太阳高度角、方位角、大气质量                ← 确定性计算
        ├─ 大气模型 ─> 直射照度、天空散射照度、色温（含云量修正）
        │
        └─ MCP ─────> 写入 DirectionalLight / SkyLight 并读回校验
```

关键设计：**大模型只负责把自然语言解析成结构化参数**，物理计算和写值全部是确定性代码。
这样同一个需求每次跑出的结果完全一致，也不会出现模型编造属性名导致的静默失败。

## 环境（uv 管理）

虚拟环境在**仓库根目录** `.venv`，由 uv 创建，Python 3.12.12。

```powershell
# 首次创建（在 UE_tools 根目录执行）
uv venv --python 3.12
uv pip install -r ue-suntime/requirements.txt

# 日常执行（--no-project 让 uv 直接用根目录的 .venv，不去找 pyproject.toml）
uv run --no-project ue-suntime/main.py "明天下午五点的上海外滩"
```

也可以按传统方式激活后执行：

```powershell
.venv\Scripts\Activate.ps1
cd ue-suntime
python main.py "明天下午五点的上海外滩"
```

其他 uv 命令：

```powershell
uv pip list                 # 查看已装依赖
uv pip install <包名>       # 新增依赖（记得同步 requirements.txt）
uv pip compile requirements.txt -o requirements.lock.txt   # 生成锁定版本
```

## 安装（不用 uv 时）

```bash
pip install -r requirements.txt
cp .env.example .env    # 填入模型 API Key（可选，见下）
```

## 用法

```bash
# 自然语言（需要配置大模型 API Key）
python main.py "明天下午五点的上海外滩"

# 不配 Key 也能用：直接给参数
python main.py --lat 31.2397 --lon 121.4900 --datetime "2026-09-10 17:00" --utc-offset 8

# 只看参数不写场景
python main.py "上海外滩日落" --dry-run

# 输出 JSON（便于接入别的流程）
python main.py "上海外滩日落" --json

# 撤销上一次修改
python main.py --undo
```

## 输出示例

```
地点    : 上海外滩  (31.2397, 121.4900)
时间    : 2026-09-10 17:00 (UTC+08:00)
云量    : 45%
--------------------------------------------------------
太阳高度角 : 13.43°
太阳方位角 : 267.54°  (正北0 / 东90 / 南180 / 西270)
大气质量   : 4.23
直射照度   : 7583 lux
天空散射   : 6728 lux
太阳色温   : 4575 K
天空色温   : 6381 K
--------------------------------------------------------
当前关卡  : /Game/MCP_Test/Levels/L_testMap

[已应用] 平行光
  Actor    : DirectionalLight_0
  旋转     : pitch=-13.43 yaw=2.46 roll=0.00
  intensity    : 2.1277
  temperature  : 4574.7
  修改前       : {"intensity": 8, "temperature": 3500}
  读回校验     : {"intensity": 2.1277, "temperature": 4574.7}
```

## 配置要点（`config.yaml`）

| 配置项 | 说明 |
|---|---|
| `lighting.north_axis` | 关卡方位约定。`y` = X东/Y北（默认），`x` = X北/Y东。方向不对时先改这个 |
| `lighting.response` | `perceptual`（默认）：以正午为参考做幂律压缩，视觉更接近现场观感；`linear`：纯物理线性，黄昏数值会很低，需场景开启自动曝光 |
| `lighting.color_mode` | `temperature`：写色温（引擎黑体计算）；`rgb`：写线性色，兼容性最好 |
| `weather.enabled` | 通过 Open-Meteo 查云量修正直射/散射比例，失败自动按晴天处理 |
| `targets.*.name_hint` | 场景里灯光 Actor 的名字关键字，默认 `DirectionalLight` / `SkyLight` |

## 已知限制

- **天光不支持色温**：`SkyLightComponent` 没有 `temperature` / `bUseTemperature` 字段，
  所以天光一律通过线性色 `lightColor` 上色（代码里已按此处理）。
- **需要大气太阳光**：平行光的 `bAtmosphereSunLight` 应为 `true`，天空大气才会跟随太阳方向。
- **感知压缩会改变光比**：`perceptual` 模式下直射与散射的比例不再等于真实比例，
  换取的是"看得见"。要严格物理请用 `linear`。
- 只改灯光参数，不保存关卡；如需落盘请在编辑器里手动保存。

## 文件说明

| 文件 | 职责 |
|---|---|
| `solar.py` | NOAA 太阳位置 + Meinel 大气衰减，纯本地无依赖 |
| `weather.py` | Open-Meteo 云量查询，失败降级为晴天 |
| `ue_profile.py` | 物理参数到 UE 参数的映射（强度、色温、旋转） |
| `llm_parse.py` | 大模型解析自然语言需求 |
| `mcp_client.py` | MCP streamable-http 客户端（兼容 mcp 1.x / 2.x） |
| `ue_apply.py` | 查找 Actor/组件、读写属性、校验、撤销 |
| `main.py` | CLI 编排 |
