// ComfyTextureStudio · Editor 模块入口

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogComfyTextureStudioEditor, Log, All);

class FComfyTextureStudioEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** Window 菜单 + 关卡编辑器工具栏按钮（PRD §7.1）。 */
	void RegisterMenus();

	/**
	 * 面板构造自检：带 `-CtsTestPanel` 启动时组装一次面板再销毁。
	 *
	 * 为什么需要：Slate 的 Construct 里最容易出问题（绑定到空成员、控件参数不合法、
	 * 计数器错位……），而且这类错误**只在人真正打开面板时才暴露**。UI 无法在无头环境
	 * 里点击，但"能不能被构造出来"是可以程序化验证的，那就先把这一步验证掉。
	 */
	void SmokeTestPanel();

	/** 导入服务实例（注册到 FComfyImportRegistry）。 */
	TUniquePtr<class FComfyTextureImportService> ImportService;
};
