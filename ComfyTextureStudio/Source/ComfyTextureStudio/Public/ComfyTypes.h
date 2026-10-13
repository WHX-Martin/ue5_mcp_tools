// ComfyTextureStudio · 基础类型、失败分类、中文名映射
//
// 这里是纯 C++ 类型，刻意不加 UHT 反射：它们要在后台线程与 commandlet 里使用，
// 不参与 GC，也不允许持有 UObject 引用（PRD §3.3 线程模型红线）。
// 唯一需要反射的是 UComfyTextureStudioSettings（配置要能被编辑器序列化）。

#pragma once

#include "CoreMinimal.h"

// --------------------------------------------------------------------------- //
// FJsonObject 键类型的兼容层
// --------------------------------------------------------------------------- //

/**
 * `FJsonObject::Values` 的**键类型随引擎版本变过**：UE 5.8 是 `UE::FSharedString`
 * （`UE_JSONOBJECT_LEGACY_STRING_KEYS=0`），更早的版本是 `FString`。
 * 遍历 `Values` 时用它把键取成 `FString`，免得把某个版本的实现细节写进业务代码。
 *
 * 注意：查单个字段请优先用 `HasField` / `TryGetField` / `TryGetXxxField`，
 * 那套 API 收 `FStringView`，天然不受键类型影响。
 */
inline FString ComfyJsonKeyToString(const FString& Key)
{
	return Key;
}

template <typename KeyType>
FString ComfyJsonKeyToString(const KeyType& Key)
{
	return FString(Key.ToView());
}

// --------------------------------------------------------------------------- //
// 状态机
// --------------------------------------------------------------------------- //

/** 任务状态机（PRD §3.2）。 */
enum class EComfyJobState : uint8
{
	Idle,        // 未开始
	Preflight,   // 探活 + 依赖/枚举校验
	Submitting,  // 正在提交
	Running,     // ComfyUI 执行中
	Downloading, // 下载产物
	StageAReady, // M2：阶段 A 完成并已下载，**停在这里等用户确认**（G2：用户可以对结果不满意而关掉面板）
	Ready,       // 阶段 B 产物齐备（M1 的终点，M2 从这里接导入）
	Importing,   // M2：导入 UE + 建材质
	Done,
	Failed,
	Cancelled,
};

inline const TCHAR* LexToString(EComfyJobState State)
{
	switch (State)
	{
	case EComfyJobState::Idle:        return TEXT("Idle");
	case EComfyJobState::Preflight:   return TEXT("Preflight");
	case EComfyJobState::Submitting:  return TEXT("Submitting");
	case EComfyJobState::Running:     return TEXT("Running");
	case EComfyJobState::Downloading: return TEXT("Downloading");
	case EComfyJobState::StageAReady: return TEXT("StageAReady");
	case EComfyJobState::Ready:       return TEXT("Ready");
	case EComfyJobState::Importing:   return TEXT("Importing");
	case EComfyJobState::Done:        return TEXT("Done");
	case EComfyJobState::Failed:      return TEXT("Failed");
	case EComfyJobState::Cancelled:   return TEXT("Cancelled");
	default:                          return TEXT("Unknown");
	}
}

/** 两个阶段（D1 拆分后的两张独立 API 图）。 */
enum class EComfyStage : uint8
{
	StageA, // SDXL + LoRA 出受光基础贴图
	StageB, // DSINE 几何反演 + 去光照 → 整套 PBR
};

inline const TCHAR* LexToString(EComfyStage Stage)
{
	return Stage == EComfyStage::StageA ? TEXT("stageA") : TEXT("stageB");
}

// --------------------------------------------------------------------------- //
// 失败分类（PRD §9.1）
// --------------------------------------------------------------------------- //

enum class EComfyErrorKind : uint8
{
	None,
	NotConnected,           // /system_stats 失败 ⇒ ComfyUI 没起
	MissingNodePack,        // /object_info 无 SeamlessDSINENormal
	MissingControlnetAux,   // 缺 comfyui_controlnet_aux（SeamlessDSINENormal 依赖它）
	MissingDsineWeight,     // dsine.pt 不存在
	DsineWeightTruncated,   // dsine.pt 字节数不符（镜像可能送残件）
	MissingModel,           // 模型 / LoRA 不在实时枚举里
	SubmitValidationFailed, // /prompt 返回 node_errors 非空 ⇒ 未入队
	NodeExecutionFailed,    // status_str == "error"
	ServerCrashed,          // 执行期连接中断/超时 —— 疑似 OOM 崩溃，**必须与 Timeout 区分**
	Timeout,                // 整任务超时
	DownloadFailed,
	UploadFailed,           // 输入图上传说给 ComfyUI 失败
	LocalIOFailed,
	Cancelled,
};

inline const TCHAR* LexToString(EComfyErrorKind Kind)
{
	switch (Kind)
	{
	case EComfyErrorKind::None:                   return TEXT("None");
	case EComfyErrorKind::NotConnected:           return TEXT("NotConnected");
	case EComfyErrorKind::MissingNodePack:        return TEXT("MissingNodePack");
	case EComfyErrorKind::MissingControlnetAux:   return TEXT("MissingControlnetAux");
	case EComfyErrorKind::MissingDsineWeight:     return TEXT("MissingDsineWeight");
	case EComfyErrorKind::DsineWeightTruncated:   return TEXT("DsineWeightTruncated");
	case EComfyErrorKind::MissingModel:           return TEXT("MissingModel");
	case EComfyErrorKind::SubmitValidationFailed: return TEXT("SubmitValidationFailed");
	case EComfyErrorKind::NodeExecutionFailed:    return TEXT("NodeExecutionFailed");
	case EComfyErrorKind::ServerCrashed:          return TEXT("ServerCrashed");
	case EComfyErrorKind::Timeout:                return TEXT("Timeout");
	case EComfyErrorKind::DownloadFailed:         return TEXT("DownloadFailed");
	case EComfyErrorKind::UploadFailed:           return TEXT("UploadFailed");
	case EComfyErrorKind::LocalIOFailed:          return TEXT("LocalIOFailed");
	case EComfyErrorKind::Cancelled:              return TEXT("Cancelled");
	default:                                      return TEXT("Unknown");
	}
}

/** 面向用户的一句话（PRD §9.1 的「面向用户的话」列）。 */
inline FString ComfyErrorUserText(EComfyErrorKind Kind)
{
	switch (Kind)
	{
	case EComfyErrorKind::NotConnected:
		return TEXT("ComfyUI 未响应。注意桌面版默认端口是 8000，不是 8188。");
	case EComfyErrorKind::MissingNodePack:
		return TEXT("缺少自定义节点包 comfyui_seamless_texture。");
	case EComfyErrorKind::MissingControlnetAux:
		return TEXT("缺少 comfyui_controlnet_aux：SeamlessDSINENormal 依赖它提供 DSINE 架构。");
	case EComfyErrorKind::MissingDsineWeight:
		return TEXT("缺少 DSINE 权重 dsine.pt。");
	case EComfyErrorKind::DsineWeightTruncated:
		return TEXT("DSINE 权重文件不完整。");
	case EComfyErrorKind::MissingModel:
		return TEXT("模型或 LoRA 不在 ComfyUI 当前可用列表里。");
	case EComfyErrorKind::SubmitValidationFailed:
		return TEXT("提交被 ComfyUI 拒绝（参数超范围或模型缺失），任务未入队。");
	case EComfyErrorKind::NodeExecutionFailed:
		return TEXT("ComfyUI 节点执行报错。");
	case EComfyErrorKind::ServerCrashed:
		return TEXT("与 ComfyUI 的连接在执行期中断，进程疑似崩溃（显存不足）。");
	case EComfyErrorKind::Timeout:
		return TEXT("任务超时。");
	case EComfyErrorKind::DownloadFailed:
		return TEXT("产物下载失败。");
	case EComfyErrorKind::UploadFailed:
		return TEXT("输入图上传给 ComfyUI 失败。");
	case EComfyErrorKind::LocalIOFailed:
		return TEXT("本地文件读写失败。");
	case EComfyErrorKind::Cancelled:
		return TEXT("任务已取消。");
	default:
		return FString();
	}
}

/** 建议动作（PRD §9.1 的「建议动作」列）。 */
inline FString ComfyErrorSuggestedAction(EComfyErrorKind Kind)
{
	switch (Kind)
	{
	case EComfyErrorKind::NotConnected:
		return TEXT("启动 ComfyUI Desktop，并确认插件设置里的 ServerUrl 指向 127.0.0.1:8000。");
	case EComfyErrorKind::MissingNodePack:
		return TEXT("把插件 Resources/comfyui_seamless_texture/ 复制到 <ComfyUI 用户数据目录>/custom_nodes/ 后重启 ComfyUI。");
	case EComfyErrorKind::MissingControlnetAux:
		return TEXT("在 ComfyUI Manager 里安装 comfyui_controlnet_aux 后重启。");
	case EComfyErrorKind::MissingDsineWeight:
		return TEXT("下载 dsine.pt 放到 comfyui_controlnet_aux/ckpts/hr16/Diffusion-Edge/ 下。");
	case EComfyErrorKind::DsineWeightTruncated:
		return TEXT("重新下载 dsine.pt，字节数必须是 291298581。");
	case EComfyErrorKind::MissingModel:
		return TEXT("按提示里列出的可用枚举值改用已有模型，或把模型放进 ComfyUI 的 models/ 对应目录。");
	case EComfyErrorKind::SubmitValidationFailed:
		return TEXT("按提示检查对应节点参数范围。");
	case EComfyErrorKind::NodeExecutionFailed:
		return TEXT("看 traceback 尾部定位节点；必要时在 ComfyUI 前端打开同一张图复现。");
	case EComfyErrorKind::ServerCrashed:
		return TEXT("降 batch_size 或降分辨率后重试，并确认 ComfyUI 进程已重启。");
	case EComfyErrorKind::Timeout:
		return TEXT("检查 ComfyUI 是否卡住；必要时增大 JobTimeoutSeconds。");
	case EComfyErrorKind::DownloadFailed:
		// 实测教训：/view 报 404 时，绝大多数情况**不是**权限或文件缺失，而是
		// subfolder 里的反斜杠没被正确百分号编码 —— Windows 上 ComfyUI 用 os.sep
		// 拼路径，history 报回来的是 `cts\xxx`，漏掉 '%' 就变成 `cts%5C` → `cts5C`。
		return TEXT("核对 /history 返回的 filename+subfolder 是否原样传给了 /view；"
					TEXT("404 通常是 subfolder 转义错误或产物已被清理。"));
	case EComfyErrorKind::UploadFailed:
		return TEXT("确认 ComfyUI 的 input 目录可写；换一个文件名重试。");
	case EComfyErrorKind::LocalIOFailed:
		return TEXT("检查输出目录是否可写、路径是否存在。");
	default:
		return FString();
	}
}

// --------------------------------------------------------------------------- //
// 节点 id → 面向用户的中文名（PRD §4.5，错误提示用）
// --------------------------------------------------------------------------- //

inline FString ComfyNodeDisplayName(const FString& NodeId)
{
	static const TMap<FString, FString> Names = {
		{ TEXT("1"),  TEXT("加载基础模型") },
		{ TEXT("2"),  TEXT("加载沙地 LoRA（受光渲染版）") },
		{ TEXT("3"),  TEXT("正向提示词") },
		{ TEXT("4"),  TEXT("负向提示词") },
		{ TEXT("5"),  TEXT("空潜空间（出图尺寸）") },
		{ TEXT("6"),  TEXT("UNet 环面 padding（四方连续）") },
		{ TEXT("7"),  TEXT("采样器") },
		{ TEXT("8"),  TEXT("VAE 解码（环形 padding）") },
		{ TEXT("20"), TEXT("保存基础贴图") },
		{ TEXT("35"), TEXT("DSINE 法线估计") },
		{ TEXT("36"), TEXT("法线基面整平") },
		{ TEXT("40"), TEXT("泊松积分求高度") },
		{ TEXT("50"), TEXT("由高度重导法线") },
		{ TEXT("60"), TEXT("由高度求 AO") },
		{ TEXT("61"), TEXT("由高度求粗糙度") },
		{ TEXT("62"), TEXT("ORM 通道打包") },
		{ TEXT("70"), TEXT("去光照（求 albedo）") },
	};
	const FString* Found = Names.Find(NodeId);
	return Found ? *Found : FString();
}

// --------------------------------------------------------------------------- //
// 交付通道
// --------------------------------------------------------------------------- //

namespace ComfyChannels
{
	/** 6 个通道，交付物在前、可选（orphan）在后。 */
	inline const TArray<FString>& All()
	{
		static const TArray<FString> V = {
			TEXT("albedo"), TEXT("normal"), TEXT("height"), TEXT("orm"),
			TEXT("ao"), TEXT("roughness")
		};
		return V;
	}

	/**
	 * 参与材质构建的通道。
	 * 决策 Q3：ORM 用合并图 ⇒ ao / roughness 是 orphan —— 下载但不导入、不接材质。
	 */
	inline const TArray<FString>& Deliverable()
	{
		static const TArray<FString> V = {
			TEXT("albedo"), TEXT("normal"), TEXT("height"), TEXT("orm")
		};
		return V;
	}

	inline bool IsDeliverable(const FString& Semantic)
	{
		return Deliverable().Contains(Semantic);
	}

	inline FString DisplayName(const FString& Semantic)
	{
		if (Semantic == TEXT("albedo"))    return TEXT("基础色");
		if (Semantic == TEXT("normal"))    return TEXT("法线");
		if (Semantic == TEXT("height"))    return TEXT("高度");
		if (Semantic == TEXT("orm"))       return TEXT("ORM 打包");
		if (Semantic == TEXT("ao"))        return TEXT("环境光遮蔽（可选）");
		if (Semantic == TEXT("roughness")) return TEXT("粗糙度（可选）");
		return Semantic;
	}
}

// --------------------------------------------------------------------------- //
// 数据载体
// --------------------------------------------------------------------------- //

/** 一张产物的引用。参数必须原样取自 /history 的 outputs，不能自己拼文件名。 */
struct FComfyImageRef
{
	FString Filename;
	FString Subfolder;
	FString Type = TEXT("output");
	FString NodeId;
	FString LocalPath;

	/** `/view` 的查询串。 */
	FString ToQueryString() const;
};

/** 一条已归类的失败。 */
struct FComfyJobError
{
	EComfyErrorKind Kind = EComfyErrorKind::None;
	FString NodeId;
	FString Message;
	FString Detail;

	bool IsSet() const { return Kind != EComfyErrorKind::None; }

	FString UserText() const { return ComfyErrorUserText(Kind); }
	FString SuggestedAction() const { return ComfyErrorSuggestedAction(Kind); }

	FString ToString() const
	{
		if (!IsSet())
		{
			return FString();
		}
		const FString NodeName = ComfyNodeDisplayName(NodeId);
		if (!NodeId.IsEmpty())
		{
			return NodeName.IsEmpty()
				? FString::Printf(TEXT("[%s] 节点 %s：%s"), LexToString(Kind), *NodeId, *Message)
				: FString::Printf(TEXT("[%s] 节点 %s（%s）：%s"), LexToString(Kind), *NodeId, *NodeName, *Message);
		}
		return FString::Printf(TEXT("[%s] %s"), LexToString(Kind), *Message);
	}
};
