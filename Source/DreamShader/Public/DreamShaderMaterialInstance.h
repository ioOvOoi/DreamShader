#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "SceneTypes.h"
#include "DreamShaderMaterialInstance.generated.h"

UENUM()
enum class EDreamShaderInstanceParameterType : uint8
{
	Scalar,
	Vector,
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

#if WITH_EDITORONLY_DATA
	/** One Custom expression per InstanceOutputs entry (index-aligned); consumed as a value bag during translation. */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialExpressionCustom>> EvalExpressions;
#endif

	//~ Begin UMaterialInstance interface
	virtual FMaterialResource* AllocatePermutationResource() override;
	virtual bool HasOverridenBaseProperties() const override;
	//~ End UMaterialInstance interface
};
