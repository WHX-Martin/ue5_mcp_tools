#include "ComfyMaterialBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ComfySamplerType.h"   // SamplerType 的单一权威来源（引擎推导 + 引擎校验器）
#include "ComfyTextureStudioEditor.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionBumpOffset.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "RHI.h"                // GMaxRHIShaderPlatform
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	// ------------------------------------------------------------------ 工具 //

	/** 表达式在材质图里的排布位置。 */
	void Place(UMaterialExpression* Expr, int32 X, int32 Y)
	{
		if (Expr)
		{
			Expr->MaterialExpressionEditorX = X;
			Expr->MaterialExpressionEditorY = Y;
		}
	}

	/** 连内部节点：把 From 的第 OutIndex 个输出接到 To 的名为 InName 的输入。 */
	bool ConnectNodes(UMaterialExpression* From, int32 OutIndex, UMaterialExpression* To, const TCHAR* InName,
		TArray<FString>& OutProblems, const FString& What)
	{
		if (!From || !To)
		{
			OutProblems.Add(FString::Printf(TEXT("%s：端点为空"), *What));
			return false;
		}
		FExpressionInput* Input = FComfyMaterialBuilder::FindInputByName(To, InName);
		if (!Input)
		{
			// 把可用输入名列出来 —— 报错要能直接定位，而不是只说“连不上”
			TArray<FString> Available;
			for (FExpressionInputIterator It{ To }; It; ++It)
			{
				Available.Add(To->GetInputName(It.Index).ToString());
			}
			OutProblems.Add(FString::Printf(TEXT("%s：在 %s 上找不到名为 `%s` 的输入（可用：%s）"),
				*What, *To->GetClass()->GetName(), InName, *FString::Join(Available, TEXT(", "))));
			return false;
		}
		Input->Connect(OutIndex, From);
		// 立刻回读，确保真的接上了（Connect 不返回值）
		if (Input->Expression != From || Input->OutputIndex != OutIndex)
		{
			OutProblems.Add(FString::Printf(TEXT("%s：连接后回读不符"), *What));
			return false;
		}
		return true;
	}

	/**
	 * 连材质主属性。
	 *
	 * TextureSample 的输出索引是构造函数里硬编码的固定顺序：
	 *   0=RGB  1=R  2=G  3=B  4=A  5=RGBA
	 * 按索引连比按名字连确定得多。
	 */
	bool ConnectProperty(UMaterial* Material, UMaterialExpression* From, int32 OutIndex,
		EMaterialProperty Property, TArray<FString>& OutProblems, const FString& What)
	{
		if (!From)
		{
			OutProblems.Add(FString::Printf(TEXT("%s：源表达式为空"), *What));
			return false;
		}
		FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
		if (!Input)
		{
			OutProblems.Add(FString::Printf(TEXT("%s：材质没有该属性输入（property %d）"), *What, static_cast<int32>(Property)));
			return false;
		}
		Input->Connect(OutIndex, From);
		if (Input->Expression != From || Input->OutputIndex != OutIndex)
		{
			OutProblems.Add(FString::Printf(TEXT("%s：连接后回读不符"), *What));
			return false;
		}
		return true;
	}

	/** 保存资产并登记到资产注册表。 */
	bool SaveAssetPackage(UObject* Asset, TArray<FString>& OutProblems, const FString& What)
	{
		if (!Asset)
		{
			OutProblems.Add(FString::Printf(TEXT("%s：资产为空"), *What));
			return false;
		}

		FAssetRegistryModule::AssetCreated(Asset);

		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			OutProblems.Add(FString::Printf(TEXT("%s：资产没有 Outer Package"), *What));
			return false;
		}
		Package->MarkPackageDirty();

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.bWarnOfLongFilename = false;

		const FSavePackageResultStruct Result = UPackage::Save(Package, Asset, *FileName, Args);
		if (Result.Result != ESavePackageResult::Success)
		{
			OutProblems.Add(FString::Printf(TEXT("%s：保存失败（result %d，路径 %s）"),
				*What, static_cast<int32>(Result.Result), *FileName));
			return false;
		}
		return true;
	}

	/** 建一个标量参数节点。 */
	UMaterialExpressionScalarParameter* MakeScalarParam(
		UMaterial* Material, const TCHAR* Name, float Default, int32 X, int32 Y)
	{
		UMaterialExpressionScalarParameter* Param = Cast<UMaterialExpressionScalarParameter>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				Material, UMaterialExpressionScalarParameter::StaticClass(), X, Y));
		if (Param)
		{
			Param->ParameterName = Name;
			Param->DefaultValue = Default;
			// 给材质实例编辑器一个好用的滑条范围
			Param->SliderMin = 0.0f;
			Param->SliderMax = FMath::Max(1.0f, Default * 4.0f);
		}
		return Param;
	}

	/**
	 * 建一个贴图采样参数节点。
	 *
	 * SamplerType **刻意不由调用方传入**：它必须与所采样贴图的压缩格式对齐，传错一个
	 * 枚举就会做出一个"打不开的材质"（引擎报 `Sampler type is X, should be Y`，是 error
	 * 级，材质编译直接失败）。
	 *
	 * 期望值统一取 ComfySamplerType::ForTexture（即引擎自己的推导）—— 调用方说了不算。
	 * 这条曾经真的踩过：PRD §6.2 原文写 ORM→LinearColor、height→Grayscale，两条都错，
	 * 而代码忠实实现了错规范，于是编辑器里一打开材质就是两行红字，commandlet 却报"0 错误"。
	 *
	 * 做法是「设值 → 立刻回读断言」（见 Build 里的 VerifySamplerTypes），
	 * 而不是信"我刚设对了"。
	 */
	UMaterialExpressionTextureSampleParameter2D* MakeTextureSample(
		UMaterial* Material, UTexture2D* Texture, const TCHAR* ParamName, int32 X, int32 Y)
	{
		UMaterialExpressionTextureSampleParameter2D* Sample = Cast<UMaterialExpressionTextureSampleParameter2D>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), X, Y));
		if (Sample)
		{
			Sample->Texture = Texture;
			Sample->SamplerType = ComfySamplerType::ForTexture(Texture);
			// 用 TextureSampleParameter2D（不是 TextureSample）就是为了让材质实例能换贴图
			Sample->ParameterName = ParamName;
			Sample->MaterialExpressionEditorX = X;
			Sample->MaterialExpressionEditorY = Y;
		}
		return Sample;
	}
}

// --------------------------------------------------------------------------- //
// 输入端口查找（构建与核对共用同一份逻辑）
// --------------------------------------------------------------------------- //

FExpressionInput* FComfyMaterialBuilder::FindInputByName(UMaterialExpression* Expr, const TCHAR* Name)
{
	if (!Expr)
	{
		return nullptr;
	}

	const FString Wanted(Name);
	const auto Matches = [&Wanted](const FString& Candidate)
	{
		if (Candidate.Equals(Wanted, ESearchCase::IgnoreCase))
		{
			return true;
		}
		// `HeightRatioInput` ↔ `HeightRatio` 都认
		if (Candidate.EndsWith(TEXT("Input"), ESearchCase::IgnoreCase))
		{
			return Candidate.LeftChop(5).Equals(Wanted, ESearchCase::IgnoreCase);
		}
		return false;
	};

	for (FExpressionInputIterator It{ Expr }; It; ++It)
	{
		if (Matches(Expr->GetInputName(It.Index).ToString()))
		{
			return It.Input;
		}
	}
	return nullptr;
}

// --------------------------------------------------------------------------- //
// 主流程
// --------------------------------------------------------------------------- //

bool FComfyMaterialBuilder::Build(const FInputs& In, FComfyMaterialBuildEntry& OutEntry, FString& OutError)
{
	OutEntry.MaterialName = In.MaterialName;
	OutEntry.MaterialPackagePath = FString::Printf(TEXT("%s/%s"), *In.DestinationPath, *In.MaterialName);
	OutEntry.MaterialInstanceName = In.MaterialInstanceName;
	OutEntry.MaterialInstancePackagePath = FString::Printf(TEXT("%s/%s"), *In.DestinationPath, *In.MaterialInstanceName);
	OutEntry.SamplesExpected = 3;

	TArray<FString> Problems;

	if (!In.Albedo)
	{
		OutError = TEXT("缺少 albedo 贴图，无法建材质。");
		return false;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

	// ---- 1. 建材质资产 ----
	UMaterialFactoryNew* MaterialFactory = NewObject<UMaterialFactoryNew>();
	UMaterial* Material = Cast<UMaterial>(AssetTools.CreateAsset(
		In.MaterialName, In.DestinationPath, UMaterial::StaticClass(), MaterialFactory,
		NAME_None, /*bOverwriteExisting=*/false));

	if (!Material)
	{
		OutError = FString::Printf(TEXT("创建材质 %s 失败（同名资产可能已存在）。"), *In.MaterialName);
		return false;
	}
	OutEntry.bCreated = true;

	Material->MaterialDomain = MD_Surface;
	Material->BlendMode = BLEND_Opaque;
	// UE 5.8 把 ShadingModel 收成了 private，只能走 setter（它会一并维护 ShadingModels 位域）
	Material->SetShadingModel(MSM_DefaultLit);
	Material->TwoSided = false;
	Material->SetFlags(RF_Transactional);

	// ---- 2. 表达式骨架 ----
	// 横向布局刻意留宽：材质图在编辑器里打开时不会挤成一团
	UMaterialExpressionTextureCoordinate* TexCoord = Cast<UMaterialExpressionTextureCoordinate>(
		UMaterialEditingLibrary::CreateMaterialExpression(
			Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1400, -100));
	if (TexCoord)
	{
		TexCoord->CoordinateIndex = 0;
		TexCoord->UTiling = 1.0f;  // 平铺交给 Tiling 参数，不用 TexCoord 自带的（它不能参数化）
		TexCoord->VTiling = 1.0f;
	}

	UMaterialExpressionScalarParameter* TilingParam =
		MakeScalarParam(Material, ParamTiling(), DefaultTiling, -1400, 80);

	UMaterialExpressionMultiply* TilingMul = Cast<UMaterialExpressionMultiply>(
		UMaterialEditingLibrary::CreateMaterialExpression(
			Material, UMaterialExpressionMultiply::StaticClass(), -1150, -60));

	// 采样节点
	// SamplerType 不由这里指定 —— 由贴图的压缩格式推导（见 MakeTextureSample 的注释）
	UMaterialExpressionTextureSampleParameter2D* AlbedoSample = MakeTextureSample(
		Material, In.Albedo, TEXT("BaseColorTexture"), -150, -400);
	UMaterialExpressionTextureSampleParameter2D* NormalSample = MakeTextureSample(
		Material, In.Normal, TEXT("NormalTexture"), -150, -20);
	UMaterialExpressionTextureSampleParameter2D* ORMSample = MakeTextureSample(
		Material, In.ORM, TEXT("ORMTexture"), -150, 380);

	UMaterialExpressionTextureSampleParameter2D* HeightSample = nullptr;
	UMaterialExpressionBumpOffset* BumpOffset = nullptr;
	UMaterialExpressionScalarParameter* BumpDepthParam = nullptr;

	if (In.bUseHeightParallax && In.Height)
	{
		HeightSample = MakeTextureSample(
			Material, In.Height, TEXT("HeightTexture"), -900, 620);
		BumpDepthParam = MakeScalarParam(Material, ParamBumpDepth(), In.BumpDepth, -1400, 760);
		BumpOffset = Cast<UMaterialExpressionBumpOffset>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				Material, UMaterialExpressionBumpOffset::StaticClass(), -620, 60));
		if (BumpOffset)
		{
			BumpOffset->ReferencePlane = 0.5f;
		}
	}
	else if (In.bUseHeightParallax && !In.Height)
	{
		OutEntry.StructuralWarnings.Add(TEXT("勾选了高度视差但没有可用的 height 贴图，已退化为普通 UV。"));
	}

	// 法线强度链
	UMaterialExpressionScalarParameter* NormalStrengthParam =
		MakeScalarParam(Material, ParamNormalStrength(), DefaultNormalStrength, -620, -520);
	UMaterialExpressionMultiply* NormalMul = Cast<UMaterialExpressionMultiply>(
		UMaterialEditingLibrary::CreateMaterialExpression(
			Material, UMaterialExpressionMultiply::StaticClass(), 120, -20));
	UMaterialExpressionNormalize* Normalize = Cast<UMaterialExpressionNormalize>(
		UMaterialEditingLibrary::CreateMaterialExpression(
			Material, UMaterialExpressionNormalize::StaticClass(), 360, -20));

	// ---- 3. 连线 ----
	// 3a. Tiling: TexCoord * Tiling
	ConnectNodes(TexCoord, 0, TilingMul, TEXT("A"), Problems, TEXT("UV × Tiling（A）"));
	ConnectNodes(TilingParam, 0, TilingMul, TEXT("B"), Problems, TEXT("UV × Tiling（B）"));

	if (BumpOffset)
	{
		// 3b. BumpOffset：Coordinate 接平铺后的 UV，Height 接高度采样，HeightRatio 接参数
		ConnectNodes(TilingMul, 0, BumpOffset, TEXT("Coordinate"), Problems, TEXT("BumpOffset.Coordinate"));
		ConnectNodes(HeightSample, 0, BumpOffset, TEXT("Height"), Problems, TEXT("BumpOffset.Height"));
		ConnectNodes(BumpDepthParam, 0, BumpOffset, TEXT("HeightRatioInput"), Problems, TEXT("BumpOffset.HeightRatio"));
	}
	else
	{
		// 没有视差时，height 采样若存在也不参与计算（避免悬空）
		if (HeightSample)
		{
			OutEntry.StructuralWarnings.Add(TEXT("height 贴图已采样但未接 BumpOffset，材质里不会生效。"));
		}
	}

	// 3c. ★ 视差分发：三张交付贴图的 Coordinates 都接 BumpOffset 的输出
	//     只接 BaseColor 是本设计里最典型的错误（PRD §6.2 的反面案例）
	UMaterialExpression* UVSource = BumpOffset ? static_cast<UMaterialExpression*>(BumpOffset)
	                                           : static_cast<UMaterialExpression*>(TilingMul);

	if (AlbedoSample) { ConnectNodes(UVSource, 0, AlbedoSample, TEXT("Coordinates"), Problems, TEXT("albedo.Coordinates")); }
	if (NormalSample) { ConnectNodes(UVSource, 0, NormalSample, TEXT("Coordinates"), Problems, TEXT("normal.Coordinates")); }
	if (ORMSample)    { ConnectNodes(UVSource, 0, ORMSample,    TEXT("Coordinates"), Problems, TEXT("orm.Coordinates")); }
	if (HeightSample)
	{
		// 高度自己的 UV 必须用**未偏移**的平铺 UV：
		// 接 BumpOffset 会形成 BumpOffset.Height ← heightSample ← BumpOffset 的环，编译直接报错。
		ConnectNodes(TilingMul, 0, HeightSample, TEXT("Coordinates"), Problems, TEXT("height.Coordinates（未偏移）"));
	}

	// 3d. BaseColor
	ConnectProperty(Material, AlbedoSample, /*RGB*/0, MP_BaseColor, Problems, TEXT("→ BaseColor"));

	// 3e. Normal：采样 → ×NormalStrength → Normalize → Normal
	if (NormalSample && NormalMul && Normalize)
	{
		ConnectNodes(NormalSample, 0, NormalMul, TEXT("A"), Problems, TEXT("法线 × 强度（A）"));
		ConnectNodes(NormalStrengthParam, 0, NormalMul, TEXT("B"), Problems, TEXT("法线 × 强度（B）"));
		ConnectNodes(NormalMul, 0, Normalize, TEXT("VectorInput"), Problems, TEXT("法线归一化"));
		ConnectProperty(Material, Normalize, /*RGB*/0, MP_Normal, Problems, TEXT("→ Normal"));
	}
	else
	{
		OutEntry.StructuralWarnings.Add(TEXT("normal 贴图缺失，材质未接法线。"));
	}

	// 3f. ORM 三通道：R→AO, G→Roughness, B→Metallic
	//     注意 PackPBRChannels 的输出顺序是 in0=AO / in1=Roughness（PRD 坑位 #1），
	//     所以这里 R=AO、G=Roughness 才是对的，不要按 "ORM" 的字面顺序猜。
	if (ORMSample)
	{
		ConnectProperty(Material, ORMSample, /*R*/1, MP_AmbientOcclusion, Problems, TEXT("→ AO（ORM.R）"));
		ConnectProperty(Material, ORMSample, /*G*/2, MP_Roughness,        Problems, TEXT("→ Roughness（ORM.G）"));
		ConnectProperty(Material, ORMSample, /*B*/3, MP_Metallic,         Problems, TEXT("→ Metallic（ORM.B）"));
	}
	else
	{
		OutEntry.StructuralWarnings.Add(TEXT("ORM 贴图缺失，材质的 AO/粗糙度/金属度未接。"));
	}

	// ---- 4. 结构自检（回读，不是"以为接上了"）----
	VerifyStructure(Material, BumpOffset, HeightSample, TilingMul,
		{ AlbedoSample, NormalSample, ORMSample }, OutEntry);

	// ---- 4b. SamplerType 自检 ----
	// 查**所有**已建出来的采样节点（不只是接了线的）：它们是我们的产物，
	// 一旦哪天被接进图里就会成为编译错误。
	{
		TArray<UMaterialExpressionTextureSampleParameter2D*> AllSamples;
		UMaterialExpressionTextureSampleParameter2D* const Candidates[] = {
			AlbedoSample, NormalSample, ORMSample, HeightSample };
		for (UMaterialExpressionTextureSampleParameter2D* S : Candidates)
		{
			if (S)
			{
				AllSamples.Add(S);
			}
		}
		VerifySamplerTypes(AllSamples, OutEntry);
	}

	// ---- 5. 编译 ----
	OutEntry.bCompileAttempted = true;
	const TArray<FString> CompileErrors = UMaterialEditingLibrary::RecompileMaterial(Material);
	OutEntry.CompileErrorCount = CompileErrors.Num();
	OutEntry.CompileMessages = CompileErrors;
	OutEntry.bCompiledClean = (CompileErrors.Num() == 0);

	// ⚠ 必须区分「编译通过」和「编译根本没跑」。
	//
	// 引擎实现是：
	//   if (const FMaterialResource* R = Material->GetMaterialResource(GMaxRHIShaderPlatform))
	//       return R->GetCompileErrors();
	//   return {};                      // ← 拿不到资源就返回空数组
	// （MaterialEditingLibrary.cpp:1054）
	//
	// 所以无头 commandlet 下 CompileErrors.Num()==0 **不代表材质能编译过**，
	// 只代表这次没编。M2 就是被这个假绿灯骗过去的：报告写着"编译 0 错误"，
	// 用户打开材质却看到两行 sampler type 报错。既然编译这条腿在无头下是瘸的，
	// 编译期错误就必须由前置自检（4b）来兜。
	OutEntry.bCompileResultValid = (Material->GetMaterialResource(GMaxRHIShaderPlatform) != nullptr);
	if (!OutEntry.bCompileResultValid)
	{
		OutEntry.StructuralWarnings.Add(TEXT(
			"材质编译未真正执行（拿不到 MaterialResource）—— 本次的“0 错误”不是编译结论，"
			"只表示没编。编译期错误由前置自检负责抓。"));
	}

	// 编译错误里如果出现循环依赖，多半是 height 的 UV 接错了 —— 提前说清楚
	for (const FString& Msg : CompileErrors)
	{
		if (Msg.Contains(TEXT("circular")) || Msg.Contains(TEXT("循环")))
		{
			OutEntry.StructuralWarnings.Add(
				TEXT("材质图出现循环依赖：height 采样的 UV 不能接 BumpOffset 的输出。"));
			break;
		}
	}

	UMaterialEditingLibrary::LayoutMaterialExpressions(Material);

	// ---- 6. 保存材质 ----
	bool bSaved = SaveAssetPackage(Material, Problems, TEXT("保存材质"));

	// ---- 7. 材质实例 ----
	if (In.bCreateInstance && OutEntry.bCompiledClean)
	{
		UMaterialInstanceConstantFactoryNew* MIFactory = NewObject<UMaterialInstanceConstantFactoryNew>();
		MIFactory->InitialParent = Material;

		UMaterialInstanceConstant* MI = Cast<UMaterialInstanceConstant>(AssetTools.CreateAsset(
			In.MaterialInstanceName, In.DestinationPath,
			UMaterialInstanceConstant::StaticClass(), MIFactory, NAME_None, false));

		if (MI)
		{
			// 标量参数：默认值与材质里一致，BumpDepth 用请求里的值
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, ParamTiling(), DefaultTiling);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, ParamNormalStrength(), DefaultNormalStrength);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, ParamBumpDepth(), In.BumpDepth);

			OutEntry.InstanceScalars.Add(ParamTiling(), DefaultTiling);
			OutEntry.InstanceScalars.Add(ParamNormalStrength(), DefaultNormalStrength);
			OutEntry.InstanceScalars.Add(ParamBumpDepth(), In.BumpDepth);

			if (SaveAssetPackage(MI, Problems, TEXT("保存材质实例")))
			{
				OutEntry.bInstanceCreated = true;
			}
		}
		else
		{
			Problems.Add(FString::Printf(TEXT("创建材质实例 %s 失败。"), *In.MaterialInstanceName));
		}
	}

	OutEntry.StructuralWarnings.Append(Problems);

	// 结构自检没过 ⇒ 即便编译通过也要报出来：这正是 PRD 点名的那种"肉眼不易察觉"的错误
	if (In.bUseHeightParallax && In.Height && !OutEntry.bParallaxAppliedToAllDeliverables)
	{
		OutError = FString::Printf(
			TEXT("材质已建，但视差没有作用到全部交付贴图（%d/%d）。"),
			OutEntry.SamplesDrivenByBumpOffset, OutEntry.SamplesExpected);
		return false;
	}

	// SamplerType 不符 = 材质在编辑器里必然编译失败，属于硬错误。
	// 这一条必须在这里拦，因为无头环境下的材质编译结果是不可信的（见上面 5 的注释）。
	if (OutEntry.SamplerTypeMismatched > 0)
	{
		OutError = FString::Printf(
			TEXT("SamplerType 自检：%d/%d 个采样节点与贴图压缩格式不符 —— 材质在编辑器里会编译失败。"),
			OutEntry.SamplerTypeMismatched, OutEntry.SamplerTypeChecked);
		return false;
	}

	if (!OutEntry.bCompiledClean)
	{
		OutError = FString::Printf(TEXT("材质编译报 %d 个错误。"), OutEntry.CompileErrorCount);
		return false;
	}

	if (!bSaved)
	{
		OutError = TEXT("材质编译通过但落盘失败。");
		return false;
	}

	return true;
}

// --------------------------------------------------------------------------- //
// 结构自检
// --------------------------------------------------------------------------- //

void FComfyMaterialBuilder::VerifyStructure(
	UMaterial* Material,
	UMaterialExpression* BumpOffset,
	UMaterialExpression* HeightSample,
	UMaterialExpression* Multiplier,
	const TArray<UMaterialExpression*>& DeliverableSamples,
	FComfyMaterialBuildEntry& OutEntry)
{
	if (BumpOffset)
	{
		if (FExpressionInput* HeightIn = FComfyMaterialBuilder::FindInputByName(BumpOffset, TEXT("Height")))
		{
			OutEntry.bBumpOffsetHeightLinked = (HeightIn->Expression == HeightSample) && HeightSample != nullptr;
		}
		if (FExpressionInput* CoordIn = FComfyMaterialBuilder::FindInputByName(BumpOffset, TEXT("Coordinate")))
		{
			OutEntry.bBumpOffsetCoordinateLinked = (CoordIn->Expression == Multiplier) && Multiplier != nullptr;
		}

		if (!OutEntry.bBumpOffsetHeightLinked)
		{
			OutEntry.StructuralWarnings.Add(TEXT("BumpOffset.Height 没接到 height 贴图，视差不会有高度变化。"));
		}
		if (!OutEntry.bBumpOffsetCoordinateLinked)
		{
			OutEntry.StructuralWarnings.Add(TEXT("BumpOffset.Coordinate 没接到平铺后的 UV。"));
		}
	}

	// 数一下有几个采样把 UV 接在 BumpOffset 的输出上
	int32 Driven = 0;
	for (UMaterialExpression* Sample : DeliverableSamples)
	{
		if (!Sample)
		{
			continue;
		}
		if (FExpressionInput* CoordIn = FComfyMaterialBuilder::FindInputByName(Sample, TEXT("Coordinates")))
		{
			if (CoordIn->Expression == BumpOffset && BumpOffset != nullptr)
			{
				++Driven;
			}
		}
	}
	OutEntry.SamplesDrivenByBumpOffset = Driven;
	OutEntry.bParallaxAppliedToAllDeliverables = (Driven == OutEntry.SamplesExpected);

	if (BumpOffset && Driven != OutEntry.SamplesExpected)
	{
		OutEntry.StructuralWarnings.Add(FString::Printf(
			TEXT("视差只驱动了 %d 张交付贴图（应 %d 张）。只接 BaseColor 会让法线与视差错位。"),
			Driven, OutEntry.SamplesExpected));
	}
}

// --------------------------------------------------------------------------- //
// SamplerType 自检
// --------------------------------------------------------------------------- //

void FComfyMaterialBuilder::VerifySamplerTypes(
	const TArray<UMaterialExpressionTextureSampleParameter2D*>& Samples,
	FComfyMaterialBuildEntry& OutEntry)
{
	for (UMaterialExpressionTextureSampleParameter2D* Sample : Samples)
	{
		if (!Sample)
		{
			continue;
		}

		++OutEntry.SamplerTypeChecked;

		FString EngineError;
		if (ComfySamplerType::Verify(Sample->Texture, Sample->SamplerType, EngineError))
		{
			continue;
		}

		++OutEntry.SamplerTypeMismatched;

		// 参数名单独取出来拼：UE 5.8 对 FString::Printf 有三元分支类型必须一致的检查，
		// `cond ? TEXT("x") : *SomeFString` 这种混用会直接 C7595。
		FString ParamName = Sample->ParameterName.ToString();
		if (ParamName.IsEmpty())
		{
			ParamName = TEXT("(未命名采样)");
		}

		// 错误文本用引擎原文（与编辑器里那条红字同源），前面补参数名便于定位。
		// 例如：Sampler type is Grayscale, should be Linear Grayscale for /Game/.../T_xxx_H
		OutEntry.SamplerTypeMismatches.Add(FString::Printf(TEXT("[%s] %s"), *ParamName, *EngineError));
	}
}
