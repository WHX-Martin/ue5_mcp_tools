// ComfyTextureStudio · 右栏预览的「焦点 + 阶段A 历史候选」模型
//
// 为什么单独抽出来：这套状态原先直接写在 Slate 面板里，跟着 widget 的匿名命名空间一起
// 变成了既不能单测、也不能在 commandlet 里回归的代码。2026-10-14 用户报
// 「新生成的贴图不出现在右栏预览，还是旧图」—— 根因就在其中一行
// `if (FocusedPreviewPath.IsEmpty())`（只有第一次有产物时才成立），
// 这种"状态迁移写错"靠肉眼看 UI 是发现不了的，必须能用断言钉住。
//
// 这里只有纯数据与状态迁移，不依赖 Slate / Engine 之外的东西。

#pragma once

#include "CoreMinimal.h"

namespace ComfyPreview
{
	/**
	 * 阶段A 的历史候选 + 当前预览焦点。
	 *
	 * 两条规则是全部要点：
	 *  1. **新结果到达时预览必须跟过去**，除非用户手动钉住在某个候选上
	 *     （否则第二次之后预览永远停在第一张）。
	 *  2. 阶段B 的输入图必须与「用户此刻看到的那张」一致，不能让 UI 显示 A、实际拿 B
	 *     （`ResolveStageBInput` 负责这件事）。
	 */
	class COMFYTEXTURESTUDIO_API FStageAHistory
	{
	public:
		/** 最多留几项（最新的在最前，超出的从尾部丢）。 */
		static constexpr int32 MaxEntries = 8;

		// ------------------------------------------------------------ 历史候选 //
		/**
		 * 记一张阶段A 产物。已在列表里则提到最前。
		 * @return 是否是新加入的（重复的返回 false）。
		 */
		bool Remember(const FString& PngPath);

		int32 Num() const { return Entries.Num(); }
		bool IsValidIndex(int32 Index) const { return Entries.IsValidIndex(Index); }
		/** 越界返回空串（不要用 Entries[Index]，那会 UB）。 */
		const FString& At(int32 Index) const;
		int32 IndexOf(const FString& PngPath) const { return Entries.IndexOfByKey(PngPath); }
		bool Contains(const FString& PngPath) const { return Entries.Contains(PngPath); }
		const TArray<FString>& All() const { return Entries; }

		// ---------------------------------------------------------------- 焦点 //
		/** 直接设焦点。PngPath 为空则什么都不做。 */
		void Focus(const FString& PngPath, bool bPinned);

		/** 用户点了历史里的第 Index 项：设焦点并**钉住**。越界返回 false。 */
		bool FocusCandidate(int32 Index);

		/**
		 * 新结果到达。
		 * @return 焦点是否被改（false = 用户钉住了，不该抢）。
		 */
		bool FollowNewResult(const FString& PngPath);

		/** 只是解除"钉住"（新一轮生成开始时用），焦点保持不变。 */
		void Unpin() { bPinned = false; }
		/** 连焦点一起清（丢弃当前结果时用）。 */
		void ClearFocus();

		const FString& GetFocused() const { return Focused; }
		bool IsPinned() const { return bPinned; }
		bool IsCandidateFocused(int32 Index) const;

		// ------------------------------------------------------- 阶段B 的输入 //
		/**
		 * 阶段B 到底该吃哪张图。
		 *
		 * @param JobBaseTexture 本次阶段A 的产物路径（Job 自己的那份）。
		 * @return 要覆盖用的路径；**空串表示"不覆盖，用 JobBaseTexture"**。
		 *
		 * 只有「焦点确实是某个历史候选」且「与本次产物不同」才覆盖。第二条很重要：
		 * 阶段B 跑完后焦点会落到 albedo（阶段B 的产物），若不加这条判断，
		 * 再点一次「生成 PBR 全套」就会拿 albedo 当输入二次反演。
		 */
		FString ResolveStageBInput(const FString& JobBaseTexture) const;

	private:
		/** 最新在前。 */
		TArray<FString> Entries;
		FString Focused;
		bool bPinned = false;
	};
}
