// ComfyTextureStudio · 插件配置（PRD §8）
//
// 落点：DefaultGame.ini 的 [/Script/ComfyTextureStudio.ComfyTextureStudioSettings]

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "ComfyTextureStudioSettings.generated.h"

/** 命名模式（决策 Q6）：模板 + 手填双通道。 */
UENUM()
enum class EComfyNamingMode : uint8
{
	Template, // 按模板自动生成
	Manual,   // 用户手填
};

/** 手填模式下重名时的行为。刻意不提供「静默覆盖」。 */
UENUM()
enum class EComfyIndexConflictPolicy : uint8
{
	Error,         // 直接报错（默认）
	AutoIncrement, // 自动避让到下一个序号
};

UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Comfy Texture Studio"))
class COMFYTEXTURESTUDIO_API UComfyTextureStudioSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UComfyTextureStudioSettings();

	// ------------------------------------------------------------------ 服务 //
	/** ComfyUI 服务地址。注意桌面版默认端口是 8000，不是 8188。 */
	UPROPERTY(EditAnywhere, config, Category = "服务")
	FString ServerUrl = TEXT("http://127.0.0.1:8000");

	/** 单次 HTTP 超时（秒）。 */
	UPROPERTY(EditAnywhere, config, Category = "服务", meta = (ClampMin = "1.0"))
	float RequestTimeoutSeconds = 60.0f;

	/** /history 轮询间隔（秒）。 */
	UPROPERTY(EditAnywhere, config, Category = "服务", meta = (ClampMin = "0.1"))
	float PollIntervalSeconds = 1.5f;

	/** 整任务上限（秒），超过判失败。 */
	UPROPERTY(EditAnywhere, config, Category = "服务", meta = (ClampMin = "10.0"))
	float JobTimeoutSeconds = 900.0f;

	/** 打印完整请求/响应体（等同 LogComfyTextureStudio Verbose）。 */
	UPROPERTY(EditAnywhere, config, Category = "服务")
	bool VerboseHttpLog = false;

	// ------------------------------------------------------------------ 路径 //
	/** 导入时的资产落点根。 */
	UPROPERTY(EditAnywhere, config, Category = "路径")
	FString AssetRootPath = TEXT("/Game/ComfyTextures");

	/** 产物中转目录。相对路径按 <项目>/Saved/ 解析。 */
	UPROPERTY(EditAnywhere, config, Category = "路径")
	FString CacheDir = TEXT("ComfyTextureStudio/Cache");

	/** 以下三个路径相对插件根目录解析（工作流随插件分发）。 */
	UPROPERTY(EditAnywhere, config, Category = "路径")
	FString StageAWorkflowFile = TEXT("Config/Workflows/stage_a_base_texture.json");

	UPROPERTY(EditAnywhere, config, Category = "路径")
	FString StageBWorkflowFile = TEXT("Config/Workflows/stage_b_pbr.json");

	UPROPERTY(EditAnywhere, config, Category = "路径")
	FString WorkflowManifestFile = TEXT("Config/Workflows/workflows.manifest.json");

	// ------------------------------------------------------------- 导入默认值 //
	/** D3：ComfyUI 出 OpenGL 法线，UE 需要 DirectX ⇒ 必须翻绿通道。 */
	UPROPERTY(EditAnywhere, config, Category = "导入默认值")
	bool DefaultFlipGreenChannel = true;

	/** Q4：高度图参与 BumpOffset 视差。 */
	UPROPERTY(EditAnywhere, config, Category = "导入默认值")
	bool DefaultImportHeight = true;

	/** Q3：ORM 已合并，AO/Roughness 默认不导入。 */
	UPROPERTY(EditAnywhere, config, Category = "导入默认值")
	bool DefaultImportOrphanMaps = false;

	UPROPERTY(EditAnywhere, config, Category = "导入默认值")
	bool DefaultCreateMaterialInstance = true;

	/** BumpOffset 的 HeightRatio 初值。纯手感参数，需实测后固化（R9）。 */
	UPROPERTY(EditAnywhere, config, Category = "导入默认值", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float DefaultBumpDepth = 0.02f;

	/** 关则导入后清掉下载缓存。 */
	UPROPERTY(EditAnywhere, config, Category = "导入默认值")
	bool PreserveDownloadedPng = false;

	// ------------------------------------------------------------ 命名（Q6） //
	UPROPERTY(EditAnywhere, config, Category = "命名")
	EComfyNamingMode NamingMode = EComfyNamingMode::Template;

	/** 可用占位符：{material} {grain} {index}。 */
	UPROPERTY(EditAnywhere, config, Category = "命名")
	FString MaterialNameTemplate = TEXT("M_{material}_{grain}_{index}");

	UPROPERTY(EditAnywhere, config, Category = "命名")
	FString TextureNameTemplate = TEXT("T_{material}_{grain}_{index}_{suffix}");

	UPROPERTY(EditAnywhere, config, Category = "命名")
	FString FolderNameTemplate = TEXT("{assetRoot}/{material}_{grain}_{index}");

	/** {index} 补零位数。 */
	UPROPERTY(EditAnywhere, config, Category = "命名", meta = (ClampMin = "0", ClampMax = "6"))
	int32 IndexPadding = 2;

	UPROPERTY(EditAnywhere, config, Category = "命名")
	EComfyIndexConflictPolicy IndexConflictPolicy = EComfyIndexConflictPolicy::Error;

	// ------------------------------------------------- ComfyUI 侧定位（Q8） //
	/**
	 * ComfyUI 的用户数据目录。
	 * ⚠ 必须是这个，**不是** comfy-cli 报的 default_workspace —— 后者下面的
	 * custom_nodes 只有示例文件，复制进去会「复制成功但节点还是缺」（PRD §5.1）。
	 */
	UPROPERTY(EditAnywhere, config, Category = "ComfyUI 侧定位")
	FString ComfyUserDataDir = TEXT("C:/Users/1142/AppData/Local/Programs/ComfyUI-data");

	/** 一键复制节点包的目标目录。留空则按 ComfyUserDataDir 推导。 */
	UPROPERTY(EditAnywhere, config, Category = "ComfyUI 侧定位")
	FString ComfyCustomNodesDir;

	/** 填了才能用「尝试启动 ComfyUI」。 */
	UPROPERTY(EditAnywhere, config, Category = "ComfyUI 侧定位")
	FString ComfyCliPath;

	/** dsine.pt 预期字节数，预检做截断校验（镜像曾送出 29 MB 残件）。 */
	UPROPERTY(EditAnywhere, config, Category = "ComfyUI 侧定位")
	int64 DsineWeightExpectedBytes = 291298581;

	// ------------------------------------------------------------------ 派生 //
	/** ComfyUI 自定义节点目录（含空值推导）。 */
	FString GetCustomNodesDir() const;

	/** dsine.pt 的绝对路径。 */
	FString GetDsineWeightPath() const;

	/** 产物中转目录的绝对路径（相对路径按 <项目>/Saved/ 解析）。 */
	FString GetCacheDirAbs() const;
};
