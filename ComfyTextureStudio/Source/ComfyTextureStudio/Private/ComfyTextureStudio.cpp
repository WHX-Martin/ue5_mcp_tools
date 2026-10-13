#include "ComfyTextureStudio.h"

#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogComfyTextureStudio);

void FComfyTextureStudioModule::StartupModule()
{
	UE_LOG(LogComfyTextureStudio, Log, TEXT("ComfyTextureStudio 模块已加载"));
}

void FComfyTextureStudioModule::ShutdownModule()
{
	UE_LOG(LogComfyTextureStudio, Log, TEXT("ComfyTextureStudio 模块已卸载"));
}

IMPLEMENT_MODULE(FComfyTextureStudioModule, ComfyTextureStudio)
