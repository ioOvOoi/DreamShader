#include "DreamShaderMaterialInstance.h"

#include "DreamShaderSettings.h"
#include "Hash/xxhash.h"
#include "MaterialShared.h"
#include "UObject/Package.h"

#if WITH_EDITOR
#include "MaterialCompiler.h"
#include "MaterialShared.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#endif

namespace UE::DreamShader::Private
{
	/**
	 * Static-permutation resource for UDreamShaderMaterialInstance. For the DSL-bound material
	 * properties it bypasses the host material's graph and compiles the instance's generated .ush
	 * (a translator-injected Custom expression plus synthesized named parameters); everything else
	 * falls through to the host material's defaults.
	 */
	class FDreamShaderInstanceResource final : public FMaterialResource
	{
	public:
		virtual void BuildShaderMapIdOverride(const FBuildShaderMapIdArgs& Args) const override
		{
			FMaterialResource::BuildShaderMapIdOverride(Args);
#if WITH_EDITOR
			const UDreamShaderMaterialInstance* Instance = Cast<UDreamShaderMaterialInstance>(MaterialInstance);
			if (Instance && Args.OutId && !Instance->SourceHash.IsEmpty())
			{
				// The generated .ush behaves like a dynamically included expression file, so fold the
				// source hash into ExpressionIncludesHash (assigned by the FMaterial base before this
				// point): distinct source revisions must map to distinct shader maps and DDC keys.
				const FXxHash64 SourceSalt = FXxHash64::HashBuffer(
					*Instance->SourceHash,
					Instance->SourceHash.Len() * sizeof(TCHAR));
				const uint64 Combined[2] = { Args.OutId->ExpressionIncludesHash, SourceSalt.Hash };
				Args.OutId->ExpressionIncludesHash = FXxHash64::HashBuffer(Combined, sizeof(Combined)).Hash;
			}
#endif
		}

#if WITH_EDITOR
		virtual int32 CompilePropertyAndSetMaterialProperty(
			EMaterialProperty Property,
			FMaterialCompiler* Compiler,
			EShaderFrequency OverrideShaderFrequency,
			bool bUsePreviousFrameTime) const override
		{
			const UDreamShaderMaterialInstance* Instance = Cast<UDreamShaderMaterialInstance>(MaterialInstance);
			int32 BoundOutputIndex = INDEX_NONE;
			if (Instance)
			{
				for (int32 Index = 0; Index < Instance->InstanceOutputs.Num(); ++Index)
				{
					if (Instance->InstanceOutputs[Index].Property == Property)
					{
						BoundOutputIndex = Index;
						break;
					}
				}
			}

			UMaterialExpressionCustom* EvalExpression = nullptr;
#if WITH_EDITORONLY_DATA
			if (Instance && BoundOutputIndex != INDEX_NONE && Instance->EvalExpressions.IsValidIndex(BoundOutputIndex))
			{
				EvalExpression = Instance->EvalExpressions[BoundOutputIndex].Get();
			}
#endif
			if (!EvalExpression)
			{
				return FMaterialResource::CompilePropertyAndSetMaterialProperty(
					Property, Compiler, OverrideShaderFrequency, bUsePreviousFrameTime);
			}

			if (Compiler->ShouldStopTranslating())
			{
				return INDEX_NONE;
			}

			// Contract of this entry point: SetMaterialProperty must run first (sets the shader
			// frequency), and the result must be cast to the property's attribute type.
			Compiler->SetMaterialProperty(Property, OverrideShaderFrequency, bUsePreviousFrameTime);

			TArray<int32> CompiledInputs;
			CompiledInputs.Reserve(Instance->InstanceParameters.Num() + Instance->UsedTexCoordCount + 1);
			for (const FDreamShaderInstanceParameter& Parameter : Instance->InstanceParameters)
			{
				CompiledInputs.Add(Parameter.Type == EDreamShaderInstanceParameterType::Scalar
					? Compiler->ScalarParameter(Parameter.Name, Parameter.ScalarDefault)
					: Compiler->VectorParameter(Parameter.Name, Parameter.VectorDefault));
			}

			// Dummy side-effect inputs (values unused by the eval code): compiling the chunk is what
			// allocates TexCoords interpolator slots / sets bUsesVertexColor, without which the
			// Parameters reads inside the generated .ush are dead. Mirrors the engine's own
			// SceneTexture-in-Custom protocol of compiling a pin purely for its registration effect.
			for (int32 CoordinateIndex = 0; CoordinateIndex < Instance->UsedTexCoordCount; ++CoordinateIndex)
			{
				CompiledInputs.Add(Compiler->TextureCoordinate(CoordinateIndex, false, false));
			}
			if (Instance->bUsesVertexColorBuiltin)
			{
				CompiledInputs.Add(Compiler->VertexColor());
			}

			const int32 Result = Compiler->CustomExpression(EvalExpression, 0, CompiledInputs);
			if (Result == INDEX_NONE)
			{
				return Result;
			}

			return Compiler->ForceCast(Result, FMaterialAttributeDefinitionMap::GetValueType(Property));
		}
#endif // WITH_EDITOR
	};
}

FMaterialResource* UDreamShaderMaterialInstance::AllocatePermutationResource()
{
	return new UE::DreamShader::Private::FDreamShaderInstanceResource();
}

bool UDreamShaderMaterialInstance::HasOverridenBaseProperties() const
{
	// Force a static permutation (an own shader map) whenever parented — the entire point of this
	// class. InitStaticPermutation recomputes bHasStaticPermutationResource from this on every load,
	// so it must not depend on transient state (mirrors ULandscapeMaterialInstanceConstant).
	if (Parent)
	{
		return true;
	}
	return Super::HasOverridenBaseProperties();
}

bool UDreamShaderMaterialInstance::IsAsset() const
{
	// Memory-only virtual instances hide from asset enumeration — the Content Browser and the
	// asset registry discover in-memory assets by iterating live objects and asking IsAsset()
	// (AssetRegistry.cpp object-iterator path), so returning false here removes them from the
	// browser and from save pickers (which also prevents an accidental explicit Save from
	// materializing them). The source file is the authoring surface; references still resolve
	// through the object path. Persisted instances behave like normal assets.
	if (GetPackage()->HasAnyPackageFlags(PKG_NewlyCreated)
		&& !GetDefault<UDreamShaderSettings>()->bShowVirtualMaterialsInContentBrowser)
	{
		return false;
	}
	return Super::IsAsset();
}
