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
	/**
	 * Convergence path (experimental): build the whole surface as one Custom node on a hidden base
	 * UMaterial and emit a lightweight material instance of it. Reuses the Graph construction, so it
	 * has the full feature surface, while keeping the instance's in-memory hiding and shader-map
	 * sharing. Opt-in via Settings = { Backend = "ThinCustom" } while it reaches Instance parity.
	 */
	ThinCustom,
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

	// The single compiler knob. DreamShader always generates materials in memory in the editor
	// (source files are the authoring surface; the editor never writes per-material .uasset files) and
	// materializes them as persistent assets during cooking — so there is no in-memory on/off toggle.
	UPROPERTY(Config, EditAnywhere, Category="Compiler",
		meta=(DisplayName="Default Compiler Backend",
			ToolTip="How DreamShader materializes a source file that does not specify Settings = { Backend = \"...\" }. Instance compiles the shading logic into a generated .ush and emits a lightweight, memory-only material instance (no per-material node graph). Graph builds a full UMaterial node graph. Files that need graph-only features (Substrate, static switches, graph/virtual functions) automatically fall back to Graph."))
	EDreamShaderDefaultBackend DefaultBackend = EDreamShaderDefaultBackend::Instance;

	UPROPERTY(Config, EditAnywhere, Category="Compiler",
		meta=(DisplayName="Show In-Memory Materials In Content Browser",
			ToolTip="When enabled, the memory-only DreamShader materials appear in the Content Browser like unsaved assets. Disabled by default: the source files are the intended authoring surface, and hiding the materials also prevents accidental Save actions from materializing them to disk."))
	bool bShowInMemoryMaterialsInContentBrowser = false;

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
