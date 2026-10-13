#include "ComfyAssetNaming.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ComfyTextureStudioEditor.h"
#include "ComfyTextureStudioSettings.h"

namespace
{
	/** 常见的材质名词，用来从描述里挑出 {material}。故意保持简短可维护。 */
	const TArray<FString>& MaterialNouns()
	{
		static const TArray<FString> V = {
			TEXT("sand"), TEXT("gravel"), TEXT("rock"), TEXT("stone"), TEXT("pebble"),
			TEXT("metal"), TEXT("steel"), TEXT("iron"), TEXT("rust"), TEXT("copper"),
			TEXT("wood"), TEXT("plank"), TEXT("bark"), TEXT("bamboo"),
			TEXT("brick"), TEXT("concrete"), TEXT("cement"), TEXT("asphalt"), TEXT("tarmac"),
			TEXT("tile"), TEXT("ceramic"), TEXT("marble"), TEXT("granite"), TEXT("slate"),
			TEXT("fabric"), TEXT("cloth"), TEXT("leather"), TEXT("canvas"), TEXT("linen"),
			TEXT("grass"), TEXT("moss"), TEXT("leaf"), TEXT("foliage"),
			TEXT("dirt"), TEXT("soil"), TEXT("mud"), TEXT("clay"), TEXT("snow"), TEXT("ice"),
			TEXT("plaster"), TEXT("stucco"), TEXT("paper"), TEXT("rubber"), TEXT("plastic"),
			TEXT("glass"), TEXT("crystal"), TEXT("lava"), TEXT("magma"), TEXT("coral"),
		};
		return V;
	}

	/** 描述里高频出现但没信息量的词。 */
	bool IsStopWord(const FString& Word)
	{
		static const TSet<FString> Stop = {
			TEXT("a"), TEXT("an"), TEXT("the"), TEXT("of"), TEXT("with"), TEXT("and"),
			TEXT("or"), TEXT("very"), TEXT("some"), TEXT("texture"), TEXT("material"),
			TEXT("surface"), TEXT("seamless"), TEXT("tileable"), TEXT("highly"),
			TEXT("high"), TEXT("quality"), TEXT("detailed"), TEXT("pattern"), TEXT("background"),
		};
		return Stop.Contains(Word);
	}

	FString ToPascalCase(const FString& Word)
	{
		if (Word.IsEmpty())
		{
			return Word;
		}
		FString Out = Word.ToLower();
		Out[0] = FChar::ToUpper(Out[0]);
		return Out;
	}

	/** 只保留字母数字与下划线，其余换成下划线；顺带合并连续下划线。 */
	FString NormalizeSegment(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		bool bLastUnderscore = false;
		for (const TCHAR Ch : In)
		{
			if (FChar::IsAlnum(Ch))
			{
				Out.AppendChar(Ch);
				bLastUnderscore = false;
			}
			else
			{
				if (!bLastUnderscore && Out.Len() > 0)
				{
					Out.AppendChar(TEXT('_'));
					bLastUnderscore = true;
				}
			}
		}
		while (Out.EndsWith(TEXT("_")))
		{
			Out.LeftChopInline(1);
		}
		return Out;
	}
}

// --------------------------------------------------------------------------- //
// 基础工具
// --------------------------------------------------------------------------- //

FString FComfyAssetNaming::SanitizeName(const FString& In)
{
	return NormalizeSegment(In.TrimStartAndEnd());
}

FString FComfyAssetNaming::ExpandTemplate(
	const FString& Template,
	const FString& Material,
	const FString& Grain,
	int32 Index,
	int32 IndexPadding,
	const FString& Suffix)
{
	FString Out = Template;
	const FString IndexText = IndexPadding > 0
		? FString::Printf(TEXT("%0*d"), IndexPadding, Index)
		: FString::FromInt(Index);

	Out.ReplaceInline(TEXT("{material}"), *Material, ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT("{grain}"), *Grain, ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT("{index}"), *IndexText, ESearchCase::CaseSensitive);
	// {suffix} 带前导下划线，如 `_BC`；模板里直接写 `T_{material}_{grain}_{index}{suffix}`
	Out.ReplaceInline(TEXT("{suffix}"), *Suffix, ESearchCase::CaseSensitive);
	return Out;
}

// --------------------------------------------------------------------------- //
// 从描述猜 material / grain
// --------------------------------------------------------------------------- //

void FComfyAssetNaming::SuggestFromDescription(const FString& Description, FString& OutMaterial, FString& OutGrain)
{
	// 切词：只保留字母数字
	TArray<FString> Words;
	FString Current;
	for (const TCHAR Ch : Description)
	{
		if (FChar::IsAlnum(Ch))
		{
			Current.AppendChar(Ch);
		}
		else if (!Current.IsEmpty())
		{
			Words.Add(Current.ToLower());
			Current.Reset();
		}
	}
	if (!Current.IsEmpty())
	{
		Words.Add(Current.ToLower());
	}

	// 第一优先：命中材质名词表
	int32 MaterialIndex = INDEX_NONE;
	for (int32 i = 0; i < Words.Num(); ++i)
	{
		if (MaterialNouns().Contains(Words[i]))
		{
			MaterialIndex = i;
			break;
		}
	}

	// 退化：第一个非停用词当 material
	if (MaterialIndex == INDEX_NONE)
	{
		for (int32 i = 0; i < Words.Num(); ++i)
		{
			if (!IsStopWord(Words[i]))
			{
				MaterialIndex = i;
				break;
			}
		}
	}

	if (MaterialIndex == INDEX_NONE)
	{
		OutMaterial = TEXT("Material");
		OutGrain = TEXT("A");
		return;
	}

	OutMaterial = ToPascalCase(Words[MaterialIndex]);

	// grain 取离 material 最近的、非停用、非材质词的形容词。
	// 先往前找（`coarse pebbly sand` 里 `coarse` 在 sand 前面），再往后找。
	FString Grain;
	const auto TryTake = [&Words, &Grain](int32 i)
	{
		if (i < 0 || i >= Words.Num())
		{
			return false;
		}
		const FString& W = Words[i];
		if (IsStopWord(W) || MaterialNouns().Contains(W))
		{
			return false;
		}
		Grain = ToPascalCase(W);
		return true;
	};

	for (int32 i = MaterialIndex - 1; i >= 0; --i)
	{
		if (TryTake(i))
		{
			break;
		}
	}
	if (Grain.IsEmpty())
	{
		for (int32 i = MaterialIndex + 1; i < Words.Num(); ++i)
		{
			if (TryTake(i))
			{
				break;
			}
		}
	}
	if (Grain.IsEmpty())
	{
		Grain = TEXT("A");
	}

	OutGrain = Grain;
}

// --------------------------------------------------------------------------- //
// 冲突检测
// --------------------------------------------------------------------------- //

bool FComfyAssetNaming::HasAssetConflict(
	const FString& DestinationPath,
	const TArray<FString>& AssetNames,
	TArray<FString>& OutConflicts)
{
	OutConflicts.Reset();

	const FAssetRegistryModule& RegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& Registry = RegistryModule.Get();

	// 只看目标文件夹这一层，不递归 —— 命名冲突本来就发生在同一文件夹内。
	TArray<FAssetData> Existing;
	Registry.GetAssetsByPath(FName(*DestinationPath), Existing, /*bRecursive=*/false);

	TSet<FString> ExistingNames;
	ExistingNames.Reserve(Existing.Num());
	for (const FAssetData& Data : Existing)
	{
		ExistingNames.Add(Data.AssetName.ToString());
	}

	for (const FString& Name : AssetNames)
	{
		if (ExistingNames.Contains(Name))
		{
			OutConflicts.Add(Name);
		}
	}
	return OutConflicts.Num() > 0;
}

// --------------------------------------------------------------------------- //
// 主解析
// --------------------------------------------------------------------------- //

FComfyResolvedAssetNaming FComfyAssetNaming::Resolve(
	const UComfyTextureStudioSettings& Settings,
	const FString& ManualName,
	const FString& Material,
	const FString& Grain,
	bool bUseTemplate,
	const TArray<FString>& OutSuffixes)
{
	FComfyResolvedAssetNaming Out;

	const FString SafeMaterial = SanitizeName(Material.IsEmpty() ? TEXT("Material") : Material);
	const FString SafeGrain = SanitizeName(Grain.IsEmpty() ? TEXT("A") : Grain);

	// 组装一组完整名字（给定 index / 基名）
	const auto BuildNames = [&](int32 Index, const FString& ExplicitBase,
		FString& OutBase, FString& OutDest, FString& OutMat, FString& OutMI,
		TMap<FString, FString>& OutTex)
	{
		if (bUseTemplate)
		{
			OutBase = SanitizeName(ExpandTemplate(
				TEXT("{material}_{grain}_{index}"), SafeMaterial, SafeGrain, Index, Settings.IndexPadding));
			OutDest = ExpandTemplate(Settings.FolderNameTemplate, SafeMaterial, SafeGrain, Index, Settings.IndexPadding);
			OutDest.ReplaceInline(TEXT("{assetRoot}"), *Settings.AssetRootPath, ESearchCase::CaseSensitive);
			OutMat = SanitizeName(ExpandTemplate(
				Settings.MaterialNameTemplate, SafeMaterial, SafeGrain, Index, Settings.IndexPadding));
		}
		else
		{
			OutBase = ExplicitBase;
			OutDest = FString::Printf(TEXT("%s/%s"), *Settings.AssetRootPath, *ExplicitBase);
			OutMat = FString::Printf(TEXT("M_%s"), *ExplicitBase);
		}

		OutMI = FString::Printf(TEXT("MI_%s"), *OutBase);
		OutTex.Reset();
		for (const FString& Suffix : OutSuffixes)
		{
			FString TexName;
			if (bUseTemplate)
			{
				TexName = SanitizeName(ExpandTemplate(
					Settings.TextureNameTemplate, SafeMaterial, SafeGrain, Index, Settings.IndexPadding, Suffix));
			}
			else
			{
				TexName = FString::Printf(TEXT("T_%s%s"), *OutBase, *Suffix);
			}
			OutTex.Add(Suffix, TexName);
		}
	};

	const auto CollectAssetNames = [&](const FString& Mat, const FString& MI, const TMap<FString, FString>& Tex)
	{
		TArray<FString> Names;
		Names.Add(Mat);
		Names.Add(MI);
		for (const TPair<FString, FString>& P : Tex)
		{
			Names.Add(P.Value);
		}
		return Names;
	};

	if (bUseTemplate)
	{
		// {index} 的语义就是「自动避让已有资产」，所以模板模式一律递增到不冲突。
		for (int32 Index = 1; Index <= 999; ++Index)
		{
			FString Base, Dest, Mat, MI;
			TMap<FString, FString> Tex;
			BuildNames(Index, FString(), Base, Dest, Mat, MI, Tex);

			TArray<FString> Conflicts;
			if (!HasAssetConflict(Dest, CollectAssetNames(Mat, MI, Tex), Conflicts))
			{
				Out.bSuccess = true;
				Out.BaseName = Base;
				Out.DestinationPath = Dest;
				Out.MaterialName = Mat;
				Out.MaterialInstanceName = MI;
				Out.TextureNames = MoveTemp(Tex);
				Out.ResolvedIndex = Index;
				return Out;
			}
		}
		Out.Error = TEXT("模板模式下连续 999 个序号都被占用，请检查资产路径或改用其他命名。");
		return Out;
	}

	// ---- 手填模式 ----
	const FString Explicit = SanitizeName(ManualName);
	if (Explicit.IsEmpty())
	{
		Out.Error = TEXT("手填模式下资产名不能为空。");
		return Out;
	}

	FString Base, Dest, Mat, MI;
	TMap<FString, FString> Tex;
	BuildNames(0, Explicit, Base, Dest, Mat, MI, Tex);

	TArray<FString> Conflicts;
	if (!HasAssetConflict(Dest, CollectAssetNames(Mat, MI, Tex), Conflicts))
	{
		Out.bSuccess = true;
		Out.BaseName = Base;
		Out.DestinationPath = Dest;
		Out.MaterialName = Mat;
		Out.MaterialInstanceName = MI;
		Out.TextureNames = MoveTemp(Tex);
		Out.ResolvedIndex = 0;
		return Out;
	}

	// 撞名了。Error 策略（默认）：**报错而不是覆盖**（Q6 的明确要求）。
	if (Settings.IndexConflictPolicy == EComfyIndexConflictPolicy::Error)
	{
		Out.Error = FString::Printf(
			TEXT("目标路径已存在同名资产：%s。换个名字，或把命名模式改成「模板」让序号自动避让。"),
			*FString::Join(Conflicts, TEXT("、")));
		return Out;
	}

	// AutoIncrement：给基名加序号后缀再试
	for (int32 i = 1; i <= 999; ++i)
	{
		const FString Candidate = FString::Printf(TEXT("%s%s"),
			*Explicit,
			*FString::Printf(TEXT("_%0*d"), FMath::Max(Settings.IndexPadding, 1), i));

		FString B2, D2, M2, MI2;
		TMap<FString, FString> T2;
		BuildNames(0, Candidate, B2, D2, M2, MI2, T2);

		TArray<FString> C2;
		if (!HasAssetConflict(D2, CollectAssetNames(M2, MI2, T2), C2))
		{
			Out.bSuccess = true;
			Out.BaseName = B2;
			Out.DestinationPath = D2;
			Out.MaterialName = M2;
			Out.MaterialInstanceName = MI2;
			Out.TextureNames = MoveTemp(T2);
			Out.ResolvedIndex = i;
			return Out;
		}
	}

	Out.Error = TEXT("自动避让连续 999 次仍冲突，请手动指定资产名。");
	return Out;
}
