#include "DreamShaderMaterialGeneratorCodeShared.h"

#include "DreamShaderModule.h"
#include "DreamShaderSettings.h"
#include "DreamShaderVersionCompat.h"

#include "Misc/Crc.h"

#include "EdGraph/EdGraphNode.h"
#include "Interfaces/IPluginManager.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionScreenPosition.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialParameterCollection.h"
#include "Engine/Texture.h"
#include "Engine/Texture2DArray.h"
#include "Engine/TextureCube.h"
#include "Engine/VolumeTexture.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "ObjectTools.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::DreamShader::Editor::Private
{

	static bool TryResolveMaterialDomain(const FString& InValue, EMaterialDomain& OutDomain)
	{
		const UDreamShaderSettings* Settings = GetDefault<UDreamShaderSettings>();
		return Settings && Settings->TryResolveMaterialDomain(InValue, OutDomain);
	}

	static bool TryResolveBlendMode(const FString& InValue, EBlendMode& OutBlendMode)
	{
		const UDreamShaderSettings* Settings = GetDefault<UDreamShaderSettings>();
		return Settings && Settings->TryResolveBlendMode(InValue, OutBlendMode);
	}

	static bool TryResolveShadingModel(const FString& InValue, EMaterialShadingModel& OutShadingModel)
	{
		const UDreamShaderSettings* Settings = GetDefault<UDreamShaderSettings>();
		return Settings && Settings->TryResolveShadingModel(InValue, OutShadingModel);
	}

	bool TryGetUEBuiltinArgument(const FTextShaderPropertyDefinition& Property, const TCHAR* Key, FString& OutValue)
	{
		if (const FString* Value = Property.UEBuiltinArguments.Find(UE::DreamShader::NormalizeSettingKey(Key)))
		{
			OutValue = *Value;
			return true;
		}

		return false;
	}

	bool ValidateUEBuiltinArgumentNames(
		const FTextShaderPropertyDefinition& Property,
		TConstArrayView<const TCHAR*> AllowedArgumentNames,
		FString& OutError)
	{
		for (const TPair<FString, FString>& Argument : Property.UEBuiltinArguments)
		{
			bool bKnownArgument = false;
			for (const TCHAR* AllowedName : AllowedArgumentNames)
			{
				if (Argument.Key == UE::DreamShader::NormalizeSettingKey(AllowedName))
				{
					bKnownArgument = true;
					break;
				}
			}

			if (!bKnownArgument)
			{
				OutError = FString::Printf(
					TEXT("UE.%s for property '%s' does not support argument '%s'."),
					*Property.UEBuiltinFunctionName,
					*Property.Name,
					*Argument.Key);
				return false;
			}
		}

		return true;
	}

	bool TryResolvePositionOrigin(const FString& InValue, EPositionOrigin& OutValue)
	{
		FString Value = InValue;
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		Value.ReplaceInline(TEXT(" "), TEXT(""));

		if (Value == TEXT("absolute") || Value == TEXT("world"))
		{
			OutValue = EPositionOrigin::Absolute;
			return true;
		}

		if (Value == TEXT("camerarelative"))
		{
			OutValue = EPositionOrigin::CameraRelative;
			return true;
		}

		return false;
	}

	bool TryResolvePropertyReference(
		const FString& InReferenceName,
		const TMap<FString, UMaterialExpression*>& AvailableExpressions,
		UMaterialExpression*& OutExpression)
	{
		const FString ReferenceName = InReferenceName.TrimStartAndEnd();
		if (ReferenceName.IsEmpty())
		{
			return false;
		}

		if (UMaterialExpression* const* ExactMatch = AvailableExpressions.Find(ReferenceName))
		{
			OutExpression = *ExactMatch;
			return true;
		}

		for (const TPair<FString, UMaterialExpression*>& Pair : AvailableExpressions)
		{
			if (Pair.Key.Equals(ReferenceName, ESearchCase::IgnoreCase))
			{
				OutExpression = Pair.Value;
				return true;
			}
		}

		return false;
	}

	UMaterialExpression* CreateOwnedMaterialExpression(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		UClass* ExpressionClass,
		const int32 PositionX,
		const int32 PositionY)
	{
		return UMaterialEditingLibrary::CreateMaterialExpressionEx(Material, MaterialFunction, ExpressionClass, nullptr, PositionX, PositionY, false);
	}

	void EnsureExpressionCanBeDeleted(UMaterialExpression* Expression)
	{
		if (Expression && Expression->IsRooted())
		{
			Expression->RemoveFromRoot();
		}
	}

	static TConstArrayView<TObjectPtr<UMaterialExpressionComment>> GetMaterialEditorComments(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction)
	{
		if (Material)
		{
			return Material->GetEditorComments();
		}

		if (MaterialFunction)
		{
			return MaterialFunction->GetEditorComments();
		}

		return TConstArrayView<TObjectPtr<UMaterialExpressionComment>>();
	}

	void ClearDreamShaderGeneratedComments(UMaterial* Material, UMaterialFunction* MaterialFunction)
	{
		if (!Material && !MaterialFunction)
		{
			return;
		}

		TArray<UMaterialExpressionComment*> CommentsToRemove;
		for (const TObjectPtr<UMaterialExpressionComment>& Comment : GetMaterialEditorComments(Material, MaterialFunction))
		{
			if (Comment && Comment->Text.StartsWith(TEXT("DreamShader: "), ESearchCase::CaseSensitive))
			{
				CommentsToRemove.Add(Comment.Get());
			}
		}

		for (UMaterialExpressionComment* Comment : CommentsToRemove)
		{
			if (Material)
			{
				Material->GetExpressionCollection().RemoveComment(Comment);
			}
			else if (MaterialFunction)
			{
				MaterialFunction->GetExpressionCollection().RemoveComment(Comment);
			}
			EnsureExpressionCanBeDeleted(Comment);
			Comment->MarkAsGarbage();
		}
	}

	UMaterialExpression* CreateScalarLiteralExpressionEx(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const double Value,
		const int32 PositionY)
	{
		auto* Expression = Cast<UMaterialExpressionConstant>(
			CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionConstant::StaticClass(), -1120, PositionY));
		if (Expression)
		{
			Expression->R = static_cast<float>(Value);
		}
		return Expression;
	}

	UMaterialExpression* CreateScalarLiteralExpression(UMaterial* Material, const double Value, const int32 PositionY)
	{
		return CreateScalarLiteralExpressionEx(Material, nullptr, Value, PositionY);
	}

	UMaterialExpression* CreateVectorLiteralExpression(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const TArray<double>& Components,
		const int32 ExpectedComponentCount,
		const int32 PositionY)
	{
		if (ExpectedComponentCount == 2)
		{
			auto* Expression = Cast<UMaterialExpressionConstant2Vector>(
				CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionConstant2Vector::StaticClass(), -1120, PositionY));
			if (Expression)
			{
				Expression->R = static_cast<float>(Components[0]);
				Expression->G = static_cast<float>(Components[1]);
			}
			return Expression;
		}

		if (ExpectedComponentCount == 3)
		{
			auto* Expression = Cast<UMaterialExpressionConstant3Vector>(
				CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionConstant3Vector::StaticClass(), -1120, PositionY));
			if (Expression)
			{
				Expression->Constant = FLinearColor(
					static_cast<float>(Components[0]),
					static_cast<float>(Components[1]),
					static_cast<float>(Components[2]),
					1.0f);
			}
			return Expression;
		}

		auto* Expression = Cast<UMaterialExpressionConstant4Vector>(
			CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionConstant4Vector::StaticClass(), -1120, PositionY));
		if (Expression)
		{
			Expression->Constant = FLinearColor(
				static_cast<float>(Components[0]),
				static_cast<float>(Components[1]),
				static_cast<float>(Components[2]),
				static_cast<float>(Components[3]));
		}
		return Expression;
	}

	static UMaterialExpression* CreateLiteralExpression(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const FString& InValueText,
		const int32 ExpectedComponentCount,
		const int32 PositionY,
		FString& OutError)
	{
		if (ExpectedComponentCount == 1)
		{
			double ParsedValue = 0.0;
			if (!ParseScalarLiteral(InValueText, ParsedValue))
			{
				OutError = FString::Printf(TEXT("Expected a scalar literal but got '%s'."), *InValueText);
				return nullptr;
			}

			UMaterialExpression* Expression = CreateScalarLiteralExpressionEx(Material, MaterialFunction, ParsedValue, PositionY);
			if (!Expression)
			{
				OutError = TEXT("Failed to create a scalar constant expression.");
			}
			return Expression;
		}

		TArray<double> Components;
		if (!ParseVectorLiteral(InValueText, Components))
		{
			OutError = FString::Printf(TEXT("Expected a float%d-style literal like '(...)' but got '%s'."), ExpectedComponentCount, *InValueText);
			return nullptr;
		}

		if (Components.Num() != ExpectedComponentCount)
		{
			OutError = FString::Printf(
				TEXT("Expected %d components but got %d in literal '%s'."),
				ExpectedComponentCount,
				Components.Num(),
				*InValueText);
			return nullptr;
		}

		UMaterialExpression* Expression = CreateVectorLiteralExpression(Material, MaterialFunction, Components, ExpectedComponentCount, PositionY);
		if (!Expression)
		{
			OutError = FString::Printf(TEXT("Failed to create a float%d constant expression."), ExpectedComponentCount);
		}
		return Expression;
	}

	bool ResolveExpressionInputValue(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const FString& InValueText,
		const TMap<FString, UMaterialExpression*>& AvailableExpressions,
		const int32 ExpectedComponentCount,
		const int32 PositionY,
		UMaterialExpression*& OutExpression,
		FString& OutError)
	{
		if (TryResolvePropertyReference(InValueText, AvailableExpressions, OutExpression))
		{
			return true;
		}

		OutExpression = CreateLiteralExpression(Material, MaterialFunction, InValueText, ExpectedComponentCount, PositionY, OutError);
		if (OutExpression)
		{
			return true;
		}

		OutError = FString::Printf(
			TEXT("%s It must reference a previously declared property or use a compatible literal."),
			*OutError);
		return false;
	}

	struct FResolvedMaterialSettingTarget
	{
		UObject* OwnerObject = nullptr;
		void* ContainerPtr = nullptr;
		FProperty* Property = nullptr;
		int32 ArrayIndex = 0;
	};

	struct FParsedMaterialSettingSegment
	{
		FString Name;
		int32 ArrayIndex = 0;
		bool bHasArrayIndex = false;
	};

	static bool TryGetSettingValue(const FTextShaderDefinition& Definition, const TCHAR* Key, FString& OutValue)
	{
		return Definition.TryGetSetting(Key, OutValue);
	}

	static bool TryGetSettingValue(const FTextShaderDefinition& Definition, const TCHAR* KeyA, const TCHAR* KeyB, FString& OutValue)
	{
		return Definition.TryGetSetting(KeyA, OutValue) || Definition.TryGetSetting(KeyB, OutValue);
	}

	static bool ValidateBooleanSetting(const FTextShaderDefinition& Definition, const TCHAR* Key, FString& OutError)
	{
		if (FString Value; TryGetSettingValue(Definition, Key, Value))
		{
			bool bParsedValue = false;
			if (!ParseBooleanLiteral(Value, bParsedValue))
			{
				OutError = FString::Printf(TEXT("Invalid boolean value '%s' for %s."), *Value, Key);
				return false;
			}
		}

		return true;
	}

	static void ApplyBooleanSetting(UMaterial* Material, const FTextShaderDefinition& Definition, const TCHAR* Key, const TFunctionRef<void(bool)>& Setter)
	{
		if (FString Value; TryGetSettingValue(Definition, Key, Value))
		{
			bool bParsedValue = false;
			verify(ParseBooleanLiteral(Value, bParsedValue));
			Setter(bParsedValue);
		}
	}

	static FString NormalizeMaterialSettingLookupKey(const FString& InKey)
	{
		FString Normalized = UE::DreamShader::NormalizeSettingKey(InKey);
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		return Normalized;
	}

	static bool SplitMaterialSettingPath(const FString& InKey, TArray<FString>& OutSegments)
	{
		OutSegments.Reset();

		FString Current;
		int32 BracketDepth = 0;
		for (int32 Index = 0; Index < InKey.Len(); ++Index)
		{
			const TCHAR Character = InKey[Index];
			if (Character == TCHAR('['))
			{
				++BracketDepth;
			}
			else if (Character == TCHAR(']'))
			{
				BracketDepth = FMath::Max(0, BracketDepth - 1);
			}
			else if (Character == TCHAR('.') && BracketDepth == 0)
			{
				OutSegments.Add(Current.TrimStartAndEnd());
				Current.Reset();
				continue;
			}

			Current.AppendChar(Character);
		}

		if (!Current.IsEmpty())
		{
			OutSegments.Add(Current.TrimStartAndEnd());
		}

		return !OutSegments.IsEmpty();
	}

	static bool ParseMaterialSettingSegment(const FString& InSegmentText, FParsedMaterialSettingSegment& OutSegment, FString& OutError)
	{
		OutSegment = {};
		FString Segment = InSegmentText.TrimStartAndEnd();
		if (Segment.IsEmpty())
		{
			OutError = TEXT("Setting path segment cannot be empty.");
			return false;
		}

		const int32 OpenBracketIndex = Segment.Find(TEXT("["));
		if (OpenBracketIndex == INDEX_NONE)
		{
			OutSegment.Name = Segment;
			return true;
		}

		const int32 CloseBracketIndex = Segment.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (CloseBracketIndex == INDEX_NONE || CloseBracketIndex <= OpenBracketIndex || CloseBracketIndex != Segment.Len() - 1)
		{
			OutError = FString::Printf(TEXT("Invalid array setting segment '%s'."), *InSegmentText);
			return false;
		}

		const FString IndexText = Segment.Mid(OpenBracketIndex + 1, CloseBracketIndex - OpenBracketIndex - 1).TrimStartAndEnd();
		int32 ParsedIndex = INDEX_NONE;
		if (!ParseIntegerLiteral(IndexText, ParsedIndex) || ParsedIndex < 0)
		{
			OutError = FString::Printf(TEXT("Invalid array index '%s' in setting segment '%s'."), *IndexText, *InSegmentText);
			return false;
		}

		OutSegment.Name = Segment.Left(OpenBracketIndex).TrimStartAndEnd();
		OutSegment.ArrayIndex = ParsedIndex;
		OutSegment.bHasArrayIndex = true;
		if (OutSegment.Name.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Invalid array setting segment '%s'."), *InSegmentText);
			return false;
		}

		return true;
	}

	static bool IsSpecialMaterialSettingKey(const FString& InKey)
	{
		const FString Key = NormalizeMaterialSettingLookupKey(InKey);
		return Key == TEXT("blendmode")
			|| Key == TEXT("rendertype")
			|| Key == TEXT("shadingmodel")
			|| Key == TEXT("materialdomain")
			|| Key == TEXT("domain");
	}

	static const TMap<FString, FString>& GetMaterialSettingAliases()
	{
		static const TMap<FString, FString> Aliases = []()
		{
			TMap<FString, FString> Result;
			Result.Add(TEXT("lightingmode"), TEXT("TranslucencyLightingMode"));
			Result.Add(TEXT("translucentlightingmode"), TEXT("TranslucencyLightingMode"));
			Result.Add(TEXT("refractionmode"), TEXT("RefractionMethod"));
			Result.Add(TEXT("physicalmaterial"), TEXT("PhysMaterial"));
			Result.Add(TEXT("physicalmaterialmask"), TEXT("PhysMaterialMask"));
			Result.Add(TEXT("lightmass"), TEXT("LightmassSettings"));
			Result.Add(TEXT("mobileseparatetranslucency"), TEXT("bEnableMobileSeparateTranslucency"));
			Result.Add(TEXT("alwaysevaluateworldpositionoffset"), TEXT("bAlwaysEvaluateWorldPositionOffset"));
			Result.Add(TEXT("responsiveaa"), TEXT("bEnableResponsiveAA"));
			Result.Add(TEXT("thinsurface"), TEXT("bIsThinSurface"));
			return Result;
		}();
		return Aliases;
	}

	static bool TryResolveMaterialSettingPropertyOnStruct(const UStruct* InStruct, const FString& InKey, FProperty*& OutProperty)
	{
		FString LookupKey = NormalizeMaterialSettingLookupKey(InKey);
		if (const FString* Alias = GetMaterialSettingAliases().Find(LookupKey))
		{
			LookupKey = NormalizeMaterialSettingLookupKey(*Alias);
		}

		for (TFieldIterator<FProperty> It(InStruct, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}

			const FString PropertyName = Property->GetName();
			if (NormalizeMaterialSettingLookupKey(PropertyName) == LookupKey)
			{
				OutProperty = Property;
				return true;
			}

			if (PropertyName.Len() > 1
				&& PropertyName[0] == TCHAR('b')
				&& FChar::IsUpper(PropertyName[1])
				&& NormalizeMaterialSettingLookupKey(PropertyName.Mid(1)) == LookupKey)
			{
				OutProperty = Property;
				return true;
			}

			const FString DisplayName = Property->GetMetaData(TEXT("DisplayName"));
			if (!DisplayName.IsEmpty() && NormalizeMaterialSettingLookupKey(DisplayName) == LookupKey)
			{
				OutProperty = Property;
				return true;
			}
		}

		OutProperty = nullptr;
		return false;
	}

	static bool ResolveMaterialSettingTarget(UObject* RootObject, const FString& InKey, FResolvedMaterialSettingTarget& OutTarget, FString& OutError)
	{
		if (!RootObject)
		{
			OutError = TEXT("Invalid material setting target.");
			return false;
		}

		TArray<FString> Segments;
		if (!SplitMaterialSettingPath(InKey, Segments))
		{
			OutError = FString::Printf(TEXT("Invalid material setting path '%s'."), *InKey);
			return false;
		}

		void* CurrentContainer = RootObject;
		const UStruct* CurrentStruct = RootObject->GetClass();

		for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
		{
			FParsedMaterialSettingSegment Segment;
			if (!ParseMaterialSettingSegment(Segments[SegmentIndex], Segment, OutError))
			{
				return false;
			}

			FProperty* Property = nullptr;
			if (!TryResolveMaterialSettingPropertyOnStruct(CurrentStruct, Segment.Name, Property))
			{
				OutError = FString::Printf(TEXT("Unsupported material setting '%s'."), *InKey);
				return false;
			}

			if (Segment.bHasArrayIndex)
			{
				if (Property->ArrayDim <= 1)
				{
					OutError = FString::Printf(TEXT("Setting '%s' is not an indexed array property."), *Segments[SegmentIndex]);
					return false;
				}

				if (Segment.ArrayIndex >= Property->ArrayDim)
				{
					OutError = FString::Printf(
						TEXT("Array index %d is out of range for setting '%s' (max %d)."),
						Segment.ArrayIndex,
						*Segments[SegmentIndex],
						Property->ArrayDim - 1);
					return false;
				}
			}
			else if (Property->ArrayDim > 1)
			{
				OutError = FString::Printf(TEXT("Setting '%s' requires an explicit [index]."), *Segments[SegmentIndex]);
				return false;
			}

			const int32 ArrayIndex = Segment.bHasArrayIndex ? Segment.ArrayIndex : 0;
			if (SegmentIndex == Segments.Num() - 1)
			{
				OutTarget.OwnerObject = RootObject;
				OutTarget.ContainerPtr = CurrentContainer;
				OutTarget.Property = Property;
				OutTarget.ArrayIndex = ArrayIndex;
				return true;
			}

			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty || !StructProperty->Struct)
			{
				OutError = FString::Printf(TEXT("Setting path '%s' cannot continue through '%s'."), *InKey, *Segments[SegmentIndex]);
				return false;
			}

			CurrentContainer = StructProperty->ContainerPtrToValuePtr<void>(CurrentContainer, ArrayIndex);
			CurrentStruct = StructProperty->Struct;
		}

		OutError = FString::Printf(TEXT("Invalid material setting path '%s'."), *InKey);
		return false;
	}

	static bool ValidateGenericMaterialSetting(const FString& InKey, const FString& InValue, FString& OutError)
	{
		UMaterial* ProbeMaterial = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!ProbeMaterial)
		{
			OutError = TEXT("Failed to create a transient material for Settings validation.");
			return false;
		}

		FResolvedMaterialSettingTarget Target;
		if (!ResolveMaterialSettingTarget(ProbeMaterial, InKey, Target, OutError))
		{
			return false;
		}

		FString LiteralError;
		void* ValuePtr = Target.Property->ContainerPtrToValuePtr<void>(Target.ContainerPtr, Target.ArrayIndex);
		if (!SetMaterialExpressionLiteralProperty(Target.OwnerObject, Target.Property, ValuePtr, InValue, LiteralError))
		{
			OutError = FString::Printf(TEXT("Invalid value '%s' for setting '%s'. %s"), *InValue, *InKey, *LiteralError);
			return false;
		}

		return true;
	}

	static bool ApplyGenericMaterialSetting(UMaterial* Material, const FString& InKey, const FString& InValue, FString& OutError)
	{
		FResolvedMaterialSettingTarget Target;
		if (!ResolveMaterialSettingTarget(Material, InKey, Target, OutError))
		{
			return false;
		}

		FString LiteralError;
		void* ValuePtr = Target.Property->ContainerPtrToValuePtr<void>(Target.ContainerPtr, Target.ArrayIndex);
		if (!SetMaterialExpressionLiteralProperty(Target.OwnerObject, Target.Property, ValuePtr, InValue, LiteralError))
		{
			OutError = FString::Printf(TEXT("Invalid value '%s' for setting '%s'. %s"), *InValue, *InKey, *LiteralError);
			return false;
		}

		return true;
	}

	FCodeValue CreateOutputRerouteValue(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const FCodeValue& SourceValue,
		const FString& RouteName,
		const int32 RouteIndex)
	{
		FCodeValue RoutedValue = SourceValue;
		if (!SourceValue.Expression || (!Material && !MaterialFunction))
		{
			return RoutedValue;
		}

		const FString BaseName = RouteName.TrimStartAndEnd().IsEmpty()
			? TEXT("Output")
			: RouteName.TrimStartAndEnd();
		FString SanitizedName = UE::DreamShader::SanitizeIdentifier(BaseName);
		if (RouteIndex >= 0)
		{
			SanitizedName += FString::Printf(TEXT("_%d"), RouteIndex);
		}
		SanitizedName = FString::Printf(TEXT("DS_%s"), *SanitizedName);

		const int32 SourceX = SourceValue.Expression->MaterialExpressionEditorX;
		const int32 SourceY = SourceValue.Expression->MaterialExpressionEditorY;
		const int32 DeclarationX = SourceX + 420;
		const int32 DeclarationY = SourceY;
		const int32 UsageX = 720;
		const int32 UsageY = -120 + FMath::Max(RouteIndex, 0) * 180;

		auto* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(
			CreateOwnedMaterialExpression(
				Material,
				MaterialFunction,
				UMaterialExpressionNamedRerouteDeclaration::StaticClass(),
				DeclarationX,
				DeclarationY));
		auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(
			CreateOwnedMaterialExpression(
				Material,
				MaterialFunction,
				UMaterialExpressionNamedRerouteUsage::StaticClass(),
				UsageX,
				UsageY));
		if (!Declaration || !Usage)
		{
			return RoutedValue;
		}

		Declaration->Name = FName(*SanitizedName);
		if (!Declaration->VariableGuid.IsValid())
		{
			Declaration->VariableGuid = FGuid::NewGuid();
		}
		ConnectCodeValueToInput(Declaration->Input, SourceValue);

		Usage->Declaration = Declaration;
		Usage->DeclarationGuid = Declaration->VariableGuid;

		RoutedValue.Expression = Usage;
		RoutedValue.OutputIndex = 0;
		ClearCodeValueInputMask(RoutedValue);
		return RoutedValue;
	}

	void ResetMaterialToDefaults(UMaterial* Material)
	{
		check(Material);

		Material->BlendMode = BLEND_Opaque;
		Material->MaterialDomain = MD_Surface;
		Material->SetShadingModel(MSM_DefaultLit);
		Material->TwoSided = false;
		Material->OpacityMaskClipValue = 0.3333f;
		Material->Wireframe = false;
		Material->DitheredLODTransition = false;
		Material->DitherOpacityMask = false;
		Material->bAllowNegativeEmissiveColor = false;
		Material->bCastDynamicShadowAsMasked = false;
		Material->bCastRayTracedShadows = true;
		Material->bEnableResponsiveAA = false;
		Material->bScreenSpaceReflections = false;
		Material->bContactShadows = false;
		Material->bDisableDepthTest = false;
		Material->bOutputTranslucentVelocity = false;
		Material->bWriteOnlyAlpha = false;
		Material->BlendableOutputAlpha = false;
		Material->TranslucencyLightingMode = TLM_VolumetricNonDirectional;
		Material->bTangentSpaceNormal = true;
		Material->bAlwaysEvaluateWorldPositionOffset = false;
		Material->bFullyRough = false;
		Material->bIsSky = false;
		Material->bIsThinSurface = false;
		Material->MaterialDecalResponse = MDR_ColorNormalRoughness;
#if DREAMSHADER_UE_VERSION_AT_LEAST(5, 4)
		Material->bHasPixelAnimation = false;
#endif
		Material->NumCustomizedUVs = 0;
	}

	bool ValidateSettings(const FTextShaderDefinition& Definition, FString& OutError)
	{
		if (FString BlendModeValue; TryGetSettingValue(Definition, TEXT("BlendMode"), TEXT("RenderType"), BlendModeValue))
		{
			EBlendMode BlendMode = BLEND_MAX;
			if (!TryResolveBlendMode(BlendModeValue, BlendMode))
			{
				OutError = FString::Printf(TEXT("Unsupported BlendMode/RenderType '%s'."), *BlendModeValue);
				return false;
			}
		}

		if (FString ShadingModelValue; Definition.TryGetSetting(TEXT("ShadingModel"), ShadingModelValue))
		{
#if !DREAMSHADER_WITH_SUBSTRATE_BUILTINS
			const FString TrimmedShadingModelValue = ShadingModelValue.TrimStartAndEnd();
			if (TrimmedShadingModelValue.Equals(TEXT("Substrate"), ESearchCase::IgnoreCase)
				|| TrimmedShadingModelValue.Equals(TEXT("Strata"), ESearchCase::IgnoreCase))
			{
				OutError = TEXT("ShadingModel=\"Substrate\" requires Unreal Engine 5.4 or newer.");
				return false;
			}
#endif
			EMaterialShadingModel ShadingModel = MSM_MAX;
			if (!TryResolveShadingModel(ShadingModelValue, ShadingModel))
			{
				OutError = FString::Printf(TEXT("Unsupported ShadingModel '%s'."), *ShadingModelValue);
				return false;
			}
		}

		if (FString MaterialDomainValue; TryGetSettingValue(Definition, TEXT("MaterialDomain"), TEXT("Domain"), MaterialDomainValue))
		{
			EMaterialDomain Domain = MD_Surface;
			if (!TryResolveMaterialDomain(MaterialDomainValue, Domain))
			{
				OutError = FString::Printf(TEXT("Unsupported MaterialDomain '%s'."), *MaterialDomainValue);
				return false;
			}
		}

		for (const TPair<FString, FString>& Setting : Definition.Settings)
		{
			if (IsSpecialMaterialSettingKey(Setting.Key))
			{
				continue;
			}

			if (!ValidateGenericMaterialSetting(Setting.Key, Setting.Value, OutError))
			{
				return false;
			}
		}

		return true;
	}

	bool ApplySettings(UMaterial* Material, const FTextShaderDefinition& Definition, FString& OutError)
	{
		check(Material);

		if (!ValidateSettings(Definition, OutError))
		{
			return false;
		}

		if (FString BlendModeValue; TryGetSettingValue(Definition, TEXT("BlendMode"), TEXT("RenderType"), BlendModeValue))
		{
			EBlendMode BlendMode = BLEND_Opaque;
			verify(TryResolveBlendMode(BlendModeValue, BlendMode));
			Material->BlendMode = BlendMode;
		}

		if (FString ShadingModelValue; Definition.TryGetSetting(TEXT("ShadingModel"), ShadingModelValue))
		{
			EMaterialShadingModel ShadingModel = MSM_DefaultLit;
			verify(TryResolveShadingModel(ShadingModelValue, ShadingModel));
			Material->SetShadingModel(ShadingModel);
		}

		if (FString MaterialDomainValue; TryGetSettingValue(Definition, TEXT("MaterialDomain"), TEXT("Domain"), MaterialDomainValue))
		{
			EMaterialDomain Domain = MD_Surface;
			verify(TryResolveMaterialDomain(MaterialDomainValue, Domain));
			Material->MaterialDomain = Domain;
		}

		for (const TPair<FString, FString>& Setting : Definition.Settings)
		{
			if (IsSpecialMaterialSettingKey(Setting.Key))
			{
				continue;
			}

			if (!ApplyGenericMaterialSetting(Material, Setting.Key, Setting.Value, OutError))
			{
				return false;
			}
		}

		return true;
	}

}
