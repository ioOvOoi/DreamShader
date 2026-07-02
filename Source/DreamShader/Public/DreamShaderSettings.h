#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "MaterialDomain.h"

#include "DreamShaderSettings.generated.h"

/** Backend used for source files that do not specify Settings = { Backend = "..." } themselves. */
UENUM()
enum class EDreamShaderDefaultBackend : uint8
{
	/** Build a UMaterial node graph per material (full DSL feature surface). */
	Graph,
	/** Compile the shading logic into a generated .ush and emit a lightweight material instance (no node graph). */
	Instance,
};

UCLASS(Config=Engine, DefaultConfig, meta=(DisplayName="DreamShader"))
class DREAMSHADER_API UDreamShaderSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UDreamShaderSettings();

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("DreamPlugin"); }
	virtual FName GetSectionName() const override { return TEXT("DreamShader"); }

#if WITH_EDITOR
	virtual FText GetSectionText() const override { return FText::FromString(TEXT("Dream Shader")); }
	virtual FText GetSectionDescription() const override { return FText::FromString(TEXT("Dream Shader Settings")); }
#endif

	bool TryResolveShadingModel(const FString& InName, EMaterialShadingModel& OutShadingModel) const;
	bool TryResolveBlendMode(const FString& InName, EBlendMode& OutBlendMode) const;
	bool TryResolveMaterialDomain(const FString& InName, EMaterialDomain& OutMaterialDomain) const;

	static FString NormalizeMappingKey(const FString& InName);
	static void BuildDefaultShadingModelMappings(TMap<FString, TEnumAsByte<EMaterialShadingModel>>& OutMappings);
	static void BuildDefaultBlendModeMappings(TMap<FString, TEnumAsByte<EBlendMode>>& OutMappings);
	static void BuildDefaultMaterialDomainMappings(TMap<FString, TEnumAsByte<EMaterialDomain>>& OutMappings);

	UPROPERTY(Config, EditAnywhere, Category="Mappings")
	TMap<FString, TEnumAsByte<EMaterialShadingModel>> ShadingModelMappings;

	UPROPERTY(Config, EditAnywhere, Category="Mappings")
	TMap<FString, TEnumAsByte<EBlendMode>> BlendModeMappings;

	UPROPERTY(Config, EditAnywhere, Category="Mappings")
	TMap<FString, TEnumAsByte<EMaterialDomain>> MaterialDomainMappings;

	UPROPERTY(Config, EditAnywhere, Category="Paths", meta=(RelativeToGameDir))
	FDirectoryPath SourceDirectory;

	UPROPERTY(Config, EditAnywhere, Category="Paths", meta=(RelativeToGameDir))
	FDirectoryPath GeneratedShaderDirectory;

	UPROPERTY(Config, EditAnywhere, Category="Virtual Materials",
		meta=(DisplayName="Enable Virtual Material Mode",
			ToolTip="When enabled, DreamShader generates materials as transient in-memory assets at editor startup instead of saving .uasset files. DreamShader source files become the single asset source. Materials are automatically generated as persistent assets during cooking for packaging."))
	bool bVirtualMaterialMode = false;

	UPROPERTY(Config, EditAnywhere, Category="Virtual Materials",
		meta=(DisplayName="Show Virtual Materials In Content Browser",
			ToolTip="When enabled, memory-only DreamShader instance materials appear in the Content Browser like unsaved assets. Disabled by default: the source files are the intended authoring surface, and hiding the instances also prevents accidental Save actions from materializing them to disk."))
	bool bShowVirtualMaterialsInContentBrowser = false;

	UPROPERTY(Config, EditAnywhere, Category="Compiler",
		meta=(DisplayName="Default Backend",
			ToolTip="Backend used when a source file does not specify Settings = { Backend = \"...\" }. Graph builds a UMaterial node graph. Instance compiles the shading logic into a generated .ush and emits a lightweight material instance of the shared host material (no node graph); files that need graph-only features (UE.*/Substrate nodes, textures, graph functions) automatically fall back to Graph."))
	EDreamShaderDefaultBackend DefaultBackend = EDreamShaderDefaultBackend::Graph;

	UPROPERTY(Config, EditAnywhere, Category="Compiler")
	bool bAutoCompileOnSave = true;

	UPROPERTY(Config, EditAnywhere, Category="Compiler", meta=(ClampMin="0.05", ClampMax="10.0", UIMin="0.05", UIMax="2.0"))
	float SaveDebounceSeconds = 0.25f;

	UPROPERTY(Config, EditAnywhere, Category="Compiler")
	bool bVerboseLogs = false;

	UPROPERTY(Config, EditAnywhere, Category="Decompiler")
	bool bExportDecompiledLayout = true;
	
	UPROPERTY(Config, EditAnywhere, Category="Editor")
	bool bOpenInNewWindow = true;

private:
	static FString NormalizeShadingModelKey(const FString& InName);
	static FString NormalizeBlendModeKey(const FString& InName);
	static FString NormalizeMaterialDomainKey(const FString& InName);
};
