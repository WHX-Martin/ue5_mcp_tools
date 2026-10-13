#include "ComfyTextureStudioSettings.h"

#include "Misc/Paths.h"

UComfyTextureStudioSettings::UComfyTextureStudioSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Comfy Texture Studio");
}

FString UComfyTextureStudioSettings::GetCustomNodesDir() const
{
	if (!ComfyCustomNodesDir.IsEmpty())
	{
		return ComfyCustomNodesDir;
	}
	return FPaths::Combine(ComfyUserDataDir, TEXT("custom_nodes"));
}

FString UComfyTextureStudioSettings::GetDsineWeightPath() const
{
	// SeamlessDSINENormal 通过 comfyui_controlnet_aux 提供的 DSINE 架构取权重，
	// 路径固定在这个位置（M0 已核实本机存在，字节数吻合）。
	return FPaths::Combine(
		GetCustomNodesDir(),
		TEXT("comfyui_controlnet_aux/ckpts/hr16/Diffusion-Edge/dsine.pt"));
}

FString UComfyTextureStudioSettings::GetCacheDirAbs() const
{
	if (FPaths::IsRelative(CacheDir))
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), CacheDir));
	}
	return FPaths::ConvertRelativePathToFull(CacheDir);
}
