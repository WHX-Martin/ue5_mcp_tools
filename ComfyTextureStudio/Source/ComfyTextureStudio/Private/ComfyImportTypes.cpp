// ComfyTextureStudio · 导入报告序列化 + 提供者注册表

#include "ComfyImportTypes.h"

#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// --------------------------------------------------------------------------- //
// FComfyTextureImportEntry
// --------------------------------------------------------------------------- //

FString FComfyTextureImportEntry::ToText() const
{
	const FString Channel = ComfyChannels::DisplayName(Semantic);

	if (!bImported)
	{
		return FString::Printf(TEXT("— %-22s %s"), *Channel, *SkipReason);
	}

	FString Line = FString::Printf(TEXT("%s %-24s %-8s sRGB=%-3s %-5s %-6s"),
		bPropertyCheckPassed ? TEXT("✓") : TEXT("✗"),
		*AssetName,
		*CompressionSettings,
		bSRGB ? TEXT("on") : TEXT("off"),
		bFlipGreenChannel ? TEXT("FlipG") : TEXT("     "),
		*AddressX);

	if (!Role.IsEmpty())
	{
		Line += FString::Printf(TEXT(" → %s"), *Role);
	}
	return Line;
}

// --------------------------------------------------------------------------- //
// FComfyImportReport
// --------------------------------------------------------------------------- //

bool FComfyImportReport::HasProblems() const
{
	if (Material.bCreated && (!Material.bCompiledClean || Material.CompileErrorCount > 0))
	{
		return true;
	}
	if (Material.bCreated && Material.SamplerTypeMismatched > 0)
	{
		return true;
	}
	if (Material.bCreated && !Material.bParallaxAppliedToAllDeliverables)
	{
		return true;
	}
	for (const FComfyTextureImportEntry& E : Textures)
	{
		if (E.bImported && !E.bPropertyCheckPassed)
		{
			return true;
		}
	}
	return false;
}

FString FComfyImportReport::ToText() const
{
	TArray<FString> Lines;

	Lines.Add(FString::Printf(TEXT("导入报告 · %s · %s"), *BaseName, *GeneratedAt));
	Lines.Add(TEXT("──────────────────────────────────────────────"));

	for (const FComfyTextureImportEntry& E : Textures)
	{
		Lines.Add(E.ToText());
	}

	Lines.Add(TEXT("──────────────────────────────────────────────"));

	if (Material.bCreated)
	{
		// 编译结论只在**真的编过**时才写成"已编译"。
		// 无头 commandlet 拿不到 MaterialResource，RecompileMaterial 会静默返回空数组，
		// 照原样印"0 错误"就是把假绿灯写进了验收报告（M2 就是这么漏的）。
		const FString CompileVerdict = Material.bCompileResultValid
			? FString::Printf(TEXT("已编译，%d 错误 %d 警告"),
				Material.CompileErrorCount, Material.CompileWarningCount)
			: FString(TEXT("未执行编译（拿不到 MaterialResource，本行不代表编译结论）"));

		const TCHAR* CompileMark = !Material.bCompileResultValid
			? TEXT("—")
			: (Material.bCompiledClean ? TEXT("✓") : TEXT("✗"));

		Lines.Add(FString::Printf(TEXT("%s %-24s %s"),
			CompileMark, *Material.MaterialName, *CompileVerdict));

		if (Material.SamplerTypeChecked > 0)
		{
			Lines.Add(FString::Printf(TEXT("%s SamplerType 自检通过 %d / %d 个采样节点"),
				Material.SamplerTypeMismatched == 0 ? TEXT("✓") : TEXT("✗"),
				Material.SamplerTypeChecked - Material.SamplerTypeMismatched,
				Material.SamplerTypeChecked));
		}

		Lines.Add(FString::Printf(TEXT("%s 视差已接入 %d / %d 张交付贴图"),
			Material.bParallaxAppliedToAllDeliverables ? TEXT("✓") : TEXT("✗"),
			Material.SamplesDrivenByBumpOffset,
			Material.SamplesExpected));

		if (Material.bInstanceCreated)
		{
			Lines.Add(FString::Printf(TEXT("✓ %-24s 已创建"), *Material.MaterialInstanceName));
		}
		for (const TPair<FString, float>& P : Material.InstanceScalars)
		{
			Lines.Add(FString::Printf(TEXT("    · %s = %.4f"), *P.Key, P.Value));
		}
		for (const FString& W : Material.StructuralWarnings)
		{
			Lines.Add(FString::Printf(TEXT("⚠ %s"), *W));
		}
		for (const FString& M : Material.CompileMessages)
		{
			Lines.Add(FString::Printf(TEXT("  %s"), *M));
		}
	}

	for (const FString& S : SkippedOrphans)
	{
		// orphan 的跳过原因已经在上面每张图的条目里写过一次了，
		// 这里不再重复整段（报告是要给人看的，不是要凑行数）
		(void)S;
	}

	// 逐项自检不符的细节单独列出来，便于定位
	for (const FComfyTextureImportEntry& E : Textures)
	{
		for (const FString& M : E.PropertyMismatches)
		{
			Lines.Add(FString::Printf(TEXT("⚠ %s：%s"), *E.Semantic, *M));
		}
	}

	// SamplerType 不符：这是**引擎会在编辑器里报红字**的那种错误，逐条原样给出
	for (const FString& M : Material.SamplerTypeMismatches)
	{
		Lines.Add(FString::Printf(TEXT("⚠ SamplerType %s"), *M));
	}

	return FString::Join(Lines, TEXT("\n"));
}

namespace
{
	TSharedPtr<FJsonObject> TextureEntryToJson(const FComfyTextureImportEntry& E)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("semantic"), E.Semantic);
		O->SetBoolField(TEXT("imported"), E.bImported);
		if (E.bImported)
		{
			O->SetStringField(TEXT("assetName"), E.AssetName);
			O->SetStringField(TEXT("packagePath"), E.PackagePath);
			O->SetStringField(TEXT("compressionSettings"), E.CompressionSettings);
			O->SetBoolField(TEXT("srgb"), E.bSRGB);
			O->SetBoolField(TEXT("flipGreenChannel"), E.bFlipGreenChannel);
			O->SetStringField(TEXT("lodGroup"), E.LODGroup);
			O->SetStringField(TEXT("addressX"), E.AddressX);
			O->SetStringField(TEXT("addressY"), E.AddressY);
			O->SetStringField(TEXT("mipGenSettings"), E.MipGenSettings);
			O->SetBoolField(TEXT("propertyCheckPassed"), E.bPropertyCheckPassed);
			O->SetStringField(TEXT("role"), E.Role);
			if (E.PropertyMismatches.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (const FString& M : E.PropertyMismatches)
				{
					Arr.Add(MakeShared<FJsonValueString>(M));
				}
				O->SetArrayField(TEXT("propertyMismatches"), Arr);
			}
		}
		else
		{
			O->SetStringField(TEXT("skipReason"), E.SkipReason);
		}
		O->SetStringField(TEXT("sourcePng"), E.SourcePng);
		return O;
	}

	TSharedPtr<FJsonObject> MaterialToJson(const FComfyMaterialBuildEntry& M)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetBoolField(TEXT("created"), M.bCreated);
		O->SetStringField(TEXT("materialName"), M.MaterialName);
		O->SetStringField(TEXT("materialPackagePath"), M.MaterialPackagePath);
		O->SetBoolField(TEXT("compiledClean"), M.bCompiledClean);
		O->SetBoolField(TEXT("compileResultValid"), M.bCompileResultValid);
		O->SetNumberField(TEXT("compileErrorCount"), M.CompileErrorCount);
		O->SetNumberField(TEXT("compileWarningCount"), M.CompileWarningCount);
		O->SetNumberField(TEXT("samplerTypeChecked"), M.SamplerTypeChecked);
		O->SetNumberField(TEXT("samplerTypeMismatched"), M.SamplerTypeMismatched);
		O->SetBoolField(TEXT("instanceCreated"), M.bInstanceCreated);
		O->SetStringField(TEXT("materialInstanceName"), M.MaterialInstanceName);
		O->SetBoolField(TEXT("bumpOffsetHeightLinked"), M.bBumpOffsetHeightLinked);
		O->SetBoolField(TEXT("bumpOffsetCoordinateLinked"), M.bBumpOffsetCoordinateLinked);
		O->SetNumberField(TEXT("samplesDrivenByBumpOffset"), M.SamplesDrivenByBumpOffset);
		O->SetNumberField(TEXT("samplesExpected"), M.SamplesExpected);
		O->SetBoolField(TEXT("parallaxAppliedToAllDeliverables"), M.bParallaxAppliedToAllDeliverables);

		if (M.InstanceScalars.Num() > 0)
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			for (const TPair<FString, float>& P : M.InstanceScalars)
			{
				S->SetNumberField(P.Key, P.Value);
			}
			O->SetObjectField(TEXT("instanceScalars"), S);
		}
		if (M.CompileMessages.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const FString& Msg : M.CompileMessages)
			{
				Arr.Add(MakeShared<FJsonValueString>(Msg));
			}
			O->SetArrayField(TEXT("compileMessages"), Arr);
		}
		if (M.StructuralWarnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const FString& W : M.StructuralWarnings)
			{
				Arr.Add(MakeShared<FJsonValueString>(W));
			}
			O->SetArrayField(TEXT("structuralWarnings"), Arr);
		}
		if (M.SamplerTypeMismatches.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const FString& X : M.SamplerTypeMismatches)
			{
				Arr.Add(MakeShared<FJsonValueString>(X));
			}
			O->SetArrayField(TEXT("samplerTypeMismatches"), Arr);
		}
		return O;
	}
}

FString FComfyImportReport::ToJson() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("baseName"), BaseName);
	Root->SetStringField(TEXT("destinationPath"), DestinationPath);
	Root->SetStringField(TEXT("generatedAt"), GeneratedAt);
	Root->SetBoolField(TEXT("hasProblems"), HasProblems());

	TArray<TSharedPtr<FJsonValue>> TexArr;
	for (const FComfyTextureImportEntry& E : Textures)
	{
		TexArr.Add(MakeShared<FJsonValueObject>(TextureEntryToJson(E)));
	}
	Root->SetArrayField(TEXT("textures"), TexArr);
	Root->SetObjectField(TEXT("material"), MaterialToJson(Material));

	if (SkippedOrphans.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& S : SkippedOrphans)
		{
			Arr.Add(MakeShared<FJsonValueString>(S));
		}
		Root->SetArrayField(TEXT("skippedOrphans"), Arr);
	}

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Out;
}

// --------------------------------------------------------------------------- //
// FComfyAssetVerification
// --------------------------------------------------------------------------- //

FString FComfyAssetVerification::ToText() const
{
	// 三元运算的两个分支必须同为 FString：UE 5.8 的格式化字符串检查不会替我们
	// 统一 `const TCHAR*` 与 `FString`（这类混用会直接编译失败）。
	const FString Verdict = bAllOk
		? FString(TEXT("全部通过"))
		: FString::Printf(TEXT("%d / %d 项不符"), Failed, Checked);

	TArray<FString> Out;
	Out.Add(FString::Printf(TEXT("冷加载核对（新进程从磁盘读回）：%s"), *Verdict));
	Out.Reserve(Out.Num() + Lines.Num());
	Out.Append(Lines);
	return FString::Join(Out, TEXT("\n"));
}

// --------------------------------------------------------------------------- //
// FComfyImportRegistry
// --------------------------------------------------------------------------- //

FComfyImportRegistry& FComfyImportRegistry::Get()
{
	static FComfyImportRegistry Instance;
	return Instance;
}

void FComfyImportRegistry::SetProvider(IComfyAssetImportProvider* InProvider)
{
	Provider = InProvider;
}
