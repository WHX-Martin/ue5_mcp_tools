#include "ComfyPaths.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace ComfyPaths
{
	FString ResolvePluginPath(const FString& MaybeRelative)
	{
		if (MaybeRelative.IsEmpty())
		{
			return MaybeRelative;
		}
		if (FPaths::IsRelative(MaybeRelative) && !MaybeRelative.StartsWith(TEXT("/")))
		{
			if (const TSharedPtr<IPlugin> Plugin =
				IPluginManager::Get().FindPlugin(TEXT("ComfyTextureStudio")))
			{
				return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / MaybeRelative);
			}
		}
		return FPaths::ConvertRelativePathToFull(MaybeRelative);
	}

	bool FileExists(const FString& Path)
	{
		return !Path.IsEmpty() && FPaths::FileExists(Path);
	}

	FString ResolveWorkflowFile(const FString& MaybeRelative, const FString& ManifestPath)
	{
		if (MaybeRelative.IsEmpty())
		{
			return FString();
		}

		// 已是绝对路径：直接用（不存在也算错，让调用方报出来）
		if (!FPaths::IsRelative(MaybeRelative) || MaybeRelative.StartsWith(TEXT("/")))
		{
			const FString Absolute = FPaths::ConvertRelativePathToFull(MaybeRelative);
			return FileExists(Absolute) ? Absolute : FString();
		}

		TArray<FString> Candidates;

		// ① 相对插件根（manifest 写全了 `Config/Workflows/x.json` 时命中）
		Candidates.Add(ResolvePluginPath(MaybeRelative));

		// ② 相对 manifest 所在目录（只写文件名时最自然的解释）
		if (!ManifestPath.IsEmpty())
		{
			Candidates.Add(FPaths::ConvertRelativePathToFull(
				FPaths::GetPath(ManifestPath) / MaybeRelative));
		}

		// ③ 插件根 / Config/Workflows /（兜底）
		Candidates.Add(ResolvePluginPath(TEXT("Config/Workflows") / MaybeRelative));

		for (const FString& Candidate : Candidates)
		{
			if (FileExists(Candidate))
			{
				return FPaths::ConvertRelativePathToFull(Candidate);
			}
		}

		// 一个都不存在 —— 返回空串，调用方必须报错。
		// 注意这里**不返回** ① 那个"看起来最像"的路径：返回死路径会让上游
		// 拿到一个非空字符串，从而把"配置错了"误当成"设置好了"。
		return FString();
	}
}
