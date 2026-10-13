// ComfyTextureStudio · 贴图导入服务（PRD §6.1 / §6.3）
//
// 两件事：
//   1. 把 PNG 导成 UTexture2D，并**逐项**写 PRD §6.3 那张属性表
//   2. 写完后**回读**每一项做自检 —— 那张表是 G3 的验收依据，不能靠"以为设对了"
//
// 属性表的**值**来自 manifest 的 outputAssets[].ue，本文件只负责字符串→引擎枚举的
// 映射与执行。映射表里出现未知值时**报错**而不是退回默认值：静默退化正是这类代码
// 最危险的失败模式。

#pragma once

#include "ComfyImportTypes.h"
#include "CoreMinimal.h"
#include "ComfyWorkflowGraph.h"

class UComfyTextureStudioSettings;
class UTexture2D;

class FComfyTextureImportService : public IComfyAssetImportProvider
{
public:
	// IComfyAssetImportProvider
	virtual FString GetProviderName() const override { return TEXT("ComfyTextureImportService"); }
	virtual FComfyImportResult ImportAndBuildMaterial(const FComfyImportRequest& Request) override;
	virtual bool VerifyImportedAssets(const FString& DestinationPath, const FString& BaseName,
		FComfyAssetVerification& OutVerification) override;

	/**
	 * 从 manifest 读阶段 B 的通道规格（含 §6.3 的属性值与 orphan 标记）。
	 * 面板与 commandlet 都用它，保证属性表只有一个来源。
	 */
	static bool LoadStageBSpecs(
		const UComfyTextureStudioSettings& Settings,
		TMap<FString, FComfyOutputAssetSpec>& OutAssets,
		FString& OutError);

	/** 相对插件根 / 相对项目 Saved 的路径解析成绝对路径。 */
	static FString ResolvePluginPath(const FString& MaybeRelative);

	/**
	 * 按当前开关，算出**本次会导入**的通道，及其对应的资产名后缀。
	 * 调用方拿去做命名冲突检测（Q6），服务内部也用它决定导入范围。
	 */
	static TArray<FString> GetSuffixesToImport(
		const TMap<FString, FComfyOutputAssetSpec>& Specs,
		bool bImportHeight,
		bool bImportOrphanMaps);

private:
	/** 导入一张 PNG，写属性，保存，回读自检。 */
	bool ImportOne(
		const FString& Semantic,
		const FComfyOutputAssetSpec& Spec,
		const FString& PngAbsPath,
		const FComfyImportRequest& Request,
		FComfyTextureImportEntry& OutEntry,
		FString& OutError);

	/** 收集已导入的贴图，供材质构建使用。 */
	UTexture2D* FindImportedTexture(
		const TArray<FComfyTextureImportEntry>& Entries,
		const FString& Semantic) const;

	/** semantic → 已导入的 texture 指针。 */
	TMap<FString, TObjectPtr<UTexture2D>> ImportedTextures;
};
