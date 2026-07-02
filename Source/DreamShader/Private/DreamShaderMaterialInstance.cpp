#include "DreamShaderMaterialInstance.h"

#include "DreamShaderSettings.h"
#include "Engine/Texture.h"
#include "Hash/xxhash.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "UObject/Package.h"

#if WITH_EDITOR
#include "MaterialCompiler.h"
#include "MaterialShared.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "ObjectCacheEventSink.h"
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
		// The translator resolves texture parameters through the material's compile-time
		// default-texture index space (GetMatDefaultTextureIdx), which is built from the base
		// material's expression graph — empty for the graphless host. Serve the instance's own
		// texture array instead; its order is deterministic and hashes into the shader map id.
		virtual TObjectPtr<UObject> GetMatDefaultTextureAtIdx(const int32 Index) const override
		{
			const UDreamShaderMaterialInstance* Instance = Cast<UDreamShaderMaterialInstance>(MaterialInstance);
			if (Instance && Instance->InstanceDefaultTextures.IsValidIndex(Index))
			{
				return Instance->InstanceDefaultTextures[Index];
			}
			return FMaterialResource::GetMatDefaultTextureAtIdx(Index);
		}

#if WITH_EDITOR
		virtual int32 GetMatDefaultTextureIdx(TObjectPtr<UObject> InTexture) const override
		{
			const UDreamShaderMaterialInstance* Instance = Cast<UDreamShaderMaterialInstance>(MaterialInstance);
			if (Instance)
			{
				const int32 Index = Instance->InstanceDefaultTextures.Find(InTexture);
				if (Index != INDEX_NONE)
				{
					return Index;
				}
			}
			return FMaterialResource::GetMatDefaultTextureIdx(InTexture);
		}

		virtual const TArray<TObjectPtr<UObject>>& GetMatDefaultTextures() const override
		{
			const UDreamShaderMaterialInstance* Instance = Cast<UDreamShaderMaterialInstance>(MaterialInstance);
			if (Instance && !Instance->InstanceDefaultTextures.IsEmpty())
			{
				return Instance->InstanceDefaultTextures;
			}
			return FMaterialResource::GetMatDefaultTextures();
		}
#endif

		// The Gate-B visibility route flips bEnableNewHLSLGenerator on the host material so the
		// material instance editor treats every enumerated parameter as visible
		// (FMaterialEditorUtilities::GetVisibleMaterialParameters new-generator branch). Translation
		// and shader map ids must stay on the legacy translator regardless.
		virtual bool IsUsingNewHLSLGenerator() const override
		{
			return false;
		}

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
		// Mirrors the engine's sampler-type derivation for reflected texture nodes: the sampler
		// metadata decides sRGB decode and normal-map unpack expectations at the sampling site.
		static EMaterialSamplerType GetSamplerTypeForTexture(const UTexture* Texture)
		{
			if (Texture)
			{
				switch (Texture->CompressionSettings)
				{
				case TC_Normalmap: return SAMPLERTYPE_Normal;
				case TC_Grayscale: return Texture->SRGB ? SAMPLERTYPE_Grayscale : SAMPLERTYPE_LinearGrayscale;
				case TC_Masks:     return SAMPLERTYPE_Masks;
				case TC_Alpha:     return SAMPLERTYPE_Alpha;
				default:           break;
				}
				return Texture->SRGB ? SAMPLERTYPE_Color : SAMPLERTYPE_LinearColor;
			}
			return SAMPLERTYPE_Color;
		}

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
				switch (Parameter.Type)
				{
				case EDreamShaderInstanceParameterType::Scalar:
					CompiledInputs.Add(Compiler->ScalarParameter(Parameter.Name, Parameter.ScalarDefault));
					break;
				case EDreamShaderInstanceParameterType::Vector:
					CompiledInputs.Add(Compiler->VectorParameter(Parameter.Name, Parameter.VectorDefault));
					break;
				case EDreamShaderInstanceParameterType::Texture:
				{
					// The uniform chunk becomes a `Texture2D X, SamplerState XSampler` pair on the
					// emitted custom function; the MIC's texture parameter value drives it by name at
					// runtime, with GetMatDefaultTextureAtIdx as the fallback.
					int32 TextureReferenceIndex = INDEX_NONE;
					CompiledInputs.Add(Compiler->TextureParameter(
						Parameter.Name,
						Parameter.TextureDefault,
						TextureReferenceIndex,
						GetSamplerTypeForTexture(Parameter.TextureDefault)));
					break;
				}
				default:
					checkNoEntry();
					break;
				}
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

void UDreamShaderMaterialInstance::RebuildSynthesizedParameterData()
{
	if (InstanceParameters.IsEmpty())
	{
		SynthesizedCachedData.Reset();
		return;
	}

	// Start from a copy of the host material's cached expression data so every analysis flag the
	// engine reads off the chain (quality levels, used coordinates, layers, ...) keeps the value it
	// had before this override existed; only the parameter entries are added on top.
	TUniquePtr<FMaterialCachedExpressionData> Data = MakeUnique<FMaterialCachedExpressionData>();
	if (const UMaterial* HostMaterial = GetMaterial())
	{
		*Data = HostMaterial->GetCachedExpressionData();
	}

#if WITH_EDITORONLY_DATA
	// Deep-copy: sharing the host's editor-only block would let AddParameter mutate the host.
	Data->EditorOnlyData = Data->EditorOnlyData.IsValid()
		? MakeShared<FMaterialCachedExpressionEditorOnlyData>(*Data->EditorOnlyData)
		: MakeShared<FMaterialCachedExpressionEditorOnlyData>();
#endif

	// FMaterialCachedExpressionData::AddParameter is not exported, so the entries are filled
	// directly, mirroring exactly what GetParameterValueByIndex reads per type
	// (MaterialCachedData.cpp): the per-type ParameterInfoSet index addresses the runtime value
	// arrays and — when the per-type EditorInfo array is non-empty — the editor-only arrays, so
	// every array a type's reader touches must grow in lockstep with the set.
#if WITH_EDITORONLY_DATA
	const auto AddEditorInfo = [&Data](EMaterialParameterType Type, const FDreamShaderInstanceParameter& Parameter)
	{
		// ExpressionGuid stays zero: it only drives rename tracking against a source expression
		// graph, which these parameters do not have.
		Data->EditorOnlyData->EditorEntries[static_cast<int32>(Type)].EditorInfo.Add(
			FMaterialCachedParameterEditorInfo(FGuid(), Parameter.Description, Parameter.Group, Parameter.SortPriority));
	};
#endif

	for (const FDreamShaderInstanceParameter& Parameter : InstanceParameters)
	{
		const FMaterialParameterInfo ParameterInfo(Parameter.Name);
		switch (Parameter.Type)
		{
		case EDreamShaderInstanceParameterType::Scalar:
		{
			FMaterialCachedParameterEntry& Entry = Data->RuntimeEntries[static_cast<int32>(EMaterialParameterType::Scalar)];
			if (Entry.ParameterInfoSet.Contains(ParameterInfo))
			{
				break;
			}
			Entry.ParameterInfoSet.Add(ParameterInfo);
			Data->ScalarValues.Add(Parameter.ScalarDefault);
			Data->ScalarPrimitiveDataIndexValues.Add(INDEX_NONE);
#if WITH_EDITORONLY_DATA
			Data->EditorOnlyData->ScalarMinMaxValues.AddDefaulted();
			Data->EditorOnlyData->ScalarEnumerationValues.AddDefaulted();
			Data->EditorOnlyData->ScalarEnumerationIndexValues.Add(INDEX_NONE);
			Data->EditorOnlyData->ScalarCurveValues.AddDefaulted();
			Data->EditorOnlyData->ScalarCurveAtlasValues.AddDefaulted();
			AddEditorInfo(EMaterialParameterType::Scalar, Parameter);
#endif
			break;
		}

		case EDreamShaderInstanceParameterType::Vector:
		{
			FMaterialCachedParameterEntry& Entry = Data->RuntimeEntries[static_cast<int32>(EMaterialParameterType::Vector)];
			if (Entry.ParameterInfoSet.Contains(ParameterInfo))
			{
				break;
			}
			Entry.ParameterInfoSet.Add(ParameterInfo);
			Data->VectorValues.Add(Parameter.VectorDefault);
			Data->VectorPrimitiveDataIndexValues.Add(INDEX_NONE);
#if WITH_EDITORONLY_DATA
			Data->EditorOnlyData->VectorChannelNameValues.AddDefaulted();
			Data->EditorOnlyData->VectorUsedAsChannelMaskValues.Add(false);
			AddEditorInfo(EMaterialParameterType::Vector, Parameter);
#endif
			break;
		}

		case EDreamShaderInstanceParameterType::Texture:
		{
			FMaterialCachedParameterEntry& Entry = Data->RuntimeEntries[static_cast<int32>(EMaterialParameterType::Texture)];
			if (Entry.ParameterInfoSet.Contains(ParameterInfo))
			{
				break;
			}
			Entry.ParameterInfoSet.Add(ParameterInfo);
			Data->TextureValues.Add(Parameter.TextureDefault.Get());
#if WITH_EDITORONLY_DATA
			Data->EditorOnlyData->TextureChannelNameValues.AddDefaulted();
			AddEditorInfo(EMaterialParameterType::Texture, Parameter);
			if (Parameter.TextureDefault)
			{
				Data->EditorOnlyData->EditorReferencedDefaultTextures.AddUnique(Parameter.TextureDefault.Get());
			}
#endif
			break;
		}

		default:
			checkNoEntry();
			break;
		}
	}

	SynthesizedCachedData = MoveTemp(Data);

#if WITH_EDITOR
	FObjectCacheEventSink::NotifyReferencedTextureChanged_Concurrent(this);
#endif
}

void UDreamShaderMaterialInstance::GetMaterialInheritanceChain(FMaterialInheritanceChain& OutChain) const
{
	// Claim the chain's cached expression data slot before the base walk reaches the graphless host
	// material (first non-null wins). A plain child MIC recurses into its Parent virtually, so this
	// serves children too. Instances that override material layers build their own data and claim
	// the slot before us — layers are unsupported on children of DreamShader materials.
	if (!OutChain.CachedExpressionData && SynthesizedCachedData.IsValid())
	{
		OutChain.CachedExpressionData = SynthesizedCachedData.Get();
	}
	Super::GetMaterialInheritanceChain(OutChain);
}

const FMaterialCachedExpressionData& UDreamShaderMaterialInstance::GetCachedExpressionData(TMicRecursionGuard RecursionGuard) const
{
	if (SynthesizedCachedData.IsValid())
	{
		return *SynthesizedCachedData;
	}
	return Super::GetCachedExpressionData(RecursionGuard);
}

void UDreamShaderMaterialInstance::PostLoad()
{
	Super::PostLoad();
	if (Parent)
	{
		// The rebuild copies the host's cached expression data; make sure it exists first.
		Parent->ConditionalPostLoad();
	}
	RebuildSynthesizedParameterData();
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
