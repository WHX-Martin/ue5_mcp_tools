#include "ComfyTextureImportService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AssetImportTask.h"
#include "ComfyMaterialBuilder.h"
#include "ComfyTextureStudioEditor.h"
#include "ComfyTextureStudioSettings.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "Factories/TextureFactory.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionBumpOffset.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// --------------------------------------------------------------------------- //
// manifest → 通道规格
// --------------------------------------------------------------------------- //

FString FComfyTextureImportService::ResolvePluginPath(const FString& MaybeRelative)
{
	if (MaybeRelative.IsEmpty())
	{
		return MaybeRelative;
	}
	if (FPaths::IsRelative(MaybeRelative) && !MaybeRelative.StartsWith(TEXT("/")))
	{
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ComfyTextureStudio")))
		{
			return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / MaybeRelative);
		}
	}
	return FPaths::ConvertRelativePathToFull(MaybeRelative);
}

bool FComfyTextureImportService::LoadStageBSpecs(
	const UComfyTextureStudioSettings& Settings,
	TMap<FString, FComfyOutputAssetSpec>& OutAssets,
	FString& OutError)
{
	const FString ManifestPath = ResolvePluginPath(Settings.WorkflowManifestFile);

	TMap<FString, FComfyStageSpec> Stages;
	if (!FComfyWorkflowGraph::LoadManifest(ManifestPath, Stages, OutError))
	{
		return false;
	}

	const FComfyStageSpec* StageB = Stages.Find(TEXT("stageB"));
	if (!StageB)
	{
		OutError = TEXT("槽位清单里没有 stageB。");
		return false;
	}
	if (StageB->OutputAssets.Num() == 0)
	{
		OutError = TEXT("槽位清单的 stageB 缺少 outputAssets —— 导入属性表没有来源。");
		return false;
	}

	OutAssets = StageB->OutputAssets;
	return true;
}

TArray<FString> FComfyTextureImportService::GetSuffixesToImport(
	const TMap<FString, FComfyOutputAssetSpec>& Specs,
	bool bImportHeight,
	bool bImportOrphanMaps)
{
	TArray<FString> Suffixes;

	// 稳定顺序：先按 ComfyChannels::All() 的固定序，避免报告行序随机
	const TArray<FString> Canonical = ComfyChannels::All();
	for (const FString& Semantic : Canonical)
	{
		const FComfyOutputAssetSpec* Spec = Specs.Find(Semantic);
		if (!Spec || Spec->Suffix.IsEmpty())
		{
			continue;
		}

		const bool bIsOrphan = !Spec->bImportByDefault;
		if (bIsOrphan && !bImportOrphanMaps)
		{
			continue;
		}
		if (Semantic == TEXT("height") && !bImportHeight)
		{
			continue;
		}
		Suffixes.Add(Spec->Suffix);
	}
	return Suffixes;
}

// --------------------------------------------------------------------------- //
// 字符串 ↔ 引擎枚举
// --------------------------------------------------------------------------- //

namespace
{
	// 这些映射表刻意**不做兜底**：manifest 里出现没见过的值时返回 false，
	// 由调用方报错。静默退回默认值会让"导入成功但属性不对"变成不可见的故障。

	bool ParseCompressionSettings(const FString& Name, TextureCompressionSettings& Out)
	{
		if (Name.Equals(TEXT("TC_Default"), ESearchCase::IgnoreCase))   { Out = TC_Default;   return true; }
		if (Name.Equals(TEXT("TC_Normalmap"), ESearchCase::IgnoreCase)) { Out = TC_Normalmap; return true; }
		if (Name.Equals(TEXT("TC_Masks"), ESearchCase::IgnoreCase))     { Out = TC_Masks;     return true; }
		if (Name.Equals(TEXT("TC_Grayscale"), ESearchCase::IgnoreCase)) { Out = TC_Grayscale; return true; }
		return false;
	}

	FString CompressionSettingsToText(TextureCompressionSettings Value)
	{
		switch (Value)
		{
		case TC_Default:   return TEXT("TC_Default");
		case TC_Normalmap: return TEXT("TC_Normalmap");
		case TC_Masks:     return TEXT("TC_Masks");
		case TC_Grayscale: return TEXT("TC_Grayscale");
		default:           return FString::Printf(TEXT("TC_%d"), static_cast<int32>(Value));
		}
	}

	bool ParseTextureGroup(const FString& Name, TextureGroup& Out)
	{
		if (Name.Equals(TEXT("TEXTUREGROUP_World"), ESearchCase::IgnoreCase))          { Out = TEXTUREGROUP_World;          return true; }
		if (Name.Equals(TEXT("TEXTUREGROUP_WorldNormalMap"), ESearchCase::IgnoreCase)) { Out = TEXTUREGROUP_WorldNormalMap; return true; }
		return false;
	}

	FString TextureGroupToText(TextureGroup Value)
	{
		switch (Value)
		{
		case TEXTUREGROUP_World:          return TEXT("TEXTUREGROUP_World");
		case TEXTUREGROUP_WorldNormalMap: return TEXT("TEXTUREGROUP_WorldNormalMap");
		default:                          return FString::Printf(TEXT("TEXTUREGROUP_%d"), static_cast<int32>(Value));
		}
	}

	bool ParseTextureAddress(const FString& Name, TextureAddress& Out)
	{
		if (Name.Equals(TEXT("TA_WRAP"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("TA_Wrap"), ESearchCase::IgnoreCase))
		{
			Out = TA_Wrap;
			return true;
		}
		if (Name.Equals(TEXT("TA_CLAMP"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("TA_Clamp"), ESearchCase::IgnoreCase))
		{
			Out = TA_Clamp;
			return true;
		}
		if (Name.Equals(TEXT("TA_MIRROR"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("TA_Mirror"), ESearchCase::IgnoreCase))
		{
			Out = TA_Mirror;
			return true;
		}
		return false;
	}

	FString TextureAddressToText(TextureAddress Value)
	{
		switch (Value)
		{
		case TA_Wrap:   return TEXT("TA_Wrap");
		case TA_Clamp:  return TEXT("TA_Clamp");
		case TA_Mirror: return TEXT("TA_Mirror");
		default:        return FString::Printf(TEXT("TA_%d"), static_cast<int32>(Value));
		}
	}

	/** 保存资产并登记。 */
	bool SaveImportedAsset(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("资产为空");
			return false;
		}
		FAssetRegistryModule::AssetCreated(Asset);

		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			OutError = TEXT("资产没有 Outer Package");
			return false;
		}
		Package->MarkPackageDirty();

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.bWarnOfLongFilename = false;

		const FSavePackageResultStruct SaveResult = UPackage::Save(Package, Asset, *FileName, Args);
		if (SaveResult.Result != ESavePackageResult::Success)
		{
			OutError = FString::Printf(TEXT("保存失败（result %d）"), static_cast<int32>(SaveResult.Result));
			return false;
		}
		return true;
	}
}

// --------------------------------------------------------------------------- //
// 单张导入 + 属性 + 回读自检
// --------------------------------------------------------------------------- //

bool FComfyTextureImportService::ImportOne(
	const FString& Semantic,
	const FComfyOutputAssetSpec& Spec,
	const FString& PngAbsPath,
	const FComfyImportRequest& Request,
	FComfyTextureImportEntry& OutEntry,
	FString& OutError)
{
	OutEntry.SourcePng = PngAbsPath;

	if (!IFileManager::Get().FileExists(*PngAbsPath))
	{
		OutError = FString::Printf(TEXT("中转 PNG 不存在：%s"), *PngAbsPath);
		return false;
	}
	if (Spec.Suffix.IsEmpty())
	{
		OutError = FString::Printf(TEXT("通道 %s 在清单里没有定义资产名后缀。"), *Semantic);
		return false;
	}

	// ---- 1. 导入 ----
	const FString AssetName = Request.GetTextureName(Spec.Suffix);
	OutEntry.AssetName = AssetName;
	OutEntry.PackagePath = FString::Printf(TEXT("%s/%s"), *Request.DestinationPath, *AssetName);

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = PngAbsPath;
	Task->DestinationPath = Request.DestinationPath;
	Task->DestinationName = AssetName;
	Task->bAutomated = true;          // 不弹任何对话框
	Task->bReplaceExisting = true;    // 重跑时覆盖自己的旧产物
	Task->bSave = false;              // 属性设完再统一落盘，避免写两遍
	Task->bAsync = false;
	Task->Options = nullptr;          // 走 TextureFactory 默认，不手搓 Options

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	TArray<UAssetImportTask*> Tasks;
	Tasks.Add(Task);
	AssetTools.ImportAssetTasks(Tasks);

	UTexture2D* Texture = nullptr;
	for (UObject* Obj : Task->GetObjects())
	{
		if (UTexture2D* AsTexture = Cast<UTexture2D>(Obj))
		{
			Texture = AsTexture;
			break;
		}
	}
	if (!Texture)
	{
		OutError = FString::Printf(TEXT("导入 %s 后拿不到 UTexture2D。"), *PngAbsPath);
		return false;
	}

	// ---- 2. 按 PRD §6.3 逐项写属性 ----
	TextureCompressionSettings Compression = TC_Default;
	if (!ParseCompressionSettings(Spec.CompressionSettings, Compression))
	{
		OutError = FString::Printf(TEXT("清单里的 compressionSettings 值不认识：`%s`（通道 %s）"),
			*Spec.CompressionSettings, *Semantic);
		return false;
	}

	TextureGroup LODGroup = TEXTUREGROUP_World;
	if (!ParseTextureGroup(Spec.LODGroup, LODGroup))
	{
		OutError = FString::Printf(TEXT("清单里的 lodGroup 值不认识：`%s`（通道 %s）"),
			*Spec.LODGroup, *Semantic);
		return false;
	}

	TextureAddress Address = TA_Wrap;
	if (!ParseTextureAddress(Spec.AddressMode, Address))
	{
		OutError = FString::Printf(TEXT("清单里的 addressMode 值不认识：`%s`（通道 %s）"),
			*Spec.AddressMode, *Semantic);
		return false;
	}

	Texture->CompressionSettings = Compression;
	Texture->SRGB = Spec.bSRGB;
	Texture->LODGroup = LODGroup;
	Texture->AddressX = Address;
	Texture->AddressY = Address;
	// MipGenSettings 在清单里没有对应项：这是引擎默认值，保持"跟随纹理组"
	Texture->MipGenSettings = TMGS_FromTextureGroup;
	// D3：**只有法线**需要翻绿通道。
	//
	// 分工：清单里的 `ue.flipGreenChannel` 决定「该通道是否需要翻」（只有 normal 为 true），
	// 请求里的 `bFlipGreenChannel` 是 D3 这条策略的总开关（用户可在面板上关掉，
	// 比如哪天把 ComfyUI 的 50.directx 打开、输出改成 DirectX 了）。
	// 两个都成立才翻。
	//
	// ⚠ 这里踩过一次坑：最初写成「全局一个布尔值」，于是 albedo / ORM / height 也被
	// 标上了 FlipG。属性表自检把这一行照了出来 —— 这正是回读自检存在的意义。
	const bool bEffectiveFlipGreen = Spec.bFlipGreenChannel && Request.bFlipGreenChannel;
	Texture->bFlipGreenChannel = bEffectiveFlipGreen;

	Texture->UpdateResource();

	// ---- 3. 落盘 ----
	FString SaveError;
	if (!SaveImportedAsset(Texture, SaveError))
	{
		OutError = FString::Printf(TEXT("贴图 %s %s"), *AssetName, *SaveError);
		return false;
	}

	OutEntry.bImported = true;

	// ---- 4. 回读自检（G3 的验收依据）----
	// 注意读的是**已落盘的 UTexture2D 字段**，不是我们刚写的局部变量。
	OutEntry.CompressionSettings = CompressionSettingsToText(Texture->CompressionSettings);
	OutEntry.bSRGB = Texture->SRGB != 0;
	OutEntry.bFlipGreenChannel = Texture->bFlipGreenChannel != 0;
	OutEntry.LODGroup = TextureGroupToText(Texture->LODGroup);
	OutEntry.AddressX = TextureAddressToText(Texture->AddressX);
	OutEntry.AddressY = TextureAddressToText(Texture->AddressY);
	OutEntry.MipGenSettings = TEXT("TMGS_FromTextureGroup");

	const auto Check = [&OutEntry](bool bOk, const TCHAR* Field, const FString& Expect, const FString& Actual)
	{
		if (!bOk)
		{
			OutEntry.PropertyMismatches.Add(FString::Printf(TEXT("%s 期望 %s，实际 %s"), Field, *Expect, *Actual));
		}
	};

	Check(OutEntry.CompressionSettings == Spec.CompressionSettings,
		TEXT("CompressionSettings"), Spec.CompressionSettings, OutEntry.CompressionSettings);
	Check(OutEntry.bSRGB == Spec.bSRGB,
		TEXT("SRGB"), Spec.bSRGB ? TEXT("true") : TEXT("false"), OutEntry.bSRGB ? TEXT("true") : TEXT("false"));
	Check(OutEntry.bFlipGreenChannel == bEffectiveFlipGreen,
		TEXT("FlipGreenChannel"), bEffectiveFlipGreen ? TEXT("true") : TEXT("false"),
		OutEntry.bFlipGreenChannel ? TEXT("true") : TEXT("false"));
	Check(OutEntry.LODGroup == Spec.LODGroup, TEXT("LODGroup"), Spec.LODGroup, OutEntry.LODGroup);
	Check(OutEntry.AddressX.Equals(TEXT("TA_Wrap")), TEXT("AddressX"), TEXT("TA_Wrap"), OutEntry.AddressX);
	Check(OutEntry.AddressY.Equals(TEXT("TA_Wrap")), TEXT("AddressY"), TEXT("TA_Wrap"), OutEntry.AddressY);

	OutEntry.bPropertyCheckPassed = (OutEntry.PropertyMismatches.Num() == 0);

	// 三个最常出错的点单独再确认一遍（PRD §6.3 的"必须写进导入后自检"）
	if (Semantic == TEXT("orm") && OutEntry.bSRGB)
	{
		OutEntry.PropertyMismatches.Add(
			TEXT("ORM 的 sRGB 为 true —— AO/粗糙度/金属度会被 gamma 整体拉偏。"));
		OutEntry.bPropertyCheckPassed = false;
	}
	if (Semantic == TEXT("normal") && !OutEntry.bFlipGreenChannel)
	{
		OutEntry.PropertyMismatches.Add(
			TEXT("法线未翻绿通道 —— 凹凸方向会反（ComfyUI 出 OpenGL，UE 要 DirectX）。"));
		OutEntry.bPropertyCheckPassed = false;
	}
	if (Semantic != TEXT("normal") && OutEntry.bFlipGreenChannel)
	{
		OutEntry.PropertyMismatches.Add(
			TEXT("非法线通道不该翻绿通道 —— 会破坏该贴图的绿色分量。"));
		OutEntry.bPropertyCheckPassed = false;
	}

	ImportedTextures.Add(Semantic, Texture);
	return true;
}

UTexture2D* FComfyTextureImportService::FindImportedTexture(
	const TArray<FComfyTextureImportEntry>& Entries,
	const FString& Semantic) const
{
	for (const FComfyTextureImportEntry& E : Entries)
	{
		if (E.Semantic == Semantic && E.bImported)
		{
			// 本函数是 const：Find 返回的是 const 指针
			if (const TObjectPtr<UTexture2D>* Found = ImportedTextures.Find(Semantic))
			{
				return Found->Get();
			}
		}
	}
	return nullptr;
}

// --------------------------------------------------------------------------- //
// 冷加载核对：新进程里从磁盘读回资产，逐项核 §6.3 与材质图结构
// --------------------------------------------------------------------------- //

bool FComfyTextureImportService::VerifyImportedAssets(
	const FString& DestinationPath, const FString& BaseName, FComfyAssetVerification& Out)
{
	Out = FComfyAssetVerification();

	const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>();
	if (!Settings)
	{
		Out.Lines.Add(TEXT("读不到插件设置"));
		return false;
	}

	TMap<FString, FComfyOutputAssetSpec> Specs;
	FString SpecError;
	if (!LoadStageBSpecs(*Settings, Specs, SpecError))
	{
		Out.Lines.Add(SpecError);
		return false;
	}

	// 计一项核对；不一致时把期望与实际并排写出
	const auto Check = [&Out](bool bOk, const TCHAR* What, const FString& Expect, const FString& Actual)
	{
		++Out.Checked;
		if (bOk)
		{
			Out.Lines.Add(FString::Printf(TEXT("  ✓ %s = %s"), What, *Actual));
		}
		else
		{
			++Out.Failed;
			Out.Lines.Add(FString::Printf(TEXT("  ✗ %s 期望 %s，实际 %s"), What, *Expect, *Actual));
		}
	};

	// ------------------------------------------------------------------ 贴图 //
	Out.Lines.Add(TEXT(""));
	Out.Lines.Add(TEXT("[贴图] 从磁盘重新加载"));

	int32 LoadedTextures = 0;
	for (const FString& Semantic : ComfyChannels::All())
	{
		const FComfyOutputAssetSpec* Spec = Specs.Find(Semantic);
		if (!Spec)
		{
			continue;
		}
		if (!Spec->bImportByDefault)
		{
			Out.Lines.Add(FString::Printf(TEXT("  — %s 未导入（orphan，Q3）"), *Semantic));
			continue;
		}
		if (Semantic == TEXT("height") && !Settings->DefaultImportHeight)
		{
			Out.Lines.Add(FString::Printf(TEXT("  — %s 未导入（用户关闭了视差）"), *Semantic));
			continue;
		}

		const FString AssetName = FString::Printf(TEXT("T_%s%s"), *BaseName, *Spec->Suffix);
		const FString PackagePath = FString::Printf(TEXT("%s/%s"), *DestinationPath, *AssetName);

		// 关键：这里**没有**任何内存里的对象，全部从磁盘 LoadObject
		UTexture2D* Texture = LoadObject<UTexture2D>(
			nullptr, *FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName));
		++Out.Checked;
		if (!Texture)
		{
			++Out.Failed;
			Out.Lines.Add(FString::Printf(TEXT("  ✗ %s 加载失败：%s"), *AssetName, *PackagePath));
			continue;
		}
		++LoadedTextures;
		Out.Lines.Add(FString::Printf(TEXT("  ✓ %s 加载成功"), *AssetName));

		Check(CompressionSettingsToText(Texture->CompressionSettings) == Spec->CompressionSettings,
			*FString::Printf(TEXT("%s.CompressionSettings"), *Spec->Suffix),
			Spec->CompressionSettings, CompressionSettingsToText(Texture->CompressionSettings));

		Check((Texture->SRGB != 0) == Spec->bSRGB,
			*FString::Printf(TEXT("%s.SRGB"), *Spec->Suffix),
			Spec->bSRGB ? TEXT("true") : TEXT("false"),
			Texture->SRGB ? TEXT("true") : TEXT("false"));

		// D3：只有法线该翻绿通道（且以「D3 总开关」为前提）
		const bool bExpectFlipGreen = Spec->bFlipGreenChannel && Settings->DefaultFlipGreenChannel;
		Check((Texture->bFlipGreenChannel != 0) == bExpectFlipGreen,
			*FString::Printf(TEXT("%s.FlipGreenChannel"), *Spec->Suffix),
			bExpectFlipGreen ? TEXT("true") : TEXT("false"),
			Texture->bFlipGreenChannel ? TEXT("true") : TEXT("false"));

		Check(TextureGroupToText(Texture->LODGroup) == Spec->LODGroup,
			*FString::Printf(TEXT("%s.LODGroup"), *Spec->Suffix),
			Spec->LODGroup, TextureGroupToText(Texture->LODGroup));

		Check(TextureAddressToText(Texture->AddressX) == TEXT("TA_Wrap")
			&& TextureAddressToText(Texture->AddressY) == TEXT("TA_Wrap"),
			*FString::Printf(TEXT("%s.AddressX/Y"), *Spec->Suffix),
			TEXT("TA_Wrap"), FString::Printf(TEXT("%s / %s"),
				*TextureAddressToText(Texture->AddressX), *TextureAddressToText(Texture->AddressY)));
	}

	// ------------------------------------------------------------------ 材质 //
	Out.Lines.Add(TEXT(""));
	Out.Lines.Add(TEXT("[材质] 从磁盘重新加载"));

	const FString MaterialName = FString::Printf(TEXT("M_%s"), *BaseName);
	const FString MaterialPath = FString::Printf(TEXT("%s/%s"), *DestinationPath, *MaterialName);
	UMaterial* Material = LoadObject<UMaterial>(
		nullptr, *FString::Printf(TEXT("%s.%s"), *MaterialPath, *MaterialName));

	++Out.Checked;
	if (!Material)
	{
		++Out.Failed;
		Out.Lines.Add(FString::Printf(TEXT("  ✗ %s 加载失败：%s"), *MaterialName, *MaterialPath));
		Out.bAllOk = false;
		return false;
	}
	Out.Lines.Add(FString::Printf(TEXT("  ✓ %s 加载成功"), *MaterialName));

	Check(Material->MaterialDomain == MD_Surface, TEXT("MaterialDomain"),
		TEXT("MD_Surface"), FString::Printf(TEXT("%d"), static_cast<int32>(Material->MaterialDomain.GetValue())));
	Check(Material->BlendMode == BLEND_Opaque, TEXT("BlendMode"),
		TEXT("BLEND_Opaque"), FString::Printf(TEXT("%d"), static_cast<int32>(Material->BlendMode.GetValue())));

	// 找 BumpOffset —— 这是 Q4 的核心：视差必须真的影响三张交付贴图
	UMaterialExpressionBumpOffset* Bump = nullptr;
	TArray<UMaterialExpression*> Samples;   // 贴图采样节点
	for (const TObjectPtr<UMaterialExpression>& Expr : Material->GetExpressions())
	{
		if (!Expr)
		{
			continue;
		}
		if (UMaterialExpressionBumpOffset* AsBump = Cast<UMaterialExpressionBumpOffset>(Expr.Get()))
		{
			Bump = AsBump;
		}
		if (Expr->IsA<UMaterialExpressionTextureSampleParameter2D>())
		{
			Samples.Add(Expr.Get());
		}
	}

	++Out.Checked;
	if (!Bump)
	{
		++Out.Failed;
		Out.Lines.Add(TEXT("  ✗ 材质里没有 BumpOffset 节点（视差缺失）"));
	}
	else
	{
		Out.Lines.Add(TEXT("  ✓ 找到 BumpOffset 节点"));

		FExpressionInput* HeightIn = FComfyMaterialBuilder::FindInputByName(Bump, TEXT("Height"));
		Check(HeightIn && HeightIn->Expression != nullptr && HeightIn->Expression->IsA<UMaterialExpressionTextureSampleParameter2D>(),
			TEXT("BumpOffset.Height"), TEXT("接 height 贴图采样"),
			HeightIn && HeightIn->Expression ? HeightIn->Expression->GetClass()->GetName() : TEXT("未接"));

		FExpressionInput* CoordIn = FComfyMaterialBuilder::FindInputByName(Bump, TEXT("Coordinate"));
		Check(CoordIn && CoordIn->Expression != nullptr,
			TEXT("BumpOffset.Coordinate"), TEXT("接平铺后的 UV"),
			CoordIn && CoordIn->Expression ? CoordIn->Expression->GetClass()->GetName() : TEXT("未接"));

		FExpressionInput* RatioIn = FComfyMaterialBuilder::FindInputByName(Bump, TEXT("HeightRatioInput"));
		Check(RatioIn && RatioIn->Expression != nullptr && RatioIn->Expression->IsA<UMaterialExpressionScalarParameter>(),
			TEXT("BumpOffset.HeightRatio"), TEXT("接 BumpDepth 标量参数"),
			RatioIn && RatioIn->Expression ? RatioIn->Expression->GetClass()->GetName() : TEXT("未接"));
	}

	// 数一数有几张交付贴图的 Coordinates 接在 BumpOffset 上 ——
	// 只接 BaseColor 是本设计里最典型的错误，必须能被这一项抓出来
	int32 Driven = 0;
	for (UMaterialExpression* Sample : Samples)
	{
		if (FExpressionInput* In = FComfyMaterialBuilder::FindInputByName(Sample, TEXT("Coordinates")))
		{
			if (Bump && In->Expression == Bump)
			{
				++Driven;
			}
		}
	}
	Check(Driven == 3, TEXT("视差驱动的采样数"), TEXT("3"), FString::Printf(TEXT("%d"), Driven));

	// ------------------------------------------------------------------ 实例 //
	Out.Lines.Add(TEXT(""));
	Out.Lines.Add(TEXT("[材质实例] 从磁盘重新加载"));

	const FString MIName = FString::Printf(TEXT("MI_%s"), *BaseName);
	const FString MIPath = FString::Printf(TEXT("%s/%s"), *DestinationPath, *MIName);
	UMaterialInstanceConstant* MI = LoadObject<UMaterialInstanceConstant>(
		nullptr, *FString::Printf(TEXT("%s.%s"), *MIPath, *MIName));

	++Out.Checked;
	if (!MI)
	{
		++Out.Failed;
		Out.Lines.Add(FString::Printf(TEXT("  ✗ %s 加载失败：%s"), *MIName, *MIPath));
	}
	else
	{
		Out.Lines.Add(FString::Printf(TEXT("  ✓ %s 加载成功"), *MIName));
		Check(MI->Parent == Material, TEXT("MI 的父材质"), MaterialName,
			MI->Parent ? MI->Parent->GetName() : TEXT("无"));

		// 三个标量参数必须真的落在实例上（这样美术才能在实例上改而不动母材质）
		TMap<FString, float> InstanceScalars;
		for (const FScalarParameterValue& SV : MI->ScalarParameterValues)
		{
			InstanceScalars.Add(SV.ParameterInfo.Name.ToString(), SV.ParameterValue);
		}
		for (const TCHAR* ParamName : { FComfyMaterialBuilder::ParamTiling(),
		                                FComfyMaterialBuilder::ParamNormalStrength(),
		                                FComfyMaterialBuilder::ParamBumpDepth() })
		{
			const float* Found = InstanceScalars.Find(ParamName);
			Check(Found != nullptr, *FString::Printf(TEXT("MI 覆盖参数 %s"), ParamName),
				TEXT("存在"), Found ? FString::Printf(TEXT("%.4f"), *Found) : TEXT("缺失"));
		}
	}

	Out.bAllOk = (Out.Failed == 0);
	return Out.bAllOk;
}

// --------------------------------------------------------------------------- //
// 主流程
// --------------------------------------------------------------------------- //

FComfyImportResult FComfyTextureImportService::ImportAndBuildMaterial(const FComfyImportRequest& Request)
{
	FComfyImportResult Result;
	FComfyImportReport& Report = Result.Report;

	Report.BaseName = Request.BaseName;
	Report.DestinationPath = Request.DestinationPath;
	Report.GeneratedAt = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"));
	Report.Material.MaterialName = Request.MaterialName;
	Report.Material.MaterialInstanceName = Request.MaterialInstanceName;

	ImportedTextures.Reset();

	const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>();
	if (!Settings)
	{
		Result.ErrorText = TEXT("读不到插件设置。");
		return Result;
	}
	if (Request.BaseName.IsEmpty() || Request.DestinationPath.IsEmpty())
	{
		Result.ErrorText = TEXT("资产名或落点为空 —— 请先解析命名。");
		return Result;
	}
	if (!Request.DestinationPath.StartsWith(TEXT("/Game")))
	{
		Result.ErrorText = FString::Printf(TEXT("资产落点必须是 /Game 下的包路径，当前为 %s。"), *Request.DestinationPath);
		return Result;
	}

	TMap<FString, FComfyOutputAssetSpec> Specs;
	FString Error;
	if (!LoadStageBSpecs(*Settings, Specs, Error))
	{
		Result.ErrorText = Error;
		return Result;
	}

	// ---- 逐通道导入 ----
	// 顺序取自 manifest 的 outputAssets（ComfyChannels::All() 的固定序），报告行序稳定。
	TArray<FString> Semantics = ComfyChannels::All();
	for (const FString& Semantic : Semantics)
	{
		const FComfyOutputAssetSpec* Spec = Specs.Find(Semantic);
		if (!Spec)
		{
			continue; // 清单里没有这个通道（比如换了工作流）
		}

		FComfyTextureImportEntry Entry;
		Entry.Semantic = Semantic;
		Entry.Role = Spec->Role;

		const bool bIsOrphan = !Spec->bImportByDefault;
		bool bShouldImport = Spec->bImportByDefault;
		if (bIsOrphan)
		{
			bShouldImport = Request.bImportOrphanMaps;
		}
		if (Semantic == TEXT("height"))
		{
			bShouldImport = bShouldImport && Request.bImportHeight;
		}

		if (!bShouldImport)
		{
			Entry.bImported = false;
			if (bIsOrphan && !Request.bImportOrphanMaps)
			{
				Entry.SkipReason = Spec->OrphanReason.IsEmpty()
					? TEXT("ORM 合并图已含此通道，默认不导入（Q3）")
					: Spec->OrphanReason;
				Report.SkippedOrphans.Add(FString::Printf(TEXT("%s：未导入（%s）"),
					*ComfyChannels::DisplayName(Semantic), *Entry.SkipReason));
			}
			else if (Semantic == TEXT("height"))
			{
				Entry.SkipReason = TEXT("用户关闭了高度图导入（材质里不做视差）");
			}
			else
			{
				Entry.SkipReason = TEXT("未勾选导入");
			}
			Entry.SourcePng = Request.ChannelPngPaths.FindRef(Semantic);
			Report.Textures.Add(MoveTemp(Entry));
			continue;
		}

		const FString* PngPath = Request.ChannelPngPaths.Find(Semantic);
		if (!PngPath || PngPath->IsEmpty())
		{
			Entry.bImported = false;
			Entry.SkipReason = TEXT("没有可用的中转文件（上游可能没产出这个通道）");
			Report.Textures.Add(MoveTemp(Entry));
			continue;
		}

		FString ImportError;
		const bool bOk = ImportOne(Semantic, *Spec, *PngPath, Request, Entry, ImportError);
		if (!bOk)
		{
			Entry.bImported = false;
			Entry.SkipReason = ImportError;
			// albedo 缺失材质就建不起来；其余通道失败只警告，让用户看到完整局面
			if (Semantic == TEXT("albedo"))
			{
				Result.ErrorText = FString::Printf(TEXT("基础色导入失败：%s"), *ImportError);
			}
		}
		Report.Textures.Add(MoveTemp(Entry));
	}

	if (!Result.ErrorText.IsEmpty())
	{
		Result.bSuccess = false;
		Result.Report.Material.bCreated = false;
		return Result;
	}

	// ---- 建材质 ----
	FComfyMaterialBuilder::FInputs BuildInputs;
	BuildInputs.DestinationPath = Request.DestinationPath;
	BuildInputs.MaterialName = Request.MaterialName;
	BuildInputs.MaterialInstanceName = Request.MaterialInstanceName;
	BuildInputs.Albedo = FindImportedTexture(Report.Textures, TEXT("albedo"));
	BuildInputs.Normal = FindImportedTexture(Report.Textures, TEXT("normal"));
	BuildInputs.Height = FindImportedTexture(Report.Textures, TEXT("height"));
	BuildInputs.ORM = FindImportedTexture(Report.Textures, TEXT("orm"));
	BuildInputs.bUseHeightParallax = Request.bImportHeight;
	BuildInputs.bCreateInstance = Request.bCreateMaterialInstance;
	BuildInputs.BumpDepth = Request.BumpDepth;

	FString BuildError;
	const bool bBuilt = FComfyMaterialBuilder::Build(BuildInputs, Report.Material, BuildError);

	// 材质建成了但编译有问题时，Build 返回 false 而 Report.Material 里已有诊断 —— 都算失败
	if (!bBuilt)
	{
		Result.bSuccess = false;
		Result.ErrorText = BuildError;
	}
	else
	{
		Result.bSuccess = true;
	}

	// ---- 收集新建资产路径（供「在 Content Browser 中显示」）----
	for (const FComfyTextureImportEntry& E : Report.Textures)
	{
		if (E.bImported)
		{
			Result.CreatedAssetPaths.Add(E.PackagePath);
		}
	}
	if (Report.Material.bCreated)
	{
		Result.CreatedAssetPaths.Add(Report.Material.MaterialPackagePath);
	}
	if (Report.Material.bInstanceCreated)
	{
		Result.CreatedAssetPaths.Add(Report.Material.MaterialInstancePackagePath);
	}

	// ---- 清理中转 PNG（可选）----
	if (Request.bDeleteSourcePng)
	{
		for (const TPair<FString, FString>& Pair : Request.ChannelPngPaths)
		{
			if (!Pair.Value.IsEmpty())
			{
				IFileManager::Get().Delete(*Pair.Value, /*RequireExists=*/false);
			}
		}
	}

	return Result;
}
