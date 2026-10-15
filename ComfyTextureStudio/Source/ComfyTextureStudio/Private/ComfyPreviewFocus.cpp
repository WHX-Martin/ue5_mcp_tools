// ComfyTextureStudio · 预览焦点模型的实现（见头文件里的动机说明）

#include "ComfyPreviewFocus.h"

namespace ComfyPreview
{
	const FString& FStageAHistory::At(int32 Index) const
	{
		static const FString Empty;
		return Entries.IsValidIndex(Index) ? Entries[Index] : Empty;
	}

	bool FStageAHistory::Remember(const FString& PngPath)
	{
		if (PngPath.IsEmpty())
		{
			return false;
		}

		const int32 Existing = Entries.IndexOfByKey(PngPath);
		if (Existing != INDEX_NONE)
		{
			// 已经在列表里：把它提到最前 —— 它确实是最新产出的那张。
			// 直接 return false 会让它留在旧位置，于是"#1 = 最新"这条 UI 约定就不成立了。
			if (Existing != 0)
			{
				Entries.RemoveAt(Existing);
				Entries.Insert(PngPath, 0);
			}
			return false;
		}

		Entries.Insert(PngPath, 0);
		while (Entries.Num() > MaxEntries)
		{
			Entries.Pop();
		}
		return true;
	}

	void FStageAHistory::Focus(const FString& PngPath, bool bInPinned)
	{
		if (PngPath.IsEmpty())
		{
			return;
		}
		Focused = PngPath;
		bPinned = bInPinned;
	}

	bool FStageAHistory::FocusCandidate(int32 Index)
	{
		if (!IsValidIndex(Index))
		{
			return false;
		}
		Focused = Entries[Index];
		bPinned = true;
		return true;
	}

	bool FStageAHistory::FollowNewResult(const FString& PngPath)
	{
		if (PngPath.IsEmpty())
		{
			return false;
		}
		// 用户钉住了某个候选（正在对比）时不许抢焦点；除此之外新结果一律接管。
		if (bPinned && !Focused.IsEmpty())
		{
			return false;
		}
		Focused = PngPath;
		return true;
	}

	void FStageAHistory::ClearFocus()
	{
		Focused.Reset();
		bPinned = false;
	}

	bool FStageAHistory::IsCandidateFocused(int32 Index) const
	{
		return IsValidIndex(Index) && Entries[Index] == Focused;
	}

	FString FStageAHistory::ResolveStageBInput(const FString& JobBaseTexture) const
	{
		if (Focused.IsEmpty())
		{
			return FString();
		}
		// 焦点不是历史候选 ⇒ 可能是阶段B 的产物（albedo），不能当输入喂回去。
		if (!Entries.Contains(Focused))
		{
			return FString();
		}
		// 就是本次产物 ⇒ 交给 Job 用默认值，别多走一条覆盖路径。
		if (Focused == JobBaseTexture)
		{
			return FString();
		}
		return Focused;
	}
}
