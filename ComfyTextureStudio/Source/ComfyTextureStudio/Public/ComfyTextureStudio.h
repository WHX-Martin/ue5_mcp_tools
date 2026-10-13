// ComfyTextureStudio · Runtime 模块入口

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "Modules/ModuleInterface.h"

/**
 * 插件日志类别（PRD §9.2）。
 *
 * 控制台里 `LogComfyTextureStudio Verbose` 可打开完整请求/响应 JSON，
 * 用于和 ComfyUI 自己的日志对照。
 */
COMFYTEXTURESTUDIO_API DECLARE_LOG_CATEGORY_EXTERN(LogComfyTextureStudio, Log, All);

class FComfyTextureStudioModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
