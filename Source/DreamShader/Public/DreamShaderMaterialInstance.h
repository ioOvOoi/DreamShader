#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialInstanceConstant.h"
#include "DreamShaderMaterialInstance.generated.h"

/**
 * The DreamShader-generated material asset: a lightweight constant instance of a hidden per-material
 * base UMaterial that carries the real node graph (the ThinCustom chain). The instance is the
 * addressable asset -- it hides from the Content Browser while memory-only (IsAsset) and owns its own
 * static-permutation shader map as the ROOT of its inheritance chain (HasOverridenBaseProperties),
 * so parameter/color child instances off it share one compiled map. Everything else -- parameters,
 * settings, domains, scene reads -- lives on the base as ordinary nodes and properties, enumerated
 * natively by the engine.
 */
UCLASS(ClassGroup = DreamShader)
class DREAMSHADER_API UDreamShaderMaterialInstance : public UMaterialInstanceConstant
{
	GENERATED_BODY()

public:
	/** DreamShader source file this instance was generated from. */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	FString SourceFilePath;

	/** Hash of the source text (the regeneration skip check compares against it). */
	UPROPERTY(VisibleAnywhere, Category = "DreamShader")
	FString SourceHash;

	//~ Begin UMaterialInstance interface
	virtual bool HasOverridenBaseProperties() const override;
	//~ End UMaterialInstance interface

	//~ Begin UObject interface
	virtual bool IsAsset() const override;
	//~ End UObject interface
};
