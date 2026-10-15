#include "ComfyPromptScaffold.h"

namespace ComfyPrompt
{
	/** 风格/技术词：判定为前缀。 */
	static bool IsStyleToken(const FString& Segment)
	{
		static const TCHAR* Keys[] = {
			TEXT("mtrlx"), TEXT("seamless"), TEXT("tileable"), TEXT("pbr"), TEXT("material")
		};
		for (const TCHAR* Key : Keys)
		{
			if (Segment.Contains(Key, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * 渲染/光照词：判定为后缀。
	 *
	 * 这些词不是"装饰"—— 它们决定基础贴图带不带方向光与高频浮雕，
	 * 而阶段B 的 DSINE 是靠明暗反演几何的。丢了它们，法线会明显偏平。
	 */
	static bool IsRenderToken(const FString& Segment)
	{
		static const TCHAR* Keys[] = {
			TEXT("lighting"), TEXT("sunlight"), TEXT("light"), TEXT("shadow"), TEXT("relief"),
			TEXT("detail"), TEXT("frequency"), TEXT("raking"), TEXT("crisp"), TEXT("soft"),
			TEXT("visible"), TEXT("scale pattern"), TEXT("top-down")
		};
		for (const TCHAR* Key : Keys)
		{
			if (Segment.Contains(Key, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	FComfyPromptScaffold Split(const FString& Prompt)
	{
		FComfyPromptScaffold Out;

		TArray<FString> Segments;
		Prompt.ParseIntoArray(Segments, TEXT(","), /*InCullEmpty=*/true);
		for (FString& S : Segments)
		{
			S.TrimStartAndEndInline();
		}
		if (Segments.Num() == 0)
		{
			Out.Description = Prompt;
			return Out;
		}

		// 前缀：从头开始连续的「风格/技术」段
		int32 First = 0;
		while (First < Segments.Num() && IsStyleToken(Segments[First]))
		{
			++First;
		}
		// 后缀：从尾开始连续的「渲染/光照」段
		int32 Last = Segments.Num() - 1;
		while (Last >= First && IsRenderToken(Segments[Last]))
		{
			--Last;
		}

		// 拆不出中间的描述（全被划成前后缀）时，退化为「整条都当描述」，不做切割。
		//
		// ⚠ 这个分支就是骨架会被弄丢的地方：一旦输入是"只有骨架"（描述为空），
		// 走到这里就返回空 Prefix/Suffix。所以调用方**必须缓存骨架**，
		// 不能在每次编辑后拿它重新推导 —— 见 FComfyPromptEditor。
		if (Last < First)
		{
			Out.Description = Prompt;
			return Out;
		}

		Out.Prefix = FString::Join(TArray<FString>(Segments.GetData(), First), TEXT(", "));
		Out.Description = FString::Join(
			TArray<FString>(Segments.GetData() + First, Last - First + 1), TEXT(", "));
		Out.Suffix = FString::Join(
			TArray<FString>(Segments.GetData() + Last + 1, Segments.Num() - Last - 1), TEXT(", "));

		return Out;
	}

	FString Join(const FString& Prefix, const FString& Description, const FString& Suffix)
	{
		TArray<FString> Parts;
		if (!Prefix.IsEmpty())      { Parts.Add(Prefix); }
		if (!Description.IsEmpty()) { Parts.Add(Description); }
		if (!Suffix.IsEmpty())      { Parts.Add(Suffix); }
		return FString::Join(Parts, TEXT(", "));
	}

	// ------------------------------------------------------------------ //
	// FComfyPromptEditor
	// ------------------------------------------------------------------ //

	void FComfyPromptEditor::ResetFromPrompt(const FString& InFullPrompt)
	{
		FullPrompt = InFullPrompt;
		bInitialized = true;

		const FComfyPromptScaffold Scaffold = Split(InFullPrompt);
		Prefix = Scaffold.Prefix;
		Suffix = Scaffold.Suffix;
		Description = Scaffold.Description;
		bHasScaffold = Scaffold.HasScaffold();

		Warning.Reset();
		if (!bHasScaffold && !InFullPrompt.IsEmpty())
		{
			Warning = FString::Printf(
				TEXT("提示词骨架拆不出来（没识别到 LoRA 触发词或光照/细节 tag），")
				TEXT("提交时会只有材质描述「%s」。"), *Description.Left(60));
		}
		else if (InFullPrompt.IsEmpty())
		{
			Warning = TEXT("正向提示词为空。");
		}
	}

	void FComfyPromptEditor::SetDescription(const FString& InDescription)
	{
		Description = InDescription;

		// 只换描述，骨架原样不动 —— 这是本次事故的修复点。
		RebuildFromScaffold();
	}

	bool FComfyPromptEditor::SetFullPrompt(const FString& InFullPrompt)
	{
		const FComfyPromptScaffold Scaffold = Split(InFullPrompt);
		if (!Scaffold.HasScaffold())
		{
			// 新文本拆不出骨架：**不覆盖骨架**，而是把它当作"新的描述内容"。
			// 保留原骨架是刻意的 —— 折叠区里一次误删不该把 LoRA 触发词永久抹掉
			// （本次事故就是这条路径的变体）。
			Description = InFullPrompt;
			RebuildFromScaffold();
			return false;
		}

		Prefix = Scaffold.Prefix;
		Suffix = Scaffold.Suffix;
		Description = Scaffold.Description;
		bHasScaffold = true;
		Warning.Reset();
		RebuildFromScaffold();
		return true;
	}

	void FComfyPromptEditor::RebuildFromScaffold()
	{
		FullPrompt = Join(Prefix, Description, Suffix);
	}
}
