// ComfyTextureStudio · 资产命名与路径解析（PRD §8 命名段 / 决策 Q6）
//
// 两种模式：
//   · 模板：由材质描述拼出 {material}_{grain}_{index}，{index} 自动避让已有资产
//   · 手填：直接用用户输入
//
// 刻意**不提供静默覆盖**：冲突策略默认 Error，撞名就报错（Q6 的明确要求）。

#pragma once

#include "CoreMinimal.h"

class UComfyTextureStudioSettings;

/** 解析结果：一次导入要用到的全部名字与路径。 */
struct FComfyResolvedAssetNaming
{
	bool bSuccess = false;
	FString Error;

	/** 资产基名，如 Sand_Coarse_01。材质名与贴图名都由它派生。 */
	FString BaseName;

	/** 包路径（资产所在文件夹），如 /Game/ComfyTextures/Sand_Coarse_01。 */
	FString DestinationPath;

	FString MaterialName;             // M_Sand_Coarse_01
	FString MaterialInstanceName;     // MI_Sand_Coarse_01

	/** 模板模式下用的是哪个 index（手填模式下为 0）。 */
	int32 ResolvedIndex = 0;

	/**
	 * 后缀 → 贴图资产名。由 TextureNameTemplate 展开填充；
	 * 手填模式下等价于 `T_<BaseName><suffix>`。
	 */
	TMap<FString, FString> TextureNames;

	/** 贴图资产名 = T_<BaseName>_<suffix>（模板没覆盖时用）。 */
	FString MakeTextureName(const FString& Suffix) const
	{
		if (const FString* Found = TextureNames.Find(Suffix))
		{
			return *Found;
		}
		return FString::Printf(TEXT("T_%s%s"), *BaseName, *Suffix);
	}

	FString MakeTexturePackagePath(const FString& Suffix) const
	{
		return FString::Printf(TEXT("%s/%s"), *DestinationPath, *MakeTextureName(Suffix));
	}
};

class FComfyAssetNaming
{
public:
	/**
	 * 从材质描述里猜 {material} 与 {grain}（PRD §7.2 的 [Sand] [Coarse]）。
	 *
	 * 刻意做得很轻：取描述里前两个「像名词」的英文词。猜错不是问题 ——
	 * 结果只是填进输入框，用户随时可改。不引入任何模型依赖。
	 */
	static void SuggestFromDescription(const FString& Description, FString& OutMaterial, FString& OutGrain);

	/**
	 * 解析出本次导入的完整命名。
	 *
	 * @param Settings    插件配置（提供模板 / 冲突策略 / AssetRootPath / 补零位数）
	 * @param ManualName  手填模式下的基名（模板模式忽略）
	 * @param Material    模板模式的 {material}
	 * @param Grain       模板模式的 {grain}
	 * @param bUseTemplate  true = 模板模式，false = 手填模式
	 * @param OutSuffixes 需要占用名字的贴图后缀列表（用于冲突检测），由调用方按开关给出
	 */
	static FComfyResolvedAssetNaming Resolve(
		const UComfyTextureStudioSettings& Settings,
		const FString& ManualName,
		const FString& Material,
		const FString& Grain,
		bool bUseTemplate,
		const TArray<FString>& OutSuffixes);

	/** 展开模板里的 {material} {grain} {index} {suffix}。 */
	static FString ExpandTemplate(
		const FString& Template,
		const FString& Material,
		const FString& Grain,
		int32 Index,
		int32 IndexPadding,
		const FString& Suffix = FString());

	/**
	 * 目标文件夹里是否已存在这些资产（任一存在即返回 true，并回报冲突名）。
	 * 用 IAssetRegistry，不加载资产本身。
	 */
	static bool HasAssetConflict(
		const FString& DestinationPath,
		const TArray<FString>& AssetNames,
		TArray<FString>& OutConflicts);

	/** 规范化：去掉首尾空白、把非法字符换成下划线。 */
	static FString SanitizeName(const FString& In);
};
