using UnrealBuildTool;

public class ComfyTextureStudioHost : ModuleRules
{
	public ComfyTextureStudioHost(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ComfyTextureStudio", // 宿主工程存在的唯一理由：把插件编进来
		});
	}
}
