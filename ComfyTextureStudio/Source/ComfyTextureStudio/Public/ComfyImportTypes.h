// ComfyTextureStudio · 导入/材质的**接口层**（PRD §6）
//
// 为什么要有这一层：
//   导入与建材质必须用编辑器专用 API（UnrealEd / MaterialEditor），而 Runtime 模块
//   明确不依赖它们（见 ComfyTextureStudio.Build.cs 的注释）。但 commandlet（在
//   Runtime 模块里）又必须能跑完整链路做程序化验收。
//
//   ⇒ 在 Runtime 里定义**纯数据结构 + 抽象接口 + 注册表**，由 Editor 模块在
//     StartupModule 时注册实现。两边都只依赖这一层，不存在模块循环依赖。
//
// 这里的所有类型都是纯 C++（无 UHT 反射），可以在 commandlet 与后台线程安全使用。

#pragma once

#include "ComfyTypes.h"
#include "CoreMinimal.h"

// --------------------------------------------------------------------------- //
// 单张贴图的导入结果
// --------------------------------------------------------------------------- //

/**
 * 一张贴图的导入与属性自检结果。
 *
 * ⚠ 这张表就是 G3 的验收依据：`bPropertyCheckPassed` 由**回读已落盘的 UTexture2D**
 * 得出，不是我们在写属性时「以为设对了」的记录。
 */
struct COMFYTEXTURESTUDIO_API FComfyTextureImportEntry
{
	/** 语义键：albedo / normal / height / orm / ao / roughness。 */
	FString Semantic;

	/** UE 资产名，如 T_Sand_Coarse_01_BC。未导入时为空。 */
	FString AssetName;

	/** 完整包路径，如 /Game/ComfyTextures/Sand_Coarse_01/T_Sand_Coarse_01_BC。 */
	FString PackagePath;

	/** 源 PNG 的绝对路径。 */
	FString SourcePng;

	bool bImported = false;

	/** 未导入时的原因（orphan 跳过 / 用户没勾 / 导入失败）。 */
	FString SkipReason;

	// ---- 下面是**回读**到的实际属性（不是我们想设的值）----
	FString CompressionSettings;
	bool bSRGB = false;
	bool bFlipGreenChannel = false;
	FString LODGroup;
	FString AddressX;
	FString AddressY;
	FString MipGenSettings;

	/** 属性自检是否全项通过（对照 PRD §6.3 表）。 */
	bool bPropertyCheckPassed = false;

	/** 自检不符时的逐项说明。 */
	TArray<FString> PropertyMismatches;

	/** 接入了材质的哪个位置，如 "BaseColor" / "R→AO, G→Roughness, B→Metallic"。 */
	FString Role;

	FString ToText() const;
};

// --------------------------------------------------------------------------- //
// 材质构建结果
// --------------------------------------------------------------------------- //

struct COMFYTEXTURESTUDIO_API FComfyMaterialBuildEntry
{
	/** 材质资产名，如 M_Sand_Coarse_01。 */
	FString MaterialName;
	FString MaterialPackagePath;
	bool bCreated = false;
	bool bCompileAttempted = false;
	bool bCompiledClean = false;

	/**
	 * 这次编译**是否真的执行了**。
	 *
	 * `UMaterialEditingLibrary::RecompileMaterial` 在拿不到 MaterialResource 时静默返回
	 * 空数组，于是无头 commandlet 下 `bCompiledClean==true` 是假绿灯（编译根本没跑）。
	 * 只有本字段为 true 时，`bCompiledClean` / `CompileErrorCount` 才具备"编译结论"的效力。
	 */
	bool bCompileResultValid = false;

	int32 CompileErrorCount = 0;
	int32 CompileWarningCount = 0;
	TArray<FString> CompileMessages;

	// ---- SamplerType 自检（采样节点 ↔ 贴图压缩格式是否对齐）----
	//
	// 为什么单独立一组字段：这类错误是**编译期**的（引擎报
	// `Sampler type is X, should be Y for <path>`，error 级），而无头环境的编译不可信，
	// 所以必须由前置自检给出结论。M2 漏掉它，代价是用户打开材质看到两行红字。
	/** 核对过的采样节点数。 */
	int32 SamplerTypeChecked = 0;
	/** 不符的节点数。> 0 ⇒ 材质在编辑器里必然编译失败。 */
	int32 SamplerTypeMismatched = 0;
	/** 不符的逐项说明（引擎原文，含贴图路径）。 */
	TArray<FString> SamplerTypeMismatches;

	/** 材质实例名，如 MI_Sand_Coarse_01。未创建时为空。 */
	FString MaterialInstanceName;
	FString MaterialInstancePackagePath;
	bool bInstanceCreated = false;

	/** 已设到 MI 上的标量参数。 */
	TMap<FString, float> InstanceScalars;

	// ---- 结构性自检（Q4 的关键：视差必须影响全部三张交付贴图）----
	/** BumpOffset.Height 是否接了 height 贴图采样。 */
	bool bBumpOffsetHeightLinked = false;
	/** BumpOffset.Coordinate 是否接了 Tiling 后的 UV。 */
	bool bBumpOffsetCoordinateLinked = false;
	/** 有几个采样节点把 Coordinates 接在 BumpOffset 的输出上。0 表示视差是死的。 */
	int32 SamplesDrivenByBumpOffset = 0;
	/** 期望被驱动的采样数（albedo + normal + ORM = 3）。 */
	int32 SamplesExpected = 3;
	/** 是否至少驱动了 3 个 —— 只接 BaseColor 是本设计里最典型的错误。 */
	bool bParallaxAppliedToAllDeliverables = false;

	TArray<FString> StructuralWarnings;
};

// --------------------------------------------------------------------------- //
// 报告与请求
// --------------------------------------------------------------------------- //

struct COMFYTEXTURESTUDIO_API FComfyImportReport
{
	FString BaseName;
	FString DestinationPath;
	FString GeneratedAt;

	TArray<FComfyTextureImportEntry> Textures;
	FComfyMaterialBuildEntry Material;

	/** 未导入的 orphan 通道摘要（Q3）。 */
	TArray<FString> SkippedOrphans;

	/** 面向用户的多行文本（PRD §7.4 的示例格式）。 */
	FString ToText() const;

	/** 落盘用。 */
	FString ToJson() const;

	/** 有没有任何一项不达标（自检失败 / 编译有错 / 结构自检失败）。 */
	bool HasProblems() const;
};

/** 导入请求。由面板或 commandlet 填，交给 IComfyAssetImportProvider 执行。 */
struct COMFYTEXTURESTUDIO_API FComfyImportRequest
{
	/** 语义键 → 本地 PNG 绝对路径。6 个通道都填（服务按开关过滤）。 */
	TMap<FString, FString> ChannelPngPaths;

	// ---- 命名（Q6）：由调用方用 FComfyAssetNaming 解析好再传进来 ----
	/** 资产基名，如 Sand_Coarse_01。 */
	FString BaseName;
	/** 包路径（资产所在文件夹），如 /Game/ComfyTextures/Sand_Coarse_01。 */
	FString DestinationPath;
	FString MaterialName;          // M_Sand_Coarse_01
	FString MaterialInstanceName;  // MI_Sand_Coarse_01

	/**
	 * 贴图后缀 → 资产名。空表时退回 `T_<BaseName><suffix>`。
	 * 之所以按后缀传：后缀来自 manifest 的 outputAssets[].ue.suffix，是**单一来源**，
	 * 服务侧不重复定义通道命名规则。
	 */
	TMap<FString, FString> TextureNames;

	// ---- 导入开关（PRD §6.3 + §8 默认值）----
	bool bFlipGreenChannel = true;      // D3 必须 true
	bool bImportHeight = true;          // Q4
	bool bImportOrphanMaps = false;     // Q3
	bool bCreateMaterialInstance = true;
	float BumpDepth = 0.02f;

	/** 关则导入后删掉中转 PNG（对应 Settings::PreserveDownloadedPng 取反）。 */
	bool bDeleteSourcePng = false;

	/** 取某个通道的贴图资产名。 */
	FString GetTextureName(const FString& Suffix) const
	{
		if (const FString* Found = TextureNames.Find(Suffix))
		{
			return *Found;
		}
		return FString::Printf(TEXT("T_%s%s"), *BaseName, *Suffix);
	}
};

struct COMFYTEXTURESTUDIO_API FComfyImportResult
{
	bool bSuccess = false;
	FString ErrorText;
	FComfyImportReport Report;

	/** 新创建资产的完整包路径，供「在 Content Browser 中显示」用。 */
	TArray<FString> CreatedAssetPaths;

	FString GetMaterialPath() const { return Report.Material.MaterialPackagePath; }
};

// --------------------------------------------------------------------------- //
// 独立验证（从磁盘冷加载后核对）
// --------------------------------------------------------------------------- //

/**
 * 「落盘之后属性还对不对」的核对结果。
 *
 * 为什么需要它：导入时的自检读的是**同一个进程里刚写过的内存对象**，
 * 属于"自己检查自己"。这层验证在**全新进程**里重新 LoadObject 这些资产，
 * 从磁盘读回来再核一遍 —— 才能证明属性真的被序列化保存了
 * （§6.3 是 G3 的验收依据，必须经得起这一点）。
 */
struct COMFYTEXTURESTUDIO_API FComfyAssetVerification
{
	bool bAllOk = false;
	int32 Checked = 0;
	int32 Failed = 0;
	TArray<FString> Lines;

	FString ToText() const;
};

// --------------------------------------------------------------------------- //
// 提供者接口与注册表
// --------------------------------------------------------------------------- //

/** Editor 模块实现它，Runtime/commandlet 通过注册表取用。 */
class COMFYTEXTURESTUDIO_API IComfyAssetImportProvider
{
public:
	virtual ~IComfyAssetImportProvider() = default;

	/** 显示名，仅日志用。 */
	virtual FString GetProviderName() const = 0;

	/**
	 * 执行导入 + 建材质。**必须在游戏线程调用**（会创建 UObject / 触碰资产注册表）。
	 * 返回后所有资产已落盘。
	 */
	virtual FComfyImportResult ImportAndBuildMaterial(const FComfyImportRequest& Request) = 0;

	/**
	 * 冷加载核对：从磁盘重新加载资产并逐项校验属性与材质图结构。
	 * 供 commandlet 的 -VerifyOnly 在**新进程**里调用。
	 *
	 * @param DestinationPath 资产所在包路径，如 /Game/ComfyTextures/cts_m2_verify2
	 * @param BaseName        资产基名，如 cts_m2_verify2
	 */
	virtual bool VerifyImportedAssets(const FString& DestinationPath, const FString& BaseName,
		FComfyAssetVerification& OutVerification) = 0;
};

/**
 * 全局注册表。Editor 模块 StartupModule 注册、ShutdownModule 注销。
 * commandlet 用它拿到实现 —— 拿不到就说明 Editor 模块没加载。
 */
class COMFYTEXTURESTUDIO_API FComfyImportRegistry
{
public:
	static FComfyImportRegistry& Get();

	void SetProvider(IComfyAssetImportProvider* InProvider);
	IComfyAssetImportProvider* GetProvider() const { return Provider; }
	bool HasProvider() const { return Provider != nullptr; }

private:
	IComfyAssetImportProvider* Provider = nullptr;
};
