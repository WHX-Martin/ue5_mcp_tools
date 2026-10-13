// ComfyTextureStudio · 正向提示词的「骨架 + 描述」编辑模型
//
// 为什么单独成文件：2026-10-13 出过一次事故 —— 提示词的固定前后缀
// （LoRA 触发词 mtrlx / 光照与细节 tag）在用户编辑「材质描述」时被**静默丢弃**，
// 提交出去的只剩一条裸描述，出图缺光照与几何线索，阶段B 的法线几乎全平，
// 看起来就是「PBR 和基础色对不上」。
//
// 根因是那段逻辑当时藏在面板的匿名命名空间里（Slate widget 内部），
// 既没法单测、也没法在 commandlet 里回归 —— 于是没人发现它会退化。
// 现在把它抽成不依赖 Slate 的纯逻辑，配 `-TestPromptScaffold` 的负向回归。
//
// 骨架为什么不能每次从完整提示词反推：
//   Split() 是启发式。当描述被清空时，整条都像"前后缀"，Split 会退化成
//   「整条都当描述」并返回空骨架；再拿这个空骨架拼回去，前缀就永久没了。
//   所以骨架**只在从 manifest 初始化、或用户在折叠区显式改动时更新**。

#pragma once

#include "CoreMinimal.h"

namespace ComfyPrompt
{
	/** 正向提示词的三段拆分结果。 */
	struct FComfyPromptScaffold
	{
		/** 前缀：LoRA 触发词等风格/技术 tag。 */
		FString Prefix;
		/** 中间：材质内容描述，用户在描述框里编辑的就是这一段。 */
		FString Description;
		/** 后缀：光照与细节 tag（决定贴图有没有立体感，阶段B 反演法线全靠它）。 */
		FString Suffix;

		bool HasScaffold() const { return !Prefix.IsEmpty() || !Suffix.IsEmpty(); }
	};

	/**
	 * 把完整正向提示词拆成「前缀 / 描述 / 后缀」。
	 *
	 * 这是**启发式**，不是真理：前后缀由 LoRA 训练集决定，靠关键词表识别。
	 * 拆出来的结果会显示在 UI 上供人核对，所以拆错是可见的，不会静默出错。
	 */
	COMFYTEXTURESTUDIO_API FComfyPromptScaffold Split(const FString& Prompt);

	/** 按 前缀, 描述, 后缀 拼回完整提示词（空段自动跳过）。 */
	COMFYTEXTURESTUDIO_API FString Join(const FString& Prefix, const FString& Description, const FString& Suffix);

	/**
	 * 提示词编辑模型：持有骨架，保证任何描述改动都不会把它弄丢。
	 *
	 * 面板只需要三个入口：ResetFromPrompt（从 manifest 初始化）、
	 * SetDescription（描述框）、SetFullPrompt（折叠区的完整文本编辑）。
	 */
	class COMFYTEXTURESTUDIO_API FComfyPromptEditor
	{
	public:
		/** 从完整提示词初始化，并重置骨架。 */
		void ResetFromPrompt(const FString& InFullPrompt);

		/** 描述框编辑：只换描述，骨架保持不变。 */
		void SetDescription(const FString& InDescription);

		/**
		 * 折叠区的完整文本编辑：尝试从中重新推导骨架。
		 *
		 * 只有当新文本**确实还能拆出骨架**时才覆盖 —— 否则（被清空、或写成一句
		 * 普通描述）保留原骨架。不这么做的话，一次误删就会把 LoRA 触发词永久抹掉，
		 * 而这正是本次事故的形态。
		 *
		 * @return true 表示骨架被更新；false 表示新文本拆不出骨架，骨架维持原样。
		 */
		bool SetFullPrompt(const FString& InFullPrompt);

		const FString& GetFullPrompt() const { return FullPrompt; }
		const FString& GetDescription() const { return Description; }
		const FString& GetPrefix() const { return Prefix; }
		const FString& GetSuffix() const { return Suffix; }

		/** 骨架是否可用（初始化时就拆不出来时为 false，UI 应显式告警）。 */
		bool HasScaffold() const { return bHasScaffold; }

		/** 提交前把三段拼回完整提示词应得的样式，用于自检。 */
		FString Compose() const { return Join(Prefix, Description, Suffix); }

		/** 骨架丢失的风险提示（空 = 正常）。 */
		FString GetWarning() const { return Warning; }

	private:
		void RebuildFromScaffold();

		FString FullPrompt;
		FString Description;
		FString Prefix;
		FString Suffix;
		bool bHasScaffold = false;
		FString Warning;
	};
}
