#include "ComfyTextureStudioCommandlet.h"

#include "ComfyImportTypes.h"
#include "ComfySamplerType.h"
#include "ComfyTextureJob.h"
#include "ComfyTextureStudio.h"
#include "ComfyTextureStudioSettings.h"
#include "Engine/Texture2D.h"
#include "HttpManager.h"      // FHttpManager 的完整定义（HttpModule.h 只有前置声明）
#include "HttpModule.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

UComfyTextureStudioCommandlet::UComfyTextureStudioCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;   // 插件在工作时要拿编辑器上下文
	LogToConsole = true;
	ShowErrorCount = true;
}

void UComfyTextureStudioCommandlet::PrintUsage()
{
	UE_LOG(LogComfyTextureStudio, Log, TEXT("ComfyTextureStudio · 无 UI 跑完整两阶段"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -run=ComfyTextureStudio -unattended -nopause -nosplash"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -JobId= -Prompt= -Negative= -Seed= -Steps= -Width= -Height= -BatchSize="));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -Lora= -Set=key=value,... -ServerUrl= -OutDir="));
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("M2 导入链路（跑完两阶段后直接导入 UE 并建材质，无 UI）："));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -Import                  开启导入"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -BaseName=Sand_Coarse_01 资产基名（默认 cts_m2_<jobId后6位>）"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -DestPath=/Game/...      资产落点（默认 <AssetRootPath>/<BaseName>）"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -NoFlipGreen             不翻法线绿通道（默认翻，D3）"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -NoBumpOffset            高度图不接 BumpOffset 视差"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -ImportOrphans           额外导入 AO/Roughness（默认不导，Q3）"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -NoMaterialInstance      不创建材质实例"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -BumpDepth=0.02          BumpOffset 的高度比例"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("M2 冷加载核对（不连 ComfyUI，验证已落盘资产的属性与材质图结构）："));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -VerifyOnly=/Game/ComfyTextures/<基名>"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("回归测试："));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -TestLiveness            复现「请求飞行中销毁客户端」的时序，验证存活令牌"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -TestSamplerTypeCheck    负向验证 SamplerType 校验器（不需要 ComfyUI）"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  -CtsTestPanel            编辑器启动时组装一次面板并销毁（需 Slate）"));
}

/**
 * HTTP 客户端存活令牌的回归测试（`-TestLiveness`）。
 *
 * 背景：编辑器里曾因为「面板发起 /object_info 后立刻被销毁」而崩溃 ——
 * 飞行中的完成回调访问了已析构的面板，报
 * `EXCEPTION_ACCESS_VIOLATION reading address 0xffffffffffffffff`，
 * 而且堆栈落在引擎的 HTTP 代码里，完全指不到业务代码。
 *
 * 这个测试复现那个时序：发一个请求 → **立刻销毁客户端** → 泵一段时间让回调有机会触发。
 * 回调用一个栈上的标志位当探针（作用域仍在，所以即使被误调用也不会崩，
 * 而是把标志改成 true 让我们看得出来）。
 *
 * 通过标准：标志位保持 false —— 也就是回调被令牌挡住了。
 */
static int32 RunLivenessTest(const FString& ServerUrl)
{
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("════ 存活令牌回归测试 ════"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  服务  %s"), *ServerUrl);

	bool bCallbackReached = false;

	{
		TUniquePtr<FComfyHttpClient> Client = MakeUnique<FComfyHttpClient>(ServerUrl, 10.0f);

		Client->GetObjectInfo([&bCallbackReached](
			bool, const TSharedPtr<FJsonObject>&, const FString&)
		{
			// 这个标志就在栈上，作用域还没结束 —— 所以即使被误调用也不会立刻崩，
			// 但能让我们看出"回调穿过了令牌"。
			bCallbackReached = true;
		});

		UE_LOG(LogComfyTextureStudio, Log, TEXT("  已发起 /object_info 并**立刻销毁客户端**"));
		// 出作用域：客户端析构 → 令牌作废 + 取消飞行中的请求
	}

	// 泵一段时间，给被取消/已完成的请求充分的回调机会。
	// commandlet 里没有引擎 tick，必须手工驱动。
	for (int32 i = 0; i < 100; ++i)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.016f);
		FPlatformProcess::Sleep(0.02f);
	}

	if (bCallbackReached)
	{
		UE_LOG(LogComfyTextureStudio, Error,
			TEXT("  ✗ 回调穿过了存活令牌 —— 客户端销毁后仍有回调被执行，这就是野指针的来源"));
		return 1;
	}

	UE_LOG(LogComfyTextureStudio, Log, TEXT("  ✓ 回调被令牌挡下，未触碰已析构对象"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  测试通过"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	return 0;
}

/**
 * SamplerType 校验器的负向回归测试（`-TestSamplerTypeCheck`）。
 *
 * 起因：M2 交付的材质在编辑器里一打开就是两行红字 ——
 *   Sampler type is Grayscale, should be Linear Grayscale for .../T_..._H
 *   Sampler type is Linear Color, should be Masks for .../T_..._ORM
 * 而同一个材质的 commandlet 报告写的是「编译 0 错误」。
 *
 * 这个测试只证明一件事：**校验器在错误输入下真的会失败**。
 * 一个永远返回 true 的校验器同样能让所有报告变绿 —— 那才是最难发现的故障，
 * 所以正反两个方向都要断言：错误组合必须被拒，正确组合必须通过。
 *
 * 不需要 ComfyUI，也不需要任何已落盘的资产（全部就地构造成 transient 对象）。
 */
static int32 RunSamplerTypeCheck()
{
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("════ SamplerType 校验器回归测试 ════"));

	struct FCase
	{
		const TCHAR* Label;
		TextureCompressionSettings Compression;
		bool bSRGB;
		EMaterialSamplerType Actual;
		bool bExpectPass;
	};

	const FCase Cases[] =
	{
		// ---- 用户实际报的两个错误组合：必须被拒 ----
		{ TEXT("height  TC_Grayscale + Grayscale（M2 踩的坑）"),       TC_Grayscale, false, SAMPLERTYPE_Grayscale,       false },
		{ TEXT("orm     TC_Masks     + LinearColor（M2 踩的坑）"),     TC_Masks,     false, SAMPLERTYPE_LinearColor,     false },

		// ---- 修正后的组合：必须通过 ----
		{ TEXT("height  TC_Grayscale + LinearGrayscale（修正后）"),     TC_Grayscale, false, SAMPLERTYPE_LinearGrayscale, true  },
		{ TEXT("orm     TC_Masks     + Masks（修正后）"),               TC_Masks,     false, SAMPLERTYPE_Masks,           true  },

		// ---- 另外两张交付贴图，确认没被误伤 ----
		{ TEXT("albedo  TC_Default   + Color"),                        TC_Default,   true,  SAMPLERTYPE_Color,           true  },
		{ TEXT("normal  TC_Normalmap + Normal"),                       TC_Normalmap, false, SAMPLERTYPE_Normal,          true  },

		// ---- 边界：TC_Grayscale 且 sRGB 打开时，Grayscale 才是对的（引擎规则） ----
		{ TEXT("边界    TC_Grayscale + sRGB + Grayscale"),              TC_Grayscale, true,  SAMPLERTYPE_Grayscale,       true  },
		// ---- 边界：TC_Masks 配 Normal 也必须被拒（不是"任何非 Color 都放过"） ----
		{ TEXT("边界    TC_Masks     + Normal"),                        TC_Masks,     false, SAMPLERTYPE_Normal,          false },
	};

	int32 Failures = 0;
	for (const FCase& C : Cases)
	{
		// 构造一张只带压缩设置的贴图即可 —— 采样类型的推导只看
		// CompressionSettings / SRGB / LODGroup / 虚拟纹理开关，不需要 RHI 资源。
		UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Texture->CompressionSettings = C.Compression;
		Texture->SRGB = C.bSRGB ? 1 : 0;
		Texture->LODGroup = TEXTUREGROUP_World;

		FString EngineError;
		const bool bPass = ComfySamplerType::Verify(Texture, C.Actual, EngineError);
		const bool bCaseOk = (bPass == C.bExpectPass);
		if (!bCaseOk)
		{
			++Failures;
		}

		// 三元分支必须是同类型：UE 5.8 的格式化检查不会替我们把 FString 与 const TCHAR* 统一。
		const FString Extra = EngineError.IsEmpty()
			? FString()
			: FString::Printf(TEXT("   ← %s"), *EngineError);

		UE_LOG(LogComfyTextureStudio, Log, TEXT("  %s %-48s 期望 %s，实际 %s%s"),
			bCaseOk ? TEXT("✓") : TEXT("✗"),
			C.Label,
			C.bExpectPass ? TEXT("通过") : TEXT("拒绝"),
			bPass ? TEXT("通过") : TEXT("拒绝"),
			*Extra);
	}

	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	if (Failures > 0)
	{
		UE_LOG(LogComfyTextureStudio, Error,
			TEXT("  ✗ %d 个用例不符 —— 校验器要么漏判、要么误判"), Failures);
		return 1;
	}

	UE_LOG(LogComfyTextureStudio, Log, TEXT("  校验器能区分正确与错误的采样类型（含 M2 踩的那两个组合）"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  测试通过"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	return 0;
}

/**
 * M2 冷加载核对：**不碰 ComfyUI**，只从磁盘重新加载已导入的资产，逐项核 §6.3
 * 与材质图结构。
 *
 * 单独跑一个进程的意义：导入时的自检读的是同一进程里刚写过的内存对象，
 * 属于"自己检查自己"。这里没有任何内存缓存 —— 一切从磁盘读回。只有这一层通过，
 * 才能说"属性真的被保存下来了"（§6.3 是 G3 的验收依据）。
 */
static int32 RunVerifyOnly(const FString& DestinationPath)
{
	FModuleManager::Get().LoadModule(TEXT("ComfyTextureStudioEditor"));

	IComfyAssetImportProvider* Provider = FComfyImportRegistry::Get().GetProvider();
	if (Provider == nullptr)
	{
		UE_LOG(LogComfyTextureStudio, Error, TEXT("导入服务未注册（Editor 模块没加载）。"));
		return 1;
	}

	// 基名就是包路径的最后一段
	const FString BaseName = FPaths::GetCleanFilename(DestinationPath);

	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("════ M2 · 冷加载核对 ════"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  落点  %s"), *DestinationPath);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  基名  %s"), *BaseName);

	FComfyAssetVerification Verification;
	const bool bOk = Provider->VerifyImportedAssets(DestinationPath, BaseName, Verification);

	TArray<FString> Lines;
	Verification.ToText().ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		UE_LOG(LogComfyTextureStudio, Log, TEXT("  %s"), *Line);
	}

	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  核对结果  %d 项，%d 项不符 → %s"),
		Verification.Checked, Verification.Failed, bOk ? TEXT("通过") : TEXT("不通过"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));

	return bOk ? 0 : 1;
}

/**
 * M2：跑完两阶段后，直接走「导入贴图 + 建材质」全链路。
 *
 * 为什么放在 commandlet 里：UI 无法在无头环境点击，但导入与建材质的**逻辑**
 * 必须真的跑过一次才算验收 —— PRD §6.3 的属性表要逐项回读核对，材质图的
 * BumpOffset 接线要回读确认。这些都只能程序化验证。
 *
 * Editor 模块（提供 IComfyAssetImportProvider）在这里显式加载：它是
 * LoadingPhase=PostEngineInit 的 Editor 模块，正常会被加载，但命令行走的是
 * 特殊路径，显式一次更稳。
 */
static int32 RunImportStage(const FString& CmdLine, const FComfyJobResult& Result)
{
	if (!Result.bSuccess)
	{
		UE_LOG(LogComfyTextureStudio, Error, TEXT("两阶段未成功，跳过导入。"));
		return 1;
	}

	FModuleManager::Get().LoadModule(TEXT("ComfyTextureStudioEditor"));

	IComfyAssetImportProvider* Provider = FComfyImportRegistry::Get().GetProvider();
	if (Provider == nullptr)
	{
		UE_LOG(LogComfyTextureStudio, Error,
			TEXT("导入服务未注册 —— ComfyTextureStudioEditor 模块没加载成功。"));
		return 1;
	}

	const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>();

	FComfyImportRequest Request;

	FParse::Value(*CmdLine, TEXT("BaseName="), Request.BaseName);
	if (Request.BaseName.IsEmpty())
	{
		Request.BaseName = FString::Printf(TEXT("cts_m2_%s"),
			*Result.JobId.Right(6).ToLower());
	}

	FString DestOverride;
	FParse::Value(*CmdLine, TEXT("DestPath="), DestOverride);
	Request.DestinationPath = DestOverride.IsEmpty()
		? FString::Printf(TEXT("%s/%s"), *Settings->AssetRootPath, *Request.BaseName)
		: DestOverride;

	// 命令行模式下材质名走默认前缀（面板那侧的模板/手填解析在 Editor 模块里）
	Request.MaterialName = FString::Printf(TEXT("M_%s"), *Request.BaseName);
	Request.MaterialInstanceName = FString::Printf(TEXT("MI_%s"), *Request.BaseName);

	Request.bFlipGreenChannel = !FParse::Param(*CmdLine, TEXT("NoFlipGreen"));
	Request.bImportHeight = !FParse::Param(*CmdLine, TEXT("NoBumpOffset"));
	Request.bImportOrphanMaps = FParse::Param(*CmdLine, TEXT("ImportOrphans"));
	Request.bCreateMaterialInstance = !FParse::Param(*CmdLine, TEXT("NoMaterialInstance"));
	Request.bDeleteSourcePng = !Settings->PreserveDownloadedPng;

	float BumpDepth = Settings->DefaultBumpDepth;
	if (FParse::Value(*CmdLine, TEXT("BumpDepth="), BumpDepth))
	{
		// 覆盖成命令行的值
	}
	Request.BumpDepth = BumpDepth;

	Request.ChannelPngPaths = Result.ChannelPaths;

	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("════ M2 · 导入与建材质 ════"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  基名        %s"), *Request.BaseName);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  落点        %s"), *Request.DestinationPath);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  翻绿通道    %s   视差 %s   orphan 导入 %s"),
		Request.bFlipGreenChannel ? TEXT("是") : TEXT("否"),
		Request.bImportHeight ? TEXT("是") : TEXT("否"),
		Request.bImportOrphanMaps ? TEXT("是") : TEXT("否"));

	const FComfyImportResult ImportResult = Provider->ImportAndBuildMaterial(Request);

	// 报告原文逐行打到日志：这是 G3 验收时要肉眼核对的东西
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	{
		TArray<FString> Lines;
		ImportResult.Report.ToText().ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogComfyTextureStudio, Log, TEXT("  %s"), *Line);
		}
	}
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));

	if (!ImportResult.bSuccess && !ImportResult.ErrorText.IsEmpty())
	{
		UE_LOG(LogComfyTextureStudio, Error, TEXT("导入失败：%s"), *ImportResult.ErrorText);
	}

	// 落一份机器可读的报告，供后续脚本比对
	const FString ReportPath = FPaths::Combine(Result.OutputDir, TEXT("report_import.json"));
	FFileHelper::SaveStringToFile(ImportResult.Report.ToJson(), *ReportPath);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  导入报告    %s"), *ReportPath);

	// 结构化自检结论（供 CI / 人工一眼判断）
	const FComfyImportReport& R = ImportResult.Report;
	int32 ImportedCount = 0;
	int32 PropOkCount = 0;
	for (const FComfyTextureImportEntry& E : R.Textures)
	{
		if (E.bImported)
		{
			++ImportedCount;
			if (E.bPropertyCheckPassed)
			{
				++PropOkCount;
			}
		}
	}

	UE_LOG(LogComfyTextureStudio, Log, TEXT("  自检汇总："));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("    导入贴图      %d 张，属性全项通过 %d 张"), ImportedCount, PropOkCount);

	// 编译那条腿在无头环境下是瘸的 —— 分开打印，别让人把"没编"读成"编过了"
	if (R.Material.bCompileResultValid)
	{
		UE_LOG(LogComfyTextureStudio, Log, TEXT("    材质编译      %s（%d 错误）"),
			R.Material.bCompiledClean ? TEXT("干净") : TEXT("有错"),
			R.Material.CompileErrorCount);
	}
	else
	{
		UE_LOG(LogComfyTextureStudio, Log,
			TEXT("    材质编译      未执行 —— 拿不到 MaterialResource，本次“0 错误”不代表编译结论"));
	}

	UE_LOG(LogComfyTextureStudio, Log, TEXT("    SamplerType   %d / %d 个采样节点与贴图压缩格式对齐%s"),
		R.Material.SamplerTypeChecked - R.Material.SamplerTypeMismatched,
		R.Material.SamplerTypeChecked,
		R.Material.SamplerTypeMismatched > 0 ? TEXT("  ← 材质在编辑器里会编译失败") : TEXT(""));

	UE_LOG(LogComfyTextureStudio, Log, TEXT("    视差驱动      %d / %d 张交付贴图"),
		R.Material.SamplesDrivenByBumpOffset, R.Material.SamplesExpected);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("    BumpOffset    Height 接线 %s · Coordinate 接线 %s"),
		R.Material.bBumpOffsetHeightLinked ? TEXT("是") : TEXT("否"),
		R.Material.bBumpOffsetCoordinateLinked ? TEXT("是") : TEXT("否"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("    材质实例      %s"),
		R.Material.bInstanceCreated ? *R.Material.MaterialInstanceName : TEXT("未创建"));

	return ImportResult.bSuccess ? 0 : 1;
}

int32 UComfyTextureStudioCommandlet::Main(const FString& Params)
{
	// Params 是 -run= 之后的残段；这里要拿完整命令行（-run= 之前的开关也在里面）。
	const FString CmdLine = FCommandLine::Get();
	(void)Params;

	if (FParse::Param(*CmdLine, TEXT("Help")))
	{
		PrintUsage();
		return 0;
	}

	// -VerifyOnly=<DestPath>：只做冷加载核对，不连 ComfyUI（M2 验收用）
	{
		FString VerifyOnlyPath;
		if (FParse::Value(*CmdLine, TEXT("VerifyOnly="), VerifyOnlyPath) && !VerifyOnlyPath.IsEmpty())
		{
			return RunVerifyOnly(VerifyOnlyPath);
		}
	}

	// -TestLiveness：HTTP 存活令牌回归测试（复现"请求飞行中销毁客户端"的时序）
	if (FParse::Param(*CmdLine, TEXT("TestLiveness")))
	{
		FString Url;
		if (!FParse::Value(*CmdLine, TEXT("ServerUrl="), Url) || Url.IsEmpty())
		{
			Url = GetDefault<UComfyTextureStudioSettings>()->ServerUrl;
		}
		return RunLivenessTest(Url);
	}

	// -TestSamplerTypeCheck：SamplerType 校验器的负向回归测试（不连 ComfyUI、不需要资产）
	if (FParse::Param(*CmdLine, TEXT("TestSamplerTypeCheck")))
	{
		return RunSamplerTypeCheck();
	}

	UE_LOG(LogComfyTextureStudio, Log, TEXT("════ ComfyTextureStudio · M1 无 UI 全链路 ════"));

	FComfyTextureJobParams JobParams;
	FString ServerUrlOverride;
	FString SetList;

	FParse::Value(*CmdLine, TEXT("JobId="), JobParams.JobId);
	FParse::Value(*CmdLine, TEXT("Prompt="), JobParams.PositivePrompt);
	FParse::Value(*CmdLine, TEXT("Negative="), JobParams.NegativePrompt);
	FParse::Value(*CmdLine, TEXT("Lora="), JobParams.LoraName);
	FParse::Value(*CmdLine, TEXT("Steps="), JobParams.Steps);
	FParse::Value(*CmdLine, TEXT("Width="), JobParams.Width);
	FParse::Value(*CmdLine, TEXT("Height="), JobParams.Height);
	FParse::Value(*CmdLine, TEXT("BatchSize="), JobParams.BatchSize);
	FParse::Value(*CmdLine, TEXT("OutDir="), JobParams.OutputRootAbs);
	FParse::Value(*CmdLine, TEXT("ServerUrl="), ServerUrlOverride);
	FParse::Value(*CmdLine, TEXT("Set="), SetList);

	// -Seed=<int> 或 -Seed=random
	//
	// M0 结论 1：ComfyUI 有**节点级结果缓存**，图和上游参数都不变时重提交直接命中 ——
	// 实测阶段A 从 ~11 s 掉到 1.5 s，用户会以为「重跑了」其实没有。
	// 换种子是最省事的失效手段，所以给一个显式的 random。
	FString SeedText;
	if (FParse::Value(*CmdLine, TEXT("Seed="), SeedText) && !SeedText.IsEmpty())
	{
		if (SeedText.Equals(TEXT("random"), ESearchCase::IgnoreCase))
		{
			JobParams.Seed = FMath::RandRange(0, MAX_int32 - 1);
			UE_LOG(LogComfyTextureStudio, Log, TEXT("使用随机种子 %d（用于避免命中缓存）"), JobParams.Seed);
		}
		else
		{
			JobParams.Seed = FCString::Atoi(*SeedText);
		}
	}

	// -Set=dsineFov=2.5,aoRadius=32
	if (!SetList.IsEmpty())
	{
		TArray<FString> Entries;
		SetList.ParseIntoArray(Entries, TEXT(","), /*InCullEmpty=*/true);
		for (const FString& Entry : Entries)
		{
			FString Key;
			FString Value;
			if (Entry.Split(TEXT("="), &Key, &Value))
			{
				JobParams.SlotOverrides.Add(Key.TrimStartAndEnd(), Value.TrimStartAndEnd());
			}
			else
			{
				UE_LOG(LogComfyTextureStudio, Warning, TEXT("忽略无法解析的 -Set 项：%s"), *Entry);
			}
		}
	}

	if (!ServerUrlOverride.IsEmpty())
	{
		// 命令行走一次性进程，改 CDO 是安全的。
		GetMutableDefault<UComfyTextureStudioSettings>()->ServerUrl = ServerUrlOverride;
		UE_LOG(LogComfyTextureStudio, Log, TEXT("服务地址覆盖为 %s"), *ServerUrlOverride);
	}

	const UComfyTextureStudioSettings* Settings = GetDefault<UComfyTextureStudioSettings>();
	UE_LOG(LogComfyTextureStudio, Log, TEXT("服务地址 %s · 产物根 %s"),
		*Settings->ServerUrl, *Settings->GetCacheDirAbs());

	// ---- 跑 ----
	FComfyTextureJob Job;
	Job.Start(JobParams);

	while (!Job.IsFinished())
	{
		Job.Tick(0.05f);
	}

	const FComfyJobResult& Result = Job.GetResult();

	// ---- 报告 ----
	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("jobId"), Result.JobId);
	Report->SetBoolField(TEXT("success"), Result.bSuccess);
	Report->SetStringField(TEXT("outputDir"), Result.OutputDir);
	Report->SetStringField(TEXT("stageAPromptId"), Result.StageAPromptId);
	Report->SetStringField(TEXT("stageBPromptId"), Result.StageBPromptId);
	Report->SetStringField(TEXT("baseTexture"), Result.BaseTexturePath);
	Report->SetStringField(TEXT("uploadedInput"), Result.UploadedInputName);
	Report->SetNumberField(TEXT("stageASeconds"), Result.StageASeconds);
	Report->SetNumberField(TEXT("stageBSeconds"), Result.StageBSeconds);

	TSharedPtr<FJsonObject> Channels = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Result.ChannelPaths)
	{
		Channels->SetStringField(Pair.Key, Pair.Value);
	}
	Report->SetObjectField(TEXT("channels"), Channels);

	if (Result.Error.IsSet())
	{
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("kind"), LexToString(Result.Error.Kind));
		Error->SetStringField(TEXT("nodeId"), Result.Error.NodeId);
		Error->SetStringField(TEXT("message"), Result.Error.Message);
		Error->SetStringField(TEXT("userText"), Result.Error.UserText());
		Error->SetStringField(TEXT("suggestedAction"), Result.Error.SuggestedAction());
		Error->SetStringField(TEXT("detail"), Result.Error.Detail);
		Report->SetObjectField(TEXT("error"), Error);
	}

	if (!FFileHelper::SaveStringToFile(
		[&Report]()
		{
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Report.ToSharedRef(), Writer);
			return Text;
		}(),
		*FPaths::Combine(Result.OutputDir, TEXT("report.json"))))
	{
		UE_LOG(LogComfyTextureStudio, Warning, TEXT("报告写盘失败：%s"), *Result.OutputDir);
	}

	// ---- 汇总 ----
	UE_LOG(LogComfyTextureStudio, Log, TEXT(""));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("════ 汇总 ════"));
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  任务 id     %s"), *Result.JobId);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  产物目录    %s"), *Result.OutputDir);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  阶段A 产物  %s"), *Result.BaseTexturePath);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  耗时        阶段A %.2f s · 阶段B %.2f s"),
		Result.StageASeconds, Result.StageBSeconds);
	UE_LOG(LogComfyTextureStudio, Log, TEXT("  通道："));

	int32 Found = 0;
	int32 DeliverableFound = 0;
	for (const FString& Semantic : ComfyChannels::All())
	{
		const FString* Path = Result.ChannelPaths.Find(Semantic);
		const bool bDeliverable = ComfyChannels::IsDeliverable(Semantic);
		if (Path != nullptr)
		{
			++Found;
			if (bDeliverable)
			{
				++DeliverableFound;
			}
		}
		UE_LOG(LogComfyTextureStudio, Log, TEXT("    %-9s %-24s %s"),
			*Semantic,
			Path != nullptr ? *FPaths::GetCleanFilename(*Path) : TEXT("（缺）"),
			bDeliverable ? TEXT("[交付]") : TEXT("[可选，Q3 不导入]"));
	}

	if (Result.bSuccess)
	{
		UE_LOG(LogComfyTextureStudio, Log,
			TEXT("结果        成功 · %d/%d 通道就绪（其中交付通道 %d/%d）"),
			Found, ComfyChannels::All().Num(), DeliverableFound, ComfyChannels::Deliverable().Num());

		// M2：-Import 时接着把贴图导进 UE 并建材质（这段需要 Editor 模块）
		if (FParse::Param(*CmdLine, TEXT("Import")))
		{
			return RunImportStage(CmdLine, Result);
		}
		return 0;
	}

	UE_LOG(LogComfyTextureStudio, Error, TEXT("结果        失败 · %s"), *Result.Error.ToString());
	UE_LOG(LogComfyTextureStudio, Error, TEXT("  说明      %s"), *Result.Error.UserText());
	UE_LOG(LogComfyTextureStudio, Error, TEXT("  建议      %s"), *Result.Error.SuggestedAction());
	return 1;
}
