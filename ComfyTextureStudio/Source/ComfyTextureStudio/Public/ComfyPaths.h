// ComfyTextureStudio · 插件内路径解析
//
// 为什么单独抽出来：这些路径散落在 Job / ImportService / Panel 三处，
// 而 `ResolvePluginPath` 曾经有**两份独立实现**（FComfyTextureJob 与
// FComfyTextureImportService 各写一遍）—— 改一处忘另一处就是迟早的事。
//
// 2026-10-14 的事故正是路径问题：manifest 里 `apiWorkflowFile` 只写了文件名
// （`stage_a_base_texture.json`），而它被直接拼到插件根，于是
// `<插件>/stage_a_base_texture.json` 当然不存在。后果不是"报个错"——
// 面板因此**从来没拿到过正向提示词模板**，骨架一直是空的；
// 用户一编辑描述框，完整模板就被一条裸描述顶掉，出图缺光照/几何线索，
// 阶段B 的法线几乎全平。详见 docs/M2.3-prompt-scaffold-and-viewer.md 的续篇。

#pragma once

#include "CoreMinimal.h"

namespace ComfyPaths
{
	/**
	 * 把「相对插件根」的路径解析成绝对路径。
	 *
	 * 例：`Config/Workflows/workflows.manifest.json` → `<插件目录>/Config/Workflows/workflows.manifest.json`
	 */
	COMFYTEXTURESTUDIO_API FString ResolvePluginPath(const FString& MaybeRelative);

	/**
	 * 解析工作流 JSON 文件的位置。
	 *
	 * manifest 里的工作流字段历史上出现过两种写法，所以按顺序试三个基准，
	 * 取第一个真实存在的：
	 *   ① 插件根 / 相对路径        —— 写法是 `Config/Workflows/x.json` 时命中
	 *   ② manifest 同目录 / 文件名 —— 只写文件名时最自然的解释（与 manifest 放一起）
	 *   ③ 插件根 / Config/Workflows / 相对路径 —— 兜底
	 *
	 * **都不存在时返回空串**，而不是返回一个不存在的路径 ——
	 * 调用方必须把空串当成"配置错了"明确报出来。返回一个"看起来像路径"的
	 * 死路径是这次事故能潜伏两个月的原因：它只在日志里留一行 Warning。
	 *
	 * @param ManifestPath 已解析好的 manifest 绝对路径（可为空，跳过 ②）
	 */
	COMFYTEXTURESTUDIO_API FString ResolveWorkflowFile(
		const FString& MaybeRelative, const FString& ManifestPath);

	/** 文件是否存在（ResolveWorkflowFile 用它判断候选）。 */
	COMFYTEXTURESTUDIO_API bool FileExists(const FString& Path);
}
