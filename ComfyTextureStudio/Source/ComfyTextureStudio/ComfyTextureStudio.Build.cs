// ComfyTextureStudio · Runtime 模块构建规则
//
// 本模块只做「ComfyUI 通信 + 工作流装载 + 任务状态机」，不碰任何编辑器专用 API
// （UnrealEd / Slate / MaterialEditor 一律留给 ComfyTextureStudioEditor，M2 建）。
// 这样它才能被 commandlet、批处理工具、甚至未来的运行时工具复用。

using UnrealBuildTool;

public class ComfyTextureStudio : ModuleRules
{
	public ComfyTextureStudio(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HTTP",              // FHttpModule / IHttpRequest
			"Json",              // FJsonObject / FJsonSerializer
			"JsonUtilities",
			"DeveloperSettings", // UDeveloperSettings（PRD §8 的配置落点）
			"Projects",          // IPluginManager —— 用来定位插件自带的 Config/Workflows
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
