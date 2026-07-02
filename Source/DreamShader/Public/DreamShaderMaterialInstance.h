#pragma once

#include "CoreMinimal.h"
#include "MaterialCachedData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "SceneTypes.h"
#include "DreamShaderMaterialInstance.generated.h"

class UTexture;

UENUM()
enum class EDreamShaderInstanceParameterType : uint8
{
	Scalar,
	Vector,
	Texture,
};

/** A material parameter synthesized during translation (no graph node) for a DreamShader instance material. */
USTRUCT()
struct FDreamShaderInstanceParameter
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	FName Name;

	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	EDreamShaderInstanceParameterType Type = EDreamShaderInstanceParameterType::Scalar;

	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	float ScalarDefault = 0.0f;

	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	FLinearColor VectorDefault = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);

	/** Default texture asset for Texture parameters (translation needs a non-null default). */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	TObjectPtr<UTexture> TextureDefault;

	/** Parameter group shown in the material instance editor (from the DSL metadata). */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	FName Group;

	/** Sort priority within the group (engine default is 32). */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	int32 SortPriority = 32;

	/** Tooltip description shown in the material instance editor. */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	FString Description;
};

/** A translator-computed value (a scene-buffer read) fed into the eval functions as a named argument. */
UENUM()
enum class EDreamShaderSceneReadKind : uint8
{
	SceneDepth,   // Compiler->SceneDepth  -> float
	SceneColor,   // Compiler->SceneColor  -> float4 (translucent Surface only)
	SceneTexture, // Compiler->SceneTextureLookup(SceneTextureId) -> float4 (PostProcess domain)
};

/**
 * A scene read the generated .ush references by name. Unlike the pure-read builtins (which inline a
 * Parameters read), a scene read's value cannot be reconstructed in HLSL: the resource must compile
 * the matching FMaterialCompiler call (which also registers the scene-texture usage / copy pass) and
 * pass the result into the eval function as a typed argument. Instance-level and de-duplicated by
 * (Kind, SceneTextureId): every UE.SceneColor()/SceneDepth()/SceneTexture(id) maps to one argument.
 */
USTRUCT()
struct FDreamShaderSceneRead
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	EDreamShaderSceneReadKind Kind = EDreamShaderSceneReadKind::SceneDepth;

	/** ESceneTextureId for Kind==SceneTexture; ignored otherwise. */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	int32 SceneTextureId = 0;

	/** Eval-function argument name the lowered HLSL references (e.g. DreamShaderSceneColor). */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	FName ArgName;
};

/** Binds one material property to an eval function inside the instance's generated .ush. */
USTRUCT()
struct FDreamShaderInstanceOutput
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	TEnumAsByte<EMaterialProperty> Property = MP_EmissiveColor;

	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	FString EvalFunctionName;

	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	TEnumAsByte<ECustomMaterialOutputType> OutputType = CMOT_Float3;
};

/**
 * DreamShader "Instance backend" material: the shading logic lives entirely in a generated .ush and
 * the asset is a lightweight constant instance of the shared host material. The instance owns its own
 * static-permutation shader map, compiled from HLSL the resource injects during translation, so no
 * per-material UMaterial graph asset exists. Mirrors the ULandscapeMaterialInstanceConstant pattern
 * (MIC subclass + AllocatePermutationResource + forced static permutation).
 */
UCLASS(ClassGroup = DreamShader)
class DREAMSHADER_API UDreamShaderMaterialInstance : public UMaterialInstanceConstant
{
	GENERATED_BODY()

public:
	/** DreamShader source file this instance was generated from. */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	FString SourceFilePath;

	/**
	 * Hash of the source text. Salted into the shader map id: the generated include is injected by the
	 * resource during translation, so it is invisible to the material's own include hashing and two
	 * revisions of the source would otherwise dedupe onto one shader map / DDC entry.
	 */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	FString SourceHash;

	/** Virtual shader path of the generated .ush holding this instance's eval functions. */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	FString GeneratedIncludeVirtualPath;

	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	TArray<FDreamShaderInstanceParameter> InstanceParameters;

	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	TArray<FDreamShaderInstanceOutput> InstanceOutputs;

	/**
	 * Texture coordinate slots the generated .ush reads (max used index + 1). The resource compiles
	 * a dummy TextureCoordinate chunk per slot during translation — reading Parameters.TexCoords
	 * only works when the translator allocated the interpolator slots.
	 */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	int32 UsedTexCoordCount = 0;

	/** The generated .ush reads Parameters.VertexColor; a dummy chunk sets bUsesVertexColor. */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	bool bUsesVertexColorBuiltin = false;

	/**
	 * Scene reads the eval functions consume as named arguments (instance-level, order = argument
	 * order in every eval signature). The resource compiles one FMaterialCompiler scene call per
	 * entry, in this order, after the parameter and dummy side-effect chunks.
	 */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	TArray<FDreamShaderSceneRead> SceneReads;

	/**
	 * Compile-time default-texture index space for this instance. The translator resolves texture
	 * parameters through FMaterial::GetMatDefaultTextureIdx, which normally reads the base
	 * material's cached expression data — empty for the graphless host — so the resource overrides
	 * those virtuals to serve this array instead. Order is deterministic (texture parameters in
	 * declaration order): it is hashed into the shader map id, invalidating shaders when defaults change.
	 */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	TArray<TObjectPtr<UObject>> InstanceDefaultTextures;

#if WITH_EDITORONLY_DATA
	/** One Custom expression per InstanceOutputs entry (index-aligned); consumed as a value bag during translation. */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialExpressionCustom>> EvalExpressions;
#endif

	/**
	 * Rebuilds SynthesizedCachedData from InstanceParameters. Must run on the game thread after the
	 * generator rewrites InstanceParameters (and on PostLoad) — inheritance chains hold a raw pointer
	 * into it, so it is rebuilt in place, never mutated while a chain may be live.
	 */
	void RebuildSynthesizedParameterData();

	//~ Begin UMaterialInstance interface
	virtual FMaterialResource* AllocatePermutationResource() override;
	virtual bool HasOverridenBaseProperties() const override;
	virtual void GetMaterialInheritanceChain(FMaterialInheritanceChain& OutChain) const override;
	virtual const FMaterialCachedExpressionData& GetCachedExpressionData(TMicRecursionGuard RecursionGuard = TMicRecursionGuard()) const override;
	//~ End UMaterialInstance interface

	//~ Begin UObject interface
	virtual void PostLoad() override;
	virtual bool IsAsset() const override;
	//~ End UObject interface

private:
	/**
	 * Parameter enumeration data for the synthesized (translation-time) parameters. The material
	 * instance editor and every GetAllParametersOfType/GetParameterDefaultValue query resolve
	 * against the inheritance chain's cached expression data, which is normally supplied by the
	 * base material's expression graph — empty for the graphless host — so this instance supplies
	 * its own: a copy of the host's data (preserving its analysis flags) with the synthesized
	 * parameters added. Not serialized; rebuilt from InstanceParameters on load. Every UObject it
	 * references is also referenced by the InstanceParameters/InstanceDefaultTextures UPROPERTYs,
	 * which keep those objects alive for GC.
	 */
	TUniquePtr<FMaterialCachedExpressionData> SynthesizedCachedData;
};
