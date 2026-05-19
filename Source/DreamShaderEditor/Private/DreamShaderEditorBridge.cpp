#include "DreamShaderEditorBridge.h"

#include "DreamShaderCompileService.h"
#include "DreamShaderCommandletRunner.h"
#include "DreamShaderDecompileService.h"
#include "DreamShaderDependencyGraphService.h"
#include "DreamShaderMaterialGeneratorCodeShared.h"
#include "DreamShaderMaterialGeneratorPrivate.h"
#include "DreamShaderModule.h"
#include "DreamShaderParser.h"
#include "DreamShaderSettings.h"
#include "DreamShaderSourceFileUtils.h"
#include "DreamShaderVirtualFunctionService.h"
#include "DreamShaderWorkspaceService.h"

#include "Async/Async.h"
#include "CoreGlobals.h"
#include "ContentBrowserMenuContexts.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveLinearColorAtlas.h"
#include "DirectoryWatcherModule.h"
#include "Dom/JsonObject.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "IDirectoryWatcher.h"
#include "IMaterialEditor.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCeil.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCosine.h"
#include "Materials/MaterialExpressionCurveAtlasRowParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDesaturation.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionIf.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMin.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionRotator.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionScreenPosition.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionSquareRoot.h"
#include "Materials/MaterialExpressionStaticComponentMaskParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "MaterialEditorContext.h"
#include "MaterialShared.h"
#include "MaterialValueType.h"
#include "Engine/Texture.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "ShaderCore.h"
#include "Styling/AppStyle.h"
#include "ToolMenu.h"
#include "ToolMenus.h"
#include "UObject/MetaData.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"
#include "Widgets/Notifications/SNotificationList.h"

#include "DreamShaderCommandlet.h"

#define LOCTEXT_NAMESPACE "DreamShaderEditorBridge"

namespace UE::DreamShader::Editor::Private
{
	namespace
	{
		static const FName DreamShaderToolMenuOwnerName(TEXT("DreamShaderEditor"));

		struct FVirtualFunctionDefinitionLocation
		{
			FString SourceFilePath;
			FString FunctionName;
			FString AssetObjectPath;
			FString CurrentText;
			TArray<FTextShaderFunctionParameter> Inputs;
			TArray<FTextShaderFunctionParameter> Outputs;
			int32 StartIndex = INDEX_NONE;
			int32 EndIndex = INDEX_NONE;
			int32 Line = 1;
			int32 Column = 1;
		};

		FString EscapeDreamShaderString(const FString& InText)
		{
			FString Result = InText;
			Result.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			Result.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return Result;
		}

		FString EscapeDreamShaderCodeString(const FString& InText)
		{
			FString Result = EscapeDreamShaderString(InText);
			Result.ReplaceInline(TEXT("\r"), TEXT("\\r"));
			Result.ReplaceInline(TEXT("\n"), TEXT("\\n"));
			Result.ReplaceInline(TEXT("\t"), TEXT("\\t"));
			return Result;
		}

		FString GetDreamShaderTypeForFunctionInput(EFunctionInputType InputType)
		{
			switch (InputType)
			{
			case FunctionInput_Scalar:
				return TEXT("float");
			case FunctionInput_Vector2:
				return TEXT("float2");
			case FunctionInput_Vector3:
				return TEXT("float3");
			case FunctionInput_Vector4:
				return TEXT("float4");
			case FunctionInput_Texture2D:
				return TEXT("Texture2D");
			case FunctionInput_TextureCube:
				return TEXT("TextureCube");
			case FunctionInput_Texture2DArray:
				return TEXT("Texture2DArray");
			case FunctionInput_StaticBool:
			case FunctionInput_Bool:
				return TEXT("bool");
			default:
				return TEXT("float4");
			}
		}

		FString GetDreamShaderTypeForMaterialValueType(EMaterialValueType ValueType)
		{
			switch (ValueType)
			{
			case MCT_Float:
			case MCT_Float1:
			case MCT_LWCScalar:
				return TEXT("float");
			case MCT_Float2:
			case MCT_LWCVector2:
				return TEXT("float2");
			case MCT_Float3:
			case MCT_LWCVector3:
				return TEXT("float3");
			case MCT_Float4:
			case MCT_LWCVector4:
				return TEXT("float4");
			case MCT_Texture2D:
			case MCT_Texture:
				return TEXT("Texture2D");
			case MCT_TextureCube:
				return TEXT("TextureCube");
			case MCT_Texture2DArray:
				return TEXT("Texture2DArray");
			case MCT_StaticBool:
			case MCT_Bool:
				return TEXT("bool");
			default:
				return TEXT("float4");
			}
		}

		FString GetDreamShaderTypeForFunctionOutput(const UMaterialExpressionFunctionOutput* OutputExpression);

		FString MakeDreamShaderDeclarationName(const FString& InName, const TCHAR* FallbackPrefix, int32 Index)
		{
			FString Result = UE::DreamShader::SanitizeIdentifier(InName.TrimStartAndEnd());
			if (Result.IsEmpty() || Result == TEXT("DreamShaderSymbol"))
			{
				Result = FString::Printf(TEXT("%s%d"), FallbackPrefix, Index + 1);
			}
			return Result;
		}

		FString MakeFunctionParameterMetadataSuffix(
			const FString& Description,
			const int32 SortPriority,
			const int32 DefaultSortPriority)
		{
			TArray<FString> MetadataEntries;
			if (!Description.TrimStartAndEnd().IsEmpty())
			{
				MetadataEntries.Add(FString::Printf(TEXT("Description=\"%s\";"), *EscapeDreamShaderString(Description.TrimStartAndEnd())));
			}
			if (SortPriority != DefaultSortPriority)
			{
				MetadataEntries.Add(FString::Printf(TEXT("SortPriority=%d;"), SortPriority));
			}

			return MetadataEntries.IsEmpty()
				? FString()
				: FString::Printf(TEXT(" [\n\t\t\t%s\n\t\t]"), *FString::Join(MetadataEntries, TEXT("\n\t\t\t")));
		}

		FString MakePreviewValueText(EFunctionInputType InputType, const FVector4f& PreviewValue)
		{
			switch (InputType)
			{
			case FunctionInput_Scalar:
				return FString::SanitizeFloat(PreviewValue.X);
			case FunctionInput_StaticBool:
			case FunctionInput_Bool:
				return PreviewValue.X != 0.0f ? TEXT("true") : TEXT("false");
			case FunctionInput_Vector2:
				return FString::Printf(TEXT("float2(%g, %g)"), PreviewValue.X, PreviewValue.Y);
			case FunctionInput_Vector3:
				return FString::Printf(TEXT("float3(%g, %g, %g)"), PreviewValue.X, PreviewValue.Y, PreviewValue.Z);
			case FunctionInput_Vector4:
				return FString::Printf(TEXT("float4(%g, %g, %g, %g)"), PreviewValue.X, PreviewValue.Y, PreviewValue.Z, PreviewValue.W);
			default:
				return FString();
			}
		}

		bool BuildVirtualFunctionDefinition(const UMaterialFunction* MaterialFunction, FString& OutDefinition, FString& OutError)
		{
			return FDreamShaderVirtualFunctionService::BuildDefinition(
				MaterialFunction,
				[](const UMaterialExpressionFunctionOutput* OutputExpression)
				{
					return GetDreamShaderTypeForFunctionOutput(OutputExpression);
				},
				OutDefinition,
				OutError);
		}

		bool BuildVirtualFunctionCallText(const UMaterialFunction* MaterialFunction, FString& OutCallText, FString& OutError)
		{
			return FDreamShaderVirtualFunctionService::BuildCallText(MaterialFunction, OutCallText, OutError);
		}

		FString FormatDreamShaderFloat(const double Value)
		{
			return FString::SanitizeFloat(Value);
		}

		FString FormatDreamShaderVector2(const double X, const double Y)
		{
			return FString::Printf(TEXT("float2(%s, %s)"), *FormatDreamShaderFloat(X), *FormatDreamShaderFloat(Y));
		}

		FString FormatDreamShaderVector3(const double X, const double Y, const double Z)
		{
			return FString::Printf(
				TEXT("float3(%s, %s, %s)"),
				*FormatDreamShaderFloat(X),
				*FormatDreamShaderFloat(Y),
				*FormatDreamShaderFloat(Z));
		}

		FString FormatDreamShaderVector4(const double X, const double Y, const double Z, const double W)
		{
			return FString::Printf(
				TEXT("float4(%s, %s, %s, %s)"),
				*FormatDreamShaderFloat(X),
				*FormatDreamShaderFloat(Y),
				*FormatDreamShaderFloat(Z),
				*FormatDreamShaderFloat(W));
		}

		FString FormatDreamShaderColor(const FLinearColor& Color)
		{
			return FormatDreamShaderVector4(Color.R, Color.G, Color.B, Color.A);
		}

		FString WrapExpressionForSuffix(const FString& ExpressionText)
		{
			const FString Trimmed = ExpressionText.TrimStartAndEnd();
			const bool bSimple =
				!Trimmed.IsEmpty()
				&& !Trimmed.Contains(TEXT(" "))
				&& !Trimmed.Contains(TEXT("+"))
				&& !Trimmed.Contains(TEXT("-"))
				&& !Trimmed.Contains(TEXT("*"))
				&& !Trimmed.Contains(TEXT("/"));
			return bSimple ? Trimmed : FString::Printf(TEXT("(%s)"), *Trimmed);
		}

		static bool IsSwizzleComponentChar(const TCHAR Character)
		{
			switch (FChar::ToLower(Character))
			{
			case TEXT('r'):
			case TEXT('g'):
			case TEXT('b'):
			case TEXT('a'):
			case TEXT('x'):
			case TEXT('y'):
			case TEXT('z'):
			case TEXT('w'):
				return true;
			default:
				return false;
			}
		}

		static bool IsSwizzleText(const FString& Text)
		{
			if (Text.IsEmpty() || Text.Len() > 4)
			{
				return false;
			}

			for (const TCHAR Character : Text)
			{
				if (!IsSwizzleComponentChar(Character))
				{
					return false;
				}
			}
			return true;
		}

		static int32 GetSwizzleComponentIndex(const TCHAR Character)
		{
			switch (FChar::ToLower(Character))
			{
			case TEXT('r'):
			case TEXT('x'):
				return 0;
			case TEXT('g'):
			case TEXT('y'):
				return 1;
			case TEXT('b'):
			case TEXT('z'):
				return 2;
			case TEXT('a'):
			case TEXT('w'):
				return 3;
			default:
				return INDEX_NONE;
			}
		}

		static bool TrySplitTrailingSwizzle(const FString& ExpressionText, FString& OutBaseText, FString& OutSwizzleText)
		{
			const FString Trimmed = ExpressionText.TrimStartAndEnd();
			int32 DotIndex = INDEX_NONE;
			if (!Trimmed.FindLastChar(TEXT('.'), DotIndex) || DotIndex <= 0 || DotIndex + 1 >= Trimmed.Len())
			{
				return false;
			}

			const FString CandidateSwizzle = Trimmed.Mid(DotIndex + 1).ToLower();
			if (!IsSwizzleText(CandidateSwizzle))
			{
				return false;
			}

			OutBaseText = Trimmed.Left(DotIndex);
			OutSwizzleText = CandidateSwizzle;
			return !OutBaseText.TrimStartAndEnd().IsEmpty();
		}

		static bool TryComposeTrailingSwizzle(
			const FString& ExpressionText,
			const FString& RequestedSwizzle,
			FString& OutExpressionText)
		{
			FString BaseText;
			FString ExistingSwizzle;
			if (!TrySplitTrailingSwizzle(ExpressionText, BaseText, ExistingSwizzle))
			{
				return false;
			}

			FString ComposedSwizzle;
			ComposedSwizzle.Reserve(RequestedSwizzle.Len());
			for (const TCHAR RequestedComponent : RequestedSwizzle)
			{
				const int32 ComponentIndex = GetSwizzleComponentIndex(RequestedComponent);
				if (!ExistingSwizzle.IsValidIndex(ComponentIndex))
				{
					return false;
				}
				ComposedSwizzle += ExistingSwizzle[ComponentIndex];
			}

			OutExpressionText = FString::Printf(TEXT("%s.%s"), *WrapExpressionForSuffix(BaseText), *ComposedSwizzle);
			return true;
		}

		FString MakeInputMaskSuffix(const FExpressionInput& Input)
		{
			if (!Input.Mask)
			{
				return FString();
			}

			FString Suffix;
			if (Input.MaskR)
			{
				Suffix += TEXT("r");
			}
			if (Input.MaskG)
			{
				Suffix += TEXT("g");
			}
			if (Input.MaskB)
			{
				Suffix += TEXT("b");
			}
			if (Input.MaskA)
			{
				Suffix += TEXT("a");
			}
			return Suffix;
		}

		FString MakeSwizzleExpression(const FString& ExpressionText, const FString& SwizzleText)
		{
			if (SwizzleText.IsEmpty())
			{
				return ExpressionText;
			}

			const FString NormalizedSwizzle = SwizzleText.ToLower();
			FString ComposedExpression;
			if (IsSwizzleText(NormalizedSwizzle)
				&& TryComposeTrailingSwizzle(ExpressionText, NormalizedSwizzle, ComposedExpression))
			{
				return ComposedExpression;
			}

			return FString::Printf(TEXT("%s.%s"), *WrapExpressionForSuffix(ExpressionText), *NormalizedSwizzle);
		}

		FString ApplyInputMask(const FString& ExpressionText, const FExpressionInput& Input)
		{
			const FString MaskSuffix = MakeInputMaskSuffix(Input);
			if (MaskSuffix.IsEmpty())
			{
				return ExpressionText;
			}

			return MakeSwizzleExpression(ExpressionText, MaskSuffix);
		}

		FString MakeDreamShaderObjectPathLiteral(const UObject* Object)
		{
			if (!Object)
			{
				return FString();
			}

			FString PackageName = Object->GetOutermost() ? Object->GetOutermost()->GetName() : FString();
			PackageName.TrimStartAndEndInline();
			PackageName.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (PackageName.IsEmpty())
			{
				return Object->GetPathName();
			}

			const auto BuildRootedLiteral = [](const TCHAR* RootName, const FString& RelativePath)
			{
				return FString::Printf(TEXT("Path(%s, \"%s\")"), RootName, *EscapeDreamShaderString(RelativePath));
			};

			if (PackageName.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase))
			{
				return BuildRootedLiteral(TEXT("Game"), PackageName.Mid(6));
			}
			if (PackageName.StartsWith(TEXT("/Engine/"), ESearchCase::IgnoreCase))
			{
				return BuildRootedLiteral(TEXT("Engine"), PackageName.Mid(8));
			}

			FString BestPluginName;
			FString BestMountedPath;
			for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPluginsWithContent())
			{
				FString MountedPath = Plugin->GetMountedAssetPath();
				MountedPath.TrimStartAndEndInline();
				MountedPath.ReplaceInline(TEXT("\\"), TEXT("/"));
				while (MountedPath.EndsWith(TEXT("/")))
				{
					MountedPath.LeftChopInline(1, EAllowShrinking::No);
				}
				if (!MountedPath.StartsWith(TEXT("/")))
				{
					MountedPath = TEXT("/") + MountedPath;
				}
				if (MountedPath.IsEmpty() || MountedPath == TEXT("/"))
				{
					MountedPath = TEXT("/") + Plugin->GetName();
				}

				if ((PackageName.Equals(MountedPath, ESearchCase::IgnoreCase)
					|| PackageName.StartsWith(MountedPath + TEXT("/"), ESearchCase::IgnoreCase))
					&& MountedPath.Len() > BestMountedPath.Len())
				{
					BestMountedPath = MountedPath;
					BestPluginName = Plugin->GetName();
				}
			}

			if (!BestPluginName.IsEmpty())
			{
				FString RelativePath = PackageName.Mid(BestMountedPath.Len());
				while (RelativePath.StartsWith(TEXT("/")))
				{
					RelativePath.RightChopInline(1, EAllowShrinking::No);
				}
				return FString::Printf(
					TEXT("Path(Plugins.%s, \"%s\")"),
					*BestPluginName,
					*EscapeDreamShaderString(RelativePath));
			}

			return FString::Printf(TEXT("Path(\"%s\")"), *EscapeDreamShaderString(PackageName));
		}

		FString GetDreamShaderTypeForCustomOutputType(const ECustomMaterialOutputType OutputType)
		{
			switch (OutputType)
			{
			case CMOT_Float1:
				return TEXT("float");
			case CMOT_Float2:
				return TEXT("float2");
			case CMOT_Float3:
				return TEXT("float3");
			case CMOT_Float4:
				return TEXT("float4");
			case CMOT_MaterialAttributes:
				return TEXT("MaterialAttributes");
			default:
				return TEXT("float4");
			}
		}

		FString GetDreamShaderTypeForComponentCount(const int32 ComponentCount)
		{
			if (ComponentCount <= 1)
			{
				return TEXT("float");
			}
			if (ComponentCount == 2)
			{
				return TEXT("float2");
			}
			if (ComponentCount == 3)
			{
				return TEXT("float3");
			}
			return TEXT("float4");
		}

		static int32 GetComponentCountForFunctionInputType(const EFunctionInputType InputType)
		{
			switch (InputType)
			{
			case FunctionInput_Vector2:
				return 2;
			case FunctionInput_Vector3:
				return 3;
			case FunctionInput_Vector4:
				return 4;
			case FunctionInput_Texture2D:
			case FunctionInput_TextureCube:
			case FunctionInput_Texture2DArray:
			case FunctionInput_VolumeTexture:
			case FunctionInput_Substrate:
				return 0;
			case FunctionInput_Scalar:
			case FunctionInput_StaticBool:
			case FunctionInput_Bool:
			default:
				return 1;
			}
		}

		static int32 GetOutputMaskComponentCount(const UMaterialExpression* Expression, const int32 OutputIndex)
		{
			if (!Expression || !Expression->Outputs.IsValidIndex(OutputIndex))
			{
				return 0;
			}

			const FExpressionOutput& Output = Expression->Outputs[OutputIndex];
			return (Output.MaskR ? 1 : 0)
				+ (Output.MaskG ? 1 : 0)
				+ (Output.MaskB ? 1 : 0)
				+ (Output.MaskA ? 1 : 0);
		}

		static UMaterialExpressionFunctionOutput* ResolveFunctionCallOutputExpression(UMaterialExpressionMaterialFunctionCall* FunctionCall, const int32 OutputIndex)
		{
			if (!FunctionCall)
			{
				return nullptr;
			}

			FString DesiredOutputName;
			if (FunctionCall->FunctionOutputs.IsValidIndex(OutputIndex))
			{
				const FFunctionExpressionOutput& FunctionOutput = FunctionCall->FunctionOutputs[OutputIndex];
				if (FunctionOutput.ExpressionOutput)
				{
					return FunctionOutput.ExpressionOutput;
				}
				DesiredOutputName = FunctionOutput.Output.OutputName.ToString();
			}

			UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(FunctionCall->MaterialFunction);
			if (!MaterialFunction)
			{
				return nullptr;
			}

			TArray<FFunctionExpressionInput> FunctionInputs;
			TArray<FFunctionExpressionOutput> FunctionOutputs;
			MaterialFunction->GetInputsAndOutputs(FunctionInputs, FunctionOutputs);
			if (FunctionOutputs.IsValidIndex(OutputIndex) && FunctionOutputs[OutputIndex].ExpressionOutput)
			{
				return FunctionOutputs[OutputIndex].ExpressionOutput;
			}

			if (!DesiredOutputName.IsEmpty())
			{
				for (const FFunctionExpressionOutput& FunctionOutput : FunctionOutputs)
				{
					UMaterialExpressionFunctionOutput* OutputExpression = FunctionOutput.ExpressionOutput;
					if (OutputExpression
						&& OutputExpression->OutputName.ToString().Equals(DesiredOutputName, ESearchCase::IgnoreCase))
					{
						return OutputExpression;
					}
				}
			}

			return nullptr;
		}

		static int32 GetExpressionOutputComponentCount(UMaterialExpression* Expression, const int32 OutputIndex, TSet<UMaterialExpression*>* VisitingExpressions = nullptr)
		{
			if (!Expression)
			{
				return 1;
			}

			TSet<UMaterialExpression*> LocalVisitingExpressions;
			if (!VisitingExpressions)
			{
				VisitingExpressions = &LocalVisitingExpressions;
			}
			if (VisitingExpressions->Contains(Expression))
			{
				return 1;
			}
			VisitingExpressions->Add(Expression);

			auto ResolveInputComponentCount = [VisitingExpressions](const FExpressionInput& Input, const int32 DefaultComponentCount) -> int32
			{
				const FExpressionInput TracedInput = Input.GetTracedInput();
				return TracedInput.Expression
					? GetExpressionOutputComponentCount(TracedInput.Expression, TracedInput.OutputIndex, VisitingExpressions)
					: DefaultComponentCount;
			};

			auto Finish = [VisitingExpressions, Expression](const int32 ComponentCount) -> int32
			{
				VisitingExpressions->Remove(Expression);
				return ComponentCount;
			};

			if (Cast<UMaterialExpressionTextureCoordinate>(Expression)
				|| Cast<UMaterialExpressionPanner>(Expression)
				|| Expression->GetClass()->GetName().Equals(TEXT("MaterialExpressionRotator"), ESearchCase::IgnoreCase))
			{
				return Finish(2);
			}

			if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
			{
				return Finish(GetComponentCountForFunctionInputType(FunctionInput->InputType.GetValue()));
			}

			if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
			{
				if (UMaterialExpressionFunctionOutput* FunctionOutput = ResolveFunctionCallOutputExpression(FunctionCall, OutputIndex))
				{
					const FString FunctionOutputType = GetDreamShaderTypeForFunctionOutput(FunctionOutput);
					if (FunctionOutputType.Equals(TEXT("float2"), ESearchCase::IgnoreCase))
					{
						return Finish(2);
					}
					if (FunctionOutputType.Equals(TEXT("float3"), ESearchCase::IgnoreCase))
					{
						return Finish(3);
					}
					if (FunctionOutputType.Equals(TEXT("float4"), ESearchCase::IgnoreCase))
					{
						return Finish(4);
					}
					if (FunctionOutputType.Equals(TEXT("MaterialAttributes"), ESearchCase::IgnoreCase)
						|| FunctionOutputType.StartsWith(TEXT("Texture"), ESearchCase::IgnoreCase))
					{
						return Finish(0);
					}
					return Finish(1);
				}
			}

			if (UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
			{
				const int32 MaskComponentCount =
					(Mask->R ? 1 : 0)
					+ (Mask->G ? 1 : 0)
					+ (Mask->B ? 1 : 0)
					+ (Mask->A ? 1 : 0);
				return Finish(MaskComponentCount > 0 ? MaskComponentCount : 1);
			}

			if (UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(Expression))
			{
				return Finish(FMath::Clamp(
					ResolveInputComponentCount(Append->A, 1) + ResolveInputComponentCount(Append->B, 1),
					1,
					4));
			}

			if (UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expression))
			{
				return Finish(FMath::Max(ResolveInputComponentCount(Add->A, 1), ResolveInputComponentCount(Add->B, 1)));
			}
			if (UMaterialExpressionSubtract* Subtract = Cast<UMaterialExpressionSubtract>(Expression))
			{
				return Finish(FMath::Max(ResolveInputComponentCount(Subtract->A, 1), ResolveInputComponentCount(Subtract->B, 1)));
			}
			if (UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				return Finish(FMath::Max(ResolveInputComponentCount(Multiply->A, 1), ResolveInputComponentCount(Multiply->B, 1)));
			}
			if (UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(Expression))
			{
				return Finish(FMath::Max(ResolveInputComponentCount(Divide->A, 1), ResolveInputComponentCount(Divide->B, 1)));
			}
			if (UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Expression))
			{
				return Finish(FMath::Max(ResolveInputComponentCount(Lerp->A, 1), ResolveInputComponentCount(Lerp->B, 1)));
			}
			if (UMaterialExpressionStaticSwitchParameter* StaticSwitch = Cast<UMaterialExpressionStaticSwitchParameter>(Expression))
			{
				return Finish(FMath::Max(ResolveInputComponentCount(StaticSwitch->A, 1), ResolveInputComponentCount(StaticSwitch->B, 1)));
			}
			if (UMaterialExpressionStaticComponentMaskParameter* StaticComponentMask = Cast<UMaterialExpressionStaticComponentMaskParameter>(Expression))
			{
				const int32 MaskComponentCount =
					(StaticComponentMask->DefaultR ? 1 : 0)
					+ (StaticComponentMask->DefaultG ? 1 : 0)
					+ (StaticComponentMask->DefaultB ? 1 : 0)
					+ (StaticComponentMask->DefaultA ? 1 : 0);
				return Finish(MaskComponentCount > 0 ? MaskComponentCount : 1);
			}
			if (UMaterialExpressionOneMinus* OneMinus = Cast<UMaterialExpressionOneMinus>(Expression))
			{
				return Finish(ResolveInputComponentCount(OneMinus->Input, 1));
			}
			if (UMaterialExpressionPower* Power = Cast<UMaterialExpressionPower>(Expression))
			{
				return Finish(ResolveInputComponentCount(Power->Base, 1));
			}
			if (UMaterialExpressionNormalize* Normalize = Cast<UMaterialExpressionNormalize>(Expression))
			{
				return Finish(ResolveInputComponentCount(Normalize->VectorInput, 1));
			}
			if (UMaterialExpressionAbs* Abs = Cast<UMaterialExpressionAbs>(Expression))
			{
				return Finish(ResolveInputComponentCount(Abs->Input, 1));
			}
			if (UMaterialExpressionSaturate* Saturate = Cast<UMaterialExpressionSaturate>(Expression))
			{
				return Finish(ResolveInputComponentCount(Saturate->Input, 1));
			}
			if (UMaterialExpressionFloor* Floor = Cast<UMaterialExpressionFloor>(Expression))
			{
				return Finish(ResolveInputComponentCount(Floor->Input, 1));
			}
			if (UMaterialExpressionCeil* Ceil = Cast<UMaterialExpressionCeil>(Expression))
			{
				return Finish(ResolveInputComponentCount(Ceil->Input, 1));
			}
			if (UMaterialExpressionFrac* Frac = Cast<UMaterialExpressionFrac>(Expression))
			{
				return Finish(ResolveInputComponentCount(Frac->Input, 1));
			}
			if (UMaterialExpressionSquareRoot* SquareRoot = Cast<UMaterialExpressionSquareRoot>(Expression))
			{
				return Finish(ResolveInputComponentCount(SquareRoot->Input, 1));
			}
			if (UMaterialExpressionSine* Sine = Cast<UMaterialExpressionSine>(Expression))
			{
				return Finish(ResolveInputComponentCount(Sine->Input, 1));
			}
			if (UMaterialExpressionCosine* Cosine = Cast<UMaterialExpressionCosine>(Expression))
			{
				return Finish(ResolveInputComponentCount(Cosine->Input, 1));
			}

			const EMaterialValueType OutputType = Expression->GetOutputValueType(OutputIndex);
			if (IsTextureMaterialValueType(OutputType) || OutputType == MCT_MaterialAttributes)
			{
				return Finish(0);
			}

			const int32 MaskComponentCount = GetOutputMaskComponentCount(Expression, OutputIndex);
			if (MaskComponentCount > 0)
			{
				return Finish(MaskComponentCount);
			}

			const int32 TypeComponentCount = GetComponentCountForMaterialValueType(OutputType);
			return Finish(TypeComponentCount > 0 ? TypeComponentCount : 4);
		}

		FString GetDreamShaderTypeForFunctionOutput(const UMaterialExpressionFunctionOutput* OutputExpression)
		{
			if (!OutputExpression)
			{
				return TEXT("float4");
			}

			const FExpressionInput TracedInput = OutputExpression->A.GetTracedInput();
			if (TracedInput.Expression)
			{
				const EMaterialValueType OutputType = TracedInput.Expression->GetOutputValueType(TracedInput.OutputIndex);
				if (OutputType == MCT_MaterialAttributes)
				{
					return TEXT("MaterialAttributes");
				}
				if (OutputType == MCT_StaticBool || OutputType == MCT_Bool || IsTextureMaterialValueType(OutputType))
				{
					return GetDreamShaderTypeForMaterialValueType(OutputType);
				}

				const FString MaskSuffix = MakeInputMaskSuffix(OutputExpression->A);
				if (!MaskSuffix.IsEmpty())
				{
					return GetDreamShaderTypeForComponentCount(MaskSuffix.Len());
				}

				const int32 ComponentCount = GetExpressionOutputComponentCount(TracedInput.Expression, TracedInput.OutputIndex);
				if (ComponentCount > 0)
				{
					return GetDreamShaderTypeForComponentCount(ComponentCount);
				}

				return GetDreamShaderTypeForMaterialValueType(OutputType);
			}

			const EMaterialValueType OutputType = const_cast<UMaterialExpressionFunctionOutput*>(OutputExpression)->GetOutputValueType(0);
			if (OutputType == MCT_MaterialAttributes)
			{
				return TEXT("MaterialAttributes");
			}
			return GetDreamShaderTypeForMaterialValueType(OutputType);
		}

		FString GetEnumLiteralText(const UEnum* Enum, const int64 Value)
		{
			if (!Enum)
			{
				return FString::FromInt(static_cast<int32>(Value));
			}

			const FString Name = Enum->GetNameStringByValue(Value);
			return Name.IsEmpty() ? FString::FromInt(static_cast<int32>(Value)) : Name;
		}

		FString GetDreamShaderTypeForExpressionOutput(UMaterialExpression* Expression, const int32 OutputIndex)
		{
			if (!Expression)
			{
				return TEXT("float4");
			}

			if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
			{
				if (UMaterialExpressionFunctionOutput* FunctionOutput = ResolveFunctionCallOutputExpression(FunctionCall, OutputIndex))
				{
					return GetDreamShaderTypeForFunctionOutput(FunctionOutput);
				}
			}

			const EMaterialValueType OutputType = Expression->GetOutputValueType(OutputIndex);
			if (OutputType == MCT_MaterialAttributes)
			{
				return TEXT("MaterialAttributes");
			}
			if (OutputType == MCT_StaticBool || OutputType == MCT_Bool || IsTextureMaterialValueType(OutputType))
			{
				return GetDreamShaderTypeForMaterialValueType(OutputType);
			}
			const int32 ComponentCount = GetExpressionOutputComponentCount(Expression, OutputIndex);
			if (ComponentCount > 0)
			{
				return GetDreamShaderTypeForComponentCount(ComponentCount);
			}
			return GetDreamShaderTypeForMaterialValueType(OutputType);
		}

		FString GetCustomOutputName(const UMaterialExpressionCustom* CustomExpression, const int32 OutputIndex)
		{
			if (!CustomExpression || OutputIndex <= 0)
			{
				return FString();
			}

			const int32 AdditionalOutputIndex = OutputIndex - 1;
			if (!CustomExpression->AdditionalOutputs.IsValidIndex(AdditionalOutputIndex))
			{
				return FString();
			}

			return CustomExpression->AdditionalOutputs[AdditionalOutputIndex].OutputName.ToString();
		}

		FString GetFunctionCallOutputName(const UMaterialExpressionMaterialFunctionCall* FunctionCall, const int32 OutputIndex)
		{
			if (!FunctionCall || !FunctionCall->FunctionOutputs.IsValidIndex(OutputIndex))
			{
				return FString();
			}

			const FFunctionExpressionOutput& Output = FunctionCall->FunctionOutputs[OutputIndex];
			if (Output.ExpressionOutput)
			{
				return Output.ExpressionOutput->OutputName.ToString();
			}
			return Output.Output.OutputName.ToString();
		}

		FString GetMaterialExpressionShortName(const UClass* Class);

		FString GetMaterialDomainText(const EMaterialDomain Domain)
		{
			switch (Domain)
			{
			case MD_Surface:
				return TEXT("Surface");
			case MD_DeferredDecal:
				return TEXT("DeferredDecal");
			case MD_LightFunction:
				return TEXT("LightFunction");
			case MD_Volume:
				return TEXT("Volume");
			case MD_PostProcess:
				return TEXT("PostProcess");
			case MD_UI:
				return TEXT("UI");
			case MD_RuntimeVirtualTexture:
				return TEXT("RuntimeVirtualTexture");
			default:
				return TEXT("Surface");
			}
		}

		FString GetBlendModeText(const EBlendMode BlendMode)
		{
			switch (BlendMode)
			{
			case BLEND_Opaque:
				return TEXT("Opaque");
			case BLEND_Masked:
				return TEXT("Masked");
			case BLEND_Translucent:
				return TEXT("Translucent");
			case BLEND_Additive:
				return TEXT("Additive");
			case BLEND_Modulate:
				return TEXT("Modulate");
			case BLEND_AlphaComposite:
				return TEXT("AlphaComposite");
			case BLEND_AlphaHoldout:
				return TEXT("AlphaHoldout");
			case BLEND_TranslucentColoredTransmittance:
				return TEXT("Translucent");
			default:
				return TEXT("Opaque");
			}
		}

		FString GetShadingModelText(const UMaterial* Material)
		{
			if (!Material)
			{
				return TEXT("DefaultLit");
			}

			const FMaterialShadingModelField ShadingModels = Material->GetShadingModels();
			if (ShadingModels.HasShadingModel(MSM_Unlit))
			{
				return TEXT("Unlit");
			}
			if (ShadingModels.HasShadingModel(MSM_Subsurface))
			{
				return TEXT("Subsurface");
			}
			if (ShadingModels.HasShadingModel(MSM_PreintegratedSkin))
			{
				return TEXT("PreintegratedSkin");
			}
			if (ShadingModels.HasShadingModel(MSM_ClearCoat))
			{
				return TEXT("ClearCoat");
			}
			if (ShadingModels.HasShadingModel(MSM_SubsurfaceProfile))
			{
				return TEXT("SubsurfaceProfile");
			}
			if (ShadingModels.HasShadingModel(MSM_TwoSidedFoliage))
			{
				return TEXT("TwoSidedFoliage");
			}
			if (ShadingModels.HasShadingModel(MSM_Hair))
			{
				return TEXT("Hair");
			}
			if (ShadingModels.HasShadingModel(MSM_Cloth))
			{
				return TEXT("Cloth");
			}
			if (ShadingModels.HasShadingModel(MSM_Eye))
			{
				return TEXT("Eye");
			}
			if (ShadingModels.HasShadingModel(MSM_SingleLayerWater))
			{
				return TEXT("SingleLayerWater");
			}
			if (ShadingModels.HasShadingModel(MSM_ThinTranslucent))
			{
				return TEXT("ThinTranslucent");
			}
			if (ShadingModels.HasShadingModel(MSM_Strata))
			{
				return TEXT("Substrate");
			}
			return TEXT("DefaultLit");
		}

		struct FDecompiledExpressionKey
		{
			const UMaterialExpression* Expression = nullptr;
			int32 OutputIndex = 0;

			friend uint32 GetTypeHash(const FDecompiledExpressionKey& Key)
			{
				return HashCombine(GetTypeHash(Key.Expression), GetTypeHash(Key.OutputIndex));
			}

			bool operator==(const FDecompiledExpressionKey& Other) const
			{
				return Expression == Other.Expression && OutputIndex == Other.OutputIndex;
			}
		};

		struct FDecompiledValue
		{
			FString Text;
			FString Type = TEXT("float");
			int32 ComponentCount = 1;
			bool bIsTextureObject = false;
			bool bIsMaterialAttributes = false;
			bool bIsSimple = true;
		};

		class FDreamShaderGraphDecompiler
		{
		public:
			bool DecompileMaterial(UMaterial* Material, const FString& DecompiledName, FString& OutSourceText, FString& OutError)
			{
				Reset();
				if (!Material)
				{
					OutError = TEXT("No Material asset was provided.");
					return false;
				}

				FScopedSlowTask DecompileSlowTask(
					FMath::Max(4.0f, static_cast<float>(Material->GetExpressions().Num()) + 4.0f),
					FText::FromString(FString::Printf(TEXT("Decompiling Material '%s'..."), *Material->GetName())));
				if (!IsRunningCommandlet())
				{
					DecompileSlowTask.MakeDialogDelayed(0.25f);
				}
				ActiveDecompileSlowTask = &DecompileSlowTask;
				ON_SCOPE_EXIT
				{
					ActiveDecompileSlowTask = nullptr;
				};

				DecompileSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Scanning material outputs for '%s'..."), *Material->GetName())));
				TArray<FString> OutputDeclarations;
				TArray<FString> OutputBindings;
				TArray<FString> OutputAssignments;

				struct FMaterialOutputBinding
				{
					EMaterialProperty Property;
					const TCHAR* Name;
					const TCHAR* Type;
					const TCHAR* Target;
					const TCHAR* DefaultValue;
				};

				const FMaterialOutputBinding Bindings[] =
				{
					{ MP_EmissiveColor, TEXT("EmissiveColor"), TEXT("float3"), TEXT("Base.EmissiveColor"), TEXT("float3(0, 0, 0)") },
					{ MP_BaseColor, TEXT("BaseColor"), TEXT("float3"), TEXT("Base.BaseColor"), TEXT("float3(0.8, 0.8, 0.8)") },
					{ MP_Metallic, TEXT("Metallic"), TEXT("float"), TEXT("Base.Metallic"), TEXT("0.0") },
					{ MP_Specular, TEXT("Specular"), TEXT("float"), TEXT("Base.Specular"), TEXT("0.5") },
					{ MP_Roughness, TEXT("Roughness"), TEXT("float"), TEXT("Base.Roughness"), TEXT("0.5") },
					{ MP_Anisotropy, TEXT("Anisotropy"), TEXT("float"), TEXT("Base.Anisotropy"), TEXT("0.0") },
					{ MP_Opacity, TEXT("Opacity"), TEXT("float"), TEXT("Base.Opacity"), TEXT("1.0") },
					{ MP_OpacityMask, TEXT("OpacityMask"), TEXT("float"), TEXT("Base.OpacityMask"), TEXT("1.0") },
					{ MP_Normal, TEXT("Normal"), TEXT("float3"), TEXT("Base.Normal"), TEXT("float3(0, 0, 1)") },
					{ MP_Tangent, TEXT("Tangent"), TEXT("float3"), TEXT("Base.Tangent"), TEXT("float3(1, 0, 0)") },
					{ MP_WorldPositionOffset, TEXT("WorldPositionOffset"), TEXT("float3"), TEXT("Base.WorldPositionOffset"), TEXT("float3(0, 0, 0)") },
					{ MP_SubsurfaceColor, TEXT("SubsurfaceColor"), TEXT("float3"), TEXT("Base.SubsurfaceColor"), TEXT("float3(1, 1, 1)") },
					{ MP_CustomData0, TEXT("CustomData0"), TEXT("float"), TEXT("Base.CustomData0"), TEXT("0.0") },
					{ MP_CustomData1, TEXT("CustomData1"), TEXT("float"), TEXT("Base.CustomData1"), TEXT("0.0") },
					{ MP_AmbientOcclusion, TEXT("AmbientOcclusion"), TEXT("float"), TEXT("Base.AmbientOcclusion"), TEXT("1.0") },
					{ MP_Refraction, TEXT("Refraction"), TEXT("float"), TEXT("Base.Refraction"), TEXT("0.0") },
					{ MP_PixelDepthOffset, TEXT("PixelDepthOffset"), TEXT("float"), TEXT("Base.PixelDepthOffset"), TEXT("0.0") },
					{ MP_MaterialAttributes, TEXT("MaterialAttributes"), TEXT("MaterialAttributes"), TEXT("Base.MaterialAttributes"), TEXT("MaterialAttributes()") },
				};

				for (const FMaterialOutputBinding& Binding : Bindings)
				{
					FExpressionInput* MaterialInput = Material->GetExpressionInputForProperty(Binding.Property);
					if (!MaterialInput || !MaterialInput->IsConnected())
					{
						continue;
					}

					ReservedNames.Add(Binding.Name);
					OutputDeclarations.Add(FString::Printf(TEXT("\t\t%s %s;"), Binding.Type, Binding.Name));
					OutputBindings.Add(FString::Printf(TEXT("\t\t%s = %s;"), Binding.Target, Binding.Name));
				}

				for (const FMaterialOutputBinding& Binding : Bindings)
				{
					FExpressionInput* MaterialInput = Material->GetExpressionInputForProperty(Binding.Property);
					if (!MaterialInput || !MaterialInput->IsConnected())
					{
						continue;
					}

					OutputAssignments.Add(FormatGraphSetStatement(Binding.Name, CompileInput(*MaterialInput, Binding.DefaultValue)));
				}

				TArray<FString> Lines;
				DecompileSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Formatting DSM source for '%s'..."), *Material->GetName())));
				Lines.Add(FString::Printf(TEXT("// Decompiled from %s"), *Material->GetPathName()));
				if (!Warnings.IsEmpty())
				{
					for (const FString& Warning : Warnings)
					{
						Lines.Add(FString::Printf(TEXT("// Warning: %s"), *Warning));
					}
				}
				if (!VirtualFunctionDefinitions.IsEmpty())
				{
					Lines.Add(TEXT(""));
					Lines.Append(VirtualFunctionDefinitions);
				}

				Lines.Add(FString::Printf(TEXT("Shader(Name=\"%s\")"), *EscapeDreamShaderString(DecompiledName)));
				Lines.Add(TEXT("{"));
				AppendSection(Lines, TEXT("Properties"), PropertyDeclarations);
				Lines.Add(TEXT("\tSettings = {"));
				Lines.Add(FString::Printf(TEXT("\t\tDomain = \"%s\";"), *GetMaterialDomainText(Material->MaterialDomain)));
				Lines.Add(FString::Printf(TEXT("\t\tShadingModel = \"%s\";"), *GetShadingModelText(Material)));
				Lines.Add(FString::Printf(TEXT("\t\tBlendMode = \"%s\";"), *GetBlendModeText(Material->BlendMode)));
				Lines.Add(TEXT("\t}"));
				Lines.Add(TEXT(""));
				AppendSection(Lines, TEXT("Outputs"), OutputDeclarations, OutputBindings);
				AppendSection(Lines, TEXT("Graph"), GraphLines, OutputAssignments);
				Lines.Add(TEXT("}"));

				OutSourceText = FString::Join(Lines, TEXT("\n"));
				return true;
			}

			bool DecompileFunction(UMaterialFunction* MaterialFunction, const FString& DecompiledName, FString& OutSourceText, FString& OutError)
			{
				Reset();
				if (!MaterialFunction)
				{
					OutError = TEXT("No MaterialFunction asset was provided.");
					return false;
				}

				FScopedSlowTask DecompileSlowTask(
					FMath::Max(4.0f, static_cast<float>(MaterialFunction->GetExpressions().Num()) + 4.0f),
					FText::FromString(FString::Printf(TEXT("Decompiling Material Function '%s'..."), *MaterialFunction->GetName())));
				if (!IsRunningCommandlet())
				{
					DecompileSlowTask.MakeDialogDelayed(0.25f);
				}
				ActiveDecompileSlowTask = &DecompileSlowTask;
				ON_SCOPE_EXIT
				{
					ActiveDecompileSlowTask = nullptr;
				};

				DecompileSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Scanning function inputs and outputs for '%s'..."), *MaterialFunction->GetName())));
				TArray<FFunctionExpressionInput> Inputs;
				TArray<FFunctionExpressionOutput> Outputs;
				MaterialFunction->GetInputsAndOutputs(Inputs, Outputs);
				if (Outputs.IsEmpty())
				{
					OutError = FString::Printf(TEXT("MaterialFunction '%s' does not expose any outputs."), *MaterialFunction->GetName());
					return false;
				}

				TArray<FString> InputDeclarations;
				for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
				{
					const FFunctionExpressionInput& Input = Inputs[InputIndex];
					UMaterialExpressionFunctionInput* InputExpression = Input.ExpressionInput;
					const FString InputName = InputExpression
						? InputExpression->InputName.ToString()
						: Input.Input.InputName.ToString();
					const EFunctionInputType InputType = InputExpression
						? InputExpression->InputType.GetValue()
						: FunctionInput_Vector4;
					const bool bOptional = InputExpression && InputExpression->bUsePreviewValueAsDefault != 0;
					const FString DefaultText = bOptional && InputExpression
						? MakePreviewValueText(InputType, InputExpression->PreviewValue)
						: FString();
					const FString DefaultSuffix = DefaultText.IsEmpty()
						? FString()
						: FString::Printf(TEXT(" = %s"), *DefaultText);
					const FString MetadataSuffix = InputExpression
						? MakeFunctionParameterMetadataSuffix(InputExpression->Description, InputExpression->SortPriority, InputIndex)
						: FString();
					const FString DeclarationName = MakeDreamShaderDeclarationName(InputName, TEXT("Input"), InputIndex);
					if (InputExpression)
					{
						FunctionInputNames.Add(InputExpression, DeclarationName);
					}
					InputDeclarations.Add(FString::Printf(
						TEXT("\t\t%s%s %s%s%s;"),
						bOptional ? TEXT("opt ") : TEXT(""),
						*GetDreamShaderTypeForFunctionInput(InputType),
						*DeclarationName,
						*DefaultSuffix,
						*MetadataSuffix));
				}

				TArray<FString> OutputDeclarations;
				TArray<FString> OutputAssignments;
				for (int32 OutputIndex = 0; OutputIndex < Outputs.Num(); ++OutputIndex)
				{
					const FFunctionExpressionOutput& Output = Outputs[OutputIndex];
					UMaterialExpressionFunctionOutput* OutputExpression = Output.ExpressionOutput;
					const FString OutputName = OutputExpression
						? OutputExpression->OutputName.ToString()
						: Output.Output.OutputName.ToString();
					const FString DeclarationName = MakeDreamShaderDeclarationName(OutputName, TEXT("Output"), OutputIndex);
					ReservedNames.Add(DeclarationName);
				}

				for (int32 OutputIndex = 0; OutputIndex < Outputs.Num(); ++OutputIndex)
				{
					const FFunctionExpressionOutput& Output = Outputs[OutputIndex];
					UMaterialExpressionFunctionOutput* OutputExpression = Output.ExpressionOutput;
					const FString OutputName = OutputExpression
						? OutputExpression->OutputName.ToString()
						: Output.Output.OutputName.ToString();
					const FString DeclarationName = MakeDreamShaderDeclarationName(OutputName, TEXT("Output"), OutputIndex);
					const FString MetadataSuffix = OutputExpression
						? MakeFunctionParameterMetadataSuffix(OutputExpression->Description, OutputExpression->SortPriority, OutputIndex)
						: FString();
					OutputDeclarations.Add(FString::Printf(
						TEXT("\t\t%s %s%s;"),
						*GetDreamShaderTypeForFunctionOutput(OutputExpression),
						*DeclarationName,
						*MetadataSuffix));

					if (OutputExpression && OutputExpression->A.IsConnected())
					{
						OutputAssignments.Add(FormatGraphSetStatement(DeclarationName, CompileInput(OutputExpression->A, TEXT("0.0"))));
					}
				}

				TArray<FString> Lines;
				DecompileSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Formatting DSF source for '%s'..."), *MaterialFunction->GetName())));
				Lines.Add(FString::Printf(TEXT("// Decompiled from %s"), *MaterialFunction->GetPathName()));
				if (!Warnings.IsEmpty())
				{
					for (const FString& Warning : Warnings)
					{
						Lines.Add(FString::Printf(TEXT("// Warning: %s"), *Warning));
					}
				}
				if (!VirtualFunctionDefinitions.IsEmpty())
				{
					Lines.Add(TEXT(""));
					Lines.Append(VirtualFunctionDefinitions);
				}

				Lines.Add(FString::Printf(TEXT("ShaderFunction(Name=\"%s\")"), *EscapeDreamShaderString(DecompiledName)));
				Lines.Add(TEXT("{"));
				AppendSection(Lines, TEXT("Properties"), PropertyDeclarations);
				AppendSection(Lines, TEXT("Inputs"), InputDeclarations);
				AppendSection(Lines, TEXT("Outputs"), OutputDeclarations);
				AppendSection(Lines, TEXT("Graph"), GraphLines, OutputAssignments);
				Lines.Add(TEXT("}"));

				OutSourceText = FString::Join(Lines, TEXT("\n"));
				return true;
			}

		private:
			struct FExpressionCallArgument
			{
				FString Name;
				FString Value;
				bool bInput = false;
			};

			void Reset()
			{
				PropertyDeclarations.Reset();
				PropertyNames.Reset();
				ReservedNames.Reset();
				FunctionInputNames.Reset();
				GraphLines.Reset();
				ExpressionTemps.Reset();
				ExpressionValues.Reset();
				TempNames.Reset();
				VirtualFunctionDefinitions.Reset();
				VirtualFunctionNames.Reset();
				Warnings.Reset();
				NextTempIndex = 0;
				ActiveDecompileSlowTask = nullptr;
				ProgressVisitedExpressions.Reset();
			}

			static void AppendSection(TArray<FString>& Lines, const TCHAR* SectionName, const TArray<FString>& LinesA)
			{
				TArray<FString> EmptyLines;
				AppendSection(Lines, SectionName, LinesA, EmptyLines);
			}

			static void AppendSection(TArray<FString>& Lines, const TCHAR* SectionName, const TArray<FString>& LinesA, const TArray<FString>& LinesB)
			{
				Lines.Add(FString::Printf(TEXT("\t%s = {"), SectionName));
				if (LinesA.IsEmpty() && LinesB.IsEmpty())
				{
					Lines.Add(TEXT("\t}"));
					Lines.Add(TEXT(""));
					return;
				}

				Lines.Append(LinesA);
				if (!LinesA.IsEmpty() && !LinesB.IsEmpty())
				{
					Lines.Add(TEXT(""));
				}
				Lines.Append(LinesB);
				Lines.Add(TEXT("\t}"));
				Lines.Add(TEXT(""));
			}

			FString MakeUniqueName(const FString& DesiredName, const TCHAR* FallbackPrefix)
			{
				FString BaseName = MakeDreamShaderDeclarationName(DesiredName, FallbackPrefix, NextTempIndex);
				FString Candidate = BaseName;
				int32 Suffix = 1;
				while (TempNames.Contains(Candidate)
					|| PropertyNames.Contains(Candidate)
					|| ReservedNames.Contains(Candidate)
					|| ContainsFunctionInputName(Candidate))
				{
					Candidate = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
				}

				TempNames.Add(Candidate);
				return Candidate;
			}

			bool ContainsFunctionInputName(const FString& Name) const
			{
				for (const TPair<const UMaterialExpressionFunctionInput*, FString>& Pair : FunctionInputNames)
				{
					if (Pair.Value.Equals(Name, ESearchCase::IgnoreCase))
					{
						return true;
					}
				}
				return false;
			}

			void AddPropertyDeclaration(const FString& Name, const FString& Declaration)
			{
				if (PropertyNames.Contains(Name))
				{
					return;
				}

				PropertyNames.Add(Name);
				ReservedNames.Add(Name);
				PropertyDeclarations.Add(TEXT("\t\t") + Declaration);
			}

			FString MakeUniquePropertyName(const FString& DesiredName, const TCHAR* FallbackPrefix)
			{
				FString BaseName = MakeDreamShaderDeclarationName(DesiredName, FallbackPrefix, PropertyNames.Num());
				FString Candidate = BaseName;
				int32 Suffix = 1;
				while (PropertyNames.Contains(Candidate)
					|| ReservedNames.Contains(Candidate)
					|| TempNames.Contains(Candidate)
					|| ContainsFunctionInputName(Candidate))
				{
					Candidate = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
				}
				return Candidate;
			}

			void AddParameterNameMetadataIfNeeded(TArray<FString>& Entries, const FString& DeclarationName, const FName ParameterName)
			{
				if (!ParameterName.IsNone() && !ParameterName.ToString().Equals(DeclarationName, ESearchCase::CaseSensitive))
				{
					Entries.Add(FString::Printf(
						TEXT("ParameterName=\"%s\";"),
						*EscapeDreamShaderString(ParameterName.ToString())));
				}
			}

			static FString BuildMetadataSuffix(const TArray<FString>& Entries)
			{
				if (Entries.IsEmpty())
				{
					return FString();
				}

				TArray<FString> Lines;
				Lines.Reserve(Entries.Num());
				for (const FString& Entry : Entries)
				{
					if (!Entry.TrimStartAndEnd().IsEmpty())
					{
						Lines.Add(TEXT("\t\t\t") + Entry.TrimStartAndEnd());
					}
				}

				return Lines.IsEmpty()
					? FString()
					: FString::Printf(TEXT(" [\n%s\n\t\t]"), *FString::Join(Lines, TEXT("\n")));
			}

			static void AddStringMetadata(TArray<FString>& Entries, const TCHAR* Key, const FString& Value)
			{
				if (!Value.TrimStartAndEnd().IsEmpty())
				{
					Entries.Add(FString::Printf(TEXT("%s=\"%s\";"), Key, *EscapeDreamShaderString(Value.TrimStartAndEnd())));
				}
			}

			static void AddIntMetadata(TArray<FString>& Entries, const TCHAR* Key, const int32 Value, const int32 DefaultValue)
			{
				if (Value != DefaultValue)
				{
					Entries.Add(FString::Printf(TEXT("%s=%d;"), Key, Value));
				}
			}

			static void AddBoolMetadata(TArray<FString>& Entries, const TCHAR* Key, const bool bValue, const bool bDefaultValue)
			{
				if (bValue != bDefaultValue)
				{
					Entries.Add(FString::Printf(TEXT("%s=%s;"), Key, bValue ? TEXT("true") : TEXT("false")));
				}
			}

			static void AddEnumMetadata(TArray<FString>& Entries, const TCHAR* Key, const UEnum* Enum, const int64 Value, const int64 DefaultValue)
			{
				if (Value != DefaultValue)
				{
					AddEnumMetadataAlways(Entries, Key, Enum, Value);
				}
			}

			static void AddEnumMetadataAlways(TArray<FString>& Entries, const TCHAR* Key, const UEnum* Enum, const int64 Value)
			{
				Entries.Add(FString::Printf(TEXT("%s=\"%s\";"), Key, *EscapeDreamShaderString(GetEnumLiteralText(Enum, Value))));
			}

			static FString BuildLiteralEnumArgument(const UEnum* Enum, const int64 Value)
			{
				return FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(GetEnumLiteralText(Enum, Value)));
			}

			static void AddParameterMetadata(TArray<FString>& Entries, const UMaterialExpressionParameter* Parameter)
			{
				if (!Parameter)
				{
					return;
				}

				if (!Parameter->Group.IsNone())
				{
					AddStringMetadata(Entries, TEXT("Group"), Parameter->Group.ToString());
				}
				AddIntMetadata(Entries, TEXT("SortPriority"), Parameter->SortPriority, 32);
				AddStringMetadata(Entries, TEXT("Description"), Parameter->Desc);
			}

			static void AddTextureParameterMetadata(TArray<FString>& Entries, const UMaterialExpressionTextureSampleParameter* Parameter)
			{
				if (!Parameter)
				{
					return;
				}

				if (!Parameter->Group.IsNone())
				{
					AddStringMetadata(Entries, TEXT("Group"), Parameter->Group.ToString());
				}
				AddIntMetadata(Entries, TEXT("SortPriority"), Parameter->SortPriority, 32);
				AddStringMetadata(Entries, TEXT("Description"), Parameter->Desc);
			}

			static void AddTextureParameterMetadata(TArray<FString>& Entries, const UMaterialExpressionTextureObjectParameter* Parameter)
			{
				AddTextureParameterMetadata(Entries, static_cast<const UMaterialExpressionTextureSampleParameter*>(Parameter));
			}

			static void AddTextureMetadata(TArray<FString>& Entries, const UMaterialExpressionTextureBase* TextureExpression)
			{
				if (!TextureExpression)
				{
					return;
				}

				AddEnumMetadataAlways(
					Entries,
					TEXT("SamplerType"),
					StaticEnum<EMaterialSamplerType>(),
					TextureExpression->SamplerType.GetValue());
				AddBoolMetadata(Entries, TEXT("IsDefaultMeshpaintTexture"), TextureExpression->IsDefaultMeshpaintTexture != 0, false);
			}

			static void AddTextureSampleMetadata(TArray<FString>& Entries, const UMaterialExpressionTextureSample* TextureSample)
			{
				if (!TextureSample)
				{
					return;
				}

				AddTextureMetadata(Entries, TextureSample);
				AddEnumMetadata(
					Entries,
					TEXT("SamplerSource"),
					StaticEnum<ESamplerSourceMode>(),
					TextureSample->SamplerSource.GetValue(),
					SSM_FromTextureAsset);
				AddEnumMetadata(
					Entries,
					TEXT("MipValueMode"),
					StaticEnum<ETextureMipValueMode>(),
					TextureSample->MipValueMode.GetValue(),
					TMVM_None);
				AddEnumMetadata(
					Entries,
					TEXT("GatherMode"),
					StaticEnum<ETextureGatherMode>(),
					TextureSample->GatherMode.GetValue(),
					TGM_None);
				AddBoolMetadata(Entries, TEXT("AutomaticViewMipBias"), TextureSample->AutomaticViewMipBias != 0, true);
				AddIntMetadata(Entries, TEXT("ConstCoordinate"), TextureSample->ConstCoordinate, 0);
				AddIntMetadata(Entries, TEXT("ConstMipValue"), TextureSample->ConstMipValue, INDEX_NONE);
			}

			static bool HasTextureSampleGraphInputs(const UMaterialExpressionTextureSample* TextureSample)
			{
				return TextureSample
					&& (TextureSample->Coordinates.IsConnected()
						|| TextureSample->TextureObject.IsConnected()
						|| TextureSample->MipValue.IsConnected()
						|| TextureSample->CoordinatesDX.IsConnected()
						|| TextureSample->CoordinatesDY.IsConnected()
						|| TextureSample->AutomaticViewMipBiasValue.IsConnected());
			}

			void AddTextureSampleExpressionArguments(UMaterialExpressionTextureSample* TextureSample, TArray<FExpressionCallArgument>& Arguments)
			{
				if (!TextureSample)
				{
					return;
				}

				if (TextureSample->Coordinates.IsConnected())
				{
					Arguments.Add({ TEXT("Coordinates"), CompileInput(TextureSample->Coordinates, TEXT("0.0")), true });
				}
				else if (TextureSample->ConstCoordinate != 0)
				{
					Arguments.Add({ TEXT("ConstCoordinate"), FString::Printf(TEXT("%d"), TextureSample->ConstCoordinate), false });
				}
				if (TextureSample->TextureObject.IsConnected())
				{
					Arguments.Add({ TEXT("TextureObject"), CompileInput(TextureSample->TextureObject, TEXT("default")), true });
				}
				else if (TextureSample->Texture)
				{
					Arguments.Add({ TEXT("Texture"), MakeDreamShaderObjectPathLiteral(TextureSample->Texture), false });
				}
				if (TextureSample->MipValue.IsConnected())
				{
					Arguments.Add({ TEXT("MipValue"), CompileInput(TextureSample->MipValue, TEXT("0.0")), true });
				}
				if (TextureSample->CoordinatesDX.IsConnected())
				{
					Arguments.Add({ TEXT("CoordinatesDX"), CompileInput(TextureSample->CoordinatesDX, TEXT("0.0")), true });
				}
				if (TextureSample->CoordinatesDY.IsConnected())
				{
					Arguments.Add({ TEXT("CoordinatesDY"), CompileInput(TextureSample->CoordinatesDY, TEXT("0.0")), true });
				}
				if (TextureSample->AutomaticViewMipBiasValue.IsConnected())
				{
					Arguments.Add({ TEXT("AutomaticViewMipBiasValue"), CompileInput(TextureSample->AutomaticViewMipBiasValue, TEXT("0.0")), true });
				}

				Arguments.Add({ TEXT("SamplerType"), BuildLiteralEnumArgument(StaticEnum<EMaterialSamplerType>(), TextureSample->SamplerType.GetValue()), false });
				if (TextureSample->SamplerSource.GetValue() != SSM_FromTextureAsset)
				{
					Arguments.Add({ TEXT("SamplerSource"), BuildLiteralEnumArgument(StaticEnum<ESamplerSourceMode>(), TextureSample->SamplerSource.GetValue()), false });
				}
				if (TextureSample->MipValueMode.GetValue() != TMVM_None)
				{
					Arguments.Add({ TEXT("MipValueMode"), BuildLiteralEnumArgument(StaticEnum<ETextureMipValueMode>(), TextureSample->MipValueMode.GetValue()), false });
				}
				if (TextureSample->GatherMode.GetValue() != TGM_None)
				{
					Arguments.Add({ TEXT("GatherMode"), BuildLiteralEnumArgument(StaticEnum<ETextureGatherMode>(), TextureSample->GatherMode.GetValue()), false });
				}
				if (!TextureSample->AutomaticViewMipBias)
				{
					Arguments.Add({ TEXT("AutomaticViewMipBias"), TEXT("false"), false });
				}
				if (TextureSample->ConstMipValue != INDEX_NONE)
				{
					Arguments.Add({ TEXT("ConstMipValue"), FString::Printf(TEXT("%d"), TextureSample->ConstMipValue), false });
				}
			}

			static void AddTextureSampleParameterExpressionArguments(
				const UMaterialExpressionTextureSampleParameter* TextureParameter,
				TArray<FExpressionCallArgument>& Arguments)
			{
				if (!TextureParameter)
				{
					return;
				}

				if (!TextureParameter->ParameterName.IsNone())
				{
					Arguments.Add({
						TEXT("ParameterName"),
						FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(TextureParameter->ParameterName.ToString())),
						false
					});
				}
				if (!TextureParameter->Group.IsNone())
				{
					Arguments.Add({
						TEXT("Group"),
						FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(TextureParameter->Group.ToString())),
						false
					});
				}
				if (TextureParameter->SortPriority != 32)
				{
					Arguments.Add({ TEXT("SortPriority"), FString::Printf(TEXT("%d"), TextureParameter->SortPriority), false });
				}
				if (!TextureParameter->Desc.IsEmpty())
				{
					Arguments.Add({
						TEXT("Desc"),
						FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(TextureParameter->Desc)),
						false
					});
				}
			}

			static int32 GetOutputComponentCount(const UMaterialExpression* Expression, const int32 OutputIndex)
			{
				return GetExpressionOutputComponentCount(const_cast<UMaterialExpression*>(Expression), OutputIndex);
			}

			static int32 GetComponentCountForExpressionOutput(UMaterialExpression* Expression, const int32 OutputIndex)
			{
				return GetExpressionOutputComponentCount(Expression, OutputIndex);
			}

			static bool IsTextureObjectOutput(UMaterialExpression* Expression, const int32 OutputIndex)
			{
				return Expression && GetDreamShaderTypeForExpressionOutput(Expression, OutputIndex).StartsWith(TEXT("Texture"));
			}

			static int32 FindExpressionOutputIndexByName(
				const UMaterialExpression* Expression,
				const TCHAR* OutputName,
				const int32 FallbackIndex)
			{
				if (!Expression || !OutputName)
				{
					return FallbackIndex;
				}

				const FString DesiredOutputName(OutputName);
				for (int32 CandidateIndex = 0; CandidateIndex < Expression->Outputs.Num(); ++CandidateIndex)
				{
					const FExpressionOutput& Output = Expression->Outputs[CandidateIndex];
					if (!Output.OutputName.IsNone()
						&& Output.OutputName.ToString().Equals(DesiredOutputName, ESearchCase::IgnoreCase))
					{
						return CandidateIndex;
					}
				}

				return FallbackIndex;
			}

			static FDecompiledValue MakeValue(
				const FString& Text,
				const FString& Type,
				const int32 ComponentCount,
				const bool bIsSimple,
				const bool bIsTextureObject = false,
				const bool bIsMaterialAttributes = false)
			{
				FDecompiledValue Value;
				Value.Text = Text;
				Value.Type = Type;
				Value.ComponentCount = ComponentCount;
				Value.bIsSimple = bIsSimple;
				Value.bIsTextureObject = bIsTextureObject;
				Value.bIsMaterialAttributes = bIsMaterialAttributes;
				return Value;
			}

			static FDecompiledValue MakeExpressionValue(
				UMaterialExpression* Expression,
				const int32 OutputIndex,
				const FString& Text,
				const bool bIsSimple)
			{
				const FString Type = GetDreamShaderTypeForExpressionOutput(Expression, OutputIndex);
				const bool bIsTextureObject = IsTextureObjectOutput(Expression, OutputIndex);
				return MakeValue(
					Text,
					Type,
					bIsTextureObject ? 0 : GetComponentCountForExpressionOutput(Expression, OutputIndex),
					bIsSimple,
					bIsTextureObject,
					Type.Equals(TEXT("MaterialAttributes"), ESearchCase::IgnoreCase));
			}

			FDecompiledValue MakeSwizzledValue(const FDecompiledValue& Source, const FString& SwizzleText)
			{
				if (SwizzleText.IsEmpty())
				{
					return Source;
				}
				if (Source.ComponentCount == 1 && IsSwizzleText(SwizzleText))
				{
					if (SwizzleText.Len() == 1)
					{
						return Source;
					}

					TArray<FDecompiledValue> Parts;
					Parts.Reserve(SwizzleText.Len());
					for (int32 Index = 0; Index < SwizzleText.Len(); ++Index)
					{
						Parts.Add(Source);
					}
					return MakeFunctionValue(
						TEXT("float") + FString::FromInt(SwizzleText.Len()),
						Parts,
						SwizzleText.Len());
				}

				const FString Text = MakeSwizzleExpression(Source.Text, SwizzleText);
				return MakeValue(
					Text,
					GetDreamShaderTypeForComponentCount(SwizzleText.Len()),
					SwizzleText.Len(),
					!Text.Contains(TEXT("\n")),
					false,
					false);
			}

			FString AddTemp(const FString& Type, const FString& ExpressionText, const FString& BaseName)
			{
				const FString Name = MakeUniqueName(BaseName, TEXT("Node"));
				GraphLines.Add(FormatGraphAssignment(Type, Name, ExpressionText));
				++NextTempIndex;
				return Name;
			}

			FString AddTempWithName(const FString& Type, const FString& ExpressionText, const FString& Name)
			{
				TempNames.Add(Name);
				ReservedNames.Add(Name);
				GraphLines.Add(FormatGraphAssignment(Type, Name, ExpressionText));
				++NextTempIndex;
				return Name;
			}

			static FString FormatGraphAssignment(const FString& Type, const FString& Name, const FString& ExpressionText)
			{
				if (ExpressionText.Contains(TEXT("\n")))
				{
					return FString::Printf(TEXT("\t\t%s %s =\n%s;"), *Type, *Name, *IndentMultiline(ExpressionText, TEXT("\t\t\t")));
				}

				return FString::Printf(TEXT("\t\t%s %s = %s;"), *Type, *Name, *ExpressionText);
			}

			static FString FormatGraphSetStatement(const FString& TargetName, const FString& ExpressionText)
			{
				if (ExpressionText.Contains(TEXT("\n")))
				{
					return FString::Printf(TEXT("\t\t%s =\n%s;"), *TargetName, *IndentMultiline(ExpressionText, TEXT("\t\t\t")));
				}

				return FString::Printf(TEXT("\t\t%s = %s;"), *TargetName, *ExpressionText);
			}

			static FString IndentMultiline(const FString& Text, const TCHAR* Indent)
			{
				TArray<FString> Lines;
				Text.ParseIntoArrayLines(Lines, false);
				for (FString& Line : Lines)
				{
					Line = FString(Indent) + Line;
				}
				return FString::Join(Lines, TEXT("\n"));
			}

			FDecompiledValue AddTempValue(const FDecompiledValue& Value, const FString& BaseName)
			{
				if (Value.bIsSimple)
				{
					return Value;
				}

				return MakeValue(
					AddTemp(Value.Type, Value.Text, BaseName),
					Value.Type,
					Value.ComponentCount,
					true,
					Value.bIsTextureObject,
					Value.bIsMaterialAttributes);
			}

			FDecompiledValue AddTempValueWithName(const FDecompiledValue& Value, const FString& Name)
			{
				if (Value.bIsSimple)
				{
					return Value;
				}

				return MakeValue(
					AddTempWithName(Value.Type, Value.Text, Name),
					Value.Type,
					Value.ComponentCount,
					true,
					Value.bIsTextureObject,
					Value.bIsMaterialAttributes);
			}

			FDecompiledValue MaybeMaterializeValue(const FDecompiledValue& Value, const FString& BaseName)
			{
				if (Value.bIsSimple)
				{
					return Value;
				}

				const FString TrimmedText = Value.Text.TrimStartAndEnd();
				if (!Value.Text.Contains(TEXT("\n")) && !TrimmedText.StartsWith(TEXT("UE.Expression(")))
				{
					return Value;
				}

				return AddTempValue(Value, BaseName);
			}

			FDecompiledValue CacheExpressionValue(const FDecompiledExpressionKey& Key, const FDecompiledValue& Value)
			{
				ExpressionTemps.Add(Key, Value.Text);
				ExpressionValues.Add(Key, Value);
				return Value;
			}

			FDecompiledValue CacheTempExpressionValue(const FDecompiledExpressionKey& Key, const FDecompiledValue& Value, const FString& BaseName)
			{
				return CacheExpressionValue(Key, AddTempValue(Value, BaseName));
			}

			FDecompiledValue CacheNamedTempExpressionValue(const FDecompiledExpressionKey& Key, const FDecompiledValue& Value, const FString& Name)
			{
				return CacheExpressionValue(Key, AddTempValueWithName(Value, Name));
			}

			FString CompileInput(const FExpressionInput& Input, const FString& DefaultText)
			{
				return CompileInputValue(Input, MakeValue(DefaultText, TEXT("float"), 1, true)).Text;
			}

			FDecompiledValue CompileInputValue(const FExpressionInput& Input, const FDecompiledValue& DefaultValue)
			{
				const FExpressionInput TracedInput = Input.GetTracedInput();
				if (!TracedInput.Expression)
				{
					return DefaultValue;
				}

				FDecompiledValue Value = CompileExpressionValue(TracedInput.Expression, TracedInput.OutputIndex);
				const FString MaskSuffix = MakeInputMaskSuffix(Input);
				if (!MaskSuffix.IsEmpty())
				{
					Value = MakeSwizzledValue(Value, MaskSuffix);
				}
				return MaybeMaterializeValue(Value, TracedInput.Expression->GetName());
			}

			FString CompileConnectedOrLiteral(const FExpressionInput& Input, const FString& LiteralText)
			{
				return Input.IsConnected() ? CompileInput(Input, LiteralText) : LiteralText;
			}

			FDecompiledValue CompileConnectedOrLiteralValue(
				const FExpressionInput& Input,
				const FString& LiteralText,
				const FString& Type,
				const int32 ComponentCount)
			{
				return Input.IsConnected()
					? CompileInputValue(Input, MakeValue(LiteralText, Type, ComponentCount, true))
					: MakeValue(LiteralText, Type, ComponentCount, true);
			}

			FDecompiledValue MakeBinaryValue(
				const FString& Operator,
				const FDecompiledValue& Left,
				const FDecompiledValue& Right)
			{
				const int32 ComponentCount = FMath::Max(Left.ComponentCount, Right.ComponentCount);
				return MakeValue(
					FString::Printf(TEXT("(%s %s %s)"), *Left.Text, *Operator, *Right.Text),
					GetDreamShaderTypeForComponentCount(ComponentCount),
					ComponentCount,
					false);
			}

			FDecompiledValue MakeFunctionValue(const FString& FunctionName, const TArray<FDecompiledValue>& Arguments, const int32 ComponentCount)
			{
				TArray<FString> ArgumentTexts;
				ArgumentTexts.Reserve(Arguments.Num());
				for (const FDecompiledValue& Argument : Arguments)
				{
					ArgumentTexts.Add(Argument.Text);
				}

				return MakeValue(
					FString::Printf(TEXT("%s(%s)"), *FunctionName, *FString::Join(ArgumentTexts, TEXT(", "))),
					GetDreamShaderTypeForComponentCount(ComponentCount),
					ComponentCount,
					false);
			}

			static int32 GetCommonNumericComponentCount(const FDecompiledValue& A, const FDecompiledValue& B)
			{
				if (A.bIsMaterialAttributes || B.bIsMaterialAttributes)
				{
					return 0;
				}

				return FMath::Max(A.ComponentCount, B.ComponentCount);
			}

			FDecompiledValue MakeExpressionValueWithComponentCount(
				UMaterialExpression* Expression,
				const int32 OutputIndex,
				const FString& Text,
				const bool bIsSimple,
				const int32 ComponentCount)
			{
				if (ComponentCount <= 0)
				{
					return MakeExpressionValue(Expression, OutputIndex, Text, bIsSimple);
				}

				return MakeValue(
					Text,
					GetDreamShaderTypeForComponentCount(ComponentCount),
					ComponentCount,
					bIsSimple);
			}

			FString BuildUEExpressionCallWithOutputType(
				UMaterialExpression* Expression,
				const int32 OutputIndex,
				const FString& OutputType,
				const TArray<FExpressionCallArgument>& Arguments) const
			{
				TArray<FString> ArgumentTexts;
				ArgumentTexts.Add(FString::Printf(TEXT("Class=\"%s\""), *GetMaterialExpressionShortName(Expression ? Expression->GetClass() : nullptr)));
				ArgumentTexts.Add(FString::Printf(TEXT("OutputType=\"%s\""), *OutputType));
				if (OutputIndex > 0)
				{
					ArgumentTexts.Add(FString::Printf(TEXT("OutputIndex=%d"), OutputIndex));
				}

				for (const FExpressionCallArgument& Argument : Arguments)
				{
					if (!Argument.Value.TrimStartAndEnd().IsEmpty() && !Argument.Value.Equals(TEXT("default"), ESearchCase::CaseSensitive))
					{
						ArgumentTexts.Add(FString::Printf(TEXT("%s=%s"), *Argument.Name, *Argument.Value));
					}
				}

				bool bUseMultiline = ArgumentTexts.Num() > 3;
				if (!bUseMultiline)
				{
					const FString SingleLine = FString::Printf(TEXT("UE.Expression(%s)"), *FString::Join(ArgumentTexts, TEXT(", ")));
					bUseMultiline = SingleLine.Len() > 120;
					if (!bUseMultiline)
					{
						return SingleLine;
					}
				}

				TArray<FString> Lines;
				Lines.Reserve(ArgumentTexts.Num());
				for (int32 ArgumentIndex = 0; ArgumentIndex < ArgumentTexts.Num(); ++ArgumentIndex)
				{
					Lines.Add(FString::Printf(
						TEXT("\t%s%s"),
						*ArgumentTexts[ArgumentIndex],
						ArgumentIndex + 1 < ArgumentTexts.Num() ? TEXT(",") : TEXT("")));
				}

				return FString::Printf(TEXT("UE.Expression(\n%s\n)"), *FString::Join(Lines, TEXT("\n")));
			}

			static bool TryCombineAppendSwizzle(
				const FDecompiledValue& A,
				const FDecompiledValue& B,
				FDecompiledValue& OutValue)
			{
				FString BaseA;
				FString SwizzleA;
				FString BaseB;
				FString SwizzleB;
				if (!TrySplitTrailingSwizzle(A.Text, BaseA, SwizzleA)
					|| !TrySplitTrailingSwizzle(B.Text, BaseB, SwizzleB)
					|| !BaseA.Equals(BaseB, ESearchCase::CaseSensitive)
					|| SwizzleA.Len() != A.ComponentCount
					|| SwizzleB.Len() != B.ComponentCount)
				{
					return false;
				}

				const FString CombinedSwizzle = SwizzleA + SwizzleB;
				if (!IsSwizzleText(CombinedSwizzle))
				{
					return false;
				}

				const FString Text = MakeSwizzleExpression(BaseA, CombinedSwizzle);
				OutValue = MakeValue(
					Text,
					GetDreamShaderTypeForComponentCount(CombinedSwizzle.Len()),
					CombinedSwizzle.Len(),
					!Text.Contains(TEXT("\n")));
				return true;
			}

			FString CompileExpression(UMaterialExpression* Expression, const int32 OutputIndex)
			{
				return CompileExpressionValue(Expression, OutputIndex).Text;
			}

			FDecompiledValue CompileExpressionValue(UMaterialExpression* Expression, const int32 OutputIndex)
			{
				if (!Expression)
				{
					return MakeValue(TEXT("0.0"), TEXT("float"), 1, true);
				}

				EnterExpressionProgressFrame(Expression);

				const FDecompiledExpressionKey Key{ Expression, OutputIndex };
				if (const FString* ExistingTemp = ExpressionTemps.Find(Key))
				{
					if (const FDecompiledValue* ExistingValue = ExpressionValues.Find(Key))
					{
						return *ExistingValue;
					}
					return MakeValue(*ExistingTemp, GetDreamShaderTypeForExpressionOutput(Expression, OutputIndex), GetComponentCountForExpressionOutput(Expression, OutputIndex), true);
				}

				if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
				{
					if (const FString* InputName = FunctionInputNames.Find(FunctionInput))
					{
						return MakeValue(
							*InputName,
							GetDreamShaderTypeForFunctionInput(FunctionInput->InputType.GetValue()),
							FunctionInput->InputType == FunctionInput_Scalar ? 1 : GetComponentCountForExpressionOutput(Expression, OutputIndex),
							true);
					}
					return MakeValue(
						MakeDreamShaderDeclarationName(FunctionInput->InputName.ToString(), TEXT("Input"), 0),
						GetDreamShaderTypeForFunctionInput(FunctionInput->InputType.GetValue()),
						FunctionInput->InputType == FunctionInput_Scalar ? 1 : GetComponentCountForExpressionOutput(Expression, OutputIndex),
						true);
				}

				if (UMaterialExpressionCurveAtlasRowParameter* CurveAtlas = Cast<UMaterialExpressionCurveAtlasRowParameter>(Expression))
				{
					TArray<FExpressionCallArgument> Arguments;
					if (!CurveAtlas->ParameterName.IsNone())
					{
						Arguments.Add({
							TEXT("ParameterName"),
							FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(CurveAtlas->ParameterName.ToString())),
							false });
					}
					if (!CurveAtlas->Group.IsNone())
					{
						Arguments.Add({
							TEXT("Group"),
							FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(CurveAtlas->Group.ToString())),
							false });
					}
					if (CurveAtlas->SortPriority != 32)
					{
						Arguments.Add({ TEXT("SortPriority"), FString::FromInt(CurveAtlas->SortPriority), false });
					}
					if (!CurveAtlas->Desc.IsEmpty())
					{
						Arguments.Add({
							TEXT("Desc"),
							FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(CurveAtlas->Desc)),
							false });
					}
					Arguments.Add({ TEXT("DefaultValue"), FormatDreamShaderFloat(CurveAtlas->DefaultValue), false });
					if (CurveAtlas->Curve)
					{
						Arguments.Add({ TEXT("Curve"), MakeDreamShaderObjectPathLiteral(CurveAtlas->Curve.Get()), false });
					}
					if (CurveAtlas->Atlas)
					{
						Arguments.Add({ TEXT("Atlas"), MakeDreamShaderObjectPathLiteral(CurveAtlas->Atlas.Get()), false });
					}
					if (CurveAtlas->bUseCustomPrimitiveData)
					{
						Arguments.Add({ TEXT("UseCustomPrimitiveData"), TEXT("true"), false });
						Arguments.Add({ TEXT("PrimitiveDataIndex"), FString::FromInt(CurveAtlas->PrimitiveDataIndex), false });
					}
					if (CurveAtlas->InputTime.GetTracedInput().Expression)
					{
						Arguments.Add({
							TEXT("CurveTime"),
							CompileInputValue(CurveAtlas->InputTime, MakeValue(TEXT("0.0"), TEXT("float"), 1, true)).Text,
							true });
					}

					const FString BaseName = CurveAtlas->ParameterName.IsNone()
						? TEXT("CurveAtlas")
						: MakeDreamShaderDeclarationName(CurveAtlas->ParameterName.ToString(), TEXT("CurveAtlas"), 0);
					return CacheTempExpressionValue(
						Key,
						MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, Arguments), false),
						BaseName);
				}

				if (UMaterialExpressionScalarParameter* ScalarParameter = Cast<UMaterialExpressionScalarParameter>(Expression))
				{
					const FString Name = MakeUniquePropertyName(ScalarParameter->ParameterName.ToString(), TEXT("Scalar"));
					TArray<FString> MetadataEntries;
					AddParameterNameMetadataIfNeeded(MetadataEntries, Name, ScalarParameter->ParameterName);
					AddParameterMetadata(MetadataEntries, ScalarParameter);
					AddPropertyDeclaration(
						Name,
						FString::Printf(
							TEXT("ScalarParameter %s = %s%s;"),
							*Name,
							*FormatDreamShaderFloat(ScalarParameter->DefaultValue),
							*BuildMetadataSuffix(MetadataEntries)));
					return MakeValue(Name, TEXT("float"), 1, true);
				}

				if (UMaterialExpressionVectorParameter* VectorParameter = Cast<UMaterialExpressionVectorParameter>(Expression))
				{
					const FString Name = MakeUniquePropertyName(VectorParameter->ParameterName.ToString(), TEXT("Vector"));
					TArray<FString> MetadataEntries;
					AddParameterNameMetadataIfNeeded(MetadataEntries, Name, VectorParameter->ParameterName);
					AddParameterMetadata(MetadataEntries, VectorParameter);
					AddPropertyDeclaration(
						Name,
						FString::Printf(
							TEXT("VectorParameter %s = %s%s;"),
							*Name,
							*FormatDreamShaderColor(VectorParameter->DefaultValue),
							*BuildMetadataSuffix(MetadataEntries)));
					return MakeExpressionOutputValue(MakeExpressionValue(Expression, 0, Name, true), Expression, OutputIndex);
				}

				if (UMaterialExpressionTextureObjectParameter* TextureObjectParameter = Cast<UMaterialExpressionTextureObjectParameter>(Expression))
				{
					const FString Name = MakeUniquePropertyName(TextureObjectParameter->ParameterName.ToString(), TEXT("Texture"));
					const FString DefaultValue = TextureObjectParameter->Texture
						? FString::Printf(TEXT(" = %s"), *MakeDreamShaderObjectPathLiteral(TextureObjectParameter->Texture))
						: FString();
					TArray<FString> MetadataEntries;
					AddParameterNameMetadataIfNeeded(MetadataEntries, Name, TextureObjectParameter->ParameterName);
					AddTextureParameterMetadata(MetadataEntries, TextureObjectParameter);
					AddTextureSampleMetadata(MetadataEntries, TextureObjectParameter);
					AddPropertyDeclaration(
						Name,
						FString::Printf(
							TEXT("TextureObjectParameter %s%s%s;"),
							*Name,
							*DefaultValue,
							*BuildMetadataSuffix(MetadataEntries)));
					return MakeValue(Name, TEXT("Texture2D"), 0, true, true);
				}

				if (UMaterialExpressionTextureSampleParameter2D* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter2D>(Expression))
				{
					const FString Name = MakeUniquePropertyName(TextureParameter->ParameterName.ToString(), TEXT("Texture"));
					if (HasTextureSampleGraphInputs(TextureParameter))
					{
						TArray<FExpressionCallArgument> Arguments;
						AddTextureSampleParameterExpressionArguments(TextureParameter, Arguments);
						AddTextureSampleExpressionArguments(TextureParameter, Arguments);

						const int32 RgbaOutputIndex = FindExpressionOutputIndexByName(Expression, TEXT("RGBA"), OutputIndex);
						const FDecompiledExpressionKey RgbaKey{ Expression, RgbaOutputIndex };
						FDecompiledValue RgbaValue;
						if (const FDecompiledValue* ExistingValue = ExpressionValues.Find(RgbaKey))
						{
							RgbaValue = *ExistingValue;
						}
						else
						{
							const FString TempName = MakeUniqueName(Name, TEXT("Texture"));
							RgbaValue = CacheNamedTempExpressionValue(
								RgbaKey,
								MakeValue(BuildUEExpressionCall(Expression, RgbaOutputIndex, Arguments), TEXT("float4"), 4, false),
								TempName);
						}

						if (OutputIndex == RgbaOutputIndex)
						{
							return RgbaValue;
						}
						return MakeExpressionOutputValue(RgbaValue, Expression, OutputIndex);
					}

					const FString DefaultValue = TextureParameter->Texture
						? FString::Printf(TEXT(" = %s"), *MakeDreamShaderObjectPathLiteral(TextureParameter->Texture))
						: FString();
					TArray<FString> MetadataEntries;
					AddParameterNameMetadataIfNeeded(MetadataEntries, Name, TextureParameter->ParameterName);
					AddTextureParameterMetadata(MetadataEntries, TextureParameter);
					AddTextureSampleMetadata(MetadataEntries, TextureParameter);
					AddPropertyDeclaration(
						Name,
						FString::Printf(
							TEXT("TextureSampleParameter2D %s%s%s;"),
							*Name,
							*DefaultValue,
							*BuildMetadataSuffix(MetadataEntries)));
					const int32 RgbaOutputIndex = FindExpressionOutputIndexByName(Expression, TEXT("RGBA"), 0);
					return MakeExpressionOutputValue(MakeExpressionValue(Expression, RgbaOutputIndex, Name, true), Expression, OutputIndex);
				}

				if (UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expression))
				{
					return MakeValue(FormatDreamShaderFloat(Constant->R), TEXT("float"), 1, true);
				}

				if (UMaterialExpressionConstant2Vector* Constant2 = Cast<UMaterialExpressionConstant2Vector>(Expression))
				{
					return MakeValue(FormatDreamShaderVector2(Constant2->R, Constant2->G), TEXT("float2"), 2, true);
				}

				if (UMaterialExpressionConstant3Vector* Constant3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
				{
					return MakeValue(FormatDreamShaderVector3(Constant3->Constant.R, Constant3->Constant.G, Constant3->Constant.B), TEXT("float3"), 3, true);
				}

				if (UMaterialExpressionConstant4Vector* Constant4 = Cast<UMaterialExpressionConstant4Vector>(Expression))
				{
					return MakeValue(FormatDreamShaderColor(Constant4->Constant), TEXT("float4"), 4, true);
				}

				if (UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expression))
				{
					return MakeBinaryValue(
						TEXT("+"),
						CompileConnectedOrLiteralValue(Add->A, FormatDreamShaderFloat(Add->ConstA), TEXT("float"), 1),
						CompileConnectedOrLiteralValue(Add->B, FormatDreamShaderFloat(Add->ConstB), TEXT("float"), 1));
				}

				if (UMaterialExpressionSubtract* Subtract = Cast<UMaterialExpressionSubtract>(Expression))
				{
					return MakeBinaryValue(
						TEXT("-"),
						CompileConnectedOrLiteralValue(Subtract->A, FormatDreamShaderFloat(Subtract->ConstA), TEXT("float"), 1),
						CompileConnectedOrLiteralValue(Subtract->B, FormatDreamShaderFloat(Subtract->ConstB), TEXT("float"), 1));
				}

				if (UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
				{
					return MakeBinaryValue(
						TEXT("*"),
						CompileConnectedOrLiteralValue(Multiply->A, FormatDreamShaderFloat(Multiply->ConstA), TEXT("float"), 1),
						CompileConnectedOrLiteralValue(Multiply->B, FormatDreamShaderFloat(Multiply->ConstB), TEXT("float"), 1));
				}

				if (UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(Expression))
				{
					return MakeBinaryValue(
						TEXT("/"),
						CompileConnectedOrLiteralValue(Divide->A, FormatDreamShaderFloat(Divide->ConstA), TEXT("float"), 1),
						CompileConnectedOrLiteralValue(Divide->B, FormatDreamShaderFloat(Divide->ConstB), TEXT("float"), 1));
				}

				if (UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Expression))
				{
					FDecompiledValue A = CompileConnectedOrLiteralValue(Lerp->A, FormatDreamShaderFloat(Lerp->ConstA), TEXT("float"), 1);
					FDecompiledValue B = CompileConnectedOrLiteralValue(Lerp->B, FormatDreamShaderFloat(Lerp->ConstB), TEXT("float"), 1);
					FDecompiledValue Alpha = CompileConnectedOrLiteralValue(Lerp->Alpha, FormatDreamShaderFloat(Lerp->ConstAlpha), TEXT("float"), 1);
					return MakeFunctionValue(TEXT("lerp"), { A, B, Alpha }, GetCommonNumericComponentCount(A, B));
				}

				if (UMaterialExpressionClamp* Clamp = Cast<UMaterialExpressionClamp>(Expression))
				{
					if (Clamp->ClampMode != CMODE_Clamp)
					{
						return MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, {
							{ TEXT("Input"), CompileInput(Clamp->Input, TEXT("0.0")), true },
							{ TEXT("Min"), CompileConnectedOrLiteral(Clamp->Min, FormatDreamShaderFloat(Clamp->MinDefault)), true },
							{ TEXT("Max"), CompileConnectedOrLiteral(Clamp->Max, FormatDreamShaderFloat(Clamp->MaxDefault)), true },
							{ TEXT("ClampMode"), Clamp->ClampMode == CMODE_ClampMin ? TEXT("\"CMODE_ClampMin\"") : TEXT("\"CMODE_ClampMax\""), false },
						}), false);
					}
					FDecompiledValue Input = CompileInputValue(Clamp->Input, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					FDecompiledValue Min = CompileConnectedOrLiteralValue(Clamp->Min, FormatDreamShaderFloat(Clamp->MinDefault), TEXT("float"), 1);
					FDecompiledValue Max = CompileConnectedOrLiteralValue(Clamp->Max, FormatDreamShaderFloat(Clamp->MaxDefault), TEXT("float"), 1);
					return MakeFunctionValue(TEXT("clamp"), { Input, Min, Max }, Input.ComponentCount);
				}

				if (UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
				{
					FString Suffix;
					if (Mask->R)
					{
						Suffix += TEXT("r");
					}
					if (Mask->G)
					{
						Suffix += TEXT("g");
					}
					if (Mask->B)
					{
						Suffix += TEXT("b");
					}
					if (Mask->A)
					{
						Suffix += TEXT("a");
					}
					FDecompiledValue Source = CompileInputValue(Mask->Input, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					return MakeSwizzledValue(Source, Suffix);
				}

				if (UMaterialExpressionStaticComponentMaskParameter* StaticComponentMask = Cast<UMaterialExpressionStaticComponentMaskParameter>(Expression))
				{
					TArray<FExpressionCallArgument> Arguments = {
						{ TEXT("Input"), CompileInput(StaticComponentMask->Input, TEXT("0.0")), true },
						{ TEXT("DefaultR"), StaticComponentMask->DefaultR ? TEXT("true") : TEXT("false"), false },
						{ TEXT("DefaultG"), StaticComponentMask->DefaultG ? TEXT("true") : TEXT("false"), false },
						{ TEXT("DefaultB"), StaticComponentMask->DefaultB ? TEXT("true") : TEXT("false"), false },
						{ TEXT("DefaultA"), StaticComponentMask->DefaultA ? TEXT("true") : TEXT("false"), false },
					};
					if (!StaticComponentMask->ParameterName.IsNone())
					{
						Arguments.Add({
							TEXT("ParameterName"),
							FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(StaticComponentMask->ParameterName.ToString())),
							false
						});
					}

					const int32 ComponentCount =
						(StaticComponentMask->DefaultR ? 1 : 0)
						+ (StaticComponentMask->DefaultG ? 1 : 0)
						+ (StaticComponentMask->DefaultB ? 1 : 0)
						+ (StaticComponentMask->DefaultA ? 1 : 0);
					const FString OutputType = GetDreamShaderTypeForComponentCount(ComponentCount > 0 ? ComponentCount : 1);
					return MakeExpressionValueWithComponentCount(
						Expression,
						OutputIndex,
						BuildUEExpressionCallWithOutputType(Expression, OutputIndex, OutputType, Arguments),
						false,
						ComponentCount > 0 ? ComponentCount : 1);
				}

				if (UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(Expression))
				{
					FDecompiledValue A = CompileInputValue(Append->A, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					FDecompiledValue B = CompileInputValue(Append->B, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					FDecompiledValue CombinedValue;
					if (TryCombineAppendSwizzle(A, B, CombinedValue))
					{
						return CombinedValue;
					}
					return MakeFunctionValue(TEXT("float") + FString::FromInt(A.ComponentCount + B.ComponentCount), { A, B }, A.ComponentCount + B.ComponentCount);
				}

				if (UMaterialExpressionOneMinus* OneMinus = Cast<UMaterialExpressionOneMinus>(Expression))
				{
					return MakeBinaryValue(TEXT("-"), MakeValue(TEXT("1.0"), TEXT("float"), 1, true), CompileInputValue(OneMinus->Input, MakeValue(TEXT("0.0"), TEXT("float"), 1, true)));
				}

				if (UMaterialExpressionPower* Power = Cast<UMaterialExpressionPower>(Expression))
				{
					FDecompiledValue Base = CompileInputValue(Power->Base, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					FDecompiledValue Exponent = CompileConnectedOrLiteralValue(Power->Exponent, FormatDreamShaderFloat(Power->ConstExponent), TEXT("float"), 1);
					return MakeFunctionValue(TEXT("pow"), { Base, Exponent }, Base.ComponentCount);
				}

				if (UMaterialExpressionDotProduct* Dot = Cast<UMaterialExpressionDotProduct>(Expression))
				{
					return MakeFunctionValue(
						TEXT("dot"),
						{
							CompileInputValue(Dot->A, MakeValue(TEXT("0.0"), TEXT("float"), 1, true)),
							CompileInputValue(Dot->B, MakeValue(TEXT("0.0"), TEXT("float"), 1, true))
						},
						1);
				}

				if (UMaterialExpressionNormalize* Normalize = Cast<UMaterialExpressionNormalize>(Expression))
				{
					FDecompiledValue Input = CompileInputValue(Normalize->VectorInput, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					return MakeFunctionValue(TEXT("normalize"), { Input }, Input.ComponentCount);
				}

				if (UMaterialExpressionMin* Min = Cast<UMaterialExpressionMin>(Expression))
				{
					FDecompiledValue A = CompileConnectedOrLiteralValue(Min->A, FormatDreamShaderFloat(Min->ConstA), TEXT("float"), 1);
					FDecompiledValue B = CompileConnectedOrLiteralValue(Min->B, FormatDreamShaderFloat(Min->ConstB), TEXT("float"), 1);
					return MakeFunctionValue(TEXT("min"), { A, B }, FMath::Max(A.ComponentCount, B.ComponentCount));
				}

				if (UMaterialExpressionMax* Max = Cast<UMaterialExpressionMax>(Expression))
				{
					FDecompiledValue A = CompileConnectedOrLiteralValue(Max->A, FormatDreamShaderFloat(Max->ConstA), TEXT("float"), 1);
					FDecompiledValue B = CompileConnectedOrLiteralValue(Max->B, FormatDreamShaderFloat(Max->ConstB), TEXT("float"), 1);
					return MakeFunctionValue(TEXT("max"), { A, B }, FMath::Max(A.ComponentCount, B.ComponentCount));
				}

				if (UMaterialExpressionAbs* Abs = Cast<UMaterialExpressionAbs>(Expression))
				{
					FDecompiledValue Input = CompileInputValue(Abs->Input, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					return MakeFunctionValue(TEXT("abs"), { Input }, Input.ComponentCount);
				}

				if (UMaterialExpressionSaturate* Saturate = Cast<UMaterialExpressionSaturate>(Expression))
				{
					FDecompiledValue Input = CompileInputValue(Saturate->Input, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					return MakeFunctionValue(TEXT("saturate"), { Input }, Input.ComponentCount);
				}

				if (UMaterialExpressionFloor* Floor = Cast<UMaterialExpressionFloor>(Expression))
				{
					FDecompiledValue Input = CompileInputValue(Floor->Input, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					return MakeFunctionValue(TEXT("floor"), { Input }, Input.ComponentCount);
				}

				if (UMaterialExpressionCeil* Ceil = Cast<UMaterialExpressionCeil>(Expression))
				{
					FDecompiledValue Input = CompileInputValue(Ceil->Input, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					return MakeFunctionValue(TEXT("ceil"), { Input }, Input.ComponentCount);
				}

				if (UMaterialExpressionFrac* Frac = Cast<UMaterialExpressionFrac>(Expression))
				{
					FDecompiledValue Input = CompileInputValue(Frac->Input, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					return MakeFunctionValue(TEXT("frac"), { Input }, Input.ComponentCount);
				}

				if (UMaterialExpressionSquareRoot* SquareRoot = Cast<UMaterialExpressionSquareRoot>(Expression))
				{
					FDecompiledValue Input = CompileInputValue(SquareRoot->Input, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					return MakeFunctionValue(TEXT("sqrt"), { Input }, Input.ComponentCount);
				}

				if (UMaterialExpressionSine* Sine = Cast<UMaterialExpressionSine>(Expression))
				{
					if (!FMath::IsNearlyEqual(Sine->Period, 1.0f))
					{
						return MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, {
							{ TEXT("Input"), CompileInput(Sine->Input, TEXT("0.0")), true },
							{ TEXT("Period"), FormatDreamShaderFloat(Sine->Period), false },
						}), false);
					}
					FDecompiledValue Input = CompileInputValue(Sine->Input, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					return MakeFunctionValue(TEXT("sin"), { Input }, Input.ComponentCount);
				}

				if (UMaterialExpressionCosine* Cosine = Cast<UMaterialExpressionCosine>(Expression))
				{
					if (!FMath::IsNearlyEqual(Cosine->Period, 1.0f))
					{
						return MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, {
							{ TEXT("Input"), CompileInput(Cosine->Input, TEXT("0.0")), true },
							{ TEXT("Period"), FormatDreamShaderFloat(Cosine->Period), false },
						}), false);
					}
					FDecompiledValue Input = CompileInputValue(Cosine->Input, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					return MakeFunctionValue(TEXT("cos"), { Input }, Input.ComponentCount);
				}

				if (UMaterialExpressionStaticSwitchParameter* StaticSwitchParameter = Cast<UMaterialExpressionStaticSwitchParameter>(Expression))
				{
					FDecompiledValue TrueValue = CompileInputValue(StaticSwitchParameter->A, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					FDecompiledValue FalseValue = CompileInputValue(StaticSwitchParameter->B, MakeValue(TEXT("0.0"), TEXT("float"), 1, true));
					const int32 ComponentCount = GetCommonNumericComponentCount(TrueValue, FalseValue);
					TArray<FExpressionCallArgument> Arguments = {
						{ TEXT("True"), TrueValue.Text, true },
						{ TEXT("False"), FalseValue.Text, true },
					};
					if (!StaticSwitchParameter->ParameterName.IsNone())
					{
						Arguments.Add({
							TEXT("ParameterName"),
							FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(StaticSwitchParameter->ParameterName.ToString())),
							false
						});
					}
					if (StaticSwitchParameter->DefaultValue)
					{
						Arguments.Add({ TEXT("DefaultValue"), TEXT("true"), false });
					}
					if (StaticSwitchParameter->DynamicBranch)
					{
						Arguments.Add({ TEXT("DynamicBranch"), TEXT("true"), false });
					}

					const FString OutputType = GetDreamShaderTypeForComponentCount(ComponentCount);
					return MakeExpressionValueWithComponentCount(
						Expression,
						OutputIndex,
						BuildUEExpressionCallWithOutputType(Expression, OutputIndex, OutputType, Arguments),
						false,
						ComponentCount);
				}

				if (UMaterialExpressionTextureCoordinate* TextureCoordinate = Cast<UMaterialExpressionTextureCoordinate>(Expression))
				{
					TArray<FExpressionCallArgument> Arguments;
					if (TextureCoordinate->CoordinateIndex != 0)
					{
						Arguments.Add({ TEXT("CoordinateIndex"), FString::Printf(TEXT("%d"), TextureCoordinate->CoordinateIndex), false });
					}
					if (!FMath::IsNearlyEqual(TextureCoordinate->UTiling, 1.0f))
					{
						Arguments.Add({ TEXT("UTiling"), FormatDreamShaderFloat(TextureCoordinate->UTiling), false });
					}
					if (!FMath::IsNearlyEqual(TextureCoordinate->VTiling, 1.0f))
					{
						Arguments.Add({ TEXT("VTiling"), FormatDreamShaderFloat(TextureCoordinate->VTiling), false });
					}
					return MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, Arguments), false);
				}

				if (UMaterialExpressionTime* Time = Cast<UMaterialExpressionTime>(Expression))
				{
					if (!Time->bIgnorePause && !Time->bOverride_Period)
					{
						return MakeValue(TEXT("UE.Time()"), TEXT("float"), 1, true);
					}

					return MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, {
						{ TEXT("bIgnorePause"), Time->bIgnorePause ? TEXT("true") : TEXT("false"), false },
						{ TEXT("bOverride_Period"), Time->bOverride_Period ? TEXT("true") : TEXT("false"), false },
						{ TEXT("Period"), FormatDreamShaderFloat(Time->Period), false },
					}), false);
				}

				if (UMaterialExpressionPanner* Panner = Cast<UMaterialExpressionPanner>(Expression))
				{
					TArray<FExpressionCallArgument> Arguments;
					if (Panner->Coordinate.IsConnected())
					{
						Arguments.Add({ TEXT("Coordinate"), CompileInput(Panner->Coordinate, TEXT("0.0")), true });
					}
					else if (Panner->ConstCoordinate != 0)
					{
						Arguments.Add({ TEXT("ConstCoordinate"), FString::Printf(TEXT("%d"), Panner->ConstCoordinate), false });
					}
					if (Panner->Time.IsConnected())
					{
						Arguments.Add({ TEXT("Time"), CompileInput(Panner->Time, TEXT("UE.Time()")), true });
					}
					if (Panner->Speed.IsConnected())
					{
						Arguments.Add({ TEXT("Speed"), CompileInput(Panner->Speed, FormatDreamShaderVector2(Panner->SpeedX, Panner->SpeedY)), true });
					}
					else
					{
						if (!FMath::IsNearlyZero(Panner->SpeedX))
						{
							Arguments.Add({ TEXT("SpeedX"), FormatDreamShaderFloat(Panner->SpeedX), false });
						}
						if (!FMath::IsNearlyZero(Panner->SpeedY))
						{
							Arguments.Add({ TEXT("SpeedY"), FormatDreamShaderFloat(Panner->SpeedY), false });
						}
					}
					if (Panner->bFractionalPart)
					{
						Arguments.Add({ TEXT("bFractionalPart"), TEXT("true"), false });
					}
					return MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, Arguments), false);
				}

				if (UMaterialExpressionRotator* Rotator = Cast<UMaterialExpressionRotator>(Expression))
				{
					TArray<FExpressionCallArgument> Arguments;
					if (Rotator->Coordinate.IsConnected())
					{
						Arguments.Add({ TEXT("Coordinate"), CompileInput(Rotator->Coordinate, TEXT("UE.TexCoord()")), true });
					}
					else if (Rotator->ConstCoordinate != 0)
					{
						Arguments.Add({ TEXT("ConstCoordinate"), FString::Printf(TEXT("%d"), Rotator->ConstCoordinate), false });
					}
					if (Rotator->Time.IsConnected())
					{
						Arguments.Add({ TEXT("Time"), CompileInput(Rotator->Time, TEXT("UE.Time()")), true });
					}
					if (!FMath::IsNearlyEqual(Rotator->CenterX, 0.5f))
					{
						Arguments.Add({ TEXT("CenterX"), FormatDreamShaderFloat(Rotator->CenterX), false });
					}
					if (!FMath::IsNearlyEqual(Rotator->CenterY, 0.5f))
					{
						Arguments.Add({ TEXT("CenterY"), FormatDreamShaderFloat(Rotator->CenterY), false });
					}
					if (!FMath::IsNearlyEqual(Rotator->Speed, 0.25f))
					{
						Arguments.Add({ TEXT("Speed"), FormatDreamShaderFloat(Rotator->Speed), false });
					}
					return MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, Arguments), false);
				}

				if (UMaterialExpressionWorldPosition* WorldPosition = Cast<UMaterialExpressionWorldPosition>(Expression))
				{
					return MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, {}), false);
				}

				if (UMaterialExpressionCameraVectorWS* CameraVector = Cast<UMaterialExpressionCameraVectorWS>(Expression))
				{
					return MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, {}), false);
				}

				if (UMaterialExpressionObjectPositionWS* ObjectPosition = Cast<UMaterialExpressionObjectPositionWS>(Expression))
				{
					return MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, {}), false);
				}

				if (UMaterialExpressionScreenPosition* ScreenPosition = Cast<UMaterialExpressionScreenPosition>(Expression))
				{
					return MakeExpressionValue(Expression, OutputIndex, BuildUEExpressionCall(Expression, OutputIndex, {}), false);
				}

				if (UMaterialExpressionVertexColor* VertexColor = Cast<UMaterialExpressionVertexColor>(Expression))
				{
					return MakeExpressionOutputValue(
						MakeExpressionValue(Expression, 0, BuildUEExpressionCall(Expression, 0, {}), false),
						Expression,
						OutputIndex);
				}

				if (UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression))
				{
					TArray<FExpressionCallArgument> Arguments;
					AddTextureSampleExpressionArguments(TextureSample, Arguments);

					const int32 RgbaOutputIndex = FindExpressionOutputIndexByName(Expression, TEXT("RGBA"), OutputIndex);
					const FDecompiledExpressionKey RgbaKey{ Expression, RgbaOutputIndex };
					FDecompiledValue RgbaValue;
					if (const FDecompiledValue* ExistingValue = ExpressionValues.Find(RgbaKey))
					{
						RgbaValue = *ExistingValue;
					}
					else
					{
						RgbaValue = CacheTempExpressionValue(
							RgbaKey,
							MakeValue(BuildUEExpressionCall(Expression, RgbaOutputIndex, Arguments), TEXT("float4"), 4, false),
							TextureSample->Desc.IsEmpty() ? TEXT("TextureSample") : TextureSample->Desc);
					}

					if (OutputIndex == RgbaOutputIndex)
					{
						return RgbaValue;
					}
					return MakeExpressionOutputValue(RgbaValue, Expression, OutputIndex);
				}

				if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
				{
					const FString FunctionCallText = BuildMaterialFunctionCall(FunctionCall, OutputIndex);
					return CacheTempExpressionValue(
						Key,
						MakeExpressionValue(Expression, OutputIndex, FunctionCallText, false),
						FunctionCall->MaterialFunction ? FunctionCall->MaterialFunction->GetName() : TEXT("Function"));
				}

				if (UMaterialExpressionCustom* CustomExpression = Cast<UMaterialExpressionCustom>(Expression))
				{
					return CacheTempExpressionValue(
						Key,
						MakeExpressionValue(Expression, OutputIndex, BuildCustomExpressionCall(CustomExpression, OutputIndex), false),
						CustomExpression->Description.IsEmpty() ? TEXT("Custom") : CustomExpression->Description);
				}

				Warnings.AddUnique(FString::Printf(
					TEXT("Exported '%s' as UE.Expression; review reflected literal properties if the node has editor-only state."),
					*Expression->GetClass()->GetName()));
				return CacheTempExpressionValue(
					Key,
					MakeExpressionValue(Expression, OutputIndex, BuildGenericExpressionCall(Expression, OutputIndex), false),
					GetMaterialExpressionShortName(Expression->GetClass()));
			}

			FString MakeExpressionOutputSelection(const FString& ExpressionText, UMaterialExpression* Expression, const int32 OutputIndex) const
			{
				if (!Expression || !Expression->Outputs.IsValidIndex(OutputIndex))
				{
					return ExpressionText;
				}

				const FExpressionOutput& Output = Expression->Outputs[OutputIndex];
				FString OutputName = Output.OutputName.ToString();
				if (OutputName.IsEmpty())
				{
					if (Output.MaskR && Output.MaskG && !Output.MaskB && !Output.MaskA)
					{
						OutputName = TEXT("rg");
					}
					else if (Output.MaskR && Output.MaskG && Output.MaskB && !Output.MaskA)
					{
						OutputName = TEXT("rgb");
					}
					else if (Output.MaskR && Output.MaskG && Output.MaskB && Output.MaskA)
					{
						OutputName = TEXT("rgba");
					}
					else if (Output.MaskR && !Output.MaskG && !Output.MaskB && !Output.MaskA)
					{
						OutputName = TEXT("r");
					}
					else if (!Output.MaskR && Output.MaskG && !Output.MaskB && !Output.MaskA)
					{
						OutputName = TEXT("g");
					}
					else if (!Output.MaskR && !Output.MaskG && Output.MaskB && !Output.MaskA)
					{
						OutputName = TEXT("b");
					}
					else if (!Output.MaskR && !Output.MaskG && !Output.MaskB && Output.MaskA)
					{
						OutputName = TEXT("a");
					}
				}

				if (OutputName.IsEmpty())
				{
					return ExpressionText;
				}

				const FString NormalizedOutputName = OutputName.ToLower();
				if (NormalizedOutputName == TEXT("rgba") || NormalizedOutputName == TEXT("xyzw"))
				{
					return ExpressionText;
				}

				return MakeSwizzleExpression(ExpressionText, NormalizedOutputName);
			}

			FDecompiledValue MakeExpressionOutputValue(FDecompiledValue Source, UMaterialExpression* Expression, const int32 OutputIndex) const
			{
				if (!Expression || !Expression->Outputs.IsValidIndex(OutputIndex))
				{
					return Source;
				}

				const FString Text = MakeExpressionOutputSelection(Source.Text, Expression, OutputIndex);
				const int32 ComponentCount = GetComponentCountForExpressionOutput(Expression, OutputIndex);
				return MakeValue(
					Text,
					GetDreamShaderTypeForComponentCount(ComponentCount),
					ComponentCount,
					!Text.Contains(TEXT("\n")));
			}

			FString BuildUEExpressionCall(UMaterialExpression* Expression, const int32 OutputIndex, const TArray<FExpressionCallArgument>& Arguments) const
			{
				return BuildUEExpressionCallWithOutputType(
					Expression,
					OutputIndex,
					GetDreamShaderTypeForExpressionOutput(Expression, OutputIndex),
					Arguments);
			}

			static bool IsControlExpressionArgumentName(const FString& Name)
			{
				const FString NormalizedName = UE::DreamShader::NormalizeSettingKey(Name);
				return NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("Class"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("OutputType"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("ResultType"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("Output"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("OutputName"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("OutputIndex"));
			}

			static bool IsEditorOnlyExpressionPropertyName(const FString& Name)
			{
				const FString NormalizedName = UE::DreamShader::NormalizeSettingKey(Name);
				return NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("MaterialExpressionEditorX"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("MaterialExpressionEditorY"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("Desc"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("bCommentBubbleVisible"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("bShowOutputNameOnPin"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("bHidePreviewWindow"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("bCollapsed"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("bShaderInputData"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("SortPriority"));
			}

			static bool IsReflectedExpressionLiteralProperty(const FProperty* Property)
			{
				if (!Property
					|| Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient | CPF_DuplicateTransient)
					|| IsMaterialExpressionInputProperty(Property)
					|| !Property->HasAnyPropertyFlags(CPF_Edit)
					|| IsControlExpressionArgumentName(Property->GetName())
					|| IsEditorOnlyExpressionPropertyName(Property->GetName()))
				{
					return false;
				}

				return CastField<FBoolProperty>(Property) != nullptr
					|| CastField<FNumericProperty>(Property) != nullptr
					|| CastField<FEnumProperty>(Property) != nullptr
					|| CastField<FByteProperty>(Property) != nullptr
					|| CastField<FNameProperty>(Property) != nullptr
					|| CastField<FStrProperty>(Property) != nullptr
					|| CastField<FTextProperty>(Property) != nullptr
					|| CastField<FObjectPropertyBase>(Property) != nullptr;
			}

			static bool IsReflectedPropertyDefaultValue(const UObject* Object, const FProperty* Property)
			{
				if (!Object || !Property)
				{
					return true;
				}

				UObject* DefaultObject = Object->GetClass() ? Object->GetClass()->GetDefaultObject(false) : nullptr;
				if (!DefaultObject)
				{
					return false;
				}

				return Property->Identical_InContainer(Object, DefaultObject);
			}

			static bool TryBuildReflectedExpressionLiteralArgument(
				const UMaterialExpression* Expression,
				const FProperty* Property,
				FExpressionCallArgument& OutArgument)
			{
				if (!Expression || !Property || !IsReflectedExpressionLiteralProperty(Property) || IsReflectedPropertyDefaultValue(Expression, Property))
				{
					return false;
				}

				const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Expression);
				if (!ValuePtr)
				{
					return false;
				}

				FString ValueText;
				if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
				{
					ValueText = BoolProperty->GetPropertyValue(ValuePtr) ? TEXT("true") : TEXT("false");
				}
				else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
				{
					if (UEnum* Enum = EnumProperty->GetEnum())
					{
						ValueText = BuildLiteralEnumArgument(Enum, EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr));
					}
				}
				else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
				{
					if (UEnum* Enum = ByteProperty->Enum)
					{
						ValueText = BuildLiteralEnumArgument(Enum, ByteProperty->GetPropertyValue(ValuePtr));
					}
					else
					{
						ValueText = FString::FromInt(ByteProperty->GetPropertyValue(ValuePtr));
					}
				}
				else if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
				{
					if (NumericProperty->IsFloatingPoint())
					{
						ValueText = FormatDreamShaderFloat(NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
					}
					else if (NumericProperty->IsInteger())
					{
						if (CastField<FUInt16Property>(Property)
							|| CastField<FUInt32Property>(Property)
							|| CastField<FUInt64Property>(Property))
						{
							ValueText = FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(NumericProperty->GetUnsignedIntPropertyValue(ValuePtr)));
						}
						else
						{
							ValueText = FString::Printf(TEXT("%lld"), static_cast<long long>(NumericProperty->GetSignedIntPropertyValue(ValuePtr)));
						}
					}
				}
				else if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
				{
					const FName NameValue = NameProperty->GetPropertyValue(ValuePtr);
					if (!NameValue.IsNone())
					{
						ValueText = FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(NameValue.ToString()));
					}
				}
				else if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
				{
					ValueText = FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(StringProperty->GetPropertyValue(ValuePtr)));
				}
				else if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
				{
					ValueText = FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(TextProperty->GetPropertyValue(ValuePtr).ToString()));
				}
				else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
				{
					if (const UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr))
					{
						ValueText = MakeDreamShaderObjectPathLiteral(ObjectValue);
					}
				}

				if (ValueText.TrimStartAndEnd().IsEmpty())
				{
					return false;
				}

				OutArgument = { Property->GetName(), ValueText, false };
				return true;
			}

			void AddReflectedExpressionLiteralArguments(
				const UMaterialExpression* Expression,
				TArray<FExpressionCallArgument>& Arguments) const
			{
				if (!Expression)
				{
					return;
				}

				TSet<FString> ExistingArgumentNames;
				for (const FExpressionCallArgument& Argument : Arguments)
				{
					ExistingArgumentNames.Add(UE::DreamShader::NormalizeSettingKey(Argument.Name));
				}

				for (TFieldIterator<FProperty> It(Expression->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
				{
					FProperty* Property = *It;
					if (!Property || ExistingArgumentNames.Contains(UE::DreamShader::NormalizeSettingKey(Property->GetName())))
					{
						continue;
					}

					FExpressionCallArgument ReflectedArgument;
					if (TryBuildReflectedExpressionLiteralArgument(Expression, Property, ReflectedArgument))
					{
						ExistingArgumentNames.Add(UE::DreamShader::NormalizeSettingKey(ReflectedArgument.Name));
						Arguments.Add(ReflectedArgument);
					}
				}
			}

			FString BuildGenericExpressionCall(UMaterialExpression* Expression, const int32 OutputIndex)
			{
				TArray<FExpressionCallArgument> Arguments;
				if (Expression)
				{
					for (int32 InputIndex = 0; InputIndex < Expression->CountInputs(); ++InputIndex)
					{
						FExpressionInput* Input = Expression->GetInput(InputIndex);
						if (!Input || !Input->IsConnected())
						{
							continue;
						}

						const FName InputName = Expression->GetInputName(InputIndex);
						const FString ArgumentName = MakeDreamShaderDeclarationName(
							InputName.IsNone() ? FString::Printf(TEXT("Input%d"), InputIndex) : InputName.ToString(),
							TEXT("Input"),
							InputIndex);
						Arguments.Add({ ArgumentName, CompileInput(*Input, TEXT("0.0")), true });
					}

					AddReflectedExpressionLiteralArguments(Expression, Arguments);
				}

				return BuildUEExpressionCall(Expression, OutputIndex, Arguments);
			}

			FString BuildCustomExpressionCall(UMaterialExpressionCustom* CustomExpression, const int32 OutputIndex)
			{
				TArray<FExpressionCallArgument> Arguments;
				Arguments.Add({ TEXT("Code"), FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderCodeString(CustomExpression ? CustomExpression->Code : FString())), false });
				if (CustomExpression && !CustomExpression->Description.IsEmpty())
				{
					Arguments.Add({ TEXT("Description"), FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(CustomExpression->Description)), false });
				}
				if (CustomExpression && OutputIndex > 0)
				{
					const FString OutputName = GetCustomOutputName(CustomExpression, OutputIndex);
					if (!OutputName.IsEmpty())
					{
						Arguments.Add({ TEXT("Output"), FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(OutputName)), false });
					}
				}
				if (CustomExpression)
				{
					for (const FCustomInput& CustomInput : CustomExpression->Inputs)
					{
						if (!CustomInput.Input.IsConnected())
						{
							continue;
						}
						const FString ArgumentName = MakeDreamShaderDeclarationName(CustomInput.InputName.ToString(), TEXT("Input"), Arguments.Num());
						Arguments.Add({ ArgumentName, CompileInput(CustomInput.Input, TEXT("0.0")), true });
					}
				}

				return BuildUEExpressionCall(CustomExpression, OutputIndex, Arguments);
			}

			FString BuildMaterialFunctionCall(UMaterialExpressionMaterialFunctionCall* FunctionCall, const int32 OutputIndex)
			{
				if (!FunctionCall || !FunctionCall->MaterialFunction)
				{
					Warnings.AddUnique(TEXT("A MaterialFunctionCall had no function asset and was exported as a zero literal."));
					return TEXT("0.0");
				}

				UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(FunctionCall->MaterialFunction);
				if (!MaterialFunction)
				{
					Warnings.AddUnique(FString::Printf(
						TEXT("MaterialFunctionCall '%s' is not a plain MaterialFunction; it was exported through UE.Expression."),
						*FunctionCall->MaterialFunction->GetPathName()));
					return BuildGenericExpressionCall(FunctionCall, OutputIndex);
				}

				const FString FunctionName = EnsureVirtualFunctionDefinition(MaterialFunction);
				TArray<FString> Arguments;
				for (int32 InputIndex = 0; InputIndex < FunctionCall->FunctionInputs.Num(); ++InputIndex)
				{
					const FFunctionExpressionInput& FunctionInput = FunctionCall->FunctionInputs[InputIndex];
					if (FunctionInput.Input.IsConnected())
					{
						Arguments.Add(CompileInput(FunctionInput.Input, TEXT("default")));
					}
					else
					{
						Arguments.Add(TEXT("default"));
					}
				}

				const FString OutputName = GetFunctionCallOutputName(FunctionCall, OutputIndex);
				if (!OutputName.IsEmpty())
				{
					Arguments.Add(FString::Printf(
						TEXT("Output=\"%s\""),
						*EscapeDreamShaderString(MakeDreamShaderDeclarationName(OutputName, TEXT("Output"), OutputIndex))));
				}
				else if (OutputIndex > 0)
				{
					Arguments.Add(FString::Printf(TEXT("OutputIndex=%d"), OutputIndex));
				}

				return FString::Printf(TEXT("%s(%s)"), *FunctionName, *FString::Join(Arguments, TEXT(", ")));
			}

			FString EnsureVirtualFunctionDefinition(UMaterialFunction* MaterialFunction)
			{
				if (!MaterialFunction)
				{
					return TEXT("MissingFunction");
				}

				if (const FString* ExistingName = VirtualFunctionNames.Find(MaterialFunction))
				{
					return *ExistingName;
				}

				FString DefinitionText;
				FString Error;
				const FString FunctionName = MakeDreamShaderDeclarationName(MaterialFunction->GetName(), TEXT("VirtualFunction"), VirtualFunctionNames.Num());
				if (BuildVirtualFunctionDefinition(MaterialFunction, DefinitionText, Error))
				{
					VirtualFunctionDefinitions.Add(DefinitionText);
				}
				else
				{
					Warnings.AddUnique(FString::Printf(
						TEXT("Failed to emit VirtualFunction for '%s': %s"),
						*MaterialFunction->GetPathName(),
						*Error));
				}

				VirtualFunctionNames.Add(MaterialFunction, FunctionName);
				return FunctionName;
			}

			TArray<FString> PropertyDeclarations;
			TSet<FString> PropertyNames;
			TSet<FString> ReservedNames;
			TMap<const UMaterialExpressionFunctionInput*, FString> FunctionInputNames;
			TArray<FString> GraphLines;
			TMap<FDecompiledExpressionKey, FString> ExpressionTemps;
			TMap<FDecompiledExpressionKey, FDecompiledValue> ExpressionValues;
			TSet<FString> TempNames;
			TArray<FString> VirtualFunctionDefinitions;
			TMap<const UMaterialFunction*, FString> VirtualFunctionNames;
			TArray<FString> Warnings;
			int32 NextTempIndex = 0;
			FScopedSlowTask* ActiveDecompileSlowTask = nullptr;
			TSet<const UMaterialExpression*> ProgressVisitedExpressions;

			void EnterExpressionProgressFrame(const UMaterialExpression* Expression)
			{
				if (!ActiveDecompileSlowTask || !Expression || ProgressVisitedExpressions.Contains(Expression))
				{
					return;
				}

				ProgressVisitedExpressions.Add(Expression);
				ActiveDecompileSlowTask->EnterProgressFrame(1.0f, FText::FromString(FString::Printf(
					TEXT("Decompiling node %d: %s"),
					ProgressVisitedExpressions.Num(),
					*GetMaterialExpressionShortName(Expression->GetClass()))));
			}
		};

		class FBridgeGraphDecompiler final : public UE::DreamShader::Editor::IDreamShaderDecompiler
		{
		public:
			virtual bool DecompileMaterial(UMaterial* Material, const FString& DecompiledName, FString& OutSourceText, FString& OutError) override
			{
				FDreamShaderGraphDecompiler Decompiler;
				return Decompiler.DecompileMaterial(Material, DecompiledName, OutSourceText, OutError);
			}

			virtual bool DecompileFunction(UMaterialFunction* MaterialFunction, const FString& DecompiledName, FString& OutSourceText, FString& OutError) override
			{
				FDreamShaderGraphDecompiler Decompiler;
				return Decompiler.DecompileFunction(MaterialFunction, DecompiledName, OutSourceText, OutError);
			}
		};

		UE::DreamShader::Editor::IDreamShaderDecompiler& GetGraphDecompiler()
		{
			static FBridgeGraphDecompiler Decompiler;
			return Decompiler;
		}

		bool IsIdentifierCharacter(TCHAR Character)
		{
			return FChar::IsAlnum(Character) || Character == TCHAR('_');
		}

		bool IsValidStringIndex(const FString& Text, int32 Index)
		{
			return Index >= 0 && Index < Text.Len();
		}

		bool IsKeywordAt(const FString& Text, int32 Index, const TCHAR* Keyword)
		{
			const int32 KeywordLength = FCString::Strlen(Keyword);
			if (Index < 0 || Index + KeywordLength > Text.Len())
			{
				return false;
			}

			if (Index > 0 && IsIdentifierCharacter(Text[Index - 1]))
			{
				return false;
			}

			if (Index + KeywordLength < Text.Len() && IsIdentifierCharacter(Text[Index + KeywordLength]))
			{
				return false;
			}

			return Text.Mid(Index, KeywordLength).Equals(Keyword, ESearchCase::CaseSensitive);
		}

		void SkipQuotedString(const FString& Text, int32& Index)
		{
			if (!IsValidStringIndex(Text, Index) || (Text[Index] != TCHAR('"') && Text[Index] != TCHAR('\'')))
			{
				return;
			}

			const TCHAR Quote = Text[Index];
			++Index;
			bool bEscaped = false;
			while (Index < Text.Len())
			{
				const TCHAR Character = Text[Index++];
				if (bEscaped)
				{
					bEscaped = false;
					continue;
				}
				if (Character == TCHAR('\\'))
				{
					bEscaped = true;
					continue;
				}
				if (Character == Quote)
				{
					return;
				}
			}
		}

		bool TrySkipComment(const FString& Text, int32& Index)
		{
			if (Index + 1 >= Text.Len() || Text[Index] != TCHAR('/'))
			{
				return false;
			}

			if (Text[Index + 1] == TCHAR('/'))
			{
				Index += 2;
				while (Index < Text.Len() && Text[Index] != TCHAR('\n'))
				{
					++Index;
				}
				return true;
			}

			if (Text[Index + 1] == TCHAR('*'))
			{
				Index += 2;
				while (Index + 1 < Text.Len())
				{
					if (Text[Index] == TCHAR('*') && Text[Index + 1] == TCHAR('/'))
					{
						Index += 2;
						return true;
					}
					++Index;
				}
				Index = Text.Len();
				return true;
			}

			return false;
		}

		void SkipIgnoredText(const FString& Text, int32& Index)
		{
			while (Index < Text.Len())
			{
				if (FChar::IsWhitespace(Text[Index]))
				{
					++Index;
					continue;
				}
				if (TrySkipComment(Text, Index))
				{
					continue;
				}
				return;
			}
		}

		bool TryExtractBalancedRange(const FString& Text, int32 OpenIndex, TCHAR OpenCharacter, TCHAR CloseCharacter, int32& OutEndIndex)
		{
			OutEndIndex = INDEX_NONE;
			if (!IsValidStringIndex(Text, OpenIndex) || Text[OpenIndex] != OpenCharacter)
			{
				return false;
			}

			int32 Depth = 0;
			int32 Index = OpenIndex;
			while (Index < Text.Len())
			{
				if (Text[Index] == TCHAR('"') || Text[Index] == TCHAR('\''))
				{
					SkipQuotedString(Text, Index);
					continue;
				}
				if (TrySkipComment(Text, Index))
				{
					continue;
				}

				if (Text[Index] == OpenCharacter)
				{
					++Depth;
				}
				else if (Text[Index] == CloseCharacter)
				{
					--Depth;
					if (Depth == 0)
					{
						OutEndIndex = Index + 1;
						return true;
					}
				}
				++Index;
			}

			return false;
		}

		void CalculateLineColumnForIndex(const FString& Text, int32 Position, int32& OutLine, int32& OutColumn)
		{
			OutLine = 1;
			OutColumn = 1;
			const int32 ClampedPosition = FMath::Clamp(Position, 0, Text.Len());
			for (int32 Index = 0; Index < ClampedPosition; ++Index)
			{
				if (Text[Index] == TCHAR('\n'))
				{
					++OutLine;
					OutColumn = 1;
				}
				else if (Text[Index] != TCHAR('\r'))
				{
					++OutColumn;
				}
			}
		}

		FString NormalizeVirtualFunctionDefinitionText(FString Text)
		{
			Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
			Text.ReplaceInline(TEXT("\r"), TEXT("\n"));
			Text.TrimStartAndEndInline();
			return Text;
		}

		FDreamShaderDiagnosticRecord MakeVirtualFunctionDiagnostic(
			const FString& SourceFilePath,
			const FString& Message,
			const FString& Detail,
			const FString& AssetPath,
			int32 Line,
			int32 Column)
		{
			FDreamShaderDiagnosticRecord Diagnostic;
			Diagnostic.FilePath = SourceFilePath;
			Diagnostic.Message = Message;
			Diagnostic.Detail = Detail;
			Diagnostic.Stage = TEXT("virtualFunctionSync");
			Diagnostic.AssetPath = AssetPath;
			Diagnostic.Code = TEXT("virtual-function-sync");
			Diagnostic.Line = FMath::Max(1, Line);
			Diagnostic.Column = FMath::Max(1, Column);
			Diagnostic.Source = TEXT("DreamShader VirtualFunction");
			return Diagnostic;
		}

		bool TryParseVirtualFunctionBlock(
			const FString& BlockText,
			FTextShaderVirtualFunctionDefinition& OutFunction,
			FString& OutError)
		{
			FTextShaderDefinition ParsedDefinition;
			if (!FTextShaderParser::Parse(BlockText, ParsedDefinition, OutError))
			{
				return false;
			}

			if (ParsedDefinition.VirtualFunctions.Num() != 1)
			{
				OutError = TEXT("Expected exactly one VirtualFunction block.");
				return false;
			}

			OutFunction = ParsedDefinition.VirtualFunctions[0];
			return true;
		}

		void CollectVirtualFunctionDefinitionLocationsFromFile(
			const FString& SourceFilePath,
			TArray<FVirtualFunctionDefinitionLocation>& OutLocations,
			FString* OutSourceText = nullptr,
			TArray<FDreamShaderDiagnosticRecord>* OutDiagnostics = nullptr)
		{
			OutLocations.Reset();

			FString SourceText;
			if (!FFileHelper::LoadFileToString(SourceText, *SourceFilePath))
			{
				if (OutDiagnostics)
				{
					OutDiagnostics->Add(MakeVirtualFunctionDiagnostic(
						SourceFilePath,
						FString::Printf(TEXT("DreamShader could not read VirtualFunction source file '%s'."), *SourceFilePath),
						FString(),
						FString(),
						1,
						1));
				}
				return;
			}

			if (OutSourceText)
			{
				*OutSourceText = SourceText;
			}

			int32 Index = 0;
			while (Index < SourceText.Len())
			{
				if (SourceText[Index] == TCHAR('"') || SourceText[Index] == TCHAR('\''))
				{
					SkipQuotedString(SourceText, Index);
					continue;
				}
				if (TrySkipComment(SourceText, Index))
				{
					continue;
				}
				if (!IsKeywordAt(SourceText, Index, TEXT("VirtualFunction")))
				{
					++Index;
					continue;
				}

				const int32 StartIndex = Index;
				Index += FCString::Strlen(TEXT("VirtualFunction"));
				SkipIgnoredText(SourceText, Index);
				if (!IsValidStringIndex(SourceText, Index) || SourceText[Index] != TCHAR('('))
				{
					Index = StartIndex + 1;
					continue;
				}

				int32 AttributesEndIndex = INDEX_NONE;
				if (!TryExtractBalancedRange(SourceText, Index, TCHAR('('), TCHAR(')'), AttributesEndIndex))
				{
					int32 Line = 1;
					int32 Column = 1;
					CalculateLineColumnForIndex(SourceText, StartIndex, Line, Column);
					if (OutDiagnostics)
					{
						OutDiagnostics->Add(MakeVirtualFunctionDiagnostic(
							SourceFilePath,
							TEXT("VirtualFunction attributes are missing a closing ')'."),
							FString(),
							FString(),
							Line,
							Column));
					}
					Index = StartIndex + 1;
					continue;
				}

				Index = AttributesEndIndex;
				SkipIgnoredText(SourceText, Index);
				if (!IsValidStringIndex(SourceText, Index) || SourceText[Index] != TCHAR('{'))
				{
					Index = StartIndex + 1;
					continue;
				}

				int32 BodyEndIndex = INDEX_NONE;
				if (!TryExtractBalancedRange(SourceText, Index, TCHAR('{'), TCHAR('}'), BodyEndIndex))
				{
					int32 Line = 1;
					int32 Column = 1;
					CalculateLineColumnForIndex(SourceText, StartIndex, Line, Column);
					if (OutDiagnostics)
					{
						OutDiagnostics->Add(MakeVirtualFunctionDiagnostic(
							SourceFilePath,
							TEXT("VirtualFunction body is missing a closing '}'."),
							FString(),
							FString(),
							Line,
							Column));
					}
					Index = StartIndex + 1;
					continue;
				}

				int32 EndIndex = BodyEndIndex;
				int32 AfterBodyIndex = EndIndex;
				SkipIgnoredText(SourceText, AfterBodyIndex);
				if (IsValidStringIndex(SourceText, AfterBodyIndex) && SourceText[AfterBodyIndex] == TCHAR(';'))
				{
					EndIndex = AfterBodyIndex + 1;
				}

				const FString BlockText = SourceText.Mid(StartIndex, EndIndex - StartIndex);
				int32 Line = 1;
				int32 Column = 1;
				CalculateLineColumnForIndex(SourceText, StartIndex, Line, Column);

				FTextShaderVirtualFunctionDefinition ParsedFunction;
				FString ParseError;
				if (!TryParseVirtualFunctionBlock(BlockText, ParsedFunction, ParseError))
				{
					if (OutDiagnostics)
					{
						OutDiagnostics->Add(MakeVirtualFunctionDiagnostic(
							SourceFilePath,
							FString::Printf(TEXT("VirtualFunction declaration is invalid: %s"), *ParseError),
							ParseError,
							FString(),
							Line,
							Column));
					}
					Index = EndIndex;
					continue;
				}

				FString ObjectPath;
				FString ResolveError;
				if (!TryResolveDreamShaderAssetReference(ParsedFunction.Asset, ObjectPath, ResolveError))
				{
					if (OutDiagnostics)
					{
						OutDiagnostics->Add(MakeVirtualFunctionDiagnostic(
							SourceFilePath,
							FString::Printf(TEXT("VirtualFunction '%s' asset reference is invalid: %s"), *ParsedFunction.Name, *ResolveError),
							ResolveError,
							ParsedFunction.Asset,
							Line,
							Column));
					}
					Index = EndIndex;
					continue;
				}

				FVirtualFunctionDefinitionLocation& Location = OutLocations.AddDefaulted_GetRef();
				Location.SourceFilePath = UE::DreamShader::NormalizeSourceFilePath(SourceFilePath);
				Location.FunctionName = ParsedFunction.Name;
				Location.AssetObjectPath = ObjectPath;
				Location.CurrentText = BlockText;
				Location.Inputs = ParsedFunction.Inputs;
				Location.Outputs = ParsedFunction.Outputs;
				Location.StartIndex = StartIndex;
				Location.EndIndex = EndIndex;
				Location.Line = Line;
				Location.Column = Column;

				Index = EndIndex;
			}
		}

		bool FindVirtualFunctionDefinitionForMaterialFunction(
			const UMaterialFunction* MaterialFunction,
			FVirtualFunctionDefinitionLocation& OutLocation)
		{
			if (!MaterialFunction)
			{
				return false;
			}

			const FString TargetObjectPath = MaterialFunction->GetPathName();
			TArray<FString> SourceFiles;
			FDreamShaderSourceFileUtils::FindProjectDreamShaderSourceFiles(SourceFiles);
			for (const FString& SourceFile : SourceFiles)
			{
				TArray<FVirtualFunctionDefinitionLocation> Locations;
				CollectVirtualFunctionDefinitionLocationsFromFile(SourceFile, Locations);
				for (const FVirtualFunctionDefinitionLocation& Location : Locations)
				{
					if (Location.AssetObjectPath.Equals(TargetObjectPath, ESearchCase::IgnoreCase))
					{
						OutLocation = Location;
						return true;
					}
				}
			}

			return false;
		}

		FString GetMaterialExpressionShortName(const UClass* Class)
		{
			if (!Class)
			{
				return FString();
			}

			FString Name = Class->GetName();
			Name.RemoveFromStart(TEXT("U"), ESearchCase::CaseSensitive);
			Name.RemoveFromStart(TEXT("MaterialExpression"), ESearchCase::CaseSensitive);
			return Name;
		}

		void ShowDreamShaderNotification(const FText& Message, SNotificationItem::ECompletionState CompletionState)
		{
			FNotificationInfo Info(Message);
			Info.ExpireDuration = 4.0f;
			Info.bUseLargeFont = false;
			if (TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info))
			{
				Notification->SetCompletionState(CompletionState);
			}
		}

		FString GetShaderPlatformLabel(const EShaderPlatform ShaderPlatform)
		{
			const FName ShaderFormat = LegacyShaderPlatformToShaderFormat(ShaderPlatform);
			return ShaderFormat.IsNone()
				? FString::Printf(TEXT("Platform %d"), static_cast<int32>(ShaderPlatform))
				: ShaderFormat.ToString();
		}

		FString GetMaterialQualityLevelLabel(const EMaterialQualityLevel::Type QualityLevel)
		{
			return LexToString(QualityLevel);
		}

		FString GetFirstMeaningfulErrorLine(const FString& InError)
		{
			TArray<FString> Lines;
			InError.ParseIntoArrayLines(Lines, false);
			for (const FString& Line : Lines)
			{
				const FString Trimmed = Line.TrimStartAndEnd();
				if (!Trimmed.IsEmpty())
				{
					return Trimmed;
				}
			}
			return InError.TrimStartAndEnd();
		}

	}

	FString FDreamShaderEditorBridge::GetBridgeDirectory()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamShader"), TEXT("Bridge"));
	}

	FString FDreamShaderEditorBridge::GetRequestDirectory()
	{
		return FPaths::Combine(GetBridgeDirectory(), TEXT("Requests"));
	}

	FString FDreamShaderEditorBridge::GetDiagnosticsFilePath()
	{
		return FPaths::Combine(GetBridgeDirectory(), TEXT("diagnostics.json"));
	}

	FString FDreamShaderEditorBridge::GetSourceFileMetadata(UObject* Asset)
	{
		if (!Asset)
		{
			return FString();
		}

		if (UPackage* Package = Asset->GetOutermost())
		{
			return Package->GetMetaData().GetValue(Asset, TEXT("DreamShader.SourceFile"));
		}

		return FString();
	}

	void FDreamShaderEditorBridge::Startup()
	{
		bIsShuttingDown = false;

		IFileManager::Get().MakeDirectory(*GetBridgeDirectory(), true);
		IFileManager::Get().MakeDirectory(*GetRequestDirectory(), true);

		FDreamShaderWorkspaceService::ExportMaterialExpressionManifest();
		FDreamShaderWorkspaceService::ExportDreamShaderSettingsManifest();
		SyncVirtualFunctionDefinitions();
		QueueFullScan();
		UpdateDiagnosticsFile();

		FDirectoryWatcherModule& DirectoryWatcherModule = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
		if (IDirectoryWatcher* DirectoryWatcher = DirectoryWatcherModule.Get())
		{
			WatchedSourceDirectory = UE::DreamShader::GetSourceShaderDirectory();
			DirectoryWatcher->RegisterDirectoryChangedCallback_Handle(
				WatchedSourceDirectory,
				IDirectoryWatcher::FDirectoryChanged::CreateSP(AsShared(), &FDreamShaderEditorBridge::OnDirectoryChanged),
				DirectoryWatcherHandle,
				IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges);
		}

		MaterialCompilationFinishedHandle = UMaterial::OnMaterialCompilationFinished().AddSP(
			AsShared(),
			&FDreamShaderEditorBridge::OnMaterialCompilationFinished);

		ToolMenusStartupCallbackHandle = UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateSP(AsShared(), &FDreamShaderEditorBridge::RegisterMenus));

		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateSP(AsShared(), &FDreamShaderEditorBridge::Tick),
			0.1f);
	}

	void FDreamShaderEditorBridge::Shutdown()
	{
		bIsShuttingDown = true;

		if (TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
			TickerHandle.Reset();
		}

		if (MaterialCompilationFinishedHandle.IsValid())
		{
			UMaterial::OnMaterialCompilationFinished().Remove(MaterialCompilationFinishedHandle);
			MaterialCompilationFinishedHandle.Reset();
		}

		if (ToolMenusStartupCallbackHandle.IsValid())
		{
			UToolMenus::UnRegisterStartupCallback(ToolMenusStartupCallbackHandle);
			ToolMenusStartupCallbackHandle.Reset();
		}

		if (!IsEngineExitRequested() && !GExitPurge)
		{
			UToolMenus::UnregisterOwner(DreamShaderToolMenuOwnerName);
		}

		if (DirectoryWatcherHandle.IsValid())
		{
			if (FDirectoryWatcherModule* DirectoryWatcherModule = FModuleManager::GetModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")))
			{
				if (IDirectoryWatcher* DirectoryWatcher = DirectoryWatcherModule->Get())
				{
					DirectoryWatcher->UnregisterDirectoryChangedCallback_Handle(WatchedSourceDirectory, DirectoryWatcherHandle);
				}
			}

			DirectoryWatcherHandle.Reset();
			WatchedSourceDirectory.Reset();
		}

		PendingFiles.Reset();
		DiagnosticsStore.Reset();
	}

	void FDreamShaderEditorBridge::QueueFullScan()
	{
		TArray<FString> SourceFiles;
		FDreamShaderSourceFileUtils::FindProjectMaterialSourceFiles(SourceFiles);
		RebuildDependencyGraph();

		const double Now = FPlatformTime::Seconds();
		for (FString& SourceFile : SourceFiles)
		{
			PendingFiles.Add(UE::DreamShader::NormalizeSourceFilePath(SourceFile), Now);
		}
	}

	void FDreamShaderEditorBridge::QueueSourceFile(const FString& SourceFilePath)
	{
		PendingFiles.Add(UE::DreamShader::NormalizeSourceFilePath(SourceFilePath), FPlatformTime::Seconds());
	}

	void FDreamShaderEditorBridge::QueueDependentSourcesForImport(const FString& ImportFilePath)
	{
		const FString NormalizedImportPath = UE::DreamShader::NormalizeSourceFilePath(ImportFilePath);
		const TSet<FString> SourcesToQueue =
			FDreamShaderDependencyGraphService::RebuildAndCollectDependentsForImport(ImportFilePath, HeaderDependentsByFile);

		const double Now = FPlatformTime::Seconds();
		for (const FString& SourceFile : SourcesToQueue)
		{
			PendingFiles.Add(SourceFile, Now);
		}

		const UDreamShaderSettings* Settings = GetDefault<UDreamShaderSettings>();
		if (Settings && Settings->bVerboseLogs)
		{
			UE_LOG(
				LogDreamShader,
				Display,
				TEXT("DreamShader queued %d dependent source file(s) for import '%s'."),
				SourcesToQueue.Num(),
				*NormalizedImportPath);
		}
	}

	void FDreamShaderEditorBridge::OnDirectoryChanged(const TArray<FFileChangeData>& FileChanges)
	{
		TArray<FFileChangeData> ChangesCopy = FileChanges;
		TWeakPtr<FDreamShaderEditorBridge, ESPMode::ThreadSafe> WeakBridge = AsWeak();
		AsyncTask(ENamedThreads::GameThread, [WeakBridge, Changes = MoveTemp(ChangesCopy)]()
		{
			TSharedPtr<FDreamShaderEditorBridge, ESPMode::ThreadSafe> Bridge = WeakBridge.Pin();
			if (!Bridge.IsValid() || Bridge->bIsShuttingDown || IsEngineExitRequested() || GExitPurge)
			{
				return;
			}

			const UDreamShaderSettings* Settings = GetDefault<UDreamShaderSettings>();
			if (Settings && !Settings->bAutoCompileOnSave)
			{
				return;
			}

			for (const FFileChangeData& FileChange : Changes)
			{
				if (FileChange.Action == FFileChangeData::FCA_RescanRequired)
				{
					Bridge->QueueFullScan();
					continue;
				}

				if (!UE::DreamShader::IsDreamShaderSourceFile(FileChange.Filename))
				{
					continue;
				}

				if (FileChange.Action == FFileChangeData::FCA_Added || FileChange.Action == FFileChangeData::FCA_Modified)
				{
					if (UE::DreamShader::IsDreamShaderHeaderFile(FileChange.Filename))
					{
						Bridge->QueueDependentSourcesForImport(FileChange.Filename);
					}
					else if (UE::DreamShader::IsDreamShaderFunctionFile(FileChange.Filename))
					{
						if (!FDreamShaderSourceFileUtils::IsPackageMaterialFile(FileChange.Filename))
						{
							Bridge->QueueSourceFile(FileChange.Filename);
						}
						Bridge->QueueDependentSourcesForImport(FileChange.Filename);
					}
					else if (FDreamShaderSourceFileUtils::IsPackageMaterialFile(FileChange.Filename))
					{
						continue;
					}
					else
					{
						Bridge->RebuildDependencyGraph();
						Bridge->QueueSourceFile(FileChange.Filename);
					}
				}
				else if (FileChange.Action == FFileChangeData::FCA_Removed)
				{
					const FString SourceFile = UE::DreamShader::NormalizeSourceFilePath(FileChange.Filename);
					if (UE::DreamShader::IsDreamShaderHeaderFile(FileChange.Filename) || UE::DreamShader::IsDreamShaderFunctionFile(FileChange.Filename))
					{
						Bridge->QueueDependentSourcesForImport(FileChange.Filename);
						if (UE::DreamShader::IsDreamShaderFunctionFile(FileChange.Filename))
						{
							Bridge->PendingFiles.Remove(SourceFile);
							Bridge->ClearDiagnosticsForSourceAndDependencies(SourceFile);
							Bridge->UpdateDiagnosticsFile();
						}
					}
					else if (FDreamShaderSourceFileUtils::IsPackageMaterialFile(FileChange.Filename))
					{
						continue;
					}
					else
					{
						Bridge->PendingFiles.Remove(SourceFile);
						Bridge->ClearDiagnosticsForSourceAndDependencies(SourceFile);
						Bridge->RebuildDependencyGraph();
						Bridge->UpdateDiagnosticsFile();
					}
					UE_LOG(LogDreamShader, Display, TEXT("DreamShader source removed, existing generated assets were left untouched: %s"), *FileChange.Filename);
				}
			}
		});
	}

	bool FDreamShaderEditorBridge::Tick(float DeltaSeconds)
	{
		(void)DeltaSeconds;

		if (bIsShuttingDown || IsEngineExitRequested() || GExitPurge)
		{
			return false;
		}

		ProcessRequestFiles();
		ProcessReadyFiles();
		return true;
	}

	void FDreamShaderEditorBridge::ProcessRequestFiles()
	{
		TArray<FString> RequestFiles;
		IFileManager::Get().FindFiles(RequestFiles, *FPaths::Combine(GetRequestDirectory(), TEXT("*.json")), true, false);

		for (const FString& RequestFileName : RequestFiles)
		{
			const FString RequestPath = FPaths::Combine(GetRequestDirectory(), RequestFileName);

			FString RequestText;
			if (!FFileHelper::LoadFileToString(RequestText, *RequestPath))
			{
				IFileManager::Get().Delete(*RequestPath);
				continue;
			}

			TSharedPtr<FJsonObject> RequestObject;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestText);
			if (FJsonSerializer::Deserialize(Reader, RequestObject) && RequestObject.IsValid())
			{
				FString Action;
				FString Scope;
				RequestObject->TryGetStringField(TEXT("action"), Action);
				RequestObject->TryGetStringField(TEXT("scope"), Scope);
				if (Action.Equals(TEXT("recompile"), ESearchCase::IgnoreCase))
				{
					if (Scope.Equals(TEXT("all"), ESearchCase::IgnoreCase))
					{
						RequestRecompileAll();
					}
					else if (Scope.Equals(TEXT("file"), ESearchCase::IgnoreCase))
					{
						FString SourceFilePath;
						if (RequestObject->TryGetStringField(TEXT("sourceFile"), SourceFilePath) && !SourceFilePath.IsEmpty())
						{
							QueueSourceFile(SourceFilePath);
						}
					}
				}
				else if (Action.Equals(TEXT("cleanGeneratedShaders"), ESearchCase::IgnoreCase))
				{
					RequestCleanGeneratedShaders();
				}
			}

			IFileManager::Get().Delete(*RequestPath);
		}
	}

	void FDreamShaderEditorBridge::ProcessReadyFiles()
	{
		const double Now = FPlatformTime::Seconds();
		const UDreamShaderSettings* Settings = GetDefault<UDreamShaderSettings>();
		const double SaveDebounceSeconds = Settings ? FMath::Clamp(static_cast<double>(Settings->SaveDebounceSeconds), 0.05, 10.0) : 0.25;
		TArray<FString> ReadyFiles;
		for (const TPair<FString, double>& PendingFile : PendingFiles)
		{
			if (Now - PendingFile.Value >= SaveDebounceSeconds)
			{
				ReadyFiles.Add(PendingFile.Key);
			}
		}

		for (const FString& ReadyFile : ReadyFiles)
		{
			PendingFiles.Remove(ReadyFile);
			if (IFileManager::Get().FileExists(*ReadyFile))
			{
				ProcessSourceFile(ReadyFile);
			}
		}
	}

	void FDreamShaderEditorBridge::ProcessSourceFile(const FString& SourceFilePath)
	{
		UE::DreamShader::Editor::FDreamShaderCompileService CompileService(UE::DreamShader::Editor::GetMaterialGeneratorCompiler());
		const UE::DreamShader::Editor::FDreamShaderCompileResult Result = CompileService.CompileAssets(SourceFilePath);
		if (Result.bSucceeded)
		{
			ClearDiagnosticsForSourceAndDependencies(SourceFilePath);
			UpdateDiagnosticsFile();
			UE_LOG(LogDreamShader, Display, TEXT("%s"), *Result.Message);
			return;
		}

		TArray<FDreamShaderDiagnosticRecord> Diagnostics =
			FDreamShaderDiagnosticsStore::BuildGenerateErrorDiagnostics(SourceFilePath, Result.Message);
		ClearDiagnosticsForSourceAndDependencies(SourceFilePath);
		SetDiagnostics(SourceFilePath, MoveTemp(Diagnostics));
		UpdateDiagnosticsFile();
		UE_LOG(LogDreamShader, Error, TEXT("%s"), *Result.Message);
	}

	void FDreamShaderEditorBridge::OnMaterialCompilationFinished(UMaterialInterface* MaterialInterface)
	{
		if (bIsShuttingDown || IsEngineExitRequested() || GExitPurge)
		{
			return;
		}

		UMaterial* Material = Cast<UMaterial>(MaterialInterface);
		if (!Material)
		{
			return;
		}

		const FString SourceFilePath = GetSourceFileMetadata(Material);
		if (SourceFilePath.IsEmpty())
		{
			return;
		}

		TArray<FDreamShaderDiagnosticRecord> Diagnostics;
		const FString MaterialAssetPath = Material->GetPathName();
		TSet<FString> SeenDiagnosticKeys;
		for (int32 ShaderPlatformIndex = 0; ShaderPlatformIndex < EShaderPlatform::SP_NumPlatforms; ++ShaderPlatformIndex)
		{
			const EShaderPlatform ShaderPlatform = static_cast<EShaderPlatform>(ShaderPlatformIndex);
			for (int32 QualityLevelIndex = 0; QualityLevelIndex <= static_cast<int32>(EMaterialQualityLevel::Num); ++QualityLevelIndex)
			{
				const EMaterialQualityLevel::Type QualityLevel = static_cast<EMaterialQualityLevel::Type>(QualityLevelIndex);
				const FMaterialResource* MaterialResource = Material->GetMaterialResource(ShaderPlatform, QualityLevel);
				if (!MaterialResource)
				{
					continue;
				}

				const FString ShaderPlatformLabel = GetShaderPlatformLabel(ShaderPlatform);
				const FString QualityLabel = GetMaterialQualityLevelLabel(QualityLevel);
				for (const FString& Error : MaterialResource->GetCompileErrors())
				{
					const FString RawError = Error.TrimStartAndEnd();
					if (RawError.IsEmpty())
					{
						continue;
					}

					FDreamShaderDiagnosticLocation ParsedLocation;
					const bool bHasParsedLocation = FDreamShaderDiagnosticsStore::TryParseErrorLocation(RawError, ParsedLocation);
					const bool bMapsToDreamShaderSource = bHasParsedLocation && UE::DreamShader::IsDreamShaderSourceFile(ParsedLocation.FilePath);

					const FString DisplayMessage = FString::Printf(
						TEXT("[%s / %s] %s"),
						*ShaderPlatformLabel,
						*QualityLabel,
						*(bHasParsedLocation ? ParsedLocation.Message : GetFirstMeaningfulErrorLine(RawError)));

					const FString DeduplicationKey = FString::Printf(
						TEXT("%s|%s|%s|%s|%d|%d"),
						*SourceFilePath,
						*ShaderPlatformLabel,
						*QualityLabel,
						*DisplayMessage,
						bMapsToDreamShaderSource ? ParsedLocation.Line : 1,
						bMapsToDreamShaderSource ? ParsedLocation.Column : 1);
					if (SeenDiagnosticKeys.Contains(DeduplicationKey))
					{
						continue;
					}
					SeenDiagnosticKeys.Add(DeduplicationKey);

					FDreamShaderDiagnosticRecord& Diagnostic = Diagnostics.AddDefaulted_GetRef();
					Diagnostic.FilePath = bMapsToDreamShaderSource ? ParsedLocation.FilePath : SourceFilePath;
					Diagnostic.Message = DisplayMessage;
					Diagnostic.Detail = RawError;
					Diagnostic.Stage = TEXT("materialCompile");
					Diagnostic.AssetPath = MaterialAssetPath;
					Diagnostic.ShaderPlatform = ShaderPlatformLabel;
					Diagnostic.QualityLevel = QualityLabel;
					Diagnostic.Code = TEXT("material-compile");
					Diagnostic.Source = TEXT("DreamShader Material Compile");
					Diagnostic.Line = bMapsToDreamShaderSource ? ParsedLocation.Line : 1;
					Diagnostic.Column = bMapsToDreamShaderSource ? ParsedLocation.Column : 1;
				}
			}
		}

		SetDiagnostics(SourceFilePath, MoveTemp(Diagnostics));
		UpdateDiagnosticsFile();
	}

	void FDreamShaderEditorBridge::RegisterMenus()
	{
		if (bIsShuttingDown || bMenusRegistered || IsEngineExitRequested() || GExitPurge)
		{
			return;
		}

		bMenusRegistered = true;

		FToolMenuOwnerScoped MenuOwner(DreamShaderToolMenuOwnerName);

		if (UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
		{
			FToolMenuSection& Section = ToolsMenu->FindOrAddSection(TEXT("DreamShader"));
			Section.AddMenuEntry(
				TEXT("DreamShader.RecompileAll"),
				LOCTEXT("DreamShaderRecompileLabel", "Recompile DSM"),
				LOCTEXT("DreamShaderRecompileTooltip", "Recompile all DreamShader .dsm and .dsf source files and refresh diagnostics."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Refresh")),
				FUIAction(FExecuteAction::CreateSP(AsShared(), &FDreamShaderEditorBridge::RequestRecompileAll)));
			Section.AddMenuEntry(
				TEXT("DreamShader.CleanGeneratedShaders"),
				LOCTEXT("DreamShaderCleanGeneratedShadersLabel", "Clean Generated Shaders"),
				LOCTEXT("DreamShaderCleanGeneratedShadersTooltip", "Delete Intermediate/DreamShader/GeneratedShaders and queue a full DreamShader recompile."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Delete")),
				FUIAction(FExecuteAction::CreateSP(AsShared(), &FDreamShaderEditorBridge::RequestCleanGeneratedShaders)));
			Section.AddMenuEntry(
				TEXT("DreamShader.OpenWorkspace"),
				LOCTEXT("DreamShaderOpenWorkspaceLabel", "Open Dream Shader Workspace (VSCode)"),
				LOCTEXT("DreamShaderOpenWorkspaceTooltip", "Open the configured DreamShader source workspace in VSCode, or Notepad if VSCode is unavailable."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.OpenInExternalEditor")),
				FUIAction(FExecuteAction::CreateSP(AsShared(), &FDreamShaderEditorBridge::OpenDreamShaderWorkspace)));
		}

		if (UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.AssetsToolBar")))
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection(TEXT("DreamShader"));
			Section.AddEntry(FToolMenuEntry::InitToolBarButton(
				TEXT("DreamShader.RecompileAllToolbar"),
				FUIAction(FExecuteAction::CreateSP(AsShared(), &FDreamShaderEditorBridge::RequestRecompileAll)),
				LOCTEXT("DreamShaderRecompileToolbarLabel", "DSM"),
				LOCTEXT("DreamShaderRecompileToolbarTooltip", "Recompile all DreamShader .dsm and .dsf source files."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Refresh"))));
			Section.AddEntry(FToolMenuEntry::InitToolBarButton(
				TEXT("DreamShader.OpenWorkspaceToolbar"),
				FUIAction(FExecuteAction::CreateSP(AsShared(), &FDreamShaderEditorBridge::OpenDreamShaderWorkspace)),
				LOCTEXT("DreamShaderOpenWorkspaceToolbarLabel", "Open Dream Shader Workspace (VSCode)"),
				LOCTEXT("DreamShaderOpenWorkspaceToolbarTooltip", "Open the configured DreamShader source workspace in VSCode, or Notepad if VSCode is unavailable."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.OpenInExternalEditor"))));
		}

		if (UToolMenu* MaterialFunctionAssetMenu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(UMaterialFunction::StaticClass()))
		{
			FToolMenuSection& Section = MaterialFunctionAssetMenu->FindOrAddSection(TEXT("GetAssetActions"));
			Section.AddDynamicEntry(
				TEXT("DreamShader.VirtualFunctionAssetActions"),
				FNewToolMenuSectionDelegate::CreateSP(AsShared(), &FDreamShaderEditorBridge::PopulateMaterialFunctionAssetMenu));
		}

		if (UToolMenu* MaterialAssetMenu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(UMaterial::StaticClass()))
		{
			FToolMenuSection& Section = MaterialAssetMenu->FindOrAddSection(TEXT("GetAssetActions"));
			Section.AddDynamicEntry(
				TEXT("DreamShader.MaterialAssetActions"),
				FNewToolMenuSectionDelegate::CreateSP(AsShared(), &FDreamShaderEditorBridge::PopulateMaterialAssetMenu));
		}

		if (UToolMenu* MaterialEditorToolbar = UToolMenus::Get()->ExtendMenu(TEXT("AssetEditor.MaterialEditor.ToolBar")))
		{
			FToolMenuSection& Section = MaterialEditorToolbar->FindOrAddSection(TEXT("DreamShader"));
			Section.AddDynamicEntry(
				TEXT("DreamShader.MaterialEditorToolbarActions"),
				FNewToolMenuSectionDelegate::CreateSP(AsShared(), &FDreamShaderEditorBridge::PopulateMaterialEditorToolbar));
		}
	}

	void FDreamShaderEditorBridge::PopulateMaterialAssetMenu(FToolMenuSection& InSection)
	{
		const UContentBrowserAssetContextMenuContext* Context = UContentBrowserAssetContextMenuContext::FindContextWithAssets(InSection);
		if (!Context || Context->SelectedAssets.Num() != 1 || !Context->SelectedAssets[0].IsInstanceOf(UMaterial::StaticClass()))
		{
			return;
		}

		UMaterial* Material = Cast<UMaterial>(Context->SelectedAssets[0].GetAsset());
		if (!Material)
		{
			return;
		}

		InSection.AddSubMenu(
			TEXT("DreamShader.MaterialActions"),
			LOCTEXT("DreamShaderMaterialActionsLabel", "DreamShader"),
			LOCTEXT("DreamShaderMaterialActionsTooltip", "DreamShader actions for this Material."),
			FNewToolMenuDelegate::CreateSP(
				AsShared(),
				&FDreamShaderEditorBridge::PopulateMaterialDreamShaderMenu,
				TWeakObjectPtr<UMaterial>(Material)),
			false,
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Settings")));
	}

	void FDreamShaderEditorBridge::PopulateMaterialFunctionAssetMenu(FToolMenuSection& InSection)
	{
		const UContentBrowserAssetContextMenuContext* Context = UContentBrowserAssetContextMenuContext::FindContextWithAssets(InSection);
		if (!Context || Context->SelectedAssets.Num() != 1 || !Context->SelectedAssets[0].IsInstanceOf(UMaterialFunction::StaticClass()))
		{
			return;
		}

		UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Context->SelectedAssets[0].GetAsset());
		if (!MaterialFunction)
		{
			return;
		}

		InSection.AddSubMenu(
			TEXT("DreamShader.MaterialFunctionActions"),
			LOCTEXT("DreamShaderMaterialFunctionActionsLabel", "DreamShader"),
			LOCTEXT("DreamShaderMaterialFunctionActionsTooltip", "DreamShader actions for this Material Function."),
			FNewToolMenuDelegate::CreateSP(
				AsShared(),
				&FDreamShaderEditorBridge::PopulateMaterialFunctionDreamShaderMenu,
				TWeakObjectPtr<UMaterialFunction>(MaterialFunction)),
			false,
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Settings")));
	}

	void FDreamShaderEditorBridge::PopulateMaterialEditorToolbar(FToolMenuSection& InSection)
	{
		const UMaterialEditorMenuContext* Context = InSection.FindContext<UMaterialEditorMenuContext>();
		TSharedPtr<IMaterialEditor> MaterialEditor = Context ? Context->MaterialEditor.Pin() : nullptr;
		if (!MaterialEditor.IsValid())
		{
			return;
		}

		UMaterial* Material = nullptr;
		UMaterialFunction* MaterialFunction = nullptr;
		const TArray<UObject*>* EditingObjects = MaterialEditor->GetObjectsCurrentlyBeingEdited();
		if (EditingObjects)
		{
			for (UObject* EditingObject : *EditingObjects)
			{
				Material = Cast<UMaterial>(EditingObject);
				if (Material)
				{
					break;
				}
				MaterialFunction = Cast<UMaterialFunction>(EditingObject);
				if (MaterialFunction)
				{
					break;
				}
			}
		}

		if (Material)
		{
			InSection.AddEntry(FToolMenuEntry::InitComboButton(
				TEXT("DreamShader.MaterialToolbarMenu"),
				FUIAction(),
				FNewToolMenuDelegate::CreateSP(
					AsShared(),
					&FDreamShaderEditorBridge::PopulateMaterialDreamShaderMenu,
					TWeakObjectPtr<UMaterial>(Material)),
				LOCTEXT("DreamShaderMaterialToolbarMenuLabel", "DreamShader"),
				LOCTEXT("DreamShaderMaterialToolbarMenuTooltip", "DreamShader actions for this Material."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Settings"))));
			return;
		}

		if (!MaterialFunction)
		{
			return;
		}

		InSection.AddEntry(FToolMenuEntry::InitComboButton(
			TEXT("DreamShader.MaterialFunctionToolbarMenu"),
			FUIAction(),
			FNewToolMenuDelegate::CreateSP(
				AsShared(),
				&FDreamShaderEditorBridge::PopulateMaterialFunctionDreamShaderMenu,
				TWeakObjectPtr<UMaterialFunction>(MaterialFunction)),
			LOCTEXT("DreamShaderMaterialFunctionToolbarMenuLabel", "DreamShader"),
			LOCTEXT("DreamShaderMaterialFunctionToolbarMenuTooltip", "DreamShader actions for this Material Function."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Settings"))));
	}

	void FDreamShaderEditorBridge::PopulateMaterialDreamShaderMenu(UToolMenu* InMenu, TWeakObjectPtr<UMaterial> Material)
	{
		if (!InMenu || !Material.IsValid())
		{
			return;
		}

		FToolMenuSection& Section = InMenu->AddSection(
			TEXT("DreamShader.DecompileActions"),
			LOCTEXT("DreamShaderDecompileActionsSection", "Decompiler"));
		Section.AddMenuEntry(
			TEXT("DreamShader.ExportMaterialDSM"),
			LOCTEXT("DreamShaderExportMaterialDSMLabel", "Export DSM"),
			LOCTEXT("DreamShaderExportMaterialDSMTooltip", "Export this Material graph to a DreamShader .dsm source file."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Save")),
			FUIAction(FExecuteAction::CreateSP(
				AsShared(),
				&FDreamShaderEditorBridge::ExportMaterialToDreamShaderFile,
				Material)));
	}

	void FDreamShaderEditorBridge::PopulateMaterialFunctionDreamShaderMenu(UToolMenu* InMenu, TWeakObjectPtr<UMaterialFunction> MaterialFunction)
	{
		if (!InMenu || !MaterialFunction.IsValid())
		{
			return;
		}

		FToolMenuSection& DecompileSection = InMenu->AddSection(
			TEXT("DreamShader.DecompileActions"),
			LOCTEXT("DreamShaderFunctionDecompileActionsSection", "Decompiler"));
		DecompileSection.AddMenuEntry(
			TEXT("DreamShader.ExportFunctionDSF"),
			LOCTEXT("DreamShaderExportFunctionDSFLabel", "Export DSF"),
			LOCTEXT("DreamShaderExportFunctionDSFTooltip", "Export this Material Function graph to a DreamShader .dsf source file."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Save")),
			FUIAction(FExecuteAction::CreateSP(
				AsShared(),
				&FDreamShaderEditorBridge::ExportMaterialFunctionToDreamShaderFile,
				MaterialFunction)));

		FToolMenuSection& Section = InMenu->AddSection(
			TEXT("DreamShader.VirtualFunctionActions"),
			LOCTEXT("DreamShaderVirtualFunctionActionsSection", "VirtualFunction"));
		FVirtualFunctionDefinitionLocation ExistingDefinition;
		if (FindVirtualFunctionDefinitionForMaterialFunction(MaterialFunction.Get(), ExistingDefinition))
		{
			Section.AddMenuEntry(
				TEXT("DreamShader.OpenVirtualFunction"),
				LOCTEXT("DreamShaderOpenVirtualFunctionLabel", "OpenVirtualFunction"),
				LOCTEXT("DreamShaderOpenVirtualFunctionTooltip", "Open the existing DreamShader VirtualFunction definition in VSCode."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.OpenInExternalEditor")),
				FUIAction(FExecuteAction::CreateSP(
					AsShared(),
					&FDreamShaderEditorBridge::OpenVirtualFunctionDefinitionFile,
					MaterialFunction)));
			Section.AddMenuEntry(
				TEXT("DreamShader.CopyVirtualFunctionReference"),
				LOCTEXT("DreamShaderCopyVirtualFunctionReferenceLabel", "Copy Virtual Function Reference"),
				LOCTEXT("DreamShaderCopyVirtualFunctionReferenceTooltip", "Copy a DreamShader Graph call that references this existing VirtualFunction."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("GenericCommands.Copy")),
				FUIAction(FExecuteAction::CreateSP(
					AsShared(),
					&FDreamShaderEditorBridge::CopyVirtualFunctionReference,
					MaterialFunction)));
			return;
		}

		Section.AddMenuEntry(
			TEXT("DreamShader.CopyVirtualFunction"),
			LOCTEXT("DreamShaderCopyVirtualFunctionLabel", "CopyVirtualFunction"),
			LOCTEXT("DreamShaderCopyVirtualFunctionTooltip", "Copy a complete DreamShader VirtualFunction declaration for this Material Function."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("GenericCommands.Copy")),
			FUIAction(FExecuteAction::CreateSP(
				AsShared(),
				&FDreamShaderEditorBridge::CopyVirtualFunctionDefinition,
				MaterialFunction)));
		Section.AddMenuEntry(
			TEXT("DreamShader.CreateVirtualFunction"),
			LOCTEXT("DreamShaderCreateVirtualFunctionLabel", "CreateVirtualFunction"),
			LOCTEXT("DreamShaderCreateVirtualFunctionTooltip", "Create a .dsh file containing the VirtualFunction declaration."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Save")),
			FUIAction(FExecuteAction::CreateSP(
				AsShared(),
				&FDreamShaderEditorBridge::CreateVirtualFunctionDefinitionFile,
				MaterialFunction)));
		Section.AddMenuEntry(
			TEXT("DreamShader.CopyVirtualFunctionCall"),
			LOCTEXT("DreamShaderCopyVirtualFunctionCallLabel", "CopyVirtualFunctionCall"),
			LOCTEXT("DreamShaderCopyVirtualFunctionCallTooltip", "Copy a DreamShader Graph call example for this VirtualFunction."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("GenericCommands.Copy")),
			FUIAction(FExecuteAction::CreateSP(
				AsShared(),
				&FDreamShaderEditorBridge::CopyVirtualFunctionCall,
				MaterialFunction)));
	}

	void FDreamShaderEditorBridge::RequestRecompileAll()
	{
		if (bIsShuttingDown || IsEngineExitRequested() || GExitPurge)
		{
			return;
		}

		QueueFullScan();
		UE_LOG(LogDreamShader, Display, TEXT("DreamShader queued a full .dsm/.dsf recompile scan."));
	}

	void FDreamShaderEditorBridge::RequestCleanGeneratedShaders()
	{
		if (bIsShuttingDown || IsEngineExitRequested() || GExitPurge)
		{
			return;
		}

		CleanGeneratedShaderDirectory();
		QueueFullScan();
		UE_LOG(LogDreamShader, Display, TEXT("DreamShader cleaned generated shader includes and queued a full .dsm/.dsf recompile scan."));
	}

	void FDreamShaderEditorBridge::OpenDreamShaderWorkspace()
	{
		if (bIsShuttingDown || IsEngineExitRequested() || GExitPurge)
		{
			return;
		}

		FDreamShaderWorkspaceService::ExportMaterialExpressionManifest();
		FDreamShaderWorkspaceService::ExportDreamShaderSettingsManifest();

		FString WorkspaceFilePath;
		FString Error;
		if (!FDreamShaderWorkspaceService::WriteDreamShaderWorkspaceFile(WorkspaceFilePath, Error))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to create workspace: %s"), *Error)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to create DreamShader workspace: %s"), *Error);
			return;
		}

		if (FDreamShaderEditorLaunchUtils::LaunchVSCodeWorkspace(WorkspaceFilePath))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("Opened DreamShader workspace in VSCode: %s"), *WorkspaceFilePath)),
				SNotificationItem::CS_Success);
			UE_LOG(LogDreamShader, Display, TEXT("Opened DreamShader workspace in VSCode: %s"), *WorkspaceFilePath);
			return;
		}

		if (FPlatformProcess::LaunchFileInDefaultExternalApplication(*WorkspaceFilePath, nullptr, ELaunchVerb::Edit, false))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("Opened DreamShader workspace: %s"), *WorkspaceFilePath)),
				SNotificationItem::CS_Success);
			UE_LOG(LogDreamShader, Display, TEXT("Opened DreamShader workspace with the default editor: %s"), *WorkspaceFilePath);
			return;
		}

		if (FDreamShaderEditorLaunchUtils::LaunchTextFileWithNotepad(WorkspaceFilePath))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("Opened DreamShader workspace in Notepad: %s"), *WorkspaceFilePath)),
				SNotificationItem::CS_Success);
			UE_LOG(LogDreamShader, Display, TEXT("Opened DreamShader workspace in Notepad: %s"), *WorkspaceFilePath);
			return;
		}

		ShowDreamShaderNotification(
			FText::FromString(FString::Printf(TEXT("DreamShader could not open workspace: %s"), *WorkspaceFilePath)),
			SNotificationItem::CS_Fail);
		UE_LOG(LogDreamShader, Warning, TEXT("Failed to open DreamShader workspace: %s"), *WorkspaceFilePath);
	}

	void FDreamShaderEditorBridge::ExportMaterialToDreamShaderFile(TWeakObjectPtr<UMaterial> Material)
	{
		UMaterial* MaterialAsset = Material.Get();
		if (!MaterialAsset)
		{
			ShowDreamShaderNotification(
				LOCTEXT("DreamShaderExportMaterialNoAsset", "DreamShader could not find the selected Material."),
				SNotificationItem::CS_Fail);
			return;
		}

		FDreamShaderDecompileService DecompileService(GetGraphDecompiler());
		UE::DreamShader::Editor::FDreamShaderDecompileRequest Request;
		Request.Asset = MaterialAsset;
		const UE::DreamShader::Editor::FDreamShaderDecompileResult Result = DecompileService.DecompileAsset(Request);
		if (!Result.bSucceeded)
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to export DSM: %s"), *Result.Error)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to export Material '%s' to DSM: %s"), *MaterialAsset->GetPathName(), *Result.Error);
			return;
		}

		const FString SourceFilePath = Result.OutputFilePath;
		FString SaveError;
		if (!FDecompiledSourceWriter::Save(Result, SaveError))
		{
			ShowDreamShaderNotification(
				FText::FromString(SaveError),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to write decompiled Material DSM file '%s': %s"), *SourceFilePath, *SaveError);
			return;
		}

		if (!FDreamShaderEditorLaunchUtils::LaunchTextFileInPreferredEditor(SourceFilePath))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("Exported DSM but could not open it: %s"), *SourceFilePath)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Exported DSM '%s' but failed to open it."), *SourceFilePath);
			return;
		}

		ShowDreamShaderNotification(
			FText::FromString(FString::Printf(TEXT("Exported DSM: %s"), *SourceFilePath)),
			SNotificationItem::CS_Success);
		UE_LOG(LogDreamShader, Display, TEXT("Exported Material '%s' to DSM '%s'."), *MaterialAsset->GetPathName(), *SourceFilePath);
	}

	void FDreamShaderEditorBridge::ExportMaterialFunctionToDreamShaderFile(TWeakObjectPtr<UMaterialFunction> MaterialFunction)
	{
		UMaterialFunction* Function = MaterialFunction.Get();
		if (!Function)
		{
			ShowDreamShaderNotification(
				LOCTEXT("DreamShaderExportFunctionNoAsset", "DreamShader could not find the selected Material Function."),
				SNotificationItem::CS_Fail);
			return;
		}

		FDreamShaderDecompileService DecompileService(GetGraphDecompiler());
		UE::DreamShader::Editor::FDreamShaderDecompileRequest Request;
		Request.Asset = Function;
		const UE::DreamShader::Editor::FDreamShaderDecompileResult Result = DecompileService.DecompileAsset(Request);
		if (!Result.bSucceeded)
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to export DSF: %s"), *Result.Error)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to export MaterialFunction '%s' to DSF: %s"), *Function->GetPathName(), *Result.Error);
			return;
		}

		const FString SourceFilePath = Result.OutputFilePath;
		FString SaveError;
		if (!FDecompiledSourceWriter::Save(Result, SaveError))
		{
			ShowDreamShaderNotification(
				FText::FromString(SaveError),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to write decompiled MaterialFunction DSF file '%s': %s"), *SourceFilePath, *SaveError);
			return;
		}

		if (!FDreamShaderEditorLaunchUtils::LaunchTextFileInPreferredEditor(SourceFilePath))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("Exported DSF but could not open it: %s"), *SourceFilePath)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Exported DSF '%s' but failed to open it."), *SourceFilePath);
			return;
		}

		ShowDreamShaderNotification(
			FText::FromString(FString::Printf(TEXT("Exported DSF: %s"), *SourceFilePath)),
			SNotificationItem::CS_Success);
		UE_LOG(LogDreamShader, Display, TEXT("Exported MaterialFunction '%s' to DSF '%s'."), *Function->GetPathName(), *SourceFilePath);
	}

	void FDreamShaderEditorBridge::CopyVirtualFunctionDefinition(TWeakObjectPtr<UMaterialFunction> MaterialFunction)
	{
		UMaterialFunction* Function = MaterialFunction.Get();
		if (!Function)
		{
			ShowDreamShaderNotification(
				LOCTEXT("DreamShaderCopyVirtualFunctionNoAsset", "DreamShader could not find the selected Material Function."),
				SNotificationItem::CS_Fail);
			return;
		}

		FString DefinitionText;
		FString Error;
		if (!BuildVirtualFunctionDefinition(Function, DefinitionText, Error))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to build VirtualFunction: %s"), *Error)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to build VirtualFunction definition for '%s': %s"), *Function->GetPathName(), *Error);
			return;
		}

		FPlatformApplicationMisc::ClipboardCopy(*DefinitionText);
		ShowDreamShaderNotification(
			FText::FromString(FString::Printf(TEXT("Copied VirtualFunction definition for %s."), *Function->GetName())),
			SNotificationItem::CS_Success);
		UE_LOG(LogDreamShader, Display, TEXT("Copied VirtualFunction definition for '%s'.\n%s"), *Function->GetPathName(), *DefinitionText);
	}

	void FDreamShaderEditorBridge::CreateVirtualFunctionDefinitionFile(TWeakObjectPtr<UMaterialFunction> MaterialFunction)
	{
		UMaterialFunction* Function = MaterialFunction.Get();
		if (!Function)
		{
			ShowDreamShaderNotification(
				LOCTEXT("DreamShaderCreateVirtualFunctionNoAsset", "DreamShader could not find the selected Material Function."),
				SNotificationItem::CS_Fail);
			return;
		}

		FVirtualFunctionDefinitionLocation ExistingDefinition;
		if (FindVirtualFunctionDefinitionForMaterialFunction(Function, ExistingDefinition))
		{
			OpenVirtualFunctionDefinitionFile(MaterialFunction);
			return;
		}

		FString DefinitionText;
		FString Error;
		if (!BuildVirtualFunctionDefinition(Function, DefinitionText, Error))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to build VirtualFunction: %s"), *Error)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to build VirtualFunction definition file for '%s': %s"), *Function->GetPathName(), *Error);
			return;
		}

		const FString DefinitionFilePath = FDreamShaderVirtualFunctionService::MakeDefinitionFilePath(Function);
		const FString DefinitionDirectory = FPaths::GetPath(DefinitionFilePath);
		if (!IFileManager::Get().MakeDirectory(*DefinitionDirectory, true))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to create directory: %s"), *DefinitionDirectory)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to create VirtualFunction definition directory '%s'."), *DefinitionDirectory);
			return;
		}

		if (!FFileHelper::SaveStringToFile(DefinitionText, *DefinitionFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to write VirtualFunction file: %s"), *DefinitionFilePath)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to write VirtualFunction definition file '%s'."), *DefinitionFilePath);
			return;
		}

		if (!FDreamShaderEditorLaunchUtils::LaunchTextFileInPreferredEditor(DefinitionFilePath))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("Created VirtualFunction file but could not open it: %s"), *DefinitionFilePath)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Created VirtualFunction definition file '%s' but failed to open it."), *DefinitionFilePath);
			return;
		}

		ShowDreamShaderNotification(
			FText::FromString(FString::Printf(TEXT("Created VirtualFunction file: %s"), *DefinitionFilePath)),
			SNotificationItem::CS_Success);
		UE_LOG(LogDreamShader, Display, TEXT("Created VirtualFunction definition file '%s' for '%s'.\n%s"), *DefinitionFilePath, *Function->GetPathName(), *DefinitionText);
	}

	void FDreamShaderEditorBridge::OpenVirtualFunctionDefinitionFile(TWeakObjectPtr<UMaterialFunction> MaterialFunction)
	{
		UMaterialFunction* Function = MaterialFunction.Get();
		if (!Function)
		{
			ShowDreamShaderNotification(
				LOCTEXT("DreamShaderOpenVirtualFunctionNoAsset", "DreamShader could not find the selected Material Function."),
				SNotificationItem::CS_Fail);
			return;
		}

		FVirtualFunctionDefinitionLocation ExistingDefinition;
		if (!FindVirtualFunctionDefinitionForMaterialFunction(Function, ExistingDefinition))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader could not find a VirtualFunction definition for %s."), *Function->GetName())),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("No VirtualFunction definition found for '%s'."), *Function->GetPathName());
			return;
		}

		if (!FDreamShaderEditorLaunchUtils::LaunchTextFileInPreferredEditor(
			ExistingDefinition.SourceFilePath,
			ExistingDefinition.Line,
			ExistingDefinition.Column))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader could not open VirtualFunction file: %s"), *ExistingDefinition.SourceFilePath)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to open VirtualFunction definition file '%s'."), *ExistingDefinition.SourceFilePath);
			return;
		}

		ShowDreamShaderNotification(
			FText::FromString(FString::Printf(TEXT("Opened VirtualFunction definition: %s"), *ExistingDefinition.SourceFilePath)),
			SNotificationItem::CS_Success);
		UE_LOG(
			LogDreamShader,
			Display,
			TEXT("Opened VirtualFunction definition '%s' for '%s'."),
			*ExistingDefinition.SourceFilePath,
			*Function->GetPathName());
	}

	void FDreamShaderEditorBridge::CopyVirtualFunctionReference(TWeakObjectPtr<UMaterialFunction> MaterialFunction)
	{
		UMaterialFunction* Function = MaterialFunction.Get();
		if (!Function)
		{
			ShowDreamShaderNotification(
				LOCTEXT("DreamShaderCopyVirtualFunctionReferenceNoAsset", "DreamShader could not find the selected Material Function."),
				SNotificationItem::CS_Fail);
			return;
		}

		FVirtualFunctionDefinitionLocation ExistingDefinition;
		if (!FindVirtualFunctionDefinitionForMaterialFunction(Function, ExistingDefinition))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader could not find a VirtualFunction definition for %s."), *Function->GetName())),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("No VirtualFunction definition found for '%s'."), *Function->GetPathName());
			return;
		}

		FString CallText;
		FString Error;
		if (!FDreamShaderVirtualFunctionService::BuildCallTextFromSignature(
			ExistingDefinition.FunctionName,
			ExistingDefinition.Inputs,
			ExistingDefinition.Outputs,
			CallText,
			Error))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to build VirtualFunction reference: %s"), *Error)),
				SNotificationItem::CS_Fail);
			UE_LOG(
				LogDreamShader,
				Warning,
				TEXT("Failed to build VirtualFunction reference for '%s' from '%s': %s"),
				*Function->GetPathName(),
				*ExistingDefinition.SourceFilePath,
				*Error);
			return;
		}

		FPlatformApplicationMisc::ClipboardCopy(*CallText);
		ShowDreamShaderNotification(
			FText::FromString(FString::Printf(TEXT("Copied VirtualFunction reference for %s."), *ExistingDefinition.FunctionName)),
			SNotificationItem::CS_Success);
		UE_LOG(
			LogDreamShader,
			Display,
			TEXT("Copied VirtualFunction reference for '%s' from '%s': %s"),
			*Function->GetPathName(),
			*ExistingDefinition.SourceFilePath,
			*CallText);
	}

	void FDreamShaderEditorBridge::CopyVirtualFunctionCall(TWeakObjectPtr<UMaterialFunction> MaterialFunction)
	{
		UMaterialFunction* Function = MaterialFunction.Get();
		if (!Function)
		{
			ShowDreamShaderNotification(
				LOCTEXT("DreamShaderCopyVirtualFunctionCallNoAsset", "DreamShader could not find the selected Material Function."),
				SNotificationItem::CS_Fail);
			return;
		}

		FString CallText;
		FString Error;
		if (!BuildVirtualFunctionCallText(Function, CallText, Error))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to build VirtualFunction call: %s"), *Error)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to build VirtualFunction call for '%s': %s"), *Function->GetPathName(), *Error);
			return;
		}

		FPlatformApplicationMisc::ClipboardCopy(*CallText);
		ShowDreamShaderNotification(
			FText::FromString(FString::Printf(TEXT("Copied VirtualFunction call for %s."), *Function->GetName())),
			SNotificationItem::CS_Success);
		UE_LOG(LogDreamShader, Display, TEXT("Copied VirtualFunction call for '%s': %s"), *Function->GetPathName(), *CallText);
	}

	void FDreamShaderEditorBridge::CleanGeneratedShaderDirectory()
	{
		const FString GeneratedShaderDirectory = UE::DreamShader::GetGeneratedShaderDirectory();
		IFileManager& FileManager = IFileManager::Get();

		TArray<FString> GeneratedShaderFiles;
		FileManager.FindFilesRecursive(
			GeneratedShaderFiles,
			*GeneratedShaderDirectory,
			TEXT("*"),
			true,
			false,
			false);

		const int32 DeletedFileCount = GeneratedShaderFiles.Num();
		FileManager.DeleteDirectory(*GeneratedShaderDirectory, false, true);
		FileManager.MakeDirectory(*GeneratedShaderDirectory, true);

		UE_LOG(
			LogDreamShader,
			Display,
			TEXT("DreamShader deleted %d generated shader file(s) from '%s'."),
			DeletedFileCount,
			*GeneratedShaderDirectory);
	}

	void FDreamShaderEditorBridge::RebuildDependencyGraph()
	{
		FDreamShaderDependencyGraphService::RebuildMaterialDependencyGraph(HeaderDependentsByFile);
	}

	void FDreamShaderEditorBridge::SyncVirtualFunctionDefinitions()
	{
		struct FVirtualFunctionReplacement
		{
			int32 StartIndex = INDEX_NONE;
			int32 EndIndex = INDEX_NONE;
			FString DefinitionText;
		};

		TArray<FString> SourceFiles;
		FDreamShaderSourceFileUtils::FindProjectDreamShaderSourceFiles(SourceFiles);

		int32 ScannedDefinitionCount = 0;
		int32 UpdatedDefinitionCount = 0;
		int32 ErrorCount = 0;

		for (const FString& SourceFile : SourceFiles)
		{
			FString SourceText;
			TArray<FVirtualFunctionDefinitionLocation> Locations;
			TArray<FDreamShaderDiagnosticRecord> Diagnostics;
			CollectVirtualFunctionDefinitionLocationsFromFile(SourceFile, Locations, &SourceText, &Diagnostics);

			TArray<FVirtualFunctionReplacement> Replacements;
			for (const FVirtualFunctionDefinitionLocation& Location : Locations)
			{
				++ScannedDefinitionCount;

				UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *Location.AssetObjectPath);
				if (!Function)
				{
					Diagnostics.Add(MakeVirtualFunctionDiagnostic(
						SourceFile,
						FString::Printf(
							TEXT("VirtualFunction '%s' references missing MaterialFunction '%s'."),
							*Location.FunctionName,
							*Location.AssetObjectPath),
						FString(),
						Location.AssetObjectPath,
						Location.Line,
						Location.Column));
					continue;
				}

				FString GeneratedDefinition;
				FString Error;
				if (!BuildVirtualFunctionDefinition(Function, GeneratedDefinition, Error))
				{
					Diagnostics.Add(MakeVirtualFunctionDiagnostic(
						SourceFile,
						FString::Printf(
							TEXT("VirtualFunction '%s' could not be refreshed from MaterialFunction '%s': %s"),
							*Location.FunctionName,
							*Location.AssetObjectPath,
							*Error),
						Error,
						Location.AssetObjectPath,
						Location.Line,
						Location.Column));
					continue;
				}

				if (NormalizeVirtualFunctionDefinitionText(Location.CurrentText)
					!= NormalizeVirtualFunctionDefinitionText(GeneratedDefinition))
				{
					FVirtualFunctionReplacement& Replacement = Replacements.AddDefaulted_GetRef();
					Replacement.StartIndex = Location.StartIndex;
					Replacement.EndIndex = Location.EndIndex;
					Replacement.DefinitionText = GeneratedDefinition;
				}
			}

			if (!Replacements.IsEmpty())
			{
				FString UpdatedSourceText = SourceText;
				Replacements.Sort([](const FVirtualFunctionReplacement& A, const FVirtualFunctionReplacement& B)
				{
					return A.StartIndex > B.StartIndex;
				});

				for (const FVirtualFunctionReplacement& Replacement : Replacements)
				{
					UpdatedSourceText =
						UpdatedSourceText.Left(Replacement.StartIndex)
						+ Replacement.DefinitionText
						+ UpdatedSourceText.Mid(Replacement.EndIndex);
				}

				if (!FFileHelper::SaveStringToFile(UpdatedSourceText, *SourceFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					Diagnostics.Add(MakeVirtualFunctionDiagnostic(
						SourceFile,
						FString::Printf(TEXT("DreamShader failed to update VirtualFunction source file '%s'."), *SourceFile),
						FString(),
						FString(),
						1,
						1));
				}
				else
				{
					UpdatedDefinitionCount += Replacements.Num();
					UE_LOG(
						LogDreamShader,
						Display,
						TEXT("DreamShader refreshed %d VirtualFunction definition(s) in '%s'."),
						Replacements.Num(),
						*SourceFile);
				}
			}

			if (Diagnostics.IsEmpty())
			{
				if (!Locations.IsEmpty())
				{
					ClearDiagnostics(SourceFile);
				}
			}
			else
			{
				ErrorCount += Diagnostics.Num();
				SetDiagnostics(SourceFile, MoveTemp(Diagnostics));
			}
		}

		if (ScannedDefinitionCount > 0 || UpdatedDefinitionCount > 0 || ErrorCount > 0)
		{
			UE_LOG(
				LogDreamShader,
				Display,
				TEXT("DreamShader scanned %d VirtualFunction definition(s), refreshed %d, reported %d issue(s)."),
				ScannedDefinitionCount,
				UpdatedDefinitionCount,
				ErrorCount);
		}
	}

	void FDreamShaderEditorBridge::SetDiagnostics(const FString& SourceFilePath, TArray<FDreamShaderDiagnosticRecord>&& Diagnostics)
	{
		DiagnosticsStore.SetDiagnostics(SourceFilePath, MoveTemp(Diagnostics));
	}

	void FDreamShaderEditorBridge::ClearDiagnostics(const FString& SourceFilePath)
	{
		DiagnosticsStore.ClearDiagnostics(SourceFilePath);
	}

	void FDreamShaderEditorBridge::ClearDiagnosticsForSourceAndDependencies(const FString& SourceFilePath)
	{
		ClearDiagnostics(SourceFilePath);

		TSet<FString> Dependencies;
		TSet<FString> VisitedFiles;
		FDreamShaderDependencyGraphService::CollectHeaderDependenciesRecursive(SourceFilePath, Dependencies, VisitedFiles);
		for (const FString& HeaderFile : Dependencies)
		{
			ClearDiagnostics(HeaderFile);
		}
	}

	void FDreamShaderEditorBridge::UpdateDiagnosticsFile() const
	{
		DiagnosticsStore.WriteToFile(GetDiagnosticsFilePath());
	}
}

UDreamShaderCommandlet::UDreamShaderCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UDreamShaderCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamValues;
	ParseCommandLine(*Params, Tokens, Switches, ParamValues);

	FString Command;
	if (!Tokens.IsEmpty())
	{
		Command = Tokens[0];
	}
	else if (!UE::DreamShader::Editor::Private::TryGetCommandletParam(Tokens, Switches, ParamValues, TEXT("Command"), Command))
	{
		UE_LOG(LogDreamShader, Error, TEXT("%s"), UE::DreamShader::Editor::Private::GetDreamShaderCommandletUsage());
		return 1;
	}

	Command.TrimStartAndEndInline();
	if (Command.Equals(TEXT("compile"), ESearchCase::IgnoreCase)
		|| Command.Equals(TEXT("generate"), ESearchCase::IgnoreCase))
	{
		return UE::DreamShader::Editor::Private::RunDreamShaderCompileCommandlet(Tokens, Switches, ParamValues) ? 0 : 1;
	}

	if (Command.Equals(TEXT("decompile"), ESearchCase::IgnoreCase)
		|| Command.Equals(TEXT("export"), ESearchCase::IgnoreCase))
	{
		return UE::DreamShader::Editor::Private::RunDreamShaderDecompileCommandlet(
			Tokens,
			Switches,
			ParamValues,
			UE::DreamShader::Editor::Private::GetGraphDecompiler()) ? 0 : 1;
	}

	UE_LOG(LogDreamShader, Error, TEXT("Unknown DreamShader command '%s'.\n%s"), *Command, UE::DreamShader::Editor::Private::GetDreamShaderCommandletUsage());
	return 1;
}

#undef LOCTEXT_NAMESPACE
