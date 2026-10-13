// ComfyTextureStudio · 无 UI 触发入口（M1）
//
// 用法：
//   UnrealEditor-Cmd.exe <项目>.uproject -run=ComfyTextureStudio -unattended -nopause -nosplash
//
// 可选参数：
//   -JobId=<名>          产物子目录名，默认时间戳 + GUID 短码
//   -Prompt="..."        正向提示词（默认用 manifest 里的原值）
//   -Negative="..."      负向提示词
//   -Seed=<int>          种子；`-Seed=random` 取随机种子（用来避开节点级缓存）
//   -Steps=<int>         步数
//   -Width= -Height=     出图尺寸
//   -BatchSize=<int>     批次数（M1 只取第 1 张作为阶段 B 输入）
//   -Lora=<文件名>       换 LoRA（Q7 预留通道）
//   -Set=key=value,...   任意语义键覆盖，逗号分隔
//   -ServerUrl=<url>     覆盖插件设置里的服务地址
//   -OutDir=<绝对路径>   覆盖产物根目录

#pragma once

#include "Commandlets/Commandlet.h"
#include "CoreMinimal.h"

#include "ComfyTextureStudioCommandlet.generated.h"

UCLASS()
class COMFYTEXTURESTUDIO_API UComfyTextureStudioCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UComfyTextureStudioCommandlet();

	virtual int32 Main(const FString& Params) override;

private:
	static void PrintUsage();
};
