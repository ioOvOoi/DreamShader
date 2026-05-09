#include "DreamShaderMaterialGeneratorCodeShared.h"

namespace UE::DreamShader::Editor::Private
{
	bool FCodeGraphBuilder::TryResolveVectorTransformBasis(const FString& InText, EMaterialVectorCoordTransformSource& OutSource) const
	{
		FString Value = UE::DreamShader::NormalizeSettingKey(InText);
		if (Value == TEXT("tangent"))
		{
			OutSource = TRANSFORMSOURCE_Tangent;
			return true;
		}
		if (Value == TEXT("local"))
		{
			OutSource = TRANSFORMSOURCE_Local;
			return true;
		}
		if (Value == TEXT("world") || Value == TEXT("absoluteworld"))
		{
			OutSource = TRANSFORMSOURCE_World;
			return true;
		}
		if (Value == TEXT("view"))
		{
			OutSource = TRANSFORMSOURCE_View;
			return true;
		}
		if (Value == TEXT("camera"))
		{
			OutSource = TRANSFORMSOURCE_Camera;
			return true;
		}
		if (Value == TEXT("instance") || Value == TEXT("particle") || Value == TEXT("instanceparticle"))
		{
			OutSource = TRANSFORMSOURCE_Instance;
			return true;
		}
		return false;
	}

	bool FCodeGraphBuilder::TryResolveVectorTransformTarget(const FString& InText, EMaterialVectorCoordTransform& OutTarget) const
	{
		FString Value = UE::DreamShader::NormalizeSettingKey(InText);
		if (Value == TEXT("tangent"))
		{
			OutTarget = TRANSFORM_Tangent;
			return true;
		}
		if (Value == TEXT("local"))
		{
			OutTarget = TRANSFORM_Local;
			return true;
		}
		if (Value == TEXT("world") || Value == TEXT("absoluteworld"))
		{
			OutTarget = TRANSFORM_World;
			return true;
		}
		if (Value == TEXT("view"))
		{
			OutTarget = TRANSFORM_View;
			return true;
		}
		if (Value == TEXT("camera"))
		{
			OutTarget = TRANSFORM_Camera;
			return true;
		}
		if (Value == TEXT("instance") || Value == TEXT("particle") || Value == TEXT("instanceparticle"))
		{
			OutTarget = TRANSFORM_Instance;
			return true;
		}
		return false;
	}

	bool FCodeGraphBuilder::TryResolvePositionTransformBasis(const FString& InText, EMaterialPositionTransformSource& OutBasis) const
	{
		FString Value = UE::DreamShader::NormalizeSettingKey(InText);
		if (Value == TEXT("local"))
		{
			OutBasis = TRANSFORMPOSSOURCE_Local;
			return true;
		}
		if (Value == TEXT("world") || Value == TEXT("absoluteworld"))
		{
			OutBasis = TRANSFORMPOSSOURCE_World;
			return true;
		}
		if (Value == TEXT("periodicworld"))
		{
			OutBasis = TRANSFORMPOSSOURCE_PeriodicWorld;
			return true;
		}
		if (Value == TEXT("translatedworld") || Value == TEXT("camerarelativeworld"))
		{
			OutBasis = TRANSFORMPOSSOURCE_TranslatedWorld;
			return true;
		}
		if (Value == TEXT("firstperson") || Value == TEXT("firstpersontranslatedworld"))
		{
			OutBasis = TRANSFORMPOSSOURCE_FirstPersonTranslatedWorld;
			return true;
		}
		if (Value == TEXT("view"))
		{
			OutBasis = TRANSFORMPOSSOURCE_View;
			return true;
		}
		if (Value == TEXT("camera"))
		{
			OutBasis = TRANSFORMPOSSOURCE_Camera;
			return true;
		}
		if (Value == TEXT("instance") || Value == TEXT("particle") || Value == TEXT("instanceparticle"))
		{
			OutBasis = TRANSFORMPOSSOURCE_Instance;
			return true;
		}
		return false;
	}

	bool FCodeGraphBuilder::EvaluateUEBuiltinCall(
		const FString& CalleeName,
		const TArray<FCodeCallArgument>& Arguments,
		FCodeValue& OutValue,
		FString& OutError)
	{
		const FString FunctionName = CalleeName.Mid(3);

		auto HandleIntegerLiteralArgument = [&](const TCHAR* ArgumentName, int32& TargetField) -> bool
		{
			const FCodeCallArgument* Argument = FindNamedArgument(Arguments, ArgumentName);
			if (!Argument)
			{
				return true;
			}

			if (!TryExtractIntegerLiteral(Argument->Expression, TargetField))
			{
				OutError = FString::Printf(TEXT("UE.%s %s must be an integer literal."), *FunctionName, ArgumentName);
				return false;
			}

			return true;
		};

		auto HandleScalarLiteralArgument = [&](const TCHAR* ArgumentName, float& TargetField) -> bool
		{
			const FCodeCallArgument* Argument = FindNamedArgument(Arguments, ArgumentName);
			if (!Argument)
			{
				return true;
			}

			double ParsedValue = 0.0;
			if (!TryExtractScalarLiteral(Argument->Expression, ParsedValue))
			{
				OutError = FString::Printf(TEXT("UE.%s %s must be a numeric literal."), *FunctionName, ArgumentName);
				return false;
			}

			TargetField = static_cast<float>(ParsedValue);
			return true;
		};

		auto HandleBooleanLiteralArgument = [&](const TCHAR* ArgumentName, TFunctionRef<void(bool)> ApplyValue) -> bool
		{
			const FCodeCallArgument* Argument = FindNamedArgument(Arguments, ArgumentName);
			if (!Argument)
			{
				return true;
			}

			bool bParsedValue = false;
			if (!TryExtractBooleanLiteral(Argument->Expression, bParsedValue))
			{
				OutError = FString::Printf(TEXT("UE.%s %s must be a boolean literal."), *FunctionName, ArgumentName);
				return false;
			}

			ApplyValue(bParsedValue);
			return true;
		};

		auto HandleOptionalTextLiteralArgument = [&](const TCHAR* ArgumentName, FString& TargetValue) -> bool
		{
			const FCodeCallArgument* Argument = FindNamedArgument(Arguments, ArgumentName);
			if (!Argument)
			{
				return true;
			}

			if (!TryExtractTextLiteral(Argument->Expression, TargetValue))
			{
				OutError = FString::Printf(TEXT("UE.%s %s must be a text value."), *FunctionName, ArgumentName);
				return false;
			}

			return true;
		};

		auto HandleOptionalInputArgument = [&](const TCHAR* ArgumentName, FExpressionInput& TargetInput) -> bool
		{
			const FCodeCallArgument* Argument = FindNamedArgument(Arguments, ArgumentName);
			if (!Argument)
			{
				return true;
			}

			FCodeValue InputValue;
			if (!EvaluateExpression(Argument->Expression, InputValue, OutError))
			{
				return false;
			}

			ConnectCodeValueToInput(TargetInput, InputValue);
			return true;
		};

		auto HandleRequiredInputArgument = [&](const TCHAR* ArgumentName, const int32 PositionalIndex, FExpressionInput& TargetInput) -> bool
		{
			const FCodeCallArgument* Argument = FindNamedArgument(Arguments, ArgumentName);
			if (!Argument)
			{
				Argument = FindPositionalArgument(Arguments, PositionalIndex);
			}
			if (!Argument)
			{
				OutError = FString::Printf(TEXT("UE.%s requires parameter: %s"), *FunctionName, ArgumentName);
				return false;
			}

			FCodeValue InputValue;
			if (!EvaluateExpression(Argument->Expression, InputValue, OutError))
			{
				return false;
			}

			ConnectCodeValueToInput(TargetInput, InputValue);
			return true;
		};

		if (FunctionName.Equals(TEXT("StaticSwitchParameter"), ESearchCase::IgnoreCase))
		{
			const FCodeCallArgument* NameArgument = FindNamedArgument(Arguments, TEXT("Name"));
			if (!NameArgument)
			{
				NameArgument = FindNamedArgument(Arguments, TEXT("ParameterName"));
			}
			if (!NameArgument)
			{
				OutError = TEXT("UE.StaticSwitchParameter requires Name=\"ParameterName\".");
				return false;
			}

			FString ParameterName;
			if (!TryExtractTextLiteral(NameArgument->Expression, ParameterName) || ParameterName.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("UE.StaticSwitchParameter Name must be a text value.");
				return false;
			}

			FTextShaderPropertyDefinition SwitchProperty;
			SwitchProperty.Name = ParameterName.TrimStartAndEnd();
			SwitchProperty.ParameterNodeType = TEXT("StaticSwitchParameter");
			if (const FCodeCallArgument* DefaultArgument = FindNamedArgument(Arguments, TEXT("Default")))
			{
				bool bDefault = false;
				if (!TryExtractBooleanLiteral(DefaultArgument->Expression, bDefault))
				{
					OutError = TEXT("UE.StaticSwitchParameter Default must be true or false.");
					return false;
				}
				SwitchProperty.bHasDefaultValue = true;
				SwitchProperty.ScalarDefaultValue = bDefault ? 1.0 : 0.0;
			}
			if (const FCodeCallArgument* GroupArgument = FindNamedArgument(Arguments, TEXT("Group")))
			{
				(void)TryExtractTextLiteral(GroupArgument->Expression, SwitchProperty.Metadata.Group);
			}
			if (const FCodeCallArgument* DescriptionArgument = FindNamedArgument(Arguments, TEXT("Description")))
			{
				(void)TryExtractTextLiteral(DescriptionArgument->Expression, SwitchProperty.Metadata.Description);
			}
			if (const FCodeCallArgument* SortArgument = FindNamedArgument(Arguments, TEXT("SortPriority")))
			{
				int32 SortPriority = 32;
				if (!TryExtractIntegerLiteral(SortArgument->Expression, SortPriority))
				{
					OutError = TEXT("UE.StaticSwitchParameter SortPriority must be an integer literal.");
					return false;
				}
				SwitchProperty.Metadata.bHasSortPriority = true;
				SwitchProperty.Metadata.SortPriority = SortPriority;
			}

			return EvaluateStaticSwitchParameterCall(SwitchProperty, Arguments, OutValue, OutError);
		}

		if (FunctionName.Equals(TEXT("CollectionParam"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("CollectionParameter"), ESearchCase::IgnoreCase))
		{
			const FCodeCallArgument* CollectionArgument = FindNamedArgument(Arguments, TEXT("Collection"));
			if (!CollectionArgument)
			{
				CollectionArgument = FindNamedArgument(Arguments, TEXT("Asset"));
			}
			if (!CollectionArgument)
			{
				OutError = TEXT("UE.CollectionParam requires Collection=Path(...).");
				return false;
			}

			FString CollectionText;
			if (!TryExtractAssetReferenceText(CollectionArgument->Expression, CollectionText))
			{
				OutError = TEXT("UE.CollectionParam Collection must be Path(...) or an Unreal object path.");
				return false;
			}

			FString CollectionObjectPath;
			if (!TryResolveDreamShaderAssetReference(CollectionText, CollectionObjectPath, OutError))
			{
				OutError = FString::Printf(TEXT("UE.CollectionParam Collection is invalid: %s"), *OutError);
				return false;
			}

			UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr, *CollectionObjectPath);
			if (!Collection)
			{
				OutError = FString::Printf(TEXT("UE.CollectionParam could not load MaterialParameterCollection '%s'."), *CollectionObjectPath);
				return false;
			}

			const FCodeCallArgument* ParameterArgument = FindNamedArgument(Arguments, TEXT("Parameter"));
			if (!ParameterArgument)
			{
				ParameterArgument = FindNamedArgument(Arguments, TEXT("ParameterName"));
			}
			if (!ParameterArgument)
			{
				OutError = TEXT("UE.CollectionParam requires Parameter=\"Name\".");
				return false;
			}

			FString ParameterText;
			if (!TryExtractTextLiteral(ParameterArgument->Expression, ParameterText) || ParameterText.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("UE.CollectionParam Parameter must be a text value.");
				return false;
			}

			const FName ParameterName(*ParameterText.TrimStartAndEnd());
			const bool bIsScalarParameter = Collection->GetScalarParameterByName(ParameterName) != nullptr;
			const bool bIsVectorParameter = Collection->GetVectorParameterByName(ParameterName) != nullptr;
			if (!bIsScalarParameter && !bIsVectorParameter)
			{
				OutError = FString::Printf(
					TEXT("UE.CollectionParam collection '%s' does not contain parameter '%s'."),
					*CollectionObjectPath,
					*ParameterText);
				return false;
			}

			auto* Expression = Cast<UMaterialExpressionCollectionParameter>(
				CreateExpression(UMaterialExpressionCollectionParameter::StaticClass(), 600, ConsumeNodeY()));
			if (!Expression)
			{
				OutError = TEXT("Failed to create UE.CollectionParam node.");
				return false;
			}

			Expression->Collection = Collection;
			Expression->ParameterName = ParameterName;
			Expression->ParameterId = Collection->GetParameterId(ParameterName);
			if (!Expression->ExpressionGUID.IsValid())
			{
				Expression->ExpressionGUID = FGuid::NewGuid();
			}

			FString GroupText;
			if (HandleOptionalTextLiteralArgument(TEXT("Group"), GroupText) && !GroupText.IsEmpty())
			{
				Expression->Group = FName(*GroupText);
			}
			else if (!OutError.IsEmpty())
			{
				return false;
			}

			if (const FCodeCallArgument* SortArgument = FindNamedArgument(Arguments, TEXT("SortPriority")))
			{
				int32 SortPriority = 32;
				if (!TryExtractIntegerLiteral(SortArgument->Expression, SortPriority))
				{
					OutError = TEXT("UE.CollectionParam SortPriority must be an integer literal.");
					return false;
				}
				Expression->SortPriority = SortPriority;
			}

			FString DescriptionText;
			if (HandleOptionalTextLiteralArgument(TEXT("Description"), DescriptionText) && !DescriptionText.IsEmpty())
			{
				Expression->Desc = DescriptionText;
			}
			else if (!OutError.IsEmpty())
			{
				return false;
			}

			OutValue.Expression = Expression;
			OutValue.OutputIndex = 0;
			OutValue.ComponentCount = bIsVectorParameter ? 4 : 1;
			OutValue.bIsTextureObject = false;
			OutValue.bIsMaterialAttributes = false;
			OutValue.bIsSubstrate = false;
			return true;
		}

		struct FUEBuiltinDescriptor
		{
			const TCHAR* Name = TEXT("");
			TSubclassOf<UMaterialExpression> ExpressionClass;
			int32 OutputComponents = 1;
			TFunction<bool(UMaterialExpression*, FString&)> Configure;
		};

		auto TryEvaluateRegisteredBuiltin = [&](const FUEBuiltinDescriptor& Builtin) -> bool
		{
			UMaterialExpression* Expression = CreateExpression(Builtin.ExpressionClass, 600, ConsumeNodeY());
			if (!Expression)
			{
				OutError = FString::Printf(TEXT("Failed to create UE.%s."), Builtin.Name);
				return false;
			}

			if (Builtin.Configure && !Builtin.Configure(Expression, OutError))
			{
				return false;
			}

			OutValue.Expression = Expression;
			OutValue.OutputIndex = 0;
			OutValue.ComponentCount = Builtin.OutputComponents;
			OutValue.bIsTextureObject = false;
			OutValue.bIsMaterialAttributes = false;
			OutValue.bIsSubstrate = false;
			return true;
		};

		// Register explicit DreamShader UE.* sugar here. Anything not listed falls back
		// to the reflected generic MaterialExpression path below.
		TArray<FUEBuiltinDescriptor> Builtins;
		Builtins.Reserve(10);

		Builtins.Add({
			TEXT("TexCoord"),
			UMaterialExpressionTextureCoordinate::StaticClass(),
			2,
			[&](UMaterialExpression* RawExpression, FString&) -> bool
			{
				auto* Expression = CastChecked<UMaterialExpressionTextureCoordinate>(RawExpression);
				return HandleIntegerLiteralArgument(TEXT("Index"), Expression->CoordinateIndex)
					&& HandleScalarLiteralArgument(TEXT("UTiling"), Expression->UTiling)
					&& HandleScalarLiteralArgument(TEXT("VTiling"), Expression->VTiling)
					&& HandleBooleanLiteralArgument(TEXT("UnMirrorU"), [&](const bool bValue) { Expression->UnMirrorU = bValue ? 1U : 0U; })
					&& HandleBooleanLiteralArgument(TEXT("UnMirrorV"), [&](const bool bValue) { Expression->UnMirrorV = bValue ? 1U : 0U; });
			}
		});

		Builtins.Add({
			TEXT("Time"),
			UMaterialExpressionTime::StaticClass(),
			1,
			[&](UMaterialExpression* RawExpression, FString& Error) -> bool
			{
				auto* Expression = CastChecked<UMaterialExpressionTime>(RawExpression);
				if (const FCodeCallArgument* Argument = FindNamedArgument(Arguments, TEXT("Period")))
				{
					double Period = 0.0;
					if (!TryExtractScalarLiteral(Argument->Expression, Period))
					{
						Error = FString::Printf(TEXT("UE.%s Period must be a numeric literal."), *FunctionName);
						return false;
					}

					Expression->bOverride_Period = true;
					Expression->Period = static_cast<float>(Period);
				}

				return HandleBooleanLiteralArgument(TEXT("IgnorePause"), [&](const bool bValue) { Expression->bIgnorePause = bValue ? 1U : 0U; });
			}
		});

		Builtins.Add({
			TEXT("Panner"),
			UMaterialExpressionPanner::StaticClass(),
			2,
			[&](UMaterialExpression* RawExpression, FString&) -> bool
			{
				auto* Expression = CastChecked<UMaterialExpressionPanner>(RawExpression);
				return HandleOptionalInputArgument(TEXT("Coordinate"), Expression->Coordinate)
					&& HandleOptionalInputArgument(TEXT("Time"), Expression->Time)
					&& HandleOptionalInputArgument(TEXT("Speed"), Expression->Speed)
					&& HandleScalarLiteralArgument(TEXT("SpeedX"), Expression->SpeedX)
					&& HandleScalarLiteralArgument(TEXT("SpeedY"), Expression->SpeedY)
					&& HandleBooleanLiteralArgument(TEXT("FractionalPart"), [&](const bool bValue) { Expression->bFractionalPart = bValue ? 1U : 0U; });
			}
		});

		Builtins.Add({ TEXT("WorldPosition"), UMaterialExpressionWorldPosition::StaticClass(), 3, {} });
		Builtins.Add({ TEXT("ObjectPositionWS"), UMaterialExpressionObjectPositionWS::StaticClass(), 3, {} });
		Builtins.Add({ TEXT("CameraVectorWS"), UMaterialExpressionCameraVectorWS::StaticClass(), 3, {} });
		Builtins.Add({ TEXT("ScreenPosition"), UMaterialExpressionScreenPosition::StaticClass(), 4, {} });
		Builtins.Add({ TEXT("VertexColor"), UMaterialExpressionVertexColor::StaticClass(), 4, {} });

		Builtins.Add({
			TEXT("TransformVector"),
			UMaterialExpressionTransform::StaticClass(),
			3,
			[&](UMaterialExpression* RawExpression, FString& Error) -> bool
			{
				auto* Expression = CastChecked<UMaterialExpressionTransform>(RawExpression);
				if (!HandleRequiredInputArgument(TEXT("Input"), 0, Expression->Input))
				{
					return false;
				}

				FString SourceText = TEXT("Tangent");
				if (!HandleOptionalTextLiteralArgument(TEXT("Source"), SourceText))
				{
					return false;
				}

				FString DestinationText = TEXT("World");
				if (!HandleOptionalTextLiteralArgument(TEXT("Destination"), DestinationText))
				{
					return false;
				}

				EMaterialVectorCoordTransformSource SourceBasis = TRANSFORMSOURCE_Tangent;
				EMaterialVectorCoordTransform DestinationBasis = TRANSFORM_World;
				if (!TryResolveVectorTransformBasis(SourceText, SourceBasis)
					|| !TryResolveVectorTransformTarget(DestinationText, DestinationBasis))
				{
					Error = TEXT("UE.TransformVector Source/Destination is invalid.");
					return false;
				}

				Expression->TransformSourceType = SourceBasis;
				Expression->TransformType = DestinationBasis;
				return true;
			}
		});

		Builtins.Add({
			TEXT("TransformPosition"),
			UMaterialExpressionTransformPosition::StaticClass(),
			3,
			[&](UMaterialExpression* RawExpression, FString& Error) -> bool
			{
				auto* Expression = CastChecked<UMaterialExpressionTransformPosition>(RawExpression);
				if (!HandleRequiredInputArgument(TEXT("Input"), 0, Expression->Input))
				{
					return false;
				}

				FString SourceText = TEXT("Local");
				if (!HandleOptionalTextLiteralArgument(TEXT("Source"), SourceText))
				{
					return false;
				}

				FString DestinationText = TEXT("World");
				if (!HandleOptionalTextLiteralArgument(TEXT("Destination"), DestinationText))
				{
					return false;
				}

				EMaterialPositionTransformSource SourceBasis = TRANSFORMPOSSOURCE_Local;
				EMaterialPositionTransformSource DestinationBasis = TRANSFORMPOSSOURCE_World;
				if (!TryResolvePositionTransformBasis(SourceText, SourceBasis)
					|| !TryResolvePositionTransformBasis(DestinationText, DestinationBasis))
				{
					Error = TEXT("UE.TransformPosition Source/Destination is invalid.");
					return false;
				}

				Expression->TransformSourceType = SourceBasis;
				Expression->TransformType = DestinationBasis;

				if (!HandleOptionalInputArgument(TEXT("PeriodicWorldTileSize"), Expression->PeriodicWorldTileSize))
				{
					return false;
				}

				return HandleOptionalInputArgument(TEXT("FirstPersonInterpolationAlpha"), Expression->FirstPersonInterpolationAlpha);
			}
		});

		for (const FUEBuiltinDescriptor& Builtin : Builtins)
		{
			if (FunctionName.Equals(Builtin.Name, ESearchCase::IgnoreCase))
			{
				return TryEvaluateRegisteredBuiltin(Builtin);
			}
		}

		static const TMap<FString, FString> SubstrateAliases =
		{
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateShadingModels")), TEXT("SubstrateShadingModels") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateSlab")), TEXT("SubstrateSlabBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateSlabBSDF")), TEXT("SubstrateSlabBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateClearCoat")), TEXT("SubstrateSimpleClearCoatBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateSimpleClearCoat")), TEXT("SubstrateSimpleClearCoatBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateSimpleClearCoatBSDF")), TEXT("SubstrateSimpleClearCoatBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateVolumetricFogCloud")), TEXT("SubstrateVolumetricFogCloudBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateVolumetricFogCloudBSDF")), TEXT("SubstrateVolumetricFogCloudBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateUnlit")), TEXT("SubstrateUnlitBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateUnlitBSDF")), TEXT("SubstrateUnlitBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateHair")), TEXT("SubstrateHairBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateHairBSDF")), TEXT("SubstrateHairBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateEye")), TEXT("SubstrateEyeBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateEyeBSDF")), TEXT("SubstrateEyeBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateSingleLayerWater")), TEXT("SubstrateSingleLayerWaterBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateSingleLayerWaterBSDF")), TEXT("SubstrateSingleLayerWaterBSDF") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateLightFunction")), TEXT("SubstrateLightFunction") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstratePostProcess")), TEXT("SubstratePostProcess") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateUI")), TEXT("SubstrateUI") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateConvertToDecal")), TEXT("SubstrateConvertToDecal") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateConvertMaterialAttributes")), TEXT("SubstrateConvertMaterialAttributes") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateHorizontalBlend")), TEXT("SubstrateHorizontalMixing") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateHorizontalMixing")), TEXT("SubstrateHorizontalMixing") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateVerticalLayer")), TEXT("SubstrateVerticalLayering") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateVerticalLayering")), TEXT("SubstrateVerticalLayering") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateAdd")), TEXT("SubstrateAdd") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateWeight")), TEXT("SubstrateWeight") },
			{ UE::DreamShader::NormalizeSettingKey(TEXT("SubstrateSelect")), TEXT("SubstrateSelect") },
		};
		const FString* SubstrateClassAlias = SubstrateAliases.Find(UE::DreamShader::NormalizeSettingKey(FunctionName));

		for (const FCodeCallArgument& Argument : Arguments)
		{
			if (!Argument.bIsNamed)
			{
				OutError = FString::Printf(TEXT("Generic UE.%s calls require named arguments."), *FunctionName);
				return false;
			}
		}

		const FCodeCallArgument* OutputTypeArgument = FindNamedArgument(Arguments, TEXT("OutputType"));
		if (!OutputTypeArgument)
		{
			OutputTypeArgument = FindNamedArgument(Arguments, TEXT("ResultType"));
		}
		if (!OutputTypeArgument && !SubstrateClassAlias)
		{
			OutError = FString::Printf(
				TEXT("Unsupported UE builtin call '%s' in Graph. For generic MaterialExpression calls, add OutputType=\"float1/2/3/4/Texture2D/Substrate\"."),
				*CalleeName);
			return false;
		}

		FString OutputTypeText = SubstrateClassAlias ? TEXT("Substrate") : FString();
		if (OutputTypeArgument && !TryExtractLiteralText(OutputTypeArgument->Expression, OutputTypeText))
		{
			OutError = FString::Printf(TEXT("UE.%s OutputType must be a literal value."), *FunctionName);
			return false;
		}

		int32 OutputComponents = 0;
		bool bIsTextureObject = false;
		if (!TryResolveCodeDeclaredType(OutputTypeText, OutputComponents, bIsTextureObject))
		{
			OutError = FString::Printf(TEXT("UE.%s OutputType '%s' is not supported."), *FunctionName, *OutputTypeText);
			return false;
		}

		FString ClassSpecifier = FunctionName;
		if (SubstrateClassAlias)
		{
			ClassSpecifier = *SubstrateClassAlias;
		}
		if (const FCodeCallArgument* ClassArgument = FindNamedArgument(Arguments, TEXT("Class")))
		{
			if (!TryExtractLiteralText(ClassArgument->Expression, ClassSpecifier))
			{
				OutError = FString::Printf(TEXT("UE.%s Class must be a literal value."), *FunctionName);
				return false;
			}
		}
		else if (FunctionName.Equals(TEXT("Expression"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("UE.Expression requires Class=\"MaterialExpressionName\".");
			return false;
		}

		UClass* ExpressionClass = ResolveMaterialExpressionClass(ClassSpecifier);
		if (!ExpressionClass)
		{
			OutError = FString::Printf(TEXT("UE.%s could not resolve MaterialExpression class '%s'."), *FunctionName, *ClassSpecifier);
			return false;
		}

		auto* Expression = Cast<UMaterialExpression>(CreateExpression(ExpressionClass, 600, ConsumeNodeY()));
		if (!Expression)
		{
			OutError = FString::Printf(TEXT("UE.%s failed to create '%s'."), *FunctionName, *ExpressionClass->GetName());
			return false;
		}

		for (const FCodeCallArgument& Argument : Arguments)
		{
			const FString NormalizedArgumentName = UE::DreamShader::NormalizeSettingKey(Argument.Name);
			if (NormalizedArgumentName == UE::DreamShader::NormalizeSettingKey(TEXT("Class"))
				|| NormalizedArgumentName == UE::DreamShader::NormalizeSettingKey(TEXT("OutputType"))
				|| NormalizedArgumentName == UE::DreamShader::NormalizeSettingKey(TEXT("ResultType"))
				|| NormalizedArgumentName == UE::DreamShader::NormalizeSettingKey(TEXT("Output"))
				|| NormalizedArgumentName == UE::DreamShader::NormalizeSettingKey(TEXT("OutputName"))
				|| NormalizedArgumentName == UE::DreamShader::NormalizeSettingKey(TEXT("OutputIndex")))
			{
				continue;
			}

			FProperty* BoundProperty = FindMaterialExpressionArgumentProperty(ExpressionClass, Argument.Name);
			if (!BoundProperty)
			{
				OutError = FString::Printf(TEXT("UE.%s: '%s' is not a property on '%s'."), *FunctionName, *Argument.Name, *ExpressionClass->GetName());
				return false;
			}

			if (IsMaterialExpressionInputProperty(BoundProperty))
			{
				FCodeValue InputValue;
				if (!EvaluateExpression(Argument.Expression, InputValue, OutError))
				{
					OutError = FString::Printf(TEXT("UE.%s input '%s': %s"), *FunctionName, *Argument.Name, *OutError);
					return false;
				}

				FExpressionInput* Input = BoundProperty->ContainerPtrToValuePtr<FExpressionInput>(Expression);
				if (!Input)
				{
					OutError = FString::Printf(TEXT("UE.%s failed to bind input '%s'."), *FunctionName, *Argument.Name);
					return false;
				}

				ConnectCodeValueToInput(*Input, InputValue);
			}
			else
			{
				FString LiteralText;
				const bool bWantsAssetReference = CastField<FObjectPropertyBase>(BoundProperty) != nullptr;
				if (bWantsAssetReference)
				{
					if (!TryExtractAssetReferenceText(Argument.Expression, LiteralText))
					{
						OutError = FString::Printf(TEXT("UE.%s property '%s' must use Path(...) or an Unreal object path."), *FunctionName, *Argument.Name);
						return false;
					}
				}
				else if (!TryExtractLiteralText(Argument.Expression, LiteralText))
				{
					OutError = FString::Printf(TEXT("UE.%s property '%s' must use a literal value."), *FunctionName, *Argument.Name);
					return false;
				}

				FString LiteralError;
				if (!SetMaterialExpressionLiteralProperty(Expression, BoundProperty, LiteralText, LiteralError))
				{
					OutError = FString::Printf(TEXT("UE.%s property '%s': %s"), *FunctionName, *Argument.Name, *LiteralError);
					return false;
				}
			}
		}

		const FCodeCallArgument* OutputNameArgument = FindNamedArgument(Arguments, TEXT("Output"));
		if (!OutputNameArgument)
		{
			OutputNameArgument = FindNamedArgument(Arguments, TEXT("OutputName"));
		}
		const FCodeCallArgument* OutputIndexArgument = FindNamedArgument(Arguments, TEXT("OutputIndex"));
		if (OutputNameArgument && OutputIndexArgument)
		{
			OutError = FString::Printf(TEXT("UE.%s cannot use OutputName/Output together with OutputIndex."), *FunctionName);
			return false;
		}

		int32 ResolvedOutputIndex = 0;
		if (OutputIndexArgument)
		{
			if (!TryExtractIntegerLiteral(OutputIndexArgument->Expression, ResolvedOutputIndex)
				|| ResolvedOutputIndex < 0
				|| !Expression->Outputs.IsValidIndex(ResolvedOutputIndex))
			{
				OutError = FString::Printf(TEXT("UE.%s OutputIndex is out of range for '%s'."), *FunctionName, *ExpressionClass->GetName());
				return false;
			}
		}
		else if (OutputNameArgument)
		{
			FString OutputNameText;
			if (!TryExtractLiteralText(OutputNameArgument->Expression, OutputNameText))
			{
				OutError = FString::Printf(TEXT("UE.%s OutputName must be a literal value."), *FunctionName);
				return false;
			}

			if (!TryResolveExpressionOutputIndex(Expression, OutputNameText, ResolvedOutputIndex))
			{
				OutError = FString::Printf(TEXT("UE.%s output '%s' was not found on '%s'."), *FunctionName, *OutputNameText, *ExpressionClass->GetName());
				return false;
			}
		}
		else if (!Expression->Outputs.IsValidIndex(0))
		{
			OutError = FString::Printf(TEXT("UE.%s created '%s', but it has no material outputs."), *FunctionName, *ExpressionClass->GetName());
			return false;
		}

		OutValue.Expression = Expression;
		OutValue.OutputIndex = ResolvedOutputIndex;
		OutValue.ComponentCount = OutputComponents;
		OutValue.bIsTextureObject = bIsTextureObject;
		OutValue.bIsMaterialAttributes = IsMaterialAttributesComponentType(OutputComponents, bIsTextureObject);
		OutValue.bIsSubstrate = IsSubstrateComponentType(OutputComponents, bIsTextureObject);
		return true;
	}
}
