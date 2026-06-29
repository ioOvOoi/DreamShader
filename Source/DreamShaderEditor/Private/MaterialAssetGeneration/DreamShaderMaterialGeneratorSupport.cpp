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
	namespace
	{
		constexpr int32 FastClearExpressionThreshold = 1200;
		constexpr int32 FastLayoutExpressionThreshold = 1200;
	}

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

	static bool TryGetUEBuiltinArgument(const FTextShaderPropertyDefinition& Property, const TCHAR* Key, FString& OutValue)
	{
		if (const FString* Value = Property.UEBuiltinArguments.Find(UE::DreamShader::NormalizeSettingKey(Key)))
		{
			OutValue = *Value;
			return true;
		}

		return false;
	}

	static bool ValidateUEBuiltinArgumentNames(
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

	static bool TryResolvePositionOrigin(const FString& InValue, EPositionOrigin& OutValue)
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

	static bool TryResolvePropertyReference(
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

	static UMaterialExpression* CreateOwnedMaterialExpression(
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

	static UMaterialExpression* CreateScalarLiteralExpressionEx(
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

	static UMaterialExpression* CreateVectorLiteralExpression(
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

	static bool ResolveExpressionInputValue(
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

	UClass* ResolveMaterialExpressionClass(const FString& ClassSpecifier)
	{
		FString Candidate = ClassSpecifier.TrimStartAndEnd();
		if (Candidate.IsEmpty())
		{
			return nullptr;
		}

		if (Candidate.Contains(TEXT("/")) || Candidate.Contains(TEXT(".")))
		{
			if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *Candidate))
			{
				if (LoadedClass->IsChildOf(UMaterialExpression::StaticClass()))
				{
					return LoadedClass;
				}
			}
		}

		TArray<FString> CandidateNames;
		CandidateNames.Add(Candidate);
		if (!Candidate.StartsWith(TEXT("U")))
		{
			CandidateNames.Add(TEXT("U") + Candidate);
		}
		if (!Candidate.StartsWith(TEXT("MaterialExpression")))
		{
			CandidateNames.Add(TEXT("MaterialExpression") + Candidate);
		}
		if (!Candidate.StartsWith(TEXT("UMaterialExpression")))
		{
			CandidateNames.Add(TEXT("UMaterialExpression") + Candidate);
		}

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class || !Class->IsChildOf(UMaterialExpression::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}

			for (const FString& NameOption : CandidateNames)
			{
				if (Class->GetName().Equals(NameOption, ESearchCase::IgnoreCase))
				{
					return Class;
				}
			}
		}

		return nullptr;
	}

	FProperty* FindMaterialExpressionArgumentProperty(UClass* ExpressionClass, const FString& ArgumentName)
	{
		if (!ExpressionClass)
		{
			return nullptr;
		}

		const FString NormalizedArgument = UE::DreamShader::NormalizeSettingKey(ArgumentName);
		for (TFieldIterator<FProperty> It(ExpressionClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (Property && UE::DreamShader::NormalizeSettingKey(Property->GetName()) == NormalizedArgument)
			{
				return Property;
			}
		}

		for (TFieldIterator<FProperty> It(ExpressionClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!CastField<FBoolProperty>(Property))
			{
				continue;
			}

			FString NormalizedPropertyName = UE::DreamShader::NormalizeSettingKey(Property->GetName());
			if (NormalizedPropertyName.StartsWith(TEXT("b")))
			{
				NormalizedPropertyName.RightChopInline(1, DREAMSHADER_ALLOW_SHRINKING_NO);
				if (NormalizedPropertyName == NormalizedArgument)
				{
					return Property;
				}
			}
		}

		return nullptr;
	}

	bool IsMaterialExpressionInputProperty(const FProperty* Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		return StructProperty
			&& StructProperty->Struct
			&& (StructProperty->Struct->GetFName() == NAME_ExpressionInput
				|| StructProperty->Struct->GetName().Equals(TEXT("MaterialAttributesInput"), ESearchCase::IgnoreCase));
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

	bool SetMaterialExpressionLiteralProperty(UObject* Target, FProperty* Property, void* ValuePtr, const FString& ValueText, FString& OutError)
	{
		if (!Target || !Property || !ValuePtr)
		{
			OutError = TEXT("Invalid reflected property target.");
			return false;
		}

		const FString TrimmedValue = ValueText.TrimStartAndEnd();
		FString AssetObjectPath;
		const bool bHasParsedAssetReference =
			CastField<FObjectPropertyBase>(Property) != nullptr
			&& TryResolveDreamShaderAssetReference(TrimmedValue, AssetObjectPath, OutError);

		if (CastField<FObjectPropertyBase>(Property) != nullptr
			&& !bHasParsedAssetReference
			&& (TrimmedValue.StartsWith(TEXT("Path("), ESearchCase::IgnoreCase)
				|| TrimmedValue.StartsWith(TEXT("/"))))
		{
			return false;
		}

		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			bool bValue = false;
			if (!ParseBooleanLiteral(TrimmedValue, bValue))
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid boolean value for '%s'."), *TrimmedValue, *Property->GetName());
				return false;
			}
			BoolProperty->SetPropertyValue(ValuePtr, bValue);
			return true;
		}

		if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
		{
			int32 ParsedValue = 0;
			if (!ParseIntegerLiteral(TrimmedValue, ParsedValue))
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid integer value for '%s'."), *TrimmedValue, *Property->GetName());
				return false;
			}
			IntProperty->SetPropertyValue(ValuePtr, ParsedValue);
			return true;
		}

		if (FUInt32Property* UIntProperty = CastField<FUInt32Property>(Property))
		{
			uint32 ParsedValue = 0;
			if (!ParseUnsignedInteger32Literal(TrimmedValue, ParsedValue))
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid unsigned integer value for '%s'."), *TrimmedValue, *Property->GetName());
				return false;
			}
			UIntProperty->SetPropertyValue(ValuePtr, ParsedValue);
			return true;
		}

		if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
		{
			double ParsedValue = 0.0;
			if (!ParseScalarLiteral(TrimmedValue, ParsedValue))
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid numeric value for '%s'."), *TrimmedValue, *Property->GetName());
				return false;
			}
			FloatProperty->SetPropertyValue(ValuePtr, static_cast<float>(ParsedValue));
			return true;
		}

		if (FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
		{
			double ParsedValue = 0.0;
			if (!ParseScalarLiteral(TrimmedValue, ParsedValue))
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid numeric value for '%s'."), *TrimmedValue, *Property->GetName());
				return false;
			}
			DoubleProperty->SetPropertyValue(ValuePtr, ParsedValue);
			return true;
		}

		if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			StringProperty->SetPropertyValue(ValuePtr, TrimmedValue);
			return true;
		}

		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			NameProperty->SetPropertyValue(ValuePtr, FName(*TrimmedValue));
			return true;
		}

		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (!bHasParsedAssetReference)
			{
				OutError = FString::Printf(TEXT("Object property '%s' expects Path(...) or an absolute Unreal object path."), *Property->GetName());
				return false;
			}

			UObject* LoadedObject = StaticLoadObject(ObjectProperty->PropertyClass, nullptr, *AssetObjectPath);
			if (!LoadedObject)
			{
				if (ObjectProperty->PropertyClass
					&& ObjectProperty->PropertyClass->IsChildOf(UTexture::StaticClass())
					&& (Property->GetName().Equals(TEXT("Texture"), ESearchCase::IgnoreCase)
						|| Property->GetName().Equals(TEXT("TextureObject"), ESearchCase::IgnoreCase)))
				{
					ObjectProperty->SetObjectPropertyValue(ValuePtr, nullptr);
					return true;
				}

				OutError = FString::Printf(TEXT("Failed to load asset '%s' for '%s'."), *AssetObjectPath, *Property->GetName());
				return false;
			}

			if (!LoadedObject->IsA(ObjectProperty->PropertyClass))
			{
				OutError = FString::Printf(
					TEXT("Asset '%s' is not compatible with '%s'. Expected '%s'."),
					*AssetObjectPath,
					*Property->GetName(),
					*ObjectProperty->PropertyClass->GetName());
				return false;
			}

			ObjectProperty->SetObjectPropertyValue(ValuePtr, LoadedObject);
			return true;
		}

		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			if (UEnum* Enum = EnumProperty->GetEnum())
			{
				int64 EnumValue = INDEX_NONE;
				if (!TryResolveEnumLiteral(Enum, TrimmedValue, EnumValue))
				{
					OutError = FString::Printf(TEXT("'%s' is not a valid enum value for '%s'."), *TrimmedValue, *Property->GetName());
					return false;
				}

				EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumValue);
				return true;
			}
		}

		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (UEnum* Enum = ByteProperty->Enum)
			{
				int64 EnumValue = INDEX_NONE;
				if (!TryResolveEnumLiteral(Enum, TrimmedValue, EnumValue))
				{
					OutError = FString::Printf(TEXT("'%s' is not a valid enum value for '%s'."), *TrimmedValue, *Property->GetName());
					return false;
				}

				ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumValue));
				return true;
			}

			int32 ParsedValue = 0;
			if (!ParseIntegerLiteral(TrimmedValue, ParsedValue) || ParsedValue < 0 || ParsedValue > MAX_uint8)
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid byte value for '%s'."), *TrimmedValue, *Property->GetName());
				return false;
			}

			ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(ParsedValue));
			return true;
		}

		FOutputDeviceNull ImportErrors;
		const FString ImportValue = bHasParsedAssetReference ? AssetObjectPath : TrimmedValue;
		if (Property->ImportText_Direct(*ImportValue, ValuePtr, Target, PPF_None, &ImportErrors) != nullptr)
		{
			return true;
		}

		OutError = FString::Printf(TEXT("Property '%s' on '%s' is not a supported literal type yet."), *Property->GetName(), *Target->GetClass()->GetName());
		return false;
	}

	bool SetMaterialExpressionLiteralProperty(UObject* Target, FProperty* Property, const FString& ValueText, FString& OutError)
	{
		if (!Target || !Property)
		{
			OutError = TEXT("Invalid reflected property target.");
			return false;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);
		return SetMaterialExpressionLiteralProperty(Target, Property, ValuePtr, ValueText, OutError);
	}

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

	namespace
	{
		static void CollectMaterialExpressions(UMaterial* Material, UMaterialFunction* MaterialFunction, TArray<UMaterialExpression*>& OutExpressions)
		{
			OutExpressions.Reset();
			if (Material)
			{
				OutExpressions.Reserve(Material->GetExpressions().Num());
				for (const TObjectPtr<UMaterialExpression>& Expression : Material->GetExpressions())
				{
					if (Expression)
					{
						OutExpressions.Add(Expression.Get());
					}
				}
				return;
			}

			if (MaterialFunction)
			{
				OutExpressions.Reserve(MaterialFunction->GetExpressions().Num());
				for (const TObjectPtr<UMaterialExpression>& Expression : MaterialFunction->GetExpressions())
				{
					if (Expression)
					{
						OutExpressions.Add(Expression.Get());
					}
				}
			}
		}

		static bool TryAddUniqueExpression(TArray<UMaterialExpression*>& Expressions, UMaterialExpression* Expression)
		{
			if (!Expression || Expressions.Contains(Expression))
			{
				return false;
			}

			Expressions.Add(Expression);
			return true;
		}

		static UMaterialExpression* GetDirectInputExpression(const FExpressionInput& Input)
		{
			if (Input.Expression)
			{
				return Input.Expression;
			}

			const FExpressionInput TracedInput = Input.GetTracedInput();
			return TracedInput.Expression;
		}

		static void SetGeneratedExpressionPosition(UMaterialExpression* Expression, const int32 PositionX, const int32 PositionY)
		{
			if (!Expression)
			{
				return;
			}

			Expression->MaterialExpressionEditorX = PositionX;
			Expression->MaterialExpressionEditorY = PositionY;
			if (Expression->GraphNode)
			{
				Expression->GraphNode->NodePosX = PositionX;
				Expression->GraphNode->NodePosY = PositionY;
			}
		}

		static void BuildExpressionDependencyMaps(
			const TArray<UMaterialExpression*>& Expressions,
			const TSet<UMaterialExpression*>& ExpressionSet,
			TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& OutDependencies,
			TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& OutConsumers)
		{
			OutDependencies.Reset();
			OutConsumers.Reset();

			for (UMaterialExpression* Expression : Expressions)
			{
				if (!Expression)
				{
					continue;
				}

				for (int32 InputIndex = 0; InputIndex < GetDreamShaderExpressionInputCount(Expression); ++InputIndex)
				{
					FExpressionInput* Input = Expression->GetInput(InputIndex);
					if (!Input)
					{
						continue;
					}

					UMaterialExpression* SourceExpression = GetDirectInputExpression(*Input);
					if (!SourceExpression || SourceExpression == Expression || !ExpressionSet.Contains(SourceExpression))
					{
						continue;
					}

					TryAddUniqueExpression(OutDependencies.FindOrAdd(Expression), SourceExpression);
					TryAddUniqueExpression(OutConsumers.FindOrAdd(SourceExpression), Expression);
				}

				if (UMaterialExpressionNamedRerouteUsage* NamedRerouteUsage = Cast<UMaterialExpressionNamedRerouteUsage>(Expression))
				{
					if (UMaterialExpressionNamedRerouteDeclaration* Declaration = NamedRerouteUsage->Declaration)
					{
						if (ExpressionSet.Contains(Declaration))
						{
							TryAddUniqueExpression(OutDependencies.FindOrAdd(Expression), Declaration);
							TryAddUniqueExpression(OutConsumers.FindOrAdd(Declaration), Expression);
						}
					}
				}
			}
		}
	}

	namespace
	{
		struct FLayoutBounds
		{
			int32 MinX = MAX_int32;
			int32 MinY = MAX_int32;
			int32 MaxX = MIN_int32;
			int32 MaxY = MIN_int32;

			bool IsValid() const
			{
				return MinX <= MaxX && MinY <= MaxY;
			}

			void IncludeNode(const int32 PositionX, const int32 PositionY)
			{
				constexpr int32 NodeWidth = 320;
				constexpr int32 NodeHeight = 150;
				MinX = FMath::Min(MinX, PositionX);
				MinY = FMath::Min(MinY, PositionY);
				MaxX = FMath::Max(MaxX, PositionX + NodeWidth);
				MaxY = FMath::Max(MaxY, PositionY + NodeHeight);
			}
		};

		struct FGeneratedLayoutBlock
		{
			FString Title;
			TSet<UMaterialExpression*> ExpressionSet;
			int32 SortKey = 0;
		};

		static void ClearExpressionInputMask(FExpressionInput& Input)
		{
			Input.Mask = 0;
			Input.MaskR = 0;
			Input.MaskG = 0;
			Input.MaskB = 0;
			Input.MaskA = 0;
		}

		static FString MakeLayoutBridgeKey(const UMaterialExpression* SourceExpression, const int32 SourceOutputIndex)
		{
			return FString::Printf(
				TEXT("%llu:%d"),
				static_cast<unsigned long long>(reinterpret_cast<UPTRINT>(SourceExpression)),
				SourceOutputIndex);
		}

		static FString MakeLayoutBridgeUsageKey(
			const UMaterialExpressionNamedRerouteDeclaration* Declaration,
			const int32 ConsumerBlockIndex)
		{
			return FString::Printf(
				TEXT("%llu:%d"),
				static_cast<unsigned long long>(reinterpret_cast<UPTRINT>(Declaration)),
				ConsumerBlockIndex);
		}

		static bool IsDistantLayoutConnection(
			const UMaterialExpression* SourceExpression,
			const UMaterialExpression* ConsumerExpression)
		{
			if (!SourceExpression || !ConsumerExpression)
			{
				return false;
			}

			constexpr int32 MinBridgeDistanceX = 900;
			constexpr int32 MinBridgeDistanceY = 540;
			const int32 DeltaX = FMath::Abs(SourceExpression->MaterialExpressionEditorX - ConsumerExpression->MaterialExpressionEditorX);
			const int32 DeltaY = FMath::Abs(SourceExpression->MaterialExpressionEditorY - ConsumerExpression->MaterialExpressionEditorY);
			return DeltaX >= MinBridgeDistanceX || DeltaY >= MinBridgeDistanceY;
		}

		static void ConnectInputToExpressionPreservingMask(
			FExpressionInput& Input,
			UMaterialExpression* Expression,
			const int32 OutputIndex)
		{
			const int32 SavedMask = Input.Mask;
			const int32 SavedMaskR = Input.MaskR;
			const int32 SavedMaskG = Input.MaskG;
			const int32 SavedMaskB = Input.MaskB;
			const int32 SavedMaskA = Input.MaskA;

			Input.Connect(OutputIndex, Expression);
			Input.Mask = SavedMask;
			Input.MaskR = SavedMaskR;
			Input.MaskG = SavedMaskG;
			Input.MaskB = SavedMaskB;
			Input.MaskA = SavedMaskA;
		}

		static FString GetMaterialPropertyLayoutName(const EMaterialProperty Property)
		{
			switch (Property)
			{
			case MP_BaseColor:
				return TEXT("BaseColor");
			case MP_MaterialAttributes:
				return TEXT("MaterialAttributes");
			case MP_EmissiveColor:
				return TEXT("EmissiveColor");
			case MP_Opacity:
				return TEXT("Opacity");
			case MP_OpacityMask:
				return TEXT("OpacityMask");
			case MP_Metallic:
				return TEXT("Metallic");
			case MP_Specular:
				return TEXT("Specular");
			case MP_Roughness:
				return TEXT("Roughness");
			case MP_Normal:
				return TEXT("Normal");
			case MP_AmbientOcclusion:
				return TEXT("AmbientOcclusion");
			case MP_Refraction:
				return TEXT("Refraction");
			case MP_WorldPositionOffset:
				return TEXT("WorldPositionOffset");
			case MP_PixelDepthOffset:
				return TEXT("PixelDepthOffset");
			case MP_SubsurfaceColor:
				return TEXT("SubsurfaceColor");
			case MP_CustomData0:
				return TEXT("CustomData0");
			case MP_CustomData1:
				return TEXT("CustomData1");
			case MP_DiffuseColor:
				return TEXT("DiffuseColor");
			case MP_SpecularColor:
				return TEXT("SpecularColor");
			case MP_SurfaceThickness:
				return TEXT("SurfaceThickness");
			case MP_Displacement:
				return TEXT("Displacement");
			case MP_CustomizedUVs0:
				return TEXT("CustomizedUV0");
			case MP_CustomizedUVs1:
				return TEXT("CustomizedUV1");
			case MP_CustomizedUVs2:
				return TEXT("CustomizedUV2");
			case MP_CustomizedUVs3:
				return TEXT("CustomizedUV3");
			case MP_CustomizedUVs4:
				return TEXT("CustomizedUV4");
			case MP_CustomizedUVs5:
				return TEXT("CustomizedUV5");
			case MP_CustomizedUVs6:
				return TEXT("CustomizedUV6");
			case MP_CustomizedUVs7:
				return TEXT("CustomizedUV7");
#ifdef MOON_ENGINE
			case MP_MooaEncodedAttribute0:
				return TEXT("MooaEncodedAttribute0");
			case MP_MooaEncodedAttribute1:
				return TEXT("MooaEncodedAttribute1");
			case MP_MooaEncodedAttribute2:
				return TEXT("MooaEncodedAttribute2");
			case MP_MooaEncodedAttribute3:
				return TEXT("MooaEncodedAttribute3");
			case MP_MooaEncodedAttribute4:
				return TEXT("MooaEncodedAttribute4");
#endif
			case MP_Anisotropy:
				return TEXT("Anisotropy");
			case MP_Tangent:
				return TEXT("Tangent");
			default:
				return FString::Printf(TEXT("MaterialProperty%d"), static_cast<int32>(Property));
			}
		}

		static void CollectDependencySubgraph(
			UMaterialExpression* Expression,
			const TSet<UMaterialExpression*>& ValidExpressions,
			const TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& Dependencies,
			TSet<UMaterialExpression*>& OutExpressions)
		{
			if (!Expression || !ValidExpressions.Contains(Expression) || OutExpressions.Contains(Expression))
			{
				return;
			}

			OutExpressions.Add(Expression);
			if (const TArray<UMaterialExpression*>* ExpressionDependencies = Dependencies.Find(Expression))
			{
				for (UMaterialExpression* Dependency : *ExpressionDependencies)
				{
					CollectDependencySubgraph(Dependency, ValidExpressions, Dependencies, OutExpressions);
				}
			}
		}

		static void AddLayoutBlock(
			TArray<FGeneratedLayoutBlock>& Blocks,
			TSet<UMaterialExpression*>& OutputSinkExpressions,
			const FString& Title,
			UMaterialExpression* SinkExpression,
			const int32 SortKey,
			const TSet<UMaterialExpression*>& ValidExpressions,
			const TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& Dependencies)
		{
			if (!SinkExpression || !ValidExpressions.Contains(SinkExpression) || OutputSinkExpressions.Contains(SinkExpression))
			{
				return;
			}

			FGeneratedLayoutBlock& Block = Blocks.AddDefaulted_GetRef();
			Block.Title = Title;
			Block.SortKey = SortKey;
			CollectDependencySubgraph(SinkExpression, ValidExpressions, Dependencies, Block.ExpressionSet);
			OutputSinkExpressions.Add(SinkExpression);
		}

		static void AddExpressionToOwnedBlock(
			TArray<FGeneratedLayoutBlock>& Blocks,
			TMap<UMaterialExpression*, int32>& OwnerBlockByExpression,
			UMaterialExpression* Expression,
			const int32 OwnerBlockIndex)
		{
			if (!Expression || !Blocks.IsValidIndex(OwnerBlockIndex))
			{
				return;
			}

			OwnerBlockByExpression.Add(Expression, OwnerBlockIndex);
			Blocks[OwnerBlockIndex].ExpressionSet.Add(Expression);
		}

		static int32 ChooseOwnerBlockByDirectConsumers(
			UMaterialExpression* Expression,
			const TArray<int32>& CandidateBlocks,
			const TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& Consumers,
			const TMap<UMaterialExpression*, int32>& OwnerBlockByExpression)
		{
			if (!Expression || CandidateBlocks.IsEmpty())
			{
				return INDEX_NONE;
			}

			int32 BestBlockIndex = INDEX_NONE;
			int32 BestScore = MIN_int32;
			if (const TArray<UMaterialExpression*>* ExpressionConsumers = Consumers.Find(Expression))
			{
				for (const int32 CandidateBlockIndex : CandidateBlocks)
				{
					int32 Score = 0;
					for (UMaterialExpression* Consumer : *ExpressionConsumers)
					{
						if (const int32* ConsumerBlockIndex = OwnerBlockByExpression.Find(Consumer))
						{
							if (*ConsumerBlockIndex == CandidateBlockIndex)
							{
								++Score;
							}
						}
					}

					if (Score > BestScore)
					{
						BestScore = Score;
						BestBlockIndex = CandidateBlockIndex;
					}
				}
			}

			return BestScore > 0 ? BestBlockIndex : INDEX_NONE;
		}

		static void AssignLayoutBlockOwners(
			const TArray<UMaterialExpression*>& Expressions,
			const TArray<FGeneratedLayoutBlock>& Blocks,
			const TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& Consumers,
			TMap<UMaterialExpression*, int32>& OwnerBlockByExpression)
		{
			OwnerBlockByExpression.Reset();

			TMap<UMaterialExpression*, TArray<int32>> CandidateBlocksByExpression;
			for (int32 BlockIndex = 0; BlockIndex < Blocks.Num(); ++BlockIndex)
			{
				for (UMaterialExpression* Expression : Blocks[BlockIndex].ExpressionSet)
				{
					if (Expression)
					{
						CandidateBlocksByExpression.FindOrAdd(Expression).AddUnique(BlockIndex);
					}
				}
			}

			for (const TPair<UMaterialExpression*, TArray<int32>>& Pair : CandidateBlocksByExpression)
			{
				if (Pair.Value.Num() == 1)
				{
					OwnerBlockByExpression.Add(Pair.Key, Pair.Value[0]);
				}
			}

			bool bChanged = true;
			for (int32 Iteration = 0; bChanged && Iteration < 16; ++Iteration)
			{
				bChanged = false;
				for (UMaterialExpression* Expression : Expressions)
				{
					const TArray<int32>* CandidateBlocks = CandidateBlocksByExpression.Find(Expression);
					if (!CandidateBlocks || CandidateBlocks->IsEmpty() || OwnerBlockByExpression.Contains(Expression))
					{
						continue;
					}

					const int32 OwnerBlockIndex = ChooseOwnerBlockByDirectConsumers(
						Expression,
						*CandidateBlocks,
						Consumers,
						OwnerBlockByExpression);
					if (OwnerBlockIndex != INDEX_NONE)
					{
						OwnerBlockByExpression.Add(Expression, OwnerBlockIndex);
						bChanged = true;
					}
				}
			}

			for (UMaterialExpression* Expression : Expressions)
			{
				if (!Expression || OwnerBlockByExpression.Contains(Expression))
				{
					continue;
				}

				if (const TArray<int32>* CandidateBlocks = CandidateBlocksByExpression.Find(Expression))
				{
					if (!CandidateBlocks->IsEmpty())
					{
						OwnerBlockByExpression.Add(Expression, (*CandidateBlocks)[0]);
					}
				}
			}

			for (UMaterialExpression* Expression : Expressions)
			{
				UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Expression);
				if (!Declaration)
				{
					continue;
				}

				UMaterialExpression* SourceExpression = GetDirectInputExpression(Declaration->Input);
				if (!SourceExpression)
				{
					continue;
				}

				if (const int32* SourceBlockIndex = OwnerBlockByExpression.Find(SourceExpression))
				{
					OwnerBlockByExpression.Add(Declaration, *SourceBlockIndex);
				}
			}
		}

		static UMaterialExpressionNamedRerouteDeclaration* FindOrCreateLayoutBridgeDeclaration(
			UMaterial* Material,
			UMaterialFunction* MaterialFunction,
			UMaterialExpression* SourceExpression,
			const int32 SourceOutputIndex,
			const int32 BridgeIndex,
			TMap<FString, UMaterialExpressionNamedRerouteDeclaration*>& DeclarationsBySourceKey,
			TArray<UMaterialExpression*>& Expressions,
			TSet<UMaterialExpression*>& ExpressionSet,
			TMap<UMaterialExpression*, int32>& OwnerBlockByExpression,
			TArray<FGeneratedLayoutBlock>& Blocks,
			const int32 SourceBlockIndex)
		{
			if (!SourceExpression || !Blocks.IsValidIndex(SourceBlockIndex) || (!Material && !MaterialFunction))
			{
				return nullptr;
			}

			const FString SourceKey = MakeLayoutBridgeKey(SourceExpression, SourceOutputIndex);
			if (UMaterialExpressionNamedRerouteDeclaration* const* ExistingDeclaration = DeclarationsBySourceKey.Find(SourceKey))
			{
				return *ExistingDeclaration;
			}

			auto* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(
				CreateOwnedMaterialExpression(
					Material,
					MaterialFunction,
					UMaterialExpressionNamedRerouteDeclaration::StaticClass(),
					SourceExpression->MaterialExpressionEditorX + 360,
					SourceExpression->MaterialExpressionEditorY));
			if (!Declaration)
			{
				return nullptr;
			}

			Declaration->Name = FName(*FString::Printf(TEXT("DS_Shared_%d"), BridgeIndex));
			if (!Declaration->VariableGuid.IsValid())
			{
				Declaration->VariableGuid = FGuid::NewGuid();
			}
			Declaration->Input.Connect(SourceOutputIndex, SourceExpression);
			ClearExpressionInputMask(Declaration->Input);

			DeclarationsBySourceKey.Add(SourceKey, Declaration);
			Expressions.Add(Declaration);
			ExpressionSet.Add(Declaration);
			AddExpressionToOwnedBlock(Blocks, OwnerBlockByExpression, Declaration, SourceBlockIndex);
			return Declaration;
		}

		static UMaterialExpressionNamedRerouteUsage* FindOrCreateLayoutBridgeUsage(
			UMaterial* Material,
			UMaterialFunction* MaterialFunction,
			UMaterialExpressionNamedRerouteDeclaration* Declaration,
			const int32 ConsumerBlockIndex,
			TMap<FString, UMaterialExpressionNamedRerouteUsage*>& UsagesByDeclarationAndBlock,
			TMap<int32, int32>& UsageSlotByBlock,
			TArray<UMaterialExpression*>& Expressions,
			TSet<UMaterialExpression*>& ExpressionSet,
			TMap<UMaterialExpression*, int32>& OwnerBlockByExpression,
			TArray<FGeneratedLayoutBlock>& Blocks,
			const UMaterialExpression* ConsumerExpression)
		{
			if (!Declaration || !Blocks.IsValidIndex(ConsumerBlockIndex) || (!Material && !MaterialFunction))
			{
				return nullptr;
			}

			const FString UsageKey = MakeLayoutBridgeUsageKey(Declaration, ConsumerBlockIndex);
			if (UMaterialExpressionNamedRerouteUsage* const* ExistingUsage = UsagesByDeclarationAndBlock.Find(UsageKey))
			{
				return *ExistingUsage;
			}

			const int32 SlotIndex = UsageSlotByBlock.FindOrAdd(ConsumerBlockIndex)++;
			const int32 SlotStep = ((SlotIndex + 1) / 2) * 80;
			const int32 SlotOffsetY = SlotIndex == 0
				? 0
				: ((SlotIndex % 2) == 0 ? -SlotStep : SlotStep);
			const int32 UsageX = ConsumerExpression
				? ConsumerExpression->MaterialExpressionEditorX - 360
				: Declaration->MaterialExpressionEditorX;
			const int32 UsageY = ConsumerExpression
				? ConsumerExpression->MaterialExpressionEditorY + SlotOffsetY
				: Declaration->MaterialExpressionEditorY + SlotOffsetY;

			auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(
				CreateOwnedMaterialExpression(
					Material,
					MaterialFunction,
					UMaterialExpressionNamedRerouteUsage::StaticClass(),
					UsageX,
					UsageY));
			if (!Usage)
			{
				return nullptr;
			}

			Usage->Declaration = Declaration;
			Usage->DeclarationGuid = Declaration->VariableGuid;

			UsagesByDeclarationAndBlock.Add(UsageKey, Usage);
			Expressions.Add(Usage);
			ExpressionSet.Add(Usage);
			AddExpressionToOwnedBlock(Blocks, OwnerBlockByExpression, Usage, ConsumerBlockIndex);
			return Usage;
		}

		static void InsertCrossBlockReroutes(
			UMaterial* Material,
			UMaterialFunction* MaterialFunction,
			TArray<UMaterialExpression*>& Expressions,
			TSet<UMaterialExpression*>& ExpressionSet,
			TArray<FGeneratedLayoutBlock>& Blocks,
			TMap<UMaterialExpression*, int32>& OwnerBlockByExpression,
			TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& Dependencies,
			TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& Consumers,
			const bool bOnlyDistantConnections)
		{
			TMap<FString, UMaterialExpressionNamedRerouteDeclaration*> DeclarationsBySourceKey;
			TMap<FString, UMaterialExpressionNamedRerouteUsage*> UsagesByDeclarationAndBlock;
			TMap<int32, int32> UsageSlotByBlock;
			int32 BridgeIndex = 0;

			TArray<UMaterialExpression*> ConsumerSnapshot = Expressions;
			for (UMaterialExpression* ConsumerExpression : ConsumerSnapshot)
			{
				if (!ConsumerExpression)
				{
					continue;
				}

				const int32* ConsumerBlockIndex = OwnerBlockByExpression.Find(ConsumerExpression);
				if (!ConsumerBlockIndex)
				{
					continue;
				}

				for (int32 InputIndex = 0; InputIndex < GetDreamShaderExpressionInputCount(ConsumerExpression); ++InputIndex)
				{
					FExpressionInput* Input = ConsumerExpression->GetInput(InputIndex);
					if (!Input || !Input->Expression)
					{
						continue;
					}

					UMaterialExpression* SourceExpression = Input->Expression;
					const int32* SourceBlockIndex = OwnerBlockByExpression.Find(SourceExpression);
					if (!SourceBlockIndex || *SourceBlockIndex == *ConsumerBlockIndex)
					{
						continue;
					}

					if (Cast<UMaterialExpressionNamedRerouteUsage>(SourceExpression))
					{
						continue;
					}

					if (bOnlyDistantConnections && !IsDistantLayoutConnection(SourceExpression, ConsumerExpression))
					{
						continue;
					}

					UMaterialExpressionNamedRerouteDeclaration* Declaration = FindOrCreateLayoutBridgeDeclaration(
						Material,
						MaterialFunction,
						SourceExpression,
						Input->OutputIndex,
						BridgeIndex++,
						DeclarationsBySourceKey,
						Expressions,
						ExpressionSet,
						OwnerBlockByExpression,
						Blocks,
						*SourceBlockIndex);
					UMaterialExpressionNamedRerouteUsage* Usage = FindOrCreateLayoutBridgeUsage(
						Material,
						MaterialFunction,
						Declaration,
						*ConsumerBlockIndex,
						UsagesByDeclarationAndBlock,
						UsageSlotByBlock,
						Expressions,
						ExpressionSet,
						OwnerBlockByExpression,
						Blocks,
						ConsumerExpression);
					if (!Usage)
					{
						continue;
					}

					ConnectInputToExpressionPreservingMask(*Input, Usage, 0);
				}
			}

			if (BridgeIndex > 0)
			{
				BuildExpressionDependencyMaps(Expressions, ExpressionSet, Dependencies, Consumers);
			}
		}

		static void PositionMaterialRootNearOutputs(
			UMaterial* Material,
			const TArray<FLayoutBounds>& BlockBounds)
		{
			if (!Material)
			{
				return;
			}

			FLayoutBounds CombinedBounds;
			for (const FLayoutBounds& Bounds : BlockBounds)
			{
				if (!Bounds.IsValid())
				{
					continue;
				}

				CombinedBounds.MinX = FMath::Min(CombinedBounds.MinX, Bounds.MinX);
				CombinedBounds.MinY = FMath::Min(CombinedBounds.MinY, Bounds.MinY);
				CombinedBounds.MaxX = FMath::Max(CombinedBounds.MaxX, Bounds.MaxX);
				CombinedBounds.MaxY = FMath::Max(CombinedBounds.MaxY, Bounds.MaxY);
			}

			if (!CombinedBounds.IsValid())
			{
				return;
			}

			constexpr int32 RootGapX = 520;
			const int32 RootX = CombinedBounds.MaxX + RootGapX;
			const int32 RootY = (CombinedBounds.MinY + CombinedBounds.MaxY) / 2 - 240;
			Material->EditorX = RootX;
			Material->EditorY = RootY;

			if (Material->MaterialGraph && Material->MaterialGraph->RootNode)
			{
				Material->MaterialGraph->RootNode->NodePosX = RootX;
				Material->MaterialGraph->RootNode->NodePosY = RootY;
			}
		}

		static void PositionMaterialRootNearConnectedOutputs(UMaterial* Material)
		{
			if (!Material)
			{
				return;
			}

			FLayoutBounds OutputBounds;
			for (int32 MaterialPropertyIndex = 0; MaterialPropertyIndex < MP_MAX; ++MaterialPropertyIndex)
			{
				const EMaterialProperty MaterialProperty = static_cast<EMaterialProperty>(MaterialPropertyIndex);
				FExpressionInput* MaterialInput = Material->GetExpressionInputForProperty(MaterialProperty);
				if (!MaterialInput || !MaterialInput->IsConnected())
				{
					continue;
				}

				if (UMaterialExpression* OutputExpression = GetDirectInputExpression(*MaterialInput))
				{
					OutputBounds.IncludeNode(
						OutputExpression->MaterialExpressionEditorX,
						OutputExpression->MaterialExpressionEditorY);
				}
			}

			if (OutputBounds.IsValid())
			{
				TArray<FLayoutBounds> Bounds;
				Bounds.Add(OutputBounds);
				PositionMaterialRootNearOutputs(Material, Bounds);
			}
		}

		static void CreateDreamShaderLayoutComment(
			UMaterial* Material,
			UMaterialFunction* MaterialFunction,
			const FString& Title,
			const FLayoutBounds& Bounds)
		{
			if (!Bounds.IsValid() || (!Material && !MaterialFunction))
			{
				return;
			}

			UObject* Outer = Material ? static_cast<UObject*>(Material) : static_cast<UObject*>(MaterialFunction);
			UMaterialExpressionComment* Comment = NewObject<UMaterialExpressionComment>(Outer, NAME_None, RF_Transactional);
			if (!Comment)
			{
				return;
			}

			constexpr int32 PaddingX = 110;
			constexpr int32 PaddingY = 90;
			Comment->Text = FString::Printf(TEXT("DreamShader: %s"), *Title);
			Comment->MaterialExpressionEditorX = Bounds.MinX - PaddingX;
			Comment->MaterialExpressionEditorY = Bounds.MinY - PaddingY;
			Comment->SizeX = FMath::Max(420, Bounds.MaxX - Bounds.MinX + PaddingX * 2);
			Comment->SizeY = FMath::Max(240, Bounds.MaxY - Bounds.MinY + PaddingY * 2);
			Comment->FontSize = 24;
			Comment->CommentColor = FLinearColor(0.10f, 0.16f, 0.22f, 0.35f);
			Comment->bCommentBubbleVisible_InDetailsPanel = true;
			Comment->bColorCommentBubble = true;
			Comment->bGroupMode = true;

			if (Material)
			{
				Material->GetExpressionCollection().AddComment(Comment);
			}
			else
			{
				MaterialFunction->GetExpressionCollection().AddComment(Comment);
			}
		}

		static void CreateDreamShaderCommentAt(
			UMaterial* Material,
			UMaterialFunction* MaterialFunction,
			const FString& Title,
			const int32 X,
			const int32 Y,
			const int32 W,
			const int32 H,
			const FLinearColor& Color)
		{
			if ((!Material && !MaterialFunction) || Title.TrimStartAndEnd().IsEmpty())
			{
				return;
			}

			UObject* Outer = Material ? static_cast<UObject*>(Material) : static_cast<UObject*>(MaterialFunction);
			UMaterialExpressionComment* Comment = NewObject<UMaterialExpressionComment>(Outer, NAME_None, RF_Transactional);
			if (!Comment)
			{
				return;
			}

			Comment->Text = FString::Printf(TEXT("DreamShader: %s"), *Title);
			Comment->MaterialExpressionEditorX = X;
			Comment->MaterialExpressionEditorY = Y;
			Comment->SizeX = FMath::Max(120, W);
			Comment->SizeY = FMath::Max(80, H);
			Comment->FontSize = 24;
			Comment->CommentColor = Color;
			Comment->bCommentBubbleVisible_InDetailsPanel = true;
			Comment->bColorCommentBubble = true;
			Comment->bGroupMode = true;

			if (Material)
			{
				Material->GetExpressionCollection().AddComment(Comment);
			}
			else
			{
				MaterialFunction->GetExpressionCollection().AddComment(Comment);
			}
		}

		static bool ApplyExplicitDreamShaderLayout(
			UMaterial* Material,
			UMaterialFunction* MaterialFunction,
			const FTextShaderLayout* Layout,
			const TMap<FString, UMaterialExpression*>* ExpressionsByVariable,
			TSet<UMaterialExpression*>& OutPositionedExpressions)
		{
			OutPositionedExpressions.Reset();
			if (!Layout || (Layout->Nodes.IsEmpty() && Layout->Comments.IsEmpty()))
			{
				return false;
			}

			bool bAppliedAnyNode = false;
			if (ExpressionsByVariable)
			{
				for (const FTextShaderLayoutNode& Node : Layout->Nodes)
				{
					if (UMaterialExpression* const* Expression = ExpressionsByVariable->Find(Node.Var))
					{
						SetGeneratedExpressionPosition(*Expression, Node.X, Node.Y);
						OutPositionedExpressions.Add(*Expression);
						bAppliedAnyNode = true;
					}
				}
			}

			for (const FTextShaderLayoutComment& Comment : Layout->Comments)
			{
				CreateDreamShaderCommentAt(
					Material,
					MaterialFunction,
					Comment.Name,
					Comment.X,
					Comment.Y,
					Comment.W,
					Comment.H,
					Comment.Color);
			}

			return bAppliedAnyNode || !Layout->Comments.IsEmpty();
		}

		static void PositionUnmatchedExplicitLayoutExpressions(
			const TArray<UMaterialExpression*>& Expressions,
			const TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& Dependencies,
			const TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& Consumers,
			TSet<UMaterialExpression*>& InOutPositionedExpressions)
		{
			TSet<UMaterialExpression*> PendingExpressions;
			for (UMaterialExpression* Expression : Expressions)
			{
				if (Expression && !InOutPositionedExpressions.Contains(Expression))
				{
					PendingExpressions.Add(Expression);
				}
			}

			if (PendingExpressions.IsEmpty())
			{
				return;
			}

			TMap<FString, int32> SlotUseCount;
			auto BuildSlotKey = [](const int32 X, const int32 Y)
			{
				return FString::Printf(TEXT("%d:%d"), X / 80, Y / 80);
			};
			auto FanOutY = [&SlotUseCount, &BuildSlotKey](const int32 X, const int32 Y)
			{
				const FString SlotKey = BuildSlotKey(X, Y);
				const int32 SlotIndex = SlotUseCount.FindOrAdd(SlotKey)++;
				if (SlotIndex == 0)
				{
					return Y;
				}

				const int32 Step = ((SlotIndex + 1) / 2) * 120;
				return Y + ((SlotIndex % 2) == 0 ? -Step : Step);
			};
			auto AveragePosition = [&InOutPositionedExpressions](
				const TArray<UMaterialExpression*>* Neighbors,
				int32& OutX,
				int32& OutY)
			{
				if (!Neighbors || Neighbors->IsEmpty())
				{
					return false;
				}

				int64 SumX = 0;
				int64 SumY = 0;
				int32 Count = 0;
				for (UMaterialExpression* Neighbor : *Neighbors)
				{
					if (!Neighbor || !InOutPositionedExpressions.Contains(Neighbor))
					{
						continue;
					}

					SumX += Neighbor->MaterialExpressionEditorX;
					SumY += Neighbor->MaterialExpressionEditorY;
					++Count;
				}

				if (Count <= 0)
				{
					return false;
				}

				OutX = static_cast<int32>(SumX / Count);
				OutY = static_cast<int32>(SumY / Count);
				return true;
			};

			bool bChanged = true;
			const int32 MaxPropagationPasses = FMath::Max(32, PendingExpressions.Num());
			for (int32 PassIndex = 0; bChanged && PassIndex < MaxPropagationPasses; ++PassIndex)
			{
				bChanged = false;
				TArray<UMaterialExpression*> PendingSnapshot = PendingExpressions.Array();
				for (UMaterialExpression* Expression : PendingSnapshot)
				{
					if (!Expression)
					{
						PendingExpressions.Remove(Expression);
						continue;
					}

					int32 DependencyX = 0;
					int32 DependencyY = 0;
					const bool bHasDependencyAnchor = AveragePosition(Dependencies.Find(Expression), DependencyX, DependencyY);
					int32 ConsumerX = 0;
					int32 ConsumerY = 0;
					const bool bHasConsumerAnchor = AveragePosition(Consumers.Find(Expression), ConsumerX, ConsumerY);
					if (!bHasDependencyAnchor && !bHasConsumerAnchor)
					{
						continue;
					}

					int32 PositionX = Expression->MaterialExpressionEditorX;
					int32 PositionY = Expression->MaterialExpressionEditorY;
					if (bHasDependencyAnchor && bHasConsumerAnchor)
					{
						PositionX = (DependencyX + ConsumerX) / 2;
						PositionY = (DependencyY + ConsumerY) / 2;
					}
					else if (bHasConsumerAnchor)
					{
						PositionX = ConsumerX - 360;
						PositionY = ConsumerY;
					}
					else
					{
						PositionX = DependencyX + 360;
						PositionY = DependencyY;
					}

					SetGeneratedExpressionPosition(Expression, PositionX, FanOutY(PositionX, PositionY));
					InOutPositionedExpressions.Add(Expression);
					PendingExpressions.Remove(Expression);
					bChanged = true;
				}
			}

			if (PendingExpressions.IsEmpty() || InOutPositionedExpressions.IsEmpty())
			{
				return;
			}

			FLayoutBounds PositionedBounds;
			for (UMaterialExpression* Expression : InOutPositionedExpressions)
			{
				if (Expression)
				{
					PositionedBounds.IncludeNode(Expression->MaterialExpressionEditorX, Expression->MaterialExpressionEditorY);
				}
			}

			const int32 FallbackX = PositionedBounds.IsValid() ? PositionedBounds.MinX - 480 : -1200;
			int32 FallbackY = PositionedBounds.IsValid() ? PositionedBounds.MaxY + 240 : -620;
			for (UMaterialExpression* Expression : PendingExpressions)
			{
				if (!Expression)
				{
					continue;
				}

				SetGeneratedExpressionPosition(Expression, FallbackX, FallbackY);
				FallbackY += 180;
				InOutPositionedExpressions.Add(Expression);
			}
		}

		static bool IsExpressionInsideExplicitLayoutComment(
			const UMaterialExpression* Expression,
			const FTextShaderLayoutComment& Comment)
		{
			if (!Expression)
			{
				return false;
			}

			const int32 X = Expression->MaterialExpressionEditorX;
			const int32 Y = Expression->MaterialExpressionEditorY;
			return X >= Comment.X
				&& Y >= Comment.Y
				&& X <= Comment.X + Comment.W
				&& Y <= Comment.Y + Comment.H;
		}

		static int32 FindOrAddExplicitLayoutBlock(
			TArray<FGeneratedLayoutBlock>& Blocks,
			TMap<int32, int32>& BlockIndexByCommentIndex,
			const FTextShaderLayoutComment& Comment,
			const int32 CommentIndex)
		{
			if (const int32* ExistingBlockIndex = BlockIndexByCommentIndex.Find(CommentIndex))
			{
				return *ExistingBlockIndex;
			}

			FGeneratedLayoutBlock& Block = Blocks.AddDefaulted_GetRef();
			Block.Title = Comment.Name;
			Block.SortKey = CommentIndex;
			const int32 BlockIndex = Blocks.Num() - 1;
			BlockIndexByCommentIndex.Add(CommentIndex, BlockIndex);
			return BlockIndex;
		}

		static int32 FindOrAddNamedExplicitLayoutBlock(
			TArray<FGeneratedLayoutBlock>& Blocks,
			TMap<FString, int32>& BlockIndexByName,
			const FString& Title,
			const int32 SortKey)
		{
			if (const int32* ExistingBlockIndex = BlockIndexByName.Find(Title))
			{
				return *ExistingBlockIndex;
			}

			FGeneratedLayoutBlock& Block = Blocks.AddDefaulted_GetRef();
			Block.Title = Title;
			Block.SortKey = SortKey;
			const int32 BlockIndex = Blocks.Num() - 1;
			BlockIndexByName.Add(Title, BlockIndex);
			return BlockIndex;
		}

		static void BuildExplicitLayoutOwnershipBlocks(
			const TArray<UMaterialExpression*>& Expressions,
			const FTextShaderLayout* Layout,
			const TMap<FString, UMaterialExpression*>* ExpressionsByVariable,
			const TMap<FString, FString>* RegionByVariable,
			TArray<FGeneratedLayoutBlock>& OutBlocks,
			TMap<UMaterialExpression*, int32>& OutOwnerBlockByExpression)
		{
			OutBlocks.Reset();
			OutOwnerBlockByExpression.Reset();

			if (Layout)
			{
				TMap<int32, int32> BlockIndexByCommentIndex;
				for (UMaterialExpression* Expression : Expressions)
				{
					if (!Expression)
					{
						continue;
					}

					int32 BestCommentIndex = INDEX_NONE;
					int32 BestCommentArea = MAX_int32;
					for (int32 CommentIndex = 0; CommentIndex < Layout->Comments.Num(); ++CommentIndex)
					{
						const FTextShaderLayoutComment& Comment = Layout->Comments[CommentIndex];
						if (!IsExpressionInsideExplicitLayoutComment(Expression, Comment))
						{
							continue;
						}

						const int32 Area = FMath::Max(1, Comment.W) * FMath::Max(1, Comment.H);
						if (BestCommentIndex == INDEX_NONE || Area < BestCommentArea)
						{
							BestCommentIndex = CommentIndex;
							BestCommentArea = Area;
						}
					}

					if (BestCommentIndex == INDEX_NONE)
					{
						continue;
					}

					const int32 BlockIndex = FindOrAddExplicitLayoutBlock(
						OutBlocks,
						BlockIndexByCommentIndex,
						Layout->Comments[BestCommentIndex],
						BestCommentIndex);
					AddExpressionToOwnedBlock(OutBlocks, OutOwnerBlockByExpression, Expression, BlockIndex);
				}
			}

			if (!ExpressionsByVariable || !RegionByVariable || RegionByVariable->IsEmpty())
			{
				return;
			}

			TSet<UMaterialExpression*> ExpressionSet;
			for (UMaterialExpression* Expression : Expressions)
			{
				if (Expression)
				{
					ExpressionSet.Add(Expression);
				}
			}

			TMap<FString, int32> BlockIndexByRegion;
			for (const TPair<FString, FString>& Pair : *RegionByVariable)
			{
				UMaterialExpression* const* Expression = ExpressionsByVariable->Find(Pair.Key);
				if (!Expression || !*Expression || !ExpressionSet.Contains(*Expression) || OutOwnerBlockByExpression.Contains(*Expression))
				{
					continue;
				}

				const int32 BlockIndex = FindOrAddNamedExplicitLayoutBlock(
					OutBlocks,
					BlockIndexByRegion,
					Pair.Value,
					100000 + BlockIndexByRegion.Num());
				AddExpressionToOwnedBlock(OutBlocks, OutOwnerBlockByExpression, *Expression, BlockIndex);
			}
		}

		static void InsertExplicitLayoutReroutes(
			UMaterial* Material,
			UMaterialFunction* MaterialFunction,
			const FTextShaderLayout* Layout,
			const TMap<FString, UMaterialExpression*>* ExpressionsByVariable,
			const TMap<FString, FString>* RegionByVariable,
			TArray<UMaterialExpression*>& Expressions,
			TSet<UMaterialExpression*>& ExpressionSet,
			TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& Dependencies,
			TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& Consumers)
		{
			TArray<FGeneratedLayoutBlock> Blocks;
			TMap<UMaterialExpression*, int32> OwnerBlockByExpression;
			BuildExplicitLayoutOwnershipBlocks(
				Expressions,
				Layout,
				ExpressionsByVariable,
				RegionByVariable,
				Blocks,
				OwnerBlockByExpression);
			if (Blocks.Num() < 2 || OwnerBlockByExpression.Num() < 2)
			{
				return;
			}

			InsertCrossBlockReroutes(
				Material,
				MaterialFunction,
				Expressions,
				ExpressionSet,
				Blocks,
				OwnerBlockByExpression,
				Dependencies,
				Consumers,
				true);
		}

		static void AddRegionLayoutBlocks(
			const TArray<UMaterialExpression*>& Expressions,
			const TMap<FString, UMaterialExpression*>* ExpressionsByVariable,
			const TMap<FString, FString>* RegionByVariable,
			TArray<FGeneratedLayoutBlock>& InOutBlocks,
			TSet<UMaterialExpression*>& InOutOutputSinkExpressions)
		{
			if (!ExpressionsByVariable || !RegionByVariable || RegionByVariable->IsEmpty())
			{
				return;
			}

			TSet<UMaterialExpression*> ExpressionSet;
			for (UMaterialExpression* Expression : Expressions)
			{
				if (Expression)
				{
					ExpressionSet.Add(Expression);
				}
			}

			TMap<FString, int32> BlockIndexByRegion;
			for (const TPair<FString, FString>& Pair : *RegionByVariable)
			{
				UMaterialExpression* const* Expression = ExpressionsByVariable->Find(Pair.Key);
				if (!Expression || !*Expression || !ExpressionSet.Contains(*Expression))
				{
					continue;
				}

				int32 BlockIndex = INDEX_NONE;
				if (const int32* ExistingIndex = BlockIndexByRegion.Find(Pair.Value))
				{
					BlockIndex = *ExistingIndex;
				}
				else
				{
					FGeneratedLayoutBlock& Block = InOutBlocks.AddDefaulted_GetRef();
					Block.Title = Pair.Value;
					Block.SortKey = InOutBlocks.Num();
					BlockIndex = InOutBlocks.Num() - 1;
					BlockIndexByRegion.Add(Pair.Value, BlockIndex);
				}

				InOutBlocks[BlockIndex].ExpressionSet.Add(*Expression);
				InOutOutputSinkExpressions.Add(*Expression);
			}
		}

		static FLayoutBounds LayoutExpressionBlock(
			const TArray<UMaterialExpression*>& BlockExpressions,
			const TMap<UMaterialExpression*, TArray<UMaterialExpression*>>& GlobalDependencies,
			const TMap<UMaterialExpression*, int32>& OriginalOrder,
			const int32 BlockTopY)
		{
			FLayoutBounds Bounds;
			if (BlockExpressions.IsEmpty())
			{
				return Bounds;
			}

			TSet<UMaterialExpression*> BlockSet;
			BlockSet.Reserve(BlockExpressions.Num());
			for (UMaterialExpression* Expression : BlockExpressions)
			{
				if (Expression)
				{
					BlockSet.Add(Expression);
				}
			}

			TMap<UMaterialExpression*, TArray<UMaterialExpression*>> Dependencies;
			TMap<UMaterialExpression*, TArray<UMaterialExpression*>> Consumers;
			for (UMaterialExpression* Expression : BlockExpressions)
			{
				if (const TArray<UMaterialExpression*>* ExpressionDependencies = GlobalDependencies.Find(Expression))
				{
					for (UMaterialExpression* Dependency : *ExpressionDependencies)
					{
						if (BlockSet.Contains(Dependency))
						{
							TryAddUniqueExpression(Dependencies.FindOrAdd(Expression), Dependency);
							TryAddUniqueExpression(Consumers.FindOrAdd(Dependency), Expression);
						}
					}
				}
			}

			TMap<UMaterialExpression*, int32> RankByExpression;
			TSet<UMaterialExpression*> Resolving;
			TFunction<int32(UMaterialExpression*)> ResolveRank;
			ResolveRank = [&](UMaterialExpression* Expression) -> int32
			{
				if (!Expression)
				{
					return 0;
				}

				if (const int32* ExistingRank = RankByExpression.Find(Expression))
				{
					return *ExistingRank;
				}

				if (Resolving.Contains(Expression))
				{
					return 0;
				}

				Resolving.Add(Expression);
				int32 Rank = 0;
				if (const TArray<UMaterialExpression*>* ExpressionConsumers = Consumers.Find(Expression))
				{
					for (UMaterialExpression* Consumer : *ExpressionConsumers)
					{
						Rank = FMath::Max(Rank, ResolveRank(Consumer) + 1);
					}
				}
				Resolving.Remove(Expression);

				RankByExpression.Add(Expression, Rank);
				return Rank;
			};

			int32 MaxRank = 0;
			for (UMaterialExpression* Expression : BlockExpressions)
			{
				MaxRank = FMath::Max(MaxRank, ResolveRank(Expression));
			}

			TMap<int32, TArray<UMaterialExpression*>> Layers;
			for (UMaterialExpression* Expression : BlockExpressions)
			{
				Layers.FindOrAdd(RankByExpression.FindRef(Expression)).Add(Expression);
			}

			for (TPair<int32, TArray<UMaterialExpression*>>& LayerPair : Layers)
			{
				LayerPair.Value.StableSort([&OriginalOrder](UMaterialExpression& Left, UMaterialExpression& Right)
				{
					if (Left.MaterialExpressionEditorY != Right.MaterialExpressionEditorY)
					{
						return Left.MaterialExpressionEditorY < Right.MaterialExpressionEditorY;
					}
					return OriginalOrder.FindRef(&Left) < OriginalOrder.FindRef(&Right);
				});
			}

			TMap<UMaterialExpression*, int32> OrderInLayer;
			auto RefreshOrder = [&]()
			{
				OrderInLayer.Reset();
				for (const TPair<int32, TArray<UMaterialExpression*>>& LayerPair : Layers)
				{
					const TArray<UMaterialExpression*>& Layer = LayerPair.Value;
					for (int32 Index = 0; Index < Layer.Num(); ++Index)
					{
						OrderInLayer.Add(Layer[Index], Index);
					}
				}
			};

			auto AverageNeighborOrder = [&OrderInLayer, &OriginalOrder](
				UMaterialExpression* Expression,
				const TArray<UMaterialExpression*>* Neighbors) -> float
			{
				if (!Expression || !Neighbors || Neighbors->IsEmpty())
				{
					return static_cast<float>(OriginalOrder.FindRef(Expression));
				}

				float Sum = 0.0f;
				int32 Count = 0;
				for (UMaterialExpression* Neighbor : *Neighbors)
				{
					if (const int32* NeighborOrder = OrderInLayer.Find(Neighbor))
					{
						Sum += static_cast<float>(*NeighborOrder);
						++Count;
					}
				}

				return Count > 0
					? Sum / static_cast<float>(Count)
					: static_cast<float>(OriginalOrder.FindRef(Expression));
			};

			RefreshOrder();
			for (int32 Iteration = 0; Iteration < 4; ++Iteration)
			{
				for (int32 Rank = MaxRank - 1; Rank >= 0; --Rank)
				{
					if (TArray<UMaterialExpression*>* Layer = Layers.Find(Rank))
					{
						Layer->StableSort([&](UMaterialExpression& Left, UMaterialExpression& Right)
						{
							const float LeftOrder = AverageNeighborOrder(&Left, Consumers.Find(&Left));
							const float RightOrder = AverageNeighborOrder(&Right, Consumers.Find(&Right));
							return LeftOrder == RightOrder
								? OriginalOrder.FindRef(&Left) < OriginalOrder.FindRef(&Right)
								: LeftOrder < RightOrder;
						});
					}
				}
				RefreshOrder();

				for (int32 Rank = 1; Rank <= MaxRank; ++Rank)
				{
					if (TArray<UMaterialExpression*>* Layer = Layers.Find(Rank))
					{
						Layer->StableSort([&](UMaterialExpression& Left, UMaterialExpression& Right)
						{
							const float LeftOrder = AverageNeighborOrder(&Left, Dependencies.Find(&Left));
							const float RightOrder = AverageNeighborOrder(&Right, Dependencies.Find(&Right));
							return LeftOrder == RightOrder
								? OriginalOrder.FindRef(&Left) < OriginalOrder.FindRef(&Right)
								: LeftOrder < RightOrder;
						});
					}
				}
				RefreshOrder();
			}

			int32 MaxLayerSize = 1;
			for (const TPair<int32, TArray<UMaterialExpression*>>& LayerPair : Layers)
			{
				MaxLayerSize = FMath::Max(MaxLayerSize, LayerPair.Value.Num());
			}

			constexpr int32 OutputX = 900;
			constexpr int32 ColumnSpacing = 420;
			constexpr int32 RowSpacing = 220;
			const int32 CenterY = BlockTopY + ((MaxLayerSize - 1) * RowSpacing) / 2;
			for (int32 Rank = 0; Rank <= MaxRank; ++Rank)
			{
				TArray<UMaterialExpression*>* Layer = Layers.Find(Rank);
				if (!Layer || Layer->IsEmpty())
				{
					continue;
				}

				const int32 PositionX = OutputX - Rank * ColumnSpacing;
				const int32 StartY = CenterY - ((Layer->Num() - 1) * RowSpacing) / 2;
				for (int32 Index = 0; Index < Layer->Num(); ++Index)
				{
					const int32 PositionY = StartY + Index * RowSpacing;
					SetGeneratedExpressionPosition((*Layer)[Index], PositionX, PositionY);
					Bounds.IncludeNode(PositionX, PositionY);
				}
			}

			return Bounds;
		}
	}

	void LayoutGeneratedExpressions(UMaterial* Material, UMaterialFunction* MaterialFunction)
	{
		LayoutGeneratedExpressions(Material, MaterialFunction, nullptr, nullptr, nullptr);
	}

	void LayoutGeneratedExpressions(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const FTextShaderLayout* Layout,
		const TMap<FString, UMaterialExpression*>* ExpressionsByVariable,
		const TMap<FString, FString>* RegionByVariable)
	{
		TArray<UMaterialExpression*> Expressions;
		CollectMaterialExpressions(Material, MaterialFunction, Expressions);
		TSet<UMaterialExpression*> ExplicitlyPositionedExpressions;
		if (ApplyExplicitDreamShaderLayout(Material, MaterialFunction, Layout, ExpressionsByVariable, ExplicitlyPositionedExpressions))
		{
			TSet<UMaterialExpression*> ExpressionSet;
			ExpressionSet.Reserve(Expressions.Num());
			for (UMaterialExpression* Expression : Expressions)
			{
				if (Expression)
				{
					ExpressionSet.Add(Expression);
				}
			}

			TMap<UMaterialExpression*, TArray<UMaterialExpression*>> Dependencies;
			TMap<UMaterialExpression*, TArray<UMaterialExpression*>> Consumers;
			BuildExpressionDependencyMaps(Expressions, ExpressionSet, Dependencies, Consumers);

			if (!ExplicitlyPositionedExpressions.IsEmpty() && ExplicitlyPositionedExpressions.Num() < Expressions.Num())
			{
				PositionUnmatchedExplicitLayoutExpressions(Expressions, Dependencies, Consumers, ExplicitlyPositionedExpressions);
			}

			InsertExplicitLayoutReroutes(
				Material,
				MaterialFunction,
				Layout,
				ExpressionsByVariable,
				RegionByVariable,
				Expressions,
				ExpressionSet,
				Dependencies,
				Consumers);
			PositionMaterialRootNearConnectedOutputs(Material);
			return;
		}

		if (Expressions.Num() < 2)
		{
			return;
		}

		if (Expressions.Num() >= FastLayoutExpressionThreshold)
		{
			UE_LOG(
				LogDreamShader,
				Display,
				TEXT("Skipping automatic layout for large DreamShader graph (%d nodes). Existing generated positions will be used."),
				Expressions.Num());
			return;
		}

		FScopedSlowTask LayoutSlowTask(
			FMath::Max(1.0f, static_cast<float>(Expressions.Num())),
			FText::FromString(TEXT("Laying out DreamShader material graph...")));

		TSet<UMaterialExpression*> ExpressionSet;
		TMap<UMaterialExpression*, int32> OriginalOrder;
		ExpressionSet.Reserve(Expressions.Num());
		OriginalOrder.Reserve(Expressions.Num());
		for (int32 Index = 0; Index < Expressions.Num(); ++Index)
		{
			ExpressionSet.Add(Expressions[Index]);
			OriginalOrder.Add(Expressions[Index], Index);
		}

		TMap<UMaterialExpression*, TArray<UMaterialExpression*>> Dependencies;
		TMap<UMaterialExpression*, TArray<UMaterialExpression*>> Consumers;
		BuildExpressionDependencyMaps(Expressions, ExpressionSet, Dependencies, Consumers);

		TArray<FGeneratedLayoutBlock> Blocks;
		TSet<UMaterialExpression*> OutputSinkExpressions;
		AddRegionLayoutBlocks(Expressions, ExpressionsByVariable, RegionByVariable, Blocks, OutputSinkExpressions);
		if (Material)
		{
			for (int32 MaterialPropertyIndex = 0; MaterialPropertyIndex < MP_MAX; ++MaterialPropertyIndex)
			{
				const EMaterialProperty MaterialProperty = static_cast<EMaterialProperty>(MaterialPropertyIndex);
				FExpressionInput* MaterialInput = Material->GetExpressionInputForProperty(MaterialProperty);
				if (!MaterialInput || !MaterialInput->IsConnected())
				{
					continue;
				}

				AddLayoutBlock(
					Blocks,
					OutputSinkExpressions,
					FString::Printf(TEXT("Output: %s"), *GetMaterialPropertyLayoutName(MaterialProperty)),
					GetDirectInputExpression(*MaterialInput),
					MaterialPropertyIndex,
					ExpressionSet,
					Dependencies);
			}
		}
		else if (MaterialFunction)
		{
			for (UMaterialExpression* Expression : Expressions)
			{
				UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(Expression);
				if (!FunctionOutput)
				{
					continue;
				}

				const FString OutputName = FunctionOutput->OutputName.IsNone()
					? TEXT("FunctionOutput")
					: FunctionOutput->OutputName.ToString();
				AddLayoutBlock(
					Blocks,
					OutputSinkExpressions,
					FString::Printf(TEXT("Output: %s"), *OutputName),
					FunctionOutput,
					1000 + OriginalOrder.FindRef(FunctionOutput),
					ExpressionSet,
					Dependencies);
			}
		}

		FGeneratedLayoutBlock LooseOutputBlock;
		LooseOutputBlock.Title = TEXT("Generated Outputs");
		LooseOutputBlock.SortKey = 100000;
		for (UMaterialExpression* Expression : Expressions)
		{
			const TArray<UMaterialExpression*>* ExpressionConsumers = Consumers.Find(Expression);
			if (!OutputSinkExpressions.Contains(Expression)
				&& (!ExpressionConsumers || ExpressionConsumers->IsEmpty()))
			{
				CollectDependencySubgraph(Expression, ExpressionSet, Dependencies, LooseOutputBlock.ExpressionSet);
				OutputSinkExpressions.Add(Expression);
			}
		}
		if (!LooseOutputBlock.ExpressionSet.IsEmpty())
		{
			Blocks.Add(MoveTemp(LooseOutputBlock));
		}

		if (Blocks.IsEmpty())
		{
			FGeneratedLayoutBlock& Block = Blocks.AddDefaulted_GetRef();
			Block.Title = TEXT("Graph");
			Block.SortKey = 0;
			for (UMaterialExpression* Expression : Expressions)
			{
				Block.ExpressionSet.Add(Expression);
			}
		}

		TSet<UMaterialExpression*> SinkExpressions = OutputSinkExpressions;
		TMap<UMaterialExpression*, int32> BlockUseCount;
		for (const FGeneratedLayoutBlock& Block : Blocks)
		{
			for (UMaterialExpression* Expression : Block.ExpressionSet)
			{
				if (Expression && !SinkExpressions.Contains(Expression))
				{
					BlockUseCount.FindOrAdd(Expression)++;
				}
			}
		}

		TSet<UMaterialExpression*> SharedExpressions;
		for (const TPair<UMaterialExpression*, int32>& Pair : BlockUseCount)
		{
			if (Pair.Value > 1)
			{
				SharedExpressions.Add(Pair.Key);
			}
		}

		bool bAddedSharedReroute = true;
		while (bAddedSharedReroute)
		{
			bAddedSharedReroute = false;
			for (UMaterialExpression* Expression : Expressions)
			{
				UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Expression);
				if (!Declaration || SharedExpressions.Contains(Declaration))
				{
					continue;
				}

				if (UMaterialExpression* SourceExpression = GetDirectInputExpression(Declaration->Input))
				{
					if (SharedExpressions.Contains(SourceExpression))
					{
						SharedExpressions.Add(Declaration);
						bAddedSharedReroute = true;
					}
				}
			}
		}

		Blocks.StableSort([](const FGeneratedLayoutBlock& Left, const FGeneratedLayoutBlock& Right)
		{
			return Left.SortKey < Right.SortKey;
		});

		TArray<FGeneratedLayoutBlock> LayoutBlocks;
		LayoutBlocks.Reserve(Blocks.Num() + 1);
		TMap<UMaterialExpression*, int32> OwnerBlockByExpression;
		AssignLayoutBlockOwners(Expressions, Blocks, Consumers, OwnerBlockByExpression);

		for (int32 BlockIndex = 0; BlockIndex < Blocks.Num(); ++BlockIndex)
		{
			const FGeneratedLayoutBlock& Block = Blocks[BlockIndex];
			FGeneratedLayoutBlock& LayoutBlock = LayoutBlocks.AddDefaulted_GetRef();
			LayoutBlock.Title = Block.Title;
			LayoutBlock.SortKey = Block.SortKey;
			for (UMaterialExpression* Expression : Block.ExpressionSet)
			{
				if (!Expression)
				{
					continue;
				}

				if (OwnerBlockByExpression.FindRef(Expression) != BlockIndex)
				{
					continue;
				}

				LayoutBlock.ExpressionSet.Add(Expression);
			}
		}

		TSet<UMaterialExpression*> AssignedExpressions;
		for (const FGeneratedLayoutBlock& LayoutBlock : LayoutBlocks)
		{
			for (UMaterialExpression* Expression : LayoutBlock.ExpressionSet)
			{
				AssignedExpressions.Add(Expression);
			}
		}

		FGeneratedLayoutBlock LooseSharedBlock;
		LooseSharedBlock.Title = TEXT("Shared Inputs");
		LooseSharedBlock.SortKey = -1000;
		for (UMaterialExpression* Expression : Expressions)
		{
			if (Expression && SharedExpressions.Contains(Expression) && !AssignedExpressions.Contains(Expression))
			{
				LooseSharedBlock.ExpressionSet.Add(Expression);
			}
		}
		if (!LooseSharedBlock.ExpressionSet.IsEmpty())
		{
			LayoutBlocks.Insert(MoveTemp(LooseSharedBlock), 0);
			for (UMaterialExpression* Expression : LayoutBlocks[0].ExpressionSet)
			{
				OwnerBlockByExpression.Add(Expression, 0);
			}
			for (TPair<UMaterialExpression*, int32>& Pair : OwnerBlockByExpression)
			{
				if (!LayoutBlocks[0].ExpressionSet.Contains(Pair.Key))
				{
					++Pair.Value;
				}
			}
		}

		InsertCrossBlockReroutes(
			Material,
			MaterialFunction,
			Expressions,
			ExpressionSet,
			LayoutBlocks,
			OwnerBlockByExpression,
			Dependencies,
			Consumers,
			false);

		OriginalOrder.Reserve(Expressions.Num());
		for (int32 Index = 0; Index < Expressions.Num(); ++Index)
		{
			if (Expressions[Index] && !OriginalOrder.Contains(Expressions[Index]))
			{
				OriginalOrder.Add(Expressions[Index], Index);
			}
		}

		int32 PositionedCount = 0;
		int32 NextBlockTopY = -620;
		constexpr int32 BlockSpacing = 420;
		TArray<FLayoutBounds> BlockBounds;
		BlockBounds.Reserve(LayoutBlocks.Num());

		// The positioning loop below calls EnterProgressFrame(1.0f) once per node in every
		// LayoutBlock. Inserted cross-block reroute nodes mean that count can exceed the
		// original Expressions.Num() the slow task was constructed with, so reconcile the
		// total here to the actual node count to avoid overrunning the slow task budget.
		int32 NodesToPosition = 0;
		for (const FGeneratedLayoutBlock& Block : LayoutBlocks)
		{
			NodesToPosition += Block.ExpressionSet.Num();
		}
		LayoutSlowTask.TotalAmountOfWork = FMath::Max(1.0f, static_cast<float>(NodesToPosition));
		LayoutSlowTask.CompletedWork = 0.0f;

		for (const FGeneratedLayoutBlock& Block : LayoutBlocks)
		{
			TArray<UMaterialExpression*> BlockExpressions;
			BlockExpressions.Reserve(Block.ExpressionSet.Num());
			for (UMaterialExpression* Expression : Block.ExpressionSet)
			{
				BlockExpressions.Add(Expression);
			}

			BlockExpressions.StableSort([&OriginalOrder](UMaterialExpression& Left, UMaterialExpression& Right)
			{
				return OriginalOrder.FindRef(&Left) < OriginalOrder.FindRef(&Right);
			});

			for (int32 Index = 0; Index < BlockExpressions.Num(); ++Index)
			{
				(void)Index;
				LayoutSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(
					TEXT("Positioning node %d of %d..."),
					++PositionedCount,
					Expressions.Num())));
			}

			FLayoutBounds Bounds = LayoutExpressionBlock(BlockExpressions, Dependencies, OriginalOrder, NextBlockTopY);
			CreateDreamShaderLayoutComment(Material, MaterialFunction, Block.Title, Bounds);
			if (Bounds.IsValid())
			{
				BlockBounds.Add(Bounds);
			}
			NextBlockTopY = Bounds.IsValid() ? Bounds.MaxY + BlockSpacing : NextBlockTopY + BlockSpacing;
		}

		PositionMaterialRootNearOutputs(Material, BlockBounds);
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

	static const TCHAR* GetTextureTypeLabel(const ETextShaderTextureType TextureType)
	{
		switch (TextureType)
		{
		case ETextShaderTextureType::TextureCube:
			return TEXT("TextureCube");
		case ETextShaderTextureType::Texture2DArray:
			return TEXT("Texture2DArray");
		case ETextShaderTextureType::VolumeTexture:
			return TEXT("VolumeTexture");
		case ETextShaderTextureType::Texture2D:
		default:
			return TEXT("Texture2D");
		}
	}

	static const TCHAR* GetDefaultTextureObjectPath(const ETextShaderTextureType TextureType)
	{
		switch (TextureType)
		{
		case ETextShaderTextureType::TextureCube:
			return TEXT("/Engine/EngineResources/DefaultTextureCube.DefaultTextureCube");
		case ETextShaderTextureType::VolumeTexture:
			return TEXT("/Engine/EngineResources/DefaultVolumeTexture.DefaultVolumeTexture");
		case ETextShaderTextureType::Texture2DArray:
			return nullptr;
		case ETextShaderTextureType::Texture2D:
		default:
			return TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");
		}
	}

	static bool DoesTextureMatchType(const UTexture* Texture, const ETextShaderTextureType TextureType)
	{
		if (!Texture)
		{
			return false;
		}

		switch (TextureType)
		{
		case ETextShaderTextureType::TextureCube:
			return Cast<UTextureCube>(Texture) != nullptr;
		case ETextShaderTextureType::Texture2DArray:
			return Cast<UTexture2DArray>(Texture) != nullptr;
		case ETextShaderTextureType::VolumeTexture:
			return Cast<UVolumeTexture>(Texture) != nullptr;
		case ETextShaderTextureType::Texture2D:
		default:
			return !Cast<UTextureCube>(Texture)
				&& !Cast<UTexture2DArray>(Texture)
				&& !Cast<UVolumeTexture>(Texture);
		}
	}

	static bool ValidateTextureType(
		const FTextShaderPropertyDefinition& Property,
		const UTexture* Texture,
		const TCHAR* Context,
		FString& OutError)
	{
		if (DoesTextureMatchType(Texture, Property.TextureType))
		{
			return true;
		}

		OutError = FString::Printf(
			TEXT("%s texture property '%s' expects %s but '%s' is a '%s'."),
			Context,
			*Property.Name,
			GetTextureTypeLabel(Property.TextureType),
			Property.TextureDefaultObjectPath.IsEmpty() ? TEXT("<default>") : *Property.TextureDefaultObjectPath,
			Texture ? *Texture->GetClass()->GetName() : TEXT("None"));
		return false;
	}

	static FString FormatMetadataContext(const FTextShaderPropertyDefinition& Property)
	{
		return FString::Printf(TEXT("property '%s'"), *Property.Name);
	}

	static FString ResolveMetadataReflectionPropertyName(const FString& Key)
	{
		const FString NormalizedKey = UE::DreamShader::NormalizeSettingKey(Key);
		if (NormalizedKey == UE::DreamShader::NormalizeSettingKey(TEXT("Description"))
			|| NormalizedKey == UE::DreamShader::NormalizeSettingKey(TEXT("Tooltip")))
		{
			return TEXT("Desc");
		}
		if (NormalizedKey == UE::DreamShader::NormalizeSettingKey(TEXT("Category")))
		{
			return TEXT("Group");
		}
		if (NormalizedKey == UE::DreamShader::NormalizeSettingKey(TEXT("Sort")))
		{
			return TEXT("SortPriority");
		}
		return Key;
	}

	bool ApplyExpressionMetadata(UMaterialExpression* Expression, const FTextShaderMetadata& Metadata, FString& OutError)
	{
		if (!Expression)
		{
			return true;
		}

		TMap<FString, FString> ReflectedProperties = Metadata.ReflectedProperties;
		const auto ContainsMetadataKey = [&ReflectedProperties](const TCHAR* Key)
		{
			return ReflectedProperties.Contains(UE::DreamShader::NormalizeSettingKey(Key));
		};

		if (!Metadata.Group.IsEmpty()
			&& !ContainsMetadataKey(TEXT("Group"))
			&& !ContainsMetadataKey(TEXT("Category")))
		{
			ReflectedProperties.Add(UE::DreamShader::NormalizeSettingKey(TEXT("Group")), Metadata.Group);
		}
		if (Metadata.bHasSortPriority
			&& !ContainsMetadataKey(TEXT("SortPriority"))
			&& !ContainsMetadataKey(TEXT("Sort")))
		{
			ReflectedProperties.Add(UE::DreamShader::NormalizeSettingKey(TEXT("SortPriority")), FString::FromInt(Metadata.SortPriority));
		}
		if (!Metadata.Description.IsEmpty()
			&& !ContainsMetadataKey(TEXT("Description"))
			&& !ContainsMetadataKey(TEXT("Desc"))
			&& !ContainsMetadataKey(TEXT("Tooltip")))
		{
			ReflectedProperties.Add(UE::DreamShader::NormalizeSettingKey(TEXT("Desc")), Metadata.Description);
		}

		for (const TPair<FString, FString>& ReflectedProperty : ReflectedProperties)
		{
			const FString PropertyName = ResolveMetadataReflectionPropertyName(ReflectedProperty.Key);
			FProperty* Property = FindMaterialExpressionArgumentProperty(Expression->GetClass(), PropertyName);
			if (!Property)
			{
				OutError = FString::Printf(
					TEXT("Metadata property '%s' is not a reflected property on '%s'."),
					*PropertyName,
					*Expression->GetClass()->GetName());
				return false;
			}

			FString LiteralError;
			if (!SetMaterialExpressionLiteralProperty(Expression, Property, ReflectedProperty.Value, LiteralError))
			{
				OutError = FString::Printf(
					TEXT("Metadata property '%s' on '%s': %s"),
					*PropertyName,
					*Expression->GetClass()->GetName(),
					*LiteralError);
				return false;
			}
		}

		return true;
	}

	static bool SetExpressionParameterName(UMaterialExpression* Expression, const FString& ParameterName, FString& OutError)
	{
		if (!Expression)
		{
			OutError = TEXT("Invalid parameter expression.");
			return false;
		}

		if (FProperty* ParameterNameProperty = FindMaterialExpressionArgumentProperty(Expression->GetClass(), TEXT("ParameterName")))
		{
			return SetMaterialExpressionLiteralProperty(Expression, ParameterNameProperty, ParameterName, OutError);
		}

		OutError = FString::Printf(TEXT("'%s' does not expose a ParameterName property."), *Expression->GetClass()->GetName());
		return false;
	}

	static FString GetPropertyParameterName(const FTextShaderPropertyDefinition& Property)
	{
		if (const FString* ParameterName = Property.Metadata.ReflectedProperties.Find(UE::DreamShader::NormalizeSettingKey(TEXT("ParameterName"))))
		{
			const FString TrimmedParameterName = ParameterName->TrimStartAndEnd();
			if (!TrimmedParameterName.IsEmpty())
			{
				return TrimmedParameterName;
			}
		}

		return Property.Name;
	}

	static bool SetExpressionDefaultValue(UMaterialExpression* Expression, const FTextShaderPropertyDefinition& Property, FString& OutError)
	{
		if (!Expression || !Property.bHasDefaultValue)
		{
			return true;
		}

		if (Property.Type == ETextShaderPropertyType::Texture2D || !Property.TextureDefaultObjectPath.IsEmpty())
		{
			if (Property.TextureDefaultObjectPath.IsEmpty())
			{
				return true;
			}

			if (FProperty* TextureProperty = FindMaterialExpressionArgumentProperty(Expression->GetClass(), TEXT("Texture")))
			{
				return SetMaterialExpressionLiteralProperty(Expression, TextureProperty, Property.TextureDefaultObjectPath, OutError);
			}
			if (FProperty* TextureObjectProperty = FindMaterialExpressionArgumentProperty(Expression->GetClass(), TEXT("TextureObject")))
			{
				return SetMaterialExpressionLiteralProperty(Expression, TextureObjectProperty, Property.TextureDefaultObjectPath, OutError);
			}

			OutError = FString::Printf(TEXT("'%s' does not expose a Texture property for %s."), *Expression->GetClass()->GetName(), *FormatMetadataContext(Property));
			return false;
		}

		FProperty* DefaultValueProperty = FindMaterialExpressionArgumentProperty(Expression->GetClass(), TEXT("DefaultValue"));
		if (!DefaultValueProperty)
		{
			return true;
		}

		if (Property.Type == ETextShaderPropertyType::Scalar)
		{
			if (Property.ParameterNodeType.Equals(TEXT("StaticBoolParameter"), ESearchCase::IgnoreCase)
				|| Property.ParameterNodeType.Equals(TEXT("StaticSwitchParameter"), ESearchCase::IgnoreCase))
			{
				return SetMaterialExpressionLiteralProperty(
					Expression,
					DefaultValueProperty,
					Property.ScalarDefaultValue != 0.0 ? TEXT("true") : TEXT("false"),
					OutError);
			}

			return SetMaterialExpressionLiteralProperty(
				Expression,
				DefaultValueProperty,
				FString::SanitizeFloat(Property.ScalarDefaultValue),
				OutError);
		}

		const FString LinearColorText = FString::Printf(
			TEXT("(R=%s,G=%s,B=%s,A=%s)"),
			*FString::SanitizeFloat(Property.VectorDefaultValue.R),
			*FString::SanitizeFloat(Property.VectorDefaultValue.G),
			*FString::SanitizeFloat(Property.VectorDefaultValue.B),
			*FString::SanitizeFloat(Property.VectorDefaultValue.A));
		if (SetMaterialExpressionLiteralProperty(Expression, DefaultValueProperty, LinearColorText, OutError))
		{
			return true;
		}

		const FString VectorText = FString::Printf(
			TEXT("(X=%s,Y=%s,Z=%s,W=%s)"),
			*FString::SanitizeFloat(Property.VectorDefaultValue.R),
			*FString::SanitizeFloat(Property.VectorDefaultValue.G),
			*FString::SanitizeFloat(Property.VectorDefaultValue.B),
			*FString::SanitizeFloat(Property.VectorDefaultValue.A));
		return SetMaterialExpressionLiteralProperty(Expression, DefaultValueProperty, VectorText, OutError);
	}

	static UMaterialExpression* CreateGenericParameterNodeExpression(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const FTextShaderPropertyDefinition& Property,
		const int32 PositionY,
		FString& OutError)
	{
		UClass* ExpressionClass = ResolveMaterialExpressionClass(Property.ParameterNodeType);
		if (!ExpressionClass)
		{
			OutError = FString::Printf(TEXT("Could not resolve MaterialExpression class for parameter type '%s'."), *Property.ParameterNodeType);
			return nullptr;
		}

		auto* Expression = Cast<UMaterialExpression>(
			CreateOwnedMaterialExpression(Material, MaterialFunction, ExpressionClass, -800, PositionY));
		if (!Expression)
		{
			OutError = FString::Printf(TEXT("Failed to create a '%s' node for property '%s'."), *Property.ParameterNodeType, *Property.Name);
			return nullptr;
		}

		if (!SetExpressionParameterName(Expression, GetPropertyParameterName(Property), OutError)
			|| !SetExpressionDefaultValue(Expression, Property, OutError))
		{
			OutError = FString::Printf(TEXT("%s: %s"), *FormatMetadataContext(Property), *OutError);
			return nullptr;
		}

		if (UMaterialExpressionTextureBase* TextureExpression = Cast<UMaterialExpressionTextureBase>(Expression))
		{
			TextureExpression->AutoSetSampleType();
		}

		if (!ApplyExpressionMetadata(Expression, Property.Metadata, OutError))
		{
			OutError = FString::Printf(TEXT("%s: %s"), *FormatMetadataContext(Property), *OutError);
			return nullptr;
		}
		return Expression;
	}

	static UMaterialExpression* CreateConstPropertyExpression(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const FTextShaderPropertyDefinition& Property,
		const int32 PositionY,
		FString& OutError)
	{
		if (Property.Source == ETextShaderPropertySource::UEBuiltin || !Property.ParameterNodeType.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("Const property '%s' must use a plain scalar, vector, or texture type instead of a parameter node or UE builtin declaration."),
				*Property.Name);
			return nullptr;
		}

		UMaterialExpression* Expression = nullptr;
		if (Property.Type == ETextShaderPropertyType::Scalar)
		{
			Expression = CreateScalarLiteralExpressionEx(Material, MaterialFunction, Property.ScalarDefaultValue, PositionY);
		}
		else if (Property.Type == ETextShaderPropertyType::Vector)
		{
			TArray<double> Components;
			Components.Reserve(Property.ComponentCount);
			Components.Add(Property.VectorDefaultValue.R);
			if (Property.ComponentCount >= 2)
			{
				Components.Add(Property.VectorDefaultValue.G);
			}
			if (Property.ComponentCount >= 3)
			{
				Components.Add(Property.VectorDefaultValue.B);
			}
			if (Property.ComponentCount >= 4)
			{
				Components.Add(Property.VectorDefaultValue.A);
			}
			Expression = CreateVectorLiteralExpression(Material, MaterialFunction, Components, Property.ComponentCount, PositionY);
		}
		else
		{
			auto* TextureObjectExpression = Cast<UMaterialExpressionTextureObject>(
				CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionTextureObject::StaticClass(), -800, PositionY));
			if (!TextureObjectExpression)
			{
				OutError = FString::Printf(TEXT("Failed to create a texture object node for const property '%s'."), *Property.Name);
				return nullptr;
			}

			if (!Property.TextureDefaultObjectPath.IsEmpty())
			{
				UTexture* DefaultTexture = LoadObject<UTexture>(nullptr, *Property.TextureDefaultObjectPath);
				if (!DefaultTexture)
				{
					OutError = FString::Printf(
						TEXT("Const texture property '%s' could not load asset '%s'."),
						*Property.Name,
						*Property.TextureDefaultObjectPath);
					return nullptr;
				}

				if (!ValidateTextureType(Property, DefaultTexture, TEXT("Const"), OutError))
				{
					return nullptr;
				}

				TextureObjectExpression->Texture = DefaultTexture;
				TextureObjectExpression->AutoSetSampleType();
			}
			else if (const TCHAR* DefaultTexturePath = GetDefaultTextureObjectPath(Property.TextureType))
			{
				UTexture* DefaultTexture = LoadObject<UTexture>(nullptr, DefaultTexturePath);
				if (!DefaultTexture || !DoesTextureMatchType(DefaultTexture, Property.TextureType))
				{
					OutError = FString::Printf(
						TEXT("Const texture property '%s' could not load default %s asset '%s'."),
						*Property.Name,
						GetTextureTypeLabel(Property.TextureType),
						DefaultTexturePath);
					return nullptr;
				}

				TextureObjectExpression->Texture = DefaultTexture;
				TextureObjectExpression->AutoSetSampleType();
			}
			else
			{
				OutError = FString::Printf(
					TEXT("Const texture property '%s' with type %s requires an explicit default asset."),
					*Property.Name,
					GetTextureTypeLabel(Property.TextureType));
				return nullptr;
			}

			Expression = TextureObjectExpression;
		}

		if (!Expression)
		{
			OutError = FString::Printf(TEXT("Failed to create a const node for property '%s'."), *Property.Name);
			return nullptr;
		}

		if (!ApplyExpressionMetadata(Expression, Property.Metadata, OutError))
		{
			OutError = FString::Printf(TEXT("%s: %s"), *FormatMetadataContext(Property), *OutError);
			return nullptr;
		}
		return Expression;
	}

	static UMaterialExpression* CreateParameterExpression(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const FTextShaderPropertyDefinition& Property,
		int32 PositionY,
		FString& OutError)
	{
		if (!Property.ParameterNodeType.IsEmpty()
			&& !Property.ParameterNodeType.Equals(TEXT("ScalarParameter"), ESearchCase::IgnoreCase)
			&& !Property.ParameterNodeType.Equals(TEXT("VectorParameter"), ESearchCase::IgnoreCase)
			&& !Property.ParameterNodeType.Equals(TEXT("TextureObjectParameter"), ESearchCase::IgnoreCase))
		{
			return CreateGenericParameterNodeExpression(Material, MaterialFunction, Property, PositionY, OutError);
		}

		if (Property.Type == ETextShaderPropertyType::Scalar)
		{
			auto* Expression = Cast<UMaterialExpressionScalarParameter>(
				CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionScalarParameter::StaticClass(), -800, PositionY));
			if (!Expression)
			{
				OutError = FString::Printf(TEXT("Failed to create a scalar parameter node for property '%s'."), *Property.Name);
				return nullptr;
			}

			Expression->ParameterName = FName(*GetPropertyParameterName(Property));
			if (Property.bHasDefaultValue)
			{
				Expression->DefaultValue = static_cast<float>(Property.ScalarDefaultValue);
			}
			if (!ApplyExpressionMetadata(Expression, Property.Metadata, OutError))
			{
				OutError = FString::Printf(TEXT("%s: %s"), *FormatMetadataContext(Property), *OutError);
				return nullptr;
			}
			return Expression;
		}

		if (Property.Type == ETextShaderPropertyType::Vector)
		{
			auto* ParameterExpression = Cast<UMaterialExpressionVectorParameter>(
				CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionVectorParameter::StaticClass(), -800, PositionY));
			if (!ParameterExpression)
			{
				OutError = FString::Printf(TEXT("Failed to create a vector parameter node for property '%s'."), *Property.Name);
				return nullptr;
			}

			ParameterExpression->ParameterName = FName(*GetPropertyParameterName(Property));
			if (Property.bHasDefaultValue)
			{
				ParameterExpression->DefaultValue = Property.VectorDefaultValue;
			}
			if (!ApplyExpressionMetadata(ParameterExpression, Property.Metadata, OutError))
			{
				OutError = FString::Printf(TEXT("%s: %s"), *FormatMetadataContext(Property), *OutError);
				return nullptr;
			}

			return ParameterExpression;
		}

		auto* Expression = Cast<UMaterialExpressionTextureObjectParameter>(
			CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionTextureObjectParameter::StaticClass(), -800, PositionY));
		if (!Expression)
		{
			OutError = FString::Printf(TEXT("Failed to create a texture parameter node for property '%s'."), *Property.Name);
			return nullptr;
		}

		Expression->ParameterName = FName(*GetPropertyParameterName(Property));
		if (Property.bHasDefaultValue && !Property.TextureDefaultObjectPath.IsEmpty())
		{
			UTexture* DefaultTexture = LoadObject<UTexture>(nullptr, *Property.TextureDefaultObjectPath);
			if (!DefaultTexture)
			{
				OutError = FString::Printf(
					TEXT("Texture property '%s' could not load asset '%s'."),
					*Property.Name,
					*Property.TextureDefaultObjectPath);
				return nullptr;
			}

			if (!ValidateTextureType(Property, DefaultTexture, TEXT("Texture"), OutError))
			{
				return nullptr;
			}

			Expression->Texture = DefaultTexture;
			Expression->AutoSetSampleType();
		}
		else if (const TCHAR* DefaultTexturePath = GetDefaultTextureObjectPath(Property.TextureType))
		{
			UTexture* DefaultTexture = LoadObject<UTexture>(nullptr, DefaultTexturePath);
			if (!DefaultTexture || !DoesTextureMatchType(DefaultTexture, Property.TextureType))
			{
				OutError = FString::Printf(
					TEXT("Texture property '%s' could not load default %s asset '%s'."),
					*Property.Name,
					GetTextureTypeLabel(Property.TextureType),
					DefaultTexturePath);
				return nullptr;
			}

			Expression->Texture = DefaultTexture;
			Expression->AutoSetSampleType();
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Texture property '%s' with type %s requires an explicit default asset."),
				*Property.Name,
				GetTextureTypeLabel(Property.TextureType));
			return nullptr;
		}
		if (!ApplyExpressionMetadata(Expression, Property.Metadata, OutError))
		{
			OutError = FString::Printf(TEXT("%s: %s"), *FormatMetadataContext(Property), *OutError);
			return nullptr;
		}
		return Expression;
	}

	static bool ResolveFlexibleExpressionInputValue(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const FString& InValueText,
		const TMap<FString, UMaterialExpression*>& AvailableExpressions,
		const int32 PositionY,
		UMaterialExpression*& OutExpression,
		FString& OutError)
	{
		if (TryResolvePropertyReference(InValueText, AvailableExpressions, OutExpression))
		{
			return true;
		}

		double ScalarValue = 0.0;
		if (ParseScalarLiteral(InValueText, ScalarValue))
		{
			OutExpression = CreateScalarLiteralExpressionEx(Material, MaterialFunction, ScalarValue, PositionY);
			if (!OutExpression)
			{
				OutError = TEXT("Failed to create a scalar constant expression.");
				return false;
			}
			return true;
		}

		TArray<double> Components;
		if (ParseVectorLiteral(InValueText, Components))
		{
			const int32 ComponentCount = Components.Num();
			if (ComponentCount < 2 || ComponentCount > 4)
			{
				OutError = FString::Printf(TEXT("Unsupported vector literal '%s'."), *InValueText);
				return false;
			}

			OutExpression = CreateVectorLiteralExpression(Material, MaterialFunction, Components, ComponentCount, PositionY);
			if (!OutExpression)
			{
				OutError = FString::Printf(TEXT("Failed to create a float%d constant expression."), ComponentCount);
				return false;
			}
			return true;
		}

		OutError = FString::Printf(TEXT("'%s' is not a valid property reference or literal input."), *InValueText);
		return false;
	}

	static UMaterialExpression* CreateGenericUEExpression(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const FTextShaderPropertyDefinition& Property,
		const TMap<FString, UMaterialExpression*>& AvailableExpressions,
		const int32 PositionY,
		FString& OutError)
	{
		FString ClassSpecifier = Property.UEBuiltinFunctionName;
		if (FString ExplicitClass; TryGetUEBuiltinArgument(Property, TEXT("Class"), ExplicitClass))
		{
			ClassSpecifier = ExplicitClass;
		}

		UClass* ExpressionClass = ResolveMaterialExpressionClass(ClassSpecifier);
		if (!ExpressionClass)
		{
			OutError = FString::Printf(TEXT("UE.%s for property '%s': could not resolve MaterialExpression class '%s'."),
				*Property.UEBuiltinFunctionName,
				*Property.Name,
				*ClassSpecifier);
			return nullptr;
		}

		auto* Expression = Cast<UMaterialExpression>(
			CreateOwnedMaterialExpression(Material, MaterialFunction, ExpressionClass, -800, PositionY));
		if (!Expression)
		{
			OutError = FString::Printf(TEXT("UE.%s for property '%s': failed to create '%s'."),
				*Property.UEBuiltinFunctionName,
				*Property.Name,
				*ExpressionClass->GetName());
			return nullptr;
		}

		if (UMaterialExpressionCustom* CustomExpression = Cast<UMaterialExpressionCustom>(Expression))
		{
			if (FString OutputTypeText; TryGetUEBuiltinArgument(Property, TEXT("OutputType"), OutputTypeText)
				|| TryGetUEBuiltinArgument(Property, TEXT("ResultType"), OutputTypeText))
			{
				ECustomMaterialOutputType CustomOutputType = CMOT_Float1;
				if (!TryResolveCustomOutputType(OutputTypeText, CustomOutputType))
				{
					OutError = FString::Printf(TEXT("UE.%s for property '%s': OutputType '%s' is not a valid Custom node output type."),
						*Property.UEBuiltinFunctionName,
						*Property.Name,
						*OutputTypeText);
					return nullptr;
				}
				CustomExpression->OutputType = CustomOutputType;
			}
			CustomExpression->Inputs.Reset();
			CustomExpression->AdditionalOutputs.Reset();
		}

		if (!Property.UEBuiltinArguments.Contains(UE::DreamShader::NormalizeSettingKey(TEXT("ParameterName")))
			&& FindMaterialExpressionArgumentProperty(ExpressionClass, TEXT("ParameterName")))
		{
			FString ParameterNameError;
			if (!SetExpressionParameterName(Expression, Property.Name, ParameterNameError))
			{
				OutError = FString::Printf(TEXT("UE.%s for property '%s': %s"),
					*Property.UEBuiltinFunctionName,
					*Property.Name,
					*ParameterNameError);
				return nullptr;
			}
		}

		for (const TPair<FString, FString>& Argument : Property.UEBuiltinArguments)
		{
			if (Argument.Key == UE::DreamShader::NormalizeSettingKey(TEXT("Class"))
				|| Argument.Key == UE::DreamShader::NormalizeSettingKey(TEXT("OutputType"))
				|| Argument.Key == UE::DreamShader::NormalizeSettingKey(TEXT("ResultType"))
				|| Argument.Key == UE::DreamShader::NormalizeSettingKey(TEXT("Output"))
				|| Argument.Key == UE::DreamShader::NormalizeSettingKey(TEXT("OutputName"))
				|| Argument.Key == UE::DreamShader::NormalizeSettingKey(TEXT("OutputIndex")))
			{
				continue;
			}

			FProperty* BoundProperty = FindMaterialExpressionArgumentProperty(ExpressionClass, Argument.Key);
			if (!BoundProperty)
			{
				if (UMaterialExpressionCustom* CustomExpression = Cast<UMaterialExpressionCustom>(Expression))
				{
					UMaterialExpression* InputExpression = nullptr;
					FString InputError;
					if (!ResolveFlexibleExpressionInputValue(Material, MaterialFunction, Argument.Value, AvailableExpressions, PositionY - 80, InputExpression, InputError))
					{
						OutError = FString::Printf(TEXT("UE.%s for property '%s': %s"), *Property.UEBuiltinFunctionName, *Property.Name, *InputError);
						return nullptr;
					}

					FCustomInput CustomInput;
					CustomInput.InputName = FName(*Argument.Key);
					CustomExpression->Inputs.Add(CustomInput);
					CustomExpression->Inputs.Last().Input.Expression = InputExpression;
					continue;
				}

				OutError = FString::Printf(TEXT("UE.%s for property '%s': '%s' is not a property on '%s'."),
					*Property.UEBuiltinFunctionName,
					*Property.Name,
					*Argument.Key,
					*ExpressionClass->GetName());
				return nullptr;
			}

			if (IsMaterialExpressionInputProperty(BoundProperty))
			{
				UMaterialExpression* InputExpression = nullptr;
				FString InputError;
				if (!ResolveFlexibleExpressionInputValue(Material, MaterialFunction, Argument.Value, AvailableExpressions, PositionY - 80, InputExpression, InputError))
				{
					OutError = FString::Printf(TEXT("UE.%s for property '%s': %s"), *Property.UEBuiltinFunctionName, *Property.Name, *InputError);
					return nullptr;
				}

				FExpressionInput* Input = BoundProperty->ContainerPtrToValuePtr<FExpressionInput>(Expression);
				if (!Input)
				{
					OutError = FString::Printf(TEXT("UE.%s for property '%s': failed to bind input '%s'."),
						*Property.UEBuiltinFunctionName,
						*Property.Name,
						*Argument.Key);
					return nullptr;
				}
				Input->Expression = InputExpression;
			}
			else
			{
				FString LiteralError;
				void* ValuePtr = BoundProperty->ContainerPtrToValuePtr<void>(Expression);
				if (!SetMaterialExpressionLiteralProperty(Expression, BoundProperty, ValuePtr, Argument.Value, LiteralError))
				{
					OutError = FString::Printf(TEXT("UE.%s for property '%s': %s"),
						*Property.UEBuiltinFunctionName,
						*Property.Name,
						*LiteralError);
					return nullptr;
				}
			}
		}

		if (UMaterialExpressionCustom* CustomExpression = Cast<UMaterialExpressionCustom>(Expression))
		{
			FString OutputNameText;
			int32 RequestedOutputIndex = 0;
			if (TryGetUEBuiltinArgument(Property, TEXT("Output"), OutputNameText)
				|| TryGetUEBuiltinArgument(Property, TEXT("OutputName"), OutputNameText))
			{
				OutputNameText.TrimStartAndEndInline();
				RequestedOutputIndex = 1;
			}
			else if (FString OutputIndexText; TryGetUEBuiltinArgument(Property, TEXT("OutputIndex"), OutputIndexText))
			{
				if (!ParseIntegerLiteral(OutputIndexText, RequestedOutputIndex) || RequestedOutputIndex < 0)
				{
					OutError = FString::Printf(TEXT("UE.%s for property '%s': OutputIndex is out of range for '%s'."),
						*Property.UEBuiltinFunctionName,
						*Property.Name,
						*ExpressionClass->GetName());
					return nullptr;
				}
			}

			for (int32 AdditionalOutputIndex = 0; AdditionalOutputIndex < RequestedOutputIndex; ++AdditionalOutputIndex)
			{
				FCustomOutput CustomOutput;
				CustomOutput.OutputName = FName(*(
					AdditionalOutputIndex == RequestedOutputIndex - 1 && !OutputNameText.IsEmpty()
						? OutputNameText
						: FString::Printf(TEXT("Output%d"), AdditionalOutputIndex + 1)));
				CustomOutput.OutputType = CustomExpression->OutputType;
				CustomExpression->AdditionalOutputs.Add(CustomOutput);
			}

			RebuildDreamShaderCustomOutputs(CustomExpression);
		}

		if (!ApplyExpressionMetadata(Expression, Property.Metadata, OutError))
		{
			OutError = FString::Printf(TEXT("UE.%s for property '%s': %s"),
				*Property.UEBuiltinFunctionName,
				*Property.Name,
				*OutError);
			return nullptr;
		}
		return Expression;
	}

	static UMaterialExpression* CreateUEBuiltinExpression(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const FTextShaderPropertyDefinition& Property,
		const TMap<FString, UMaterialExpression*>& AvailableExpressions,
		const int32 PositionY,
		FString& OutError)
	{
		const auto MakeError = [&Property](const FString& Message)
		{
			return FString::Printf(TEXT("UE.%s for property '%s': %s"), *Property.UEBuiltinFunctionName, *Property.Name, *Message);
		};

		if (Property.UEBuiltinFunctionName.Equals(TEXT("CollectionParam"), ESearchCase::IgnoreCase)
			|| Property.UEBuiltinFunctionName.Equals(TEXT("CollectionParameter"), ESearchCase::IgnoreCase))
		{
			static const TCHAR* AllowedArguments[] =
			{
				TEXT("Collection"),
				TEXT("Asset"),
				TEXT("Parameter"),
				TEXT("ParameterName"),
				TEXT("OutputType"),
				TEXT("ResultType"),
			};
			if (!ValidateUEBuiltinArgumentNames(Property, AllowedArguments, OutError))
			{
				return nullptr;
			}

			FString CollectionText;
			if (!TryGetUEBuiltinArgument(Property, TEXT("Collection"), CollectionText))
			{
				TryGetUEBuiltinArgument(Property, TEXT("Asset"), CollectionText);
			}
			if (CollectionText.IsEmpty())
			{
				OutError = MakeError(TEXT("CollectionParam requires Collection=Path(...)."));
				return nullptr;
			}

			FString CollectionObjectPath;
			if (!TryResolveDreamShaderAssetReference(CollectionText, CollectionObjectPath, OutError))
			{
				OutError = MakeError(FString::Printf(TEXT("Collection is invalid: %s"), *OutError));
				return nullptr;
			}

			UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr, *CollectionObjectPath);
			if (!Collection)
			{
				OutError = MakeError(FString::Printf(TEXT("Could not load MaterialParameterCollection '%s'."), *CollectionObjectPath));
				return nullptr;
			}

			FString ParameterText;
			if (!TryGetUEBuiltinArgument(Property, TEXT("Parameter"), ParameterText))
			{
				TryGetUEBuiltinArgument(Property, TEXT("ParameterName"), ParameterText);
			}
			if (ParameterText.IsEmpty())
			{
				OutError = MakeError(TEXT("CollectionParam requires Parameter=\"Name\"."));
				return nullptr;
			}

			const FName ParameterName(*ParameterText);
			if (!Collection->GetScalarParameterByName(ParameterName) && !Collection->GetVectorParameterByName(ParameterName))
			{
				OutError = MakeError(FString::Printf(TEXT("Collection '%s' does not contain parameter '%s'."), *CollectionObjectPath, *ParameterText));
				return nullptr;
			}

			auto* Expression = Cast<UMaterialExpressionCollectionParameter>(
				CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionCollectionParameter::StaticClass(), -800, PositionY));
			if (!Expression)
			{
				OutError = MakeError(TEXT("Failed to create the native CollectionParameter node."));
				return nullptr;
			}

			Expression->Collection = Collection;
			Expression->ParameterName = ParameterName;
			Expression->ParameterId = Collection->GetParameterId(ParameterName);
#if DREAMSHADER_UE_VERSION_AT_LEAST(5, 7)
			if (!Expression->ExpressionGUID.IsValid())
			{
				Expression->ExpressionGUID = FGuid::NewGuid();
			}
#endif
			if (!ApplyExpressionMetadata(Expression, Property.Metadata, OutError))
			{
				OutError = MakeError(OutError);
				return nullptr;
			}
			return Expression;
		}

		if (Property.UEBuiltinFunctionName.Equals(TEXT("TexCoord"), ESearchCase::IgnoreCase))
		{
			static const TCHAR* AllowedArguments[] =
			{
				TEXT("Index"),
				TEXT("CoordinateIndex"),
				TEXT("UTiling"),
				TEXT("VTiling"),
				TEXT("UnMirrorU"),
				TEXT("UnMirrorV"),
			};
			if (!ValidateUEBuiltinArgumentNames(Property, AllowedArguments, OutError))
			{
				return nullptr;
			}

			auto* Expression = Cast<UMaterialExpressionTextureCoordinate>(
				CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionTextureCoordinate::StaticClass(), -800, PositionY));
			if (!Expression)
			{
				OutError = MakeError(TEXT("Failed to create the native TexCoord node."));
				return nullptr;
			}

			FString IndexValue;
			const bool bHasIndex = TryGetUEBuiltinArgument(Property, TEXT("Index"), IndexValue);
			FString CoordinateIndexValue;
			const bool bHasCoordinateIndex = TryGetUEBuiltinArgument(Property, TEXT("CoordinateIndex"), CoordinateIndexValue);
			if (bHasIndex && bHasCoordinateIndex)
			{
				OutError = MakeError(TEXT("Use either Index or CoordinateIndex, not both."));
				return nullptr;
			}

			if (bHasIndex || bHasCoordinateIndex)
			{
				int32 ParsedIndex = 0;
				const FString& ValueToParse = bHasIndex ? IndexValue : CoordinateIndexValue;
				if (!ParseIntegerLiteral(ValueToParse, ParsedIndex) || ParsedIndex < 0)
				{
					OutError = MakeError(FString::Printf(TEXT("'%s' is not a valid non-negative UV channel index."), *ValueToParse));
					return nullptr;
				}

				Expression->CoordinateIndex = ParsedIndex;
			}

			if (FString TilingValue; TryGetUEBuiltinArgument(Property, TEXT("UTiling"), TilingValue))
			{
				double ParsedTiling = 1.0;
				if (!ParseScalarLiteral(TilingValue, ParsedTiling))
				{
					OutError = MakeError(FString::Printf(TEXT("UTiling value '%s' is invalid."), *TilingValue));
					return nullptr;
				}
				Expression->UTiling = static_cast<float>(ParsedTiling);
			}

			if (FString TilingValue; TryGetUEBuiltinArgument(Property, TEXT("VTiling"), TilingValue))
			{
				double ParsedTiling = 1.0;
				if (!ParseScalarLiteral(TilingValue, ParsedTiling))
				{
					OutError = MakeError(FString::Printf(TEXT("VTiling value '%s' is invalid."), *TilingValue));
					return nullptr;
				}
				Expression->VTiling = static_cast<float>(ParsedTiling);
			}

			if (FString FlagValue; TryGetUEBuiltinArgument(Property, TEXT("UnMirrorU"), FlagValue))
			{
				bool bParsedFlag = false;
				if (!ParseBooleanLiteral(FlagValue, bParsedFlag))
				{
					OutError = MakeError(FString::Printf(TEXT("UnMirrorU value '%s' is invalid."), *FlagValue));
					return nullptr;
				}
				Expression->UnMirrorU = bParsedFlag ? 1U : 0U;
			}

			if (FString FlagValue; TryGetUEBuiltinArgument(Property, TEXT("UnMirrorV"), FlagValue))
			{
				bool bParsedFlag = false;
				if (!ParseBooleanLiteral(FlagValue, bParsedFlag))
				{
					OutError = MakeError(FString::Printf(TEXT("UnMirrorV value '%s' is invalid."), *FlagValue));
					return nullptr;
				}
				Expression->UnMirrorV = bParsedFlag ? 1U : 0U;
			}

			return Expression;
		}

		if (Property.UEBuiltinFunctionName.Equals(TEXT("Time"), ESearchCase::IgnoreCase))
		{
			static const TCHAR* AllowedArguments[] =
			{
				TEXT("IgnorePause"),
				TEXT("Period"),
			};
			if (!ValidateUEBuiltinArgumentNames(Property, AllowedArguments, OutError))
			{
				return nullptr;
			}

			auto* Expression = Cast<UMaterialExpressionTime>(
				CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionTime::StaticClass(), -800, PositionY));
			if (!Expression)
			{
				OutError = MakeError(TEXT("Failed to create the native Time node."));
				return nullptr;
			}

			if (FString FlagValue; TryGetUEBuiltinArgument(Property, TEXT("IgnorePause"), FlagValue))
			{
				bool bParsedFlag = false;
				if (!ParseBooleanLiteral(FlagValue, bParsedFlag))
				{
					OutError = MakeError(FString::Printf(TEXT("IgnorePause value '%s' is invalid."), *FlagValue));
					return nullptr;
				}
				Expression->bIgnorePause = bParsedFlag ? 1U : 0U;
			}

			if (FString PeriodValue; TryGetUEBuiltinArgument(Property, TEXT("Period"), PeriodValue))
			{
				double ParsedPeriod = 0.0;
				if (!ParseScalarLiteral(PeriodValue, ParsedPeriod) || ParsedPeriod < 0.0)
				{
					OutError = MakeError(FString::Printf(TEXT("Period value '%s' is invalid."), *PeriodValue));
					return nullptr;
				}
				Expression->bOverride_Period = true;
				Expression->Period = static_cast<float>(ParsedPeriod);
			}

			return Expression;
		}

		if (Property.UEBuiltinFunctionName.Equals(TEXT("Panner"), ESearchCase::IgnoreCase))
		{
			static const TCHAR* AllowedArguments[] =
			{
				TEXT("Coordinate"),
				TEXT("Time"),
				TEXT("Speed"),
				TEXT("SpeedX"),
				TEXT("SpeedY"),
				TEXT("ConstCoordinate"),
				TEXT("FractionalPart"),
			};
			if (!ValidateUEBuiltinArgumentNames(Property, AllowedArguments, OutError))
			{
				return nullptr;
			}

			auto* Expression = Cast<UMaterialExpressionPanner>(
				CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionPanner::StaticClass(), -800, PositionY));
			if (!Expression)
			{
				OutError = MakeError(TEXT("Failed to create the native Panner node."));
				return nullptr;
			}

			if (FString CoordinateValue; TryGetUEBuiltinArgument(Property, TEXT("Coordinate"), CoordinateValue))
			{
				int32 ParsedCoordinateIndex = 0;
				if (ParseIntegerLiteral(CoordinateValue, ParsedCoordinateIndex))
				{
					Expression->ConstCoordinate = ParsedCoordinateIndex;
				}
				else
				{
					UMaterialExpression* CoordinateExpression = nullptr;
					FString InputError;
					if (!ResolveExpressionInputValue(Material, MaterialFunction, CoordinateValue, AvailableExpressions, 2, PositionY - 80, CoordinateExpression, InputError))
					{
						OutError = MakeError(FString::Printf(TEXT("Coordinate input is invalid. %s"), *InputError));
						return nullptr;
					}
					Expression->Coordinate.Expression = CoordinateExpression;
				}
			}

			if (FString CoordinateValue; TryGetUEBuiltinArgument(Property, TEXT("ConstCoordinate"), CoordinateValue))
			{
				int32 ParsedCoordinateIndex = 0;
				if (!ParseIntegerLiteral(CoordinateValue, ParsedCoordinateIndex) || ParsedCoordinateIndex < 0)
				{
					OutError = MakeError(FString::Printf(TEXT("ConstCoordinate value '%s' is invalid."), *CoordinateValue));
					return nullptr;
				}
				Expression->ConstCoordinate = ParsedCoordinateIndex;
			}

			if (FString TimeValue; TryGetUEBuiltinArgument(Property, TEXT("Time"), TimeValue))
			{
				UMaterialExpression* TimeExpression = nullptr;
				FString InputError;
				if (!ResolveExpressionInputValue(Material, MaterialFunction, TimeValue, AvailableExpressions, 1, PositionY - 40, TimeExpression, InputError))
				{
					OutError = MakeError(FString::Printf(TEXT("Time input is invalid. %s"), *InputError));
					return nullptr;
				}
				Expression->Time.Expression = TimeExpression;
			}

			if (FString SpeedValue; TryGetUEBuiltinArgument(Property, TEXT("Speed"), SpeedValue))
			{
				UMaterialExpression* SpeedExpression = nullptr;
				FString InputError;
				if (!ResolveExpressionInputValue(Material, MaterialFunction, SpeedValue, AvailableExpressions, 2, PositionY + 40, SpeedExpression, InputError))
				{
					OutError = MakeError(FString::Printf(TEXT("Speed input is invalid. %s"), *InputError));
					return nullptr;
				}
				Expression->Speed.Expression = SpeedExpression;
			}

			if (FString SpeedValue; TryGetUEBuiltinArgument(Property, TEXT("SpeedX"), SpeedValue))
			{
				double ParsedSpeed = 0.0;
				if (!ParseScalarLiteral(SpeedValue, ParsedSpeed))
				{
					OutError = MakeError(FString::Printf(TEXT("SpeedX value '%s' is invalid."), *SpeedValue));
					return nullptr;
				}
				Expression->SpeedX = static_cast<float>(ParsedSpeed);
			}

			if (FString SpeedValue; TryGetUEBuiltinArgument(Property, TEXT("SpeedY"), SpeedValue))
			{
				double ParsedSpeed = 0.0;
				if (!ParseScalarLiteral(SpeedValue, ParsedSpeed))
				{
					OutError = MakeError(FString::Printf(TEXT("SpeedY value '%s' is invalid."), *SpeedValue));
					return nullptr;
				}
				Expression->SpeedY = static_cast<float>(ParsedSpeed);
			}

			if (FString FlagValue; TryGetUEBuiltinArgument(Property, TEXT("FractionalPart"), FlagValue))
			{
				bool bParsedFlag = false;
				if (!ParseBooleanLiteral(FlagValue, bParsedFlag))
				{
					OutError = MakeError(FString::Printf(TEXT("FractionalPart value '%s' is invalid."), *FlagValue));
					return nullptr;
				}
				Expression->bFractionalPart = bParsedFlag;
			}

			return Expression;
		}

		if (Property.UEBuiltinFunctionName.Equals(TEXT("WorldPosition"), ESearchCase::IgnoreCase))
		{
			static const TCHAR* AllowedArguments[] =
			{
				TEXT("ShaderOffsets"),
			};
			if (!ValidateUEBuiltinArgumentNames(Property, AllowedArguments, OutError))
			{
				return nullptr;
			}

			auto* Expression = Cast<UMaterialExpressionWorldPosition>(
				CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionWorldPosition::StaticClass(), -800, PositionY));
			if (!Expression)
			{
				OutError = MakeError(TEXT("Failed to create the native WorldPosition node."));
				return nullptr;
			}

			if (FString OffsetValue; TryGetUEBuiltinArgument(Property, TEXT("ShaderOffsets"), OffsetValue))
			{
				EWorldPositionIncludedOffsets ParsedOffset = WPT_Default;
				if (!TryResolveWorldPositionShaderOffset(OffsetValue, ParsedOffset))
				{
					OutError = MakeError(FString::Printf(TEXT("ShaderOffsets value '%s' is invalid."), *OffsetValue));
					return nullptr;
				}
				Expression->WorldPositionShaderOffset = ParsedOffset;
			}

			return Expression;
		}

		if (Property.UEBuiltinFunctionName.Equals(TEXT("ObjectPositionWS"), ESearchCase::IgnoreCase))
		{
			static const TCHAR* AllowedArguments[] =
			{
				TEXT("Origin"),
			};
			if (!ValidateUEBuiltinArgumentNames(Property, AllowedArguments, OutError))
			{
				return nullptr;
			}

			UClass* ObjectPositionClass = GetDreamShaderObjectPositionExpressionClass();
			auto* Expression = ObjectPositionClass
				? static_cast<UMaterialExpressionObjectPositionWS*>(CreateOwnedMaterialExpression(Material, MaterialFunction, ObjectPositionClass, -800, PositionY))
				: nullptr;
			if (!Expression)
			{
				OutError = MakeError(TEXT("Failed to create the native ObjectPositionWS node."));
				return nullptr;
			}

			if (FString OriginValue; TryGetUEBuiltinArgument(Property, TEXT("Origin"), OriginValue))
			{
				EPositionOrigin ParsedOrigin = EPositionOrigin::Absolute;
				if (!TryResolvePositionOrigin(OriginValue, ParsedOrigin))
				{
					OutError = MakeError(FString::Printf(TEXT("Origin value '%s' is invalid."), *OriginValue));
					return nullptr;
				}
				Expression->OriginType = ParsedOrigin;
			}

			return Expression;
		}

		if (Property.UEBuiltinFunctionName.Equals(TEXT("CameraVectorWS"), ESearchCase::IgnoreCase))
		{
			if (!Property.UEBuiltinArguments.IsEmpty())
			{
				OutError = MakeError(TEXT("CameraVectorWS does not take any arguments."));
				return nullptr;
			}

			UMaterialExpression* Expression =
				Cast<UMaterialExpressionCameraVectorWS>(
					CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionCameraVectorWS::StaticClass(), -800, PositionY));
			if (!Expression)
			{
				OutError = MakeError(TEXT("Failed to create the native CameraVectorWS node."));
			}
			return Expression;
		}

		if (Property.UEBuiltinFunctionName.Equals(TEXT("ScreenPosition"), ESearchCase::IgnoreCase))
		{
			if (!Property.UEBuiltinArguments.IsEmpty())
			{
				OutError = MakeError(TEXT("ScreenPosition does not take any arguments."));
				return nullptr;
			}

			UClass* ScreenPositionClass = GetDreamShaderScreenPositionExpressionClass();
			UMaterialExpression* Expression = ScreenPositionClass
				? CreateOwnedMaterialExpression(Material, MaterialFunction, ScreenPositionClass, -800, PositionY)
				: nullptr;
			if (!Expression)
			{
				OutError = MakeError(TEXT("Failed to create the native ScreenPosition node."));
			}
			return Expression;
		}

		if (Property.UEBuiltinFunctionName.Equals(TEXT("VertexColor"), ESearchCase::IgnoreCase))
		{
			if (!Property.UEBuiltinArguments.IsEmpty())
			{
				OutError = MakeError(TEXT("VertexColor does not take any arguments."));
				return nullptr;
			}

			UMaterialExpression* Expression =
				Cast<UMaterialExpressionVertexColor>(
					CreateOwnedMaterialExpression(Material, MaterialFunction, UMaterialExpressionVertexColor::StaticClass(), -800, PositionY));
			if (!Expression)
			{
				OutError = MakeError(TEXT("Failed to create the native VertexColor node."));
			}
			return Expression;
		}

		if (Property.UEBuiltinArguments.Contains(UE::DreamShader::NormalizeSettingKey(TEXT("OutputType")))
			|| Property.UEBuiltinArguments.Contains(UE::DreamShader::NormalizeSettingKey(TEXT("ResultType"))))
		{
			return CreateGenericUEExpression(Material, MaterialFunction, Property, AvailableExpressions, PositionY, OutError);
		}

		OutError = MakeError(TEXT("This builtin is not implemented by the material generator yet. For generic MaterialExpression support, add OutputType=\"float1/2/3/4/Texture2D/TextureCube/Texture2DArray/VolumeTexture\"."));
		return nullptr;
	}

	UMaterialExpression* CreatePropertyExpression(
		UMaterial* Material,
		UMaterialFunction* MaterialFunction,
		const FTextShaderPropertyDefinition& Property,
		const TMap<FString, UMaterialExpression*>& AvailableExpressions,
		const int32 PositionY,
		FString& OutError)
	{
		if (Property.bConst)
		{
			return CreateConstPropertyExpression(Material, MaterialFunction, Property, PositionY, OutError);
		}

		if (Property.Source == ETextShaderPropertySource::UEBuiltin)
		{
			return CreateUEBuiltinExpression(Material, MaterialFunction, Property, AvailableExpressions, PositionY, OutError);
		}

		UMaterialExpression* Expression = CreateParameterExpression(Material, MaterialFunction, Property, PositionY, OutError);
		if (!Expression)
		{
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Failed to create a parameter node for property '%s'."), *Property.Name);
			}
		}
		return Expression;
	}

	UMaterialExpression* CreatePropertyExpression(
		UMaterial* Material,
		const FTextShaderPropertyDefinition& Property,
		const TMap<FString, UMaterialExpression*>& AvailableExpressions,
		const int32 PositionY,
		FString& OutError)
	{
		return CreatePropertyExpression(Material, nullptr, Property, AvailableExpressions, PositionY, OutError);
	}

}
