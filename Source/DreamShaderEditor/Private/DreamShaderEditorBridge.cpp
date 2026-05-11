#include "DreamShaderEditorBridge.h"

#include "DreamShaderMaterialGenerator.h"
#include "DreamShaderMaterialGeneratorPrivate.h"
#include "DreamShaderModule.h"
#include "DreamShaderParser.h"
#include "DreamShaderSettings.h"

#include "Async/Async.h"
#include "CoreGlobals.h"
#include "ContentBrowserMenuContexts.h"
#include "DirectoryWatcherModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
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
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionScreenPosition.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionSquareRoot.h"
#include "Materials/MaterialExpressionSubtract.h"
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
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ShaderCore.h"
#include "Styling/AppStyle.h"
#include "ToolMenu.h"
#include "ToolMenus.h"
#include "UObject/MetaData.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"

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

		bool IsPathUnderDirectory(const FString& InPath, const FString& InDirectory)
		{
			const FString Path = UE::DreamShader::NormalizeSourceFilePath(InPath);
			FString Directory = UE::DreamShader::NormalizeSourceFilePath(InDirectory);
			Directory.RemoveFromEnd(TEXT("/"));

			return Path.Equals(Directory, ESearchCase::IgnoreCase)
				|| Path.StartsWith(Directory + TEXT("/"), ESearchCase::IgnoreCase);
		}

		bool IsPackageMaterialFile(const FString& InPath)
		{
			return UE::DreamShader::IsDreamShaderMaterialFile(InPath)
				&& IsPathUnderDirectory(InPath, UE::DreamShader::GetPackageShaderDirectory());
		}

		FString EscapeDreamShaderString(const FString& InText)
		{
			FString Result = InText;
			Result.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			Result.ReplaceInline(TEXT("\""), TEXT("\\\""));
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

		bool TryMakeVirtualFunctionAssetLiteral(const UMaterialFunction* MaterialFunction, FString& OutLiteral, FString& OutError)
		{
			if (!MaterialFunction)
			{
				OutError = TEXT("No MaterialFunction asset was provided.");
				return false;
			}

			FString PackageName = MaterialFunction->GetOutermost() ? MaterialFunction->GetOutermost()->GetName() : FString();
			PackageName.TrimStartAndEndInline();
			PackageName.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (PackageName.IsEmpty() || !PackageName.StartsWith(TEXT("/")))
			{
				OutError = FString::Printf(TEXT("MaterialFunction '%s' does not have a valid package path."), *MaterialFunction->GetName());
				return false;
			}

			const auto BuildLiteral = [&OutLiteral](const TCHAR* RootName, const FString& RelativePath)
			{
				OutLiteral = FString::Printf(TEXT("Path(%s, %s)"), RootName, *RelativePath);
			};

			if (PackageName.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase))
			{
				BuildLiteral(TEXT("Game"), PackageName.Mid(6));
				return true;
			}
			if (PackageName.StartsWith(TEXT("/Engine/"), ESearchCase::IgnoreCase))
			{
				BuildLiteral(TEXT("Engine"), PackageName.Mid(8));
				return true;
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
				OutLiteral = FString::Printf(
					TEXT("Path(Plugins.%s, %s)"),
					*BestPluginName,
					*RelativePath);
				return true;
			}

			OutLiteral = MaterialFunction->GetPathName();
			return true;
		}

		bool BuildVirtualFunctionDefinition(const UMaterialFunction* MaterialFunction, FString& OutDefinition, FString& OutError)
		{
			if (!MaterialFunction)
			{
				OutError = TEXT("No MaterialFunction asset was provided.");
				return false;
			}

			FString AssetLiteral;
			if (!TryMakeVirtualFunctionAssetLiteral(MaterialFunction, AssetLiteral, OutError))
			{
				return false;
			}

			TArray<FFunctionExpressionInput> Inputs;
			TArray<FFunctionExpressionOutput> Outputs;
			MaterialFunction->GetInputsAndOutputs(Inputs, Outputs);

			if (Outputs.IsEmpty())
			{
				OutError = FString::Printf(TEXT("MaterialFunction '%s' does not expose any outputs."), *MaterialFunction->GetName());
				return false;
			}

			TArray<FString> Lines;
			Lines.Add(FString::Printf(
				TEXT("VirtualFunction(Name=\"%s\")"),
				*EscapeDreamShaderString(MakeDreamShaderDeclarationName(MaterialFunction->GetName(), TEXT("VirtualFunction"), 0))));
			Lines.Add(TEXT("{"));
			Lines.Add(TEXT("\tOptions = {"));
			Lines.Add(FString::Printf(TEXT("\t\tAsset = %s;"), *AssetLiteral));
			Lines.Add(FString::Printf(
				TEXT("\t\tDescription = \"Generated from %s\";"),
				*EscapeDreamShaderString(MaterialFunction->GetPathName())));
			Lines.Add(TEXT("\t}"));
			Lines.Add(TEXT(""));
			Lines.Add(TEXT("\tInputs = {"));
			for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
			{
				const FFunctionExpressionInput& Input = Inputs[InputIndex];
				const UMaterialExpressionFunctionInput* InputExpression = Input.ExpressionInput;
				const FString InputName = InputExpression
					? Input.ExpressionInput->InputName.ToString()
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
				Lines.Add(FString::Printf(
					TEXT("\t\t%s%s %s%s%s;"),
					bOptional ? TEXT("opt ") : TEXT(""),
					*GetDreamShaderTypeForFunctionInput(InputType),
					*MakeDreamShaderDeclarationName(InputName, TEXT("Input"), InputIndex),
					*DefaultSuffix,
					*MetadataSuffix));
			}
			Lines.Add(TEXT("\t}"));
			Lines.Add(TEXT(""));
			Lines.Add(TEXT("\tOutputs = {"));
			for (int32 OutputIndex = 0; OutputIndex < Outputs.Num(); ++OutputIndex)
			{
				const FFunctionExpressionOutput& Output = Outputs[OutputIndex];
				UMaterialExpressionFunctionOutput* OutputExpression = Output.ExpressionOutput;
				const FString OutputName = OutputExpression
					? OutputExpression->OutputName.ToString()
					: Output.Output.OutputName.ToString();
				const EMaterialValueType OutputType = OutputExpression
					? OutputExpression->GetInputValueType(0)
					: MCT_Float4;
				const FString MetadataSuffix = OutputExpression
					? MakeFunctionParameterMetadataSuffix(OutputExpression->Description, OutputExpression->SortPriority, OutputIndex)
					: FString();
				Lines.Add(FString::Printf(
					TEXT("\t\t%s %s%s;"),
					*GetDreamShaderTypeForMaterialValueType(OutputType),
					*MakeDreamShaderDeclarationName(OutputName, TEXT("Output"), OutputIndex),
					*MetadataSuffix));
			}
			Lines.Add(TEXT("\t}"));
			Lines.Add(TEXT("}"));

			OutDefinition = FString::Join(Lines, TEXT("\n"));
			return true;
		}

		bool BuildVirtualFunctionCallTextFromSignature(
			const FString& FunctionName,
			const TArray<FTextShaderFunctionParameter>& Inputs,
			const TArray<FTextShaderFunctionParameter>& Outputs,
			FString& OutCallText,
			FString& OutError)
		{
			if (FunctionName.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("VirtualFunction name cannot be empty.");
				return false;
			}

			if (Outputs.IsEmpty())
			{
				OutError = FString::Printf(TEXT("VirtualFunction '%s' does not expose any outputs."), *FunctionName);
				return false;
			}

			TArray<FString> Arguments;
			for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
			{
				Arguments.Add(Inputs[InputIndex].bOptional
					? TEXT("default")
					: MakeDreamShaderDeclarationName(Inputs[InputIndex].Name, TEXT("Input"), InputIndex));
			}

			Arguments.Add(FString::Printf(
				TEXT("Output=\"%s\""),
				*EscapeDreamShaderString(MakeDreamShaderDeclarationName(Outputs[0].Name, TEXT("Output"), 0))));

			OutCallText = FString::Printf(
				TEXT("%s(%s)"),
				*MakeDreamShaderDeclarationName(FunctionName, TEXT("VirtualFunction"), 0),
				*FString::Join(Arguments, TEXT(", ")));
			return true;
		}

		bool BuildVirtualFunctionCallText(const UMaterialFunction* MaterialFunction, FString& OutCallText, FString& OutError)
		{
			if (!MaterialFunction)
			{
				OutError = TEXT("No MaterialFunction asset was provided.");
				return false;
			}

			TArray<FFunctionExpressionInput> FunctionInputs;
			TArray<FFunctionExpressionOutput> FunctionOutputs;
			MaterialFunction->GetInputsAndOutputs(FunctionInputs, FunctionOutputs);

			TArray<FTextShaderFunctionParameter> Inputs;
			for (int32 InputIndex = 0; InputIndex < FunctionInputs.Num(); ++InputIndex)
			{
				const FFunctionExpressionInput& Input = FunctionInputs[InputIndex];
				const FString InputName = Input.ExpressionInput
					? Input.ExpressionInput->InputName.ToString()
					: Input.Input.InputName.ToString();
				FTextShaderFunctionParameter& Parameter = Inputs.AddDefaulted_GetRef();
				Parameter.Name = InputName;
				Parameter.bOptional = Input.ExpressionInput && Input.ExpressionInput->bUsePreviewValueAsDefault != 0;
			}

			TArray<FTextShaderFunctionParameter> Outputs;
			for (int32 OutputIndex = 0; OutputIndex < FunctionOutputs.Num(); ++OutputIndex)
			{
				const FFunctionExpressionOutput& Output = FunctionOutputs[OutputIndex];
				const FString OutputName = Output.ExpressionOutput
					? Output.ExpressionOutput->OutputName.ToString()
					: Output.Output.OutputName.ToString();
				FTextShaderFunctionParameter& Parameter = Outputs.AddDefaulted_GetRef();
				Parameter.Name = OutputName;
			}

			return BuildVirtualFunctionCallTextFromSignature(
				MaterialFunction->GetName(),
				Inputs,
				Outputs,
				OutCallText,
				OutError);
		}

		FString MakeVirtualFunctionDefinitionFilePath(const UMaterialFunction* MaterialFunction)
		{
			const FString DefinitionDirectory = FPaths::Combine(
				UE::DreamShader::GetSourceShaderDirectory(),
				TEXT("VirtualFunctions"));
			const FString BaseName = MakeDreamShaderDeclarationName(
				MaterialFunction ? MaterialFunction->GetName() : FString(),
				TEXT("VirtualFunction"),
				0);

			const FString PreferredCandidate = FPaths::Combine(DefinitionDirectory, BaseName + TEXT(".dsh"));
			if (!IFileManager::Get().FileExists(*PreferredCandidate) || !MaterialFunction)
			{
				return UE::DreamShader::NormalizeSourceFilePath(PreferredCandidate);
			}

			const uint32 AssetPathHash = FCrc::StrCrc32(*MaterialFunction->GetPathName());
			return UE::DreamShader::NormalizeSourceFilePath(FPaths::Combine(
				DefinitionDirectory,
				FString::Printf(TEXT("%s_%08x.dsh"), *BaseName, AssetPathHash)));
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
			if (SwizzleText.Len() == 1)
			{
				return FString::Printf(TEXT("%s.%s"), *WrapExpressionForSuffix(ExpressionText), *SwizzleText);
			}

			TArray<FString> Channels;
			Channels.Reserve(SwizzleText.Len());
			for (int32 Index = 0; Index < SwizzleText.Len(); ++Index)
			{
				Channels.Add(FString::Printf(
					TEXT("%s.%c"),
					*WrapExpressionForSuffix(ExpressionText),
					SwizzleText[Index]));
			}

			return FString::Printf(TEXT("float%d(%s)"), SwizzleText.Len(), *FString::Join(Channels, TEXT(", ")));
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

		FString GetDreamShaderTypeForFunctionOutput(const UMaterialExpressionFunctionOutput* OutputExpression)
		{
			if (!OutputExpression)
			{
				return TEXT("float4");
			}

			const EMaterialValueType OutputType = const_cast<UMaterialExpressionFunctionOutput*>(OutputExpression)->GetInputValueType(0);
			if (OutputType == MCT_MaterialAttributes)
			{
				return TEXT("MaterialAttributes");
			}
			return GetDreamShaderTypeForMaterialValueType(OutputType);
		}

		FString GetDreamShaderTypeForExpressionOutput(UMaterialExpression* Expression, const int32 OutputIndex)
		{
			if (!Expression)
			{
				return TEXT("float4");
			}

			const EMaterialValueType OutputType = Expression->GetOutputValueType(OutputIndex);
			if (OutputType == MCT_MaterialAttributes)
			{
				return TEXT("MaterialAttributes");
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

		FString MakeUniqueDecompiledSourcePath(const FString& Directory, const FString& BaseName, const TCHAR* Extension)
		{
			const FString SanitizedBaseName = MakeDreamShaderDeclarationName(BaseName, TEXT("Export"), 0);
			FString Candidate = FPaths::Combine(Directory, SanitizedBaseName + Extension);
			if (!IFileManager::Get().FileExists(*Candidate))
			{
				return UE::DreamShader::NormalizeSourceFilePath(Candidate);
			}

			for (int32 Index = 1; Index < 10000; ++Index)
			{
				Candidate = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%d%s"), *SanitizedBaseName, Index, Extension));
				if (!IFileManager::Get().FileExists(*Candidate))
				{
					return UE::DreamShader::NormalizeSourceFilePath(Candidate);
				}
			}

			const uint32 PathHash = FCrc::StrCrc32(*Candidate);
			return UE::DreamShader::NormalizeSourceFilePath(FPaths::Combine(
				Directory,
				FString::Printf(TEXT("%s_%08x%s"), *SanitizedBaseName, PathHash, Extension)));
		}

		FString MakeDecompiledMaterialFilePath(const UMaterial* Material)
		{
			return MakeUniqueDecompiledSourcePath(
				FPaths::Combine(UE::DreamShader::GetSourceShaderDirectory(), TEXT("Decompiled/Materials")),
				Material ? Material->GetName() : FString(),
				TEXT(".dsm"));
		}

		FString MakeDecompiledFunctionFilePath(const UMaterialFunction* MaterialFunction)
		{
			return MakeUniqueDecompiledSourcePath(
				FPaths::Combine(UE::DreamShader::GetSourceShaderDirectory(), TEXT("Decompiled/Functions")),
				MaterialFunction ? MaterialFunction->GetName() : FString(),
				TEXT(".dsf"));
		}

		FString MakeDecompiledAssetName(const UObject* Asset, const TCHAR* Category)
		{
			return FString::Printf(
				TEXT("Decompiled/%s/%s"),
				Category,
				*MakeDreamShaderDeclarationName(Asset ? Asset->GetName() : FString(), TEXT("Asset"), 0));
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

					OutputDeclarations.Add(FString::Printf(TEXT("\t\t%s %s;"), Binding.Type, Binding.Name));
					OutputBindings.Add(FString::Printf(TEXT("\t\t%s = %s;"), Binding.Target, Binding.Name));
					OutputAssignments.Add(FString::Printf(
						TEXT("\t\t%s = %s;"),
						Binding.Name,
						*CompileInput(*MaterialInput, Binding.DefaultValue)));
				}

				TArray<FString> Lines;
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
						OutputAssignments.Add(FString::Printf(
							TEXT("\t\t%s = %s;"),
							*DeclarationName,
							*CompileInput(OutputExpression->A, TEXT("0.0"))));
					}
				}

				TArray<FString> Lines;
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
				FunctionInputNames.Reset();
				GraphLines.Reset();
				ExpressionTemps.Reset();
				TempNames.Reset();
				VirtualFunctionDefinitions.Reset();
				VirtualFunctionNames.Reset();
				Warnings.Reset();
				NextTempIndex = 0;
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
				PropertyDeclarations.Add(TEXT("\t\t") + Declaration);
			}

			FString AddTemp(const FString& Type, const FString& ExpressionText, const FString& BaseName)
			{
				const FString Name = MakeUniqueName(BaseName, TEXT("Node"));
				GraphLines.Add(FString::Printf(TEXT("\t\t%s %s = %s;"), *Type, *Name, *ExpressionText));
				++NextTempIndex;
				return Name;
			}

			FString CompileInput(const FExpressionInput& Input, const FString& DefaultText)
			{
				const FExpressionInput TracedInput = Input.GetTracedInput();
				if (!TracedInput.Expression)
				{
					return DefaultText;
				}

				return ApplyInputMask(CompileExpression(TracedInput.Expression, TracedInput.OutputIndex), Input);
			}

			FString CompileConnectedOrLiteral(const FExpressionInput& Input, const FString& LiteralText)
			{
				return Input.IsConnected() ? CompileInput(Input, LiteralText) : LiteralText;
			}

			FString CompileExpression(UMaterialExpression* Expression, const int32 OutputIndex)
			{
				if (!Expression)
				{
					return TEXT("0.0");
				}

				const FDecompiledExpressionKey Key{ Expression, OutputIndex };
				if (const FString* ExistingTemp = ExpressionTemps.Find(Key))
				{
					return *ExistingTemp;
				}

				if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
				{
					if (const FString* InputName = FunctionInputNames.Find(FunctionInput))
					{
						return *InputName;
					}
					return MakeDreamShaderDeclarationName(FunctionInput->InputName.ToString(), TEXT("Input"), 0);
				}

				if (UMaterialExpressionScalarParameter* ScalarParameter = Cast<UMaterialExpressionScalarParameter>(Expression))
				{
					const FString Name = MakeDreamShaderDeclarationName(ScalarParameter->ParameterName.ToString(), TEXT("Scalar"), 0);
					AddPropertyDeclaration(
						Name,
						FString::Printf(TEXT("ScalarParameter %s = %s;"), *Name, *FormatDreamShaderFloat(ScalarParameter->DefaultValue)));
					return Name;
				}

				if (UMaterialExpressionVectorParameter* VectorParameter = Cast<UMaterialExpressionVectorParameter>(Expression))
				{
					const FString Name = MakeDreamShaderDeclarationName(VectorParameter->ParameterName.ToString(), TEXT("Vector"), 0);
					AddPropertyDeclaration(
						Name,
						FString::Printf(TEXT("VectorParameter %s = %s;"), *Name, *FormatDreamShaderColor(VectorParameter->DefaultValue)));
					return Name;
				}

				if (UMaterialExpressionTextureObjectParameter* TextureObjectParameter = Cast<UMaterialExpressionTextureObjectParameter>(Expression))
				{
					const FString Name = MakeDreamShaderDeclarationName(TextureObjectParameter->ParameterName.ToString(), TEXT("Texture"), 0);
					const FString DefaultValue = TextureObjectParameter->Texture
						? FString::Printf(TEXT(" = %s"), *MakeDreamShaderObjectPathLiteral(TextureObjectParameter->Texture))
						: FString();
					AddPropertyDeclaration(
						Name,
						FString::Printf(TEXT("TextureObjectParameter %s%s;"), *Name, *DefaultValue));
					return Name;
				}

				if (UMaterialExpressionTextureSampleParameter2D* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter2D>(Expression))
				{
					const FString Name = MakeDreamShaderDeclarationName(TextureParameter->ParameterName.ToString(), TEXT("Texture"), 0);
					const FString DefaultValue = TextureParameter->Texture
						? FString::Printf(TEXT(" = %s"), *MakeDreamShaderObjectPathLiteral(TextureParameter->Texture))
						: FString();
					AddPropertyDeclaration(
						Name,
						FString::Printf(TEXT("TextureSampleParameter2D %s%s;"), *Name, *DefaultValue));
					return Name;
				}

				if (UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expression))
				{
					return FormatDreamShaderFloat(Constant->R);
				}

				if (UMaterialExpressionConstant2Vector* Constant2 = Cast<UMaterialExpressionConstant2Vector>(Expression))
				{
					return FormatDreamShaderVector2(Constant2->R, Constant2->G);
				}

				if (UMaterialExpressionConstant3Vector* Constant3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
				{
					return FormatDreamShaderVector3(Constant3->Constant.R, Constant3->Constant.G, Constant3->Constant.B);
				}

				if (UMaterialExpressionConstant4Vector* Constant4 = Cast<UMaterialExpressionConstant4Vector>(Expression))
				{
					return FormatDreamShaderColor(Constant4->Constant);
				}

				if (UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expression))
				{
					return FString::Printf(
						TEXT("(%s + %s)"),
						*CompileConnectedOrLiteral(Add->A, FormatDreamShaderFloat(Add->ConstA)),
						*CompileConnectedOrLiteral(Add->B, FormatDreamShaderFloat(Add->ConstB)));
				}

				if (UMaterialExpressionSubtract* Subtract = Cast<UMaterialExpressionSubtract>(Expression))
				{
					return FString::Printf(
						TEXT("(%s - %s)"),
						*CompileConnectedOrLiteral(Subtract->A, FormatDreamShaderFloat(Subtract->ConstA)),
						*CompileConnectedOrLiteral(Subtract->B, FormatDreamShaderFloat(Subtract->ConstB)));
				}

				if (UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
				{
					return FString::Printf(
						TEXT("(%s * %s)"),
						*CompileConnectedOrLiteral(Multiply->A, FormatDreamShaderFloat(Multiply->ConstA)),
						*CompileConnectedOrLiteral(Multiply->B, FormatDreamShaderFloat(Multiply->ConstB)));
				}

				if (UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(Expression))
				{
					return FString::Printf(
						TEXT("(%s / %s)"),
						*CompileConnectedOrLiteral(Divide->A, FormatDreamShaderFloat(Divide->ConstA)),
						*CompileConnectedOrLiteral(Divide->B, FormatDreamShaderFloat(Divide->ConstB)));
				}

				if (UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Expression))
				{
					return FString::Printf(
						TEXT("lerp(%s, %s, %s)"),
						*CompileConnectedOrLiteral(Lerp->A, FormatDreamShaderFloat(Lerp->ConstA)),
						*CompileConnectedOrLiteral(Lerp->B, FormatDreamShaderFloat(Lerp->ConstB)),
						*CompileConnectedOrLiteral(Lerp->Alpha, FormatDreamShaderFloat(Lerp->ConstAlpha)));
				}

				if (UMaterialExpressionClamp* Clamp = Cast<UMaterialExpressionClamp>(Expression))
				{
					if (Clamp->ClampMode != CMODE_Clamp)
					{
						return BuildUEExpressionCall(Expression, OutputIndex, {
							{ TEXT("Input"), CompileInput(Clamp->Input, TEXT("0.0")), true },
							{ TEXT("Min"), CompileConnectedOrLiteral(Clamp->Min, FormatDreamShaderFloat(Clamp->MinDefault)), true },
							{ TEXT("Max"), CompileConnectedOrLiteral(Clamp->Max, FormatDreamShaderFloat(Clamp->MaxDefault)), true },
							{ TEXT("ClampMode"), Clamp->ClampMode == CMODE_ClampMin ? TEXT("\"CMODE_ClampMin\"") : TEXT("\"CMODE_ClampMax\""), false },
						});
					}
					return FString::Printf(
						TEXT("clamp(%s, %s, %s)"),
						*CompileInput(Clamp->Input, TEXT("0.0")),
						*CompileConnectedOrLiteral(Clamp->Min, FormatDreamShaderFloat(Clamp->MinDefault)),
						*CompileConnectedOrLiteral(Clamp->Max, FormatDreamShaderFloat(Clamp->MaxDefault)));
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
					const FString Source = CompileInput(Mask->Input, TEXT("0.0"));
					return MakeSwizzleExpression(Source, Suffix);
				}

				if (UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(Expression))
				{
					return FString::Printf(
						TEXT("append(%s, %s)"),
						*CompileInput(Append->A, TEXT("0.0")),
						*CompileInput(Append->B, TEXT("0.0")));
				}

				if (UMaterialExpressionOneMinus* OneMinus = Cast<UMaterialExpressionOneMinus>(Expression))
				{
					return FString::Printf(TEXT("(1.0 - %s)"), *CompileInput(OneMinus->Input, TEXT("0.0")));
				}

				if (UMaterialExpressionPower* Power = Cast<UMaterialExpressionPower>(Expression))
				{
					return FString::Printf(
						TEXT("pow(%s, %s)"),
						*CompileInput(Power->Base, TEXT("0.0")),
						*CompileConnectedOrLiteral(Power->Exponent, FormatDreamShaderFloat(Power->ConstExponent)));
				}

				if (UMaterialExpressionDotProduct* Dot = Cast<UMaterialExpressionDotProduct>(Expression))
				{
					return FString::Printf(
						TEXT("dot(%s, %s)"),
						*CompileInput(Dot->A, TEXT("0.0")),
						*CompileInput(Dot->B, TEXT("0.0")));
				}

				if (UMaterialExpressionNormalize* Normalize = Cast<UMaterialExpressionNormalize>(Expression))
				{
					return FString::Printf(TEXT("normalize(%s)"), *CompileInput(Normalize->VectorInput, TEXT("0.0")));
				}

				if (UMaterialExpressionMin* Min = Cast<UMaterialExpressionMin>(Expression))
				{
					return FString::Printf(
						TEXT("min(%s, %s)"),
						*CompileConnectedOrLiteral(Min->A, FormatDreamShaderFloat(Min->ConstA)),
						*CompileConnectedOrLiteral(Min->B, FormatDreamShaderFloat(Min->ConstB)));
				}

				if (UMaterialExpressionMax* Max = Cast<UMaterialExpressionMax>(Expression))
				{
					return FString::Printf(
						TEXT("max(%s, %s)"),
						*CompileConnectedOrLiteral(Max->A, FormatDreamShaderFloat(Max->ConstA)),
						*CompileConnectedOrLiteral(Max->B, FormatDreamShaderFloat(Max->ConstB)));
				}

				if (UMaterialExpressionAbs* Abs = Cast<UMaterialExpressionAbs>(Expression))
				{
					return FString::Printf(TEXT("abs(%s)"), *CompileInput(Abs->Input, TEXT("0.0")));
				}

				if (UMaterialExpressionFloor* Floor = Cast<UMaterialExpressionFloor>(Expression))
				{
					return FString::Printf(TEXT("floor(%s)"), *CompileInput(Floor->Input, TEXT("0.0")));
				}

				if (UMaterialExpressionCeil* Ceil = Cast<UMaterialExpressionCeil>(Expression))
				{
					return FString::Printf(TEXT("ceil(%s)"), *CompileInput(Ceil->Input, TEXT("0.0")));
				}

				if (UMaterialExpressionFrac* Frac = Cast<UMaterialExpressionFrac>(Expression))
				{
					return FString::Printf(TEXT("frac(%s)"), *CompileInput(Frac->Input, TEXT("0.0")));
				}

				if (UMaterialExpressionSquareRoot* SquareRoot = Cast<UMaterialExpressionSquareRoot>(Expression))
				{
					return FString::Printf(TEXT("sqrt(%s)"), *CompileInput(SquareRoot->Input, TEXT("0.0")));
				}

				if (UMaterialExpressionSine* Sine = Cast<UMaterialExpressionSine>(Expression))
				{
					if (!FMath::IsNearlyEqual(Sine->Period, 1.0f))
					{
						return BuildUEExpressionCall(Expression, OutputIndex, {
							{ TEXT("Input"), CompileInput(Sine->Input, TEXT("0.0")), true },
							{ TEXT("Period"), FormatDreamShaderFloat(Sine->Period), false },
						});
					}
					return FString::Printf(TEXT("sin(%s)"), *CompileInput(Sine->Input, TEXT("0.0")));
				}

				if (UMaterialExpressionCosine* Cosine = Cast<UMaterialExpressionCosine>(Expression))
				{
					if (!FMath::IsNearlyEqual(Cosine->Period, 1.0f))
					{
						return BuildUEExpressionCall(Expression, OutputIndex, {
							{ TEXT("Input"), CompileInput(Cosine->Input, TEXT("0.0")), true },
							{ TEXT("Period"), FormatDreamShaderFloat(Cosine->Period), false },
						});
					}
					return FString::Printf(TEXT("cos(%s)"), *CompileInput(Cosine->Input, TEXT("0.0")));
				}

				if (UMaterialExpressionTextureCoordinate* TextureCoordinate = Cast<UMaterialExpressionTextureCoordinate>(Expression))
				{
					return BuildUEExpressionCall(Expression, OutputIndex, {
						{ TEXT("CoordinateIndex"), FString::Printf(TEXT("%d"), TextureCoordinate->CoordinateIndex), false },
						{ TEXT("UTiling"), FormatDreamShaderFloat(TextureCoordinate->UTiling), false },
						{ TEXT("VTiling"), FormatDreamShaderFloat(TextureCoordinate->VTiling), false },
					});
				}

				if (UMaterialExpressionTime* Time = Cast<UMaterialExpressionTime>(Expression))
				{
					if (!Time->bIgnorePause && !Time->bOverride_Period)
					{
						return TEXT("UE.Time()");
					}

					return BuildUEExpressionCall(Expression, OutputIndex, {
						{ TEXT("bIgnorePause"), Time->bIgnorePause ? TEXT("true") : TEXT("false"), false },
						{ TEXT("bOverride_Period"), Time->bOverride_Period ? TEXT("true") : TEXT("false"), false },
						{ TEXT("Period"), FormatDreamShaderFloat(Time->Period), false },
					});
				}

				if (UMaterialExpressionPanner* Panner = Cast<UMaterialExpressionPanner>(Expression))
				{
					return BuildUEExpressionCall(Expression, OutputIndex, {
						{ TEXT("Coordinate"), CompileInput(Panner->Coordinate, TEXT("0.0")), true },
						{ TEXT("Time"), CompileInput(Panner->Time, TEXT("UE.Time()")), true },
						{ TEXT("Speed"), CompileInput(Panner->Speed, FormatDreamShaderVector2(Panner->SpeedX, Panner->SpeedY)), true },
						{ TEXT("SpeedX"), FormatDreamShaderFloat(Panner->SpeedX), false },
						{ TEXT("SpeedY"), FormatDreamShaderFloat(Panner->SpeedY), false },
						{ TEXT("ConstCoordinate"), FString::Printf(TEXT("%d"), Panner->ConstCoordinate), false },
						{ TEXT("bFractionalPart"), Panner->bFractionalPart ? TEXT("true") : TEXT("false"), false },
					});
				}

				if (UMaterialExpressionWorldPosition* WorldPosition = Cast<UMaterialExpressionWorldPosition>(Expression))
				{
					return BuildUEExpressionCall(Expression, OutputIndex, {});
				}

				if (UMaterialExpressionCameraVectorWS* CameraVector = Cast<UMaterialExpressionCameraVectorWS>(Expression))
				{
					return BuildUEExpressionCall(Expression, OutputIndex, {});
				}

				if (UMaterialExpressionObjectPositionWS* ObjectPosition = Cast<UMaterialExpressionObjectPositionWS>(Expression))
				{
					return BuildUEExpressionCall(Expression, OutputIndex, {});
				}

				if (UMaterialExpressionScreenPosition* ScreenPosition = Cast<UMaterialExpressionScreenPosition>(Expression))
				{
					return BuildUEExpressionCall(Expression, OutputIndex, {});
				}

				if (UMaterialExpressionVertexColor* VertexColor = Cast<UMaterialExpressionVertexColor>(Expression))
				{
					return MakeExpressionOutputSelection(BuildUEExpressionCall(Expression, 0, {}), Expression, OutputIndex);
				}

				if (UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression))
				{
					TArray<FExpressionCallArgument> Arguments;
					if (TextureSample->Coordinates.IsConnected())
					{
						Arguments.Add({ TEXT("Coordinates"), CompileInput(TextureSample->Coordinates, TEXT("0.0")), true });
					}
					else
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
					return BuildUEExpressionCall(Expression, OutputIndex, Arguments);
				}

				if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
				{
					const FString FunctionCallText = BuildMaterialFunctionCall(FunctionCall, OutputIndex);
					const FString TempName = AddTemp(
						GetDreamShaderTypeForExpressionOutput(Expression, OutputIndex),
						FunctionCallText,
						FunctionCall->MaterialFunction ? FunctionCall->MaterialFunction->GetName() : TEXT("Function"));
					ExpressionTemps.Add(Key, TempName);
					return TempName;
				}

				if (UMaterialExpressionCustom* CustomExpression = Cast<UMaterialExpressionCustom>(Expression))
				{
					const FString TempName = AddTemp(
						GetDreamShaderTypeForExpressionOutput(Expression, OutputIndex),
						BuildCustomExpressionCall(CustomExpression, OutputIndex),
						CustomExpression->Description.IsEmpty() ? TEXT("Custom") : CustomExpression->Description);
					ExpressionTemps.Add(Key, TempName);
					return TempName;
				}

				const FString TempName = AddTemp(
					GetDreamShaderTypeForExpressionOutput(Expression, OutputIndex),
					BuildGenericExpressionCall(Expression, OutputIndex),
					GetMaterialExpressionShortName(Expression->GetClass()));
				ExpressionTemps.Add(Key, TempName);
				Warnings.AddUnique(FString::Printf(
					TEXT("Exported '%s' as UE.Expression; review reflected literal properties if the node has editor-only state."),
					*Expression->GetClass()->GetName()));
				return TempName;
			}

			FString MakeExpressionOutputSelection(const FString& ExpressionText, UMaterialExpression* Expression, const int32 OutputIndex) const
			{
				if (!Expression || OutputIndex == 0 || !Expression->Outputs.IsValidIndex(OutputIndex))
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

				return OutputName.IsEmpty()
					? ExpressionText
					: MakeSwizzleExpression(ExpressionText, OutputName.ToLower());
			}

			FString BuildUEExpressionCall(UMaterialExpression* Expression, const int32 OutputIndex, const TArray<FExpressionCallArgument>& Arguments) const
			{
				TArray<FString> ArgumentTexts;
				ArgumentTexts.Add(FString::Printf(TEXT("Class=\"%s\""), *GetMaterialExpressionShortName(Expression ? Expression->GetClass() : nullptr)));
				ArgumentTexts.Add(FString::Printf(TEXT("OutputType=\"%s\""), *GetDreamShaderTypeForExpressionOutput(Expression, OutputIndex)));
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

				return FString::Printf(TEXT("UE.Expression(%s)"), *FString::Join(ArgumentTexts, TEXT(", ")));
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
				}

				return BuildUEExpressionCall(Expression, OutputIndex, Arguments);
			}

			FString BuildCustomExpressionCall(UMaterialExpressionCustom* CustomExpression, const int32 OutputIndex)
			{
				TArray<FExpressionCallArgument> Arguments;
				Arguments.Add({ TEXT("Code"), FString::Printf(TEXT("\"%s\""), *EscapeDreamShaderString(CustomExpression ? CustomExpression->Code : FString())), false });
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
			TMap<const UMaterialExpressionFunctionInput*, FString> FunctionInputNames;
			TArray<FString> GraphLines;
			TMap<FDecompiledExpressionKey, FString> ExpressionTemps;
			TSet<FString> TempNames;
			TArray<FString> VirtualFunctionDefinitions;
			TMap<const UMaterialFunction*, FString> VirtualFunctionNames;
			TArray<FString> Warnings;
			int32 NextTempIndex = 0;
		};

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

		FDreamShaderEditorBridge::FDiagnosticRecord MakeVirtualFunctionDiagnostic(
			const FString& SourceFilePath,
			const FString& Message,
			const FString& Detail,
			const FString& AssetPath,
			int32 Line,
			int32 Column)
		{
			FDreamShaderEditorBridge::FDiagnosticRecord Diagnostic;
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

		void FindProjectDreamShaderSourceFiles(TArray<FString>& OutSourceFiles)
		{
			TArray<FString> MaterialFiles;
			TArray<FString> HeaderFiles;
			TArray<FString> FunctionFiles;
			IFileManager::Get().FindFilesRecursive(
				MaterialFiles,
				*UE::DreamShader::GetSourceShaderDirectory(),
				TEXT("*.dsm"),
				true,
				false,
				false);
			IFileManager::Get().FindFilesRecursive(
				HeaderFiles,
				*UE::DreamShader::GetSourceShaderDirectory(),
				TEXT("*.dsh"),
				true,
				false,
				false);
			IFileManager::Get().FindFilesRecursive(
				FunctionFiles,
				*UE::DreamShader::GetSourceShaderDirectory(),
				TEXT("*.dsf"),
				true,
				false,
				false);

			OutSourceFiles.Reset();
			OutSourceFiles.Append(MaterialFiles);
			OutSourceFiles.Append(HeaderFiles);
			OutSourceFiles.Append(FunctionFiles);

			for (FString& SourceFile : OutSourceFiles)
			{
				SourceFile = UE::DreamShader::NormalizeSourceFilePath(SourceFile);
			}

			OutSourceFiles.RemoveAll([](const FString& SourceFile)
			{
				return IsPathUnderDirectory(SourceFile, UE::DreamShader::GetPackageShaderDirectory());
			});
			OutSourceFiles.Sort();
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
			TArray<FDreamShaderEditorBridge::FDiagnosticRecord>* OutDiagnostics = nullptr)
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
			FindProjectDreamShaderSourceFiles(SourceFiles);
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

		FString QuoteProcessArgument(const FString& Argument)
		{
			FString Escaped = Argument;
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return FString::Printf(TEXT("\"%s\""), *Escaped);
		}

		FString GetMaterialExpressionManifestFilePath()
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamShader/Bridge/material-expressions.json"));
		}

		FString GetDreamShaderSettingsManifestFilePath()
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamShader/Bridge/settings.json"));
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

		FString GetReflectedPropertyTypeName(const FProperty* Property)
		{
			if (!Property)
			{
				return TEXT("unknown");
			}

			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				if (StructProperty->Struct && StructProperty->Struct->GetFName() == NAME_ExpressionInput)
				{
					return TEXT("input");
				}
				return StructProperty->Struct ? StructProperty->Struct->GetName() : TEXT("struct");
			}
			if (CastField<FBoolProperty>(Property))
			{
				return TEXT("bool");
			}
			if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
			{
				if (NumericProperty->IsFloatingPoint())
				{
					return TEXT("float");
				}
				if (NumericProperty->IsInteger())
				{
					return TEXT("int");
				}
				return TEXT("number");
			}
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				return EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetName() : TEXT("enum");
			}
			if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
			{
				return ByteProperty->Enum ? ByteProperty->Enum->GetName() : TEXT("byte");
			}
			if (CastField<FNameProperty>(Property))
			{
				return TEXT("name");
			}
			if (CastField<FStrProperty>(Property) || CastField<FTextProperty>(Property))
			{
				return TEXT("string");
			}
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				return ObjectProperty->PropertyClass ? ObjectProperty->PropertyClass->GetName() : TEXT("object");
			}
			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				return FString::Printf(TEXT("array<%s>"), *GetReflectedPropertyTypeName(ArrayProperty->Inner));
			}

			return Property->GetCPPType();
		}

		bool IsExportedMaterialExpressionProperty(const FProperty* Property)
		{
			if (!Property || Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient | CPF_DuplicateTransient))
			{
				return false;
			}

			return UE::DreamShader::Editor::Private::IsMaterialExpressionInputProperty(Property)
				|| Property->HasAnyPropertyFlags(CPF_Edit);
		}

		int32 GetExpressionOutputComponentCount(const FExpressionOutput& Output)
		{
			const int32 MaskCount =
				(Output.MaskR ? 1 : 0)
				+ (Output.MaskG ? 1 : 0)
				+ (Output.MaskB ? 1 : 0)
				+ (Output.MaskA ? 1 : 0);
			return MaskCount > 0 ? MaskCount : 1;
		}

		FString GetOutputTypeNameFromComponentCount(const int32 ComponentCount)
		{
			if (ComponentCount <= 1)
			{
				return TEXT("float1");
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

		template<typename EnumType>
		TArray<TSharedPtr<FJsonValue>> BuildSettingsMappingValues(
			const TMap<FString, TEnumAsByte<EnumType>>& Mappings,
			const UEnum* Enum)
		{
			TArray<FString> Aliases;
			Mappings.GetKeys(Aliases);
			Aliases.Sort([](const FString& Left, const FString& Right)
			{
				return Left < Right;
			});

			TArray<TSharedPtr<FJsonValue>> MappingValues;
			for (const FString& Alias : Aliases)
			{
				const TEnumAsByte<EnumType>* Value = Mappings.Find(Alias);
				if (!Value)
				{
					continue;
				}

				const int64 EnumValue = static_cast<int64>(Value->GetValue());
				TSharedRef<FJsonObject> MappingObject = MakeShared<FJsonObject>();
				MappingObject->SetStringField(TEXT("alias"), Alias);
				MappingObject->SetNumberField(TEXT("value"), static_cast<double>(EnumValue));
				MappingObject->SetStringField(
					TEXT("name"),
					Enum ? Enum->GetNameStringByValue(EnumValue) : FString::FromInt(EnumValue));
				MappingObject->SetStringField(
					TEXT("displayName"),
					Enum ? Enum->GetDisplayNameTextByValue(EnumValue).ToString() : FString());
				MappingValues.Add(MakeShared<FJsonValueObject>(MappingObject));
			}

			return MappingValues;
		}

		void ExportDreamShaderSettingsManifest()
		{
			const FString ManifestPath = GetDreamShaderSettingsManifestFilePath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ManifestPath), true);

			const UDreamShaderSettings* Settings = GetDefault<UDreamShaderSettings>();
			if (!Settings)
			{
				UE_LOG(LogDreamShader, Warning, TEXT("Failed to read DreamShader settings for Bridge manifest."));
				return;
			}

			TSharedRef<FJsonObject> MappingsObject = MakeShared<FJsonObject>();
			MappingsObject->SetArrayField(
				TEXT("ShadingModel"),
				BuildSettingsMappingValues(Settings->ShadingModelMappings, StaticEnum<EMaterialShadingModel>()));
			MappingsObject->SetArrayField(
				TEXT("BlendMode"),
				BuildSettingsMappingValues(Settings->BlendModeMappings, StaticEnum<EBlendMode>()));
			MappingsObject->SetArrayField(
				TEXT("MaterialDomain"),
				BuildSettingsMappingValues(Settings->MaterialDomainMappings, StaticEnum<EMaterialDomain>()));

			TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
			RootObject->SetStringField(TEXT("schema"), TEXT("DreamShader.Settings"));
			RootObject->SetNumberField(TEXT("version"), 1);
			RootObject->SetStringField(TEXT("generatedAt"), FDateTime::UtcNow().ToIso8601());
			RootObject->SetObjectField(TEXT("mappings"), MappingsObject);

			FString ManifestText;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ManifestText);
			FJsonSerializer::Serialize(RootObject, Writer);

			if (FFileHelper::SaveStringToFile(ManifestText, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogDreamShader, Display, TEXT("Wrote DreamShader settings manifest: %s"), *ManifestPath);
			}
			else
			{
				UE_LOG(LogDreamShader, Warning, TEXT("Failed to write DreamShader settings manifest: %s"), *ManifestPath);
			}
		}

		void ExportMaterialExpressionManifest()
		{
			const FString ManifestPath = GetMaterialExpressionManifestFilePath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ManifestPath), true);

			TArray<TSharedPtr<FJsonValue>> ExpressionValues;

			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (!Class
					|| !Class->IsChildOf(UMaterialExpression::StaticClass())
					|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				{
					continue;
				}

				const FString ShortName = GetMaterialExpressionShortName(Class);
				if (ShortName.IsEmpty())
				{
					continue;
				}

				TSharedRef<FJsonObject> ExpressionObject = MakeShared<FJsonObject>();
				ExpressionObject->SetStringField(TEXT("name"), ShortName);
				ExpressionObject->SetStringField(TEXT("className"), Class->GetName());
				ExpressionObject->SetStringField(TEXT("pathName"), Class->GetPathName());

				TArray<TSharedPtr<FJsonValue>> PropertyValues;
				TArray<TSharedPtr<FJsonValue>> InputValues;
				for (TFieldIterator<FProperty> PropertyIt(Class, EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
				{
					FProperty* Property = *PropertyIt;
					if (!IsExportedMaterialExpressionProperty(Property))
					{
						continue;
					}

					const bool bIsInput = UE::DreamShader::Editor::Private::IsMaterialExpressionInputProperty(Property);
					TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
					PropertyObject->SetStringField(TEXT("name"), Property->GetName());
					PropertyObject->SetStringField(TEXT("type"), GetReflectedPropertyTypeName(Property));
					PropertyObject->SetBoolField(TEXT("isInput"), bIsInput);

					PropertyValues.Add(MakeShared<FJsonValueObject>(PropertyObject));
					if (bIsInput)
					{
						InputValues.Add(MakeShared<FJsonValueObject>(PropertyObject));
					}
				}
				ExpressionObject->SetArrayField(TEXT("properties"), PropertyValues);
				ExpressionObject->SetArrayField(TEXT("inputs"), InputValues);

				TArray<TSharedPtr<FJsonValue>> OutputValues;
				if (const UMaterialExpression* DefaultExpression = Cast<UMaterialExpression>(Class->GetDefaultObject(false)))
				{
					for (int32 OutputIndex = 0; OutputIndex < DefaultExpression->Outputs.Num(); ++OutputIndex)
					{
						const FExpressionOutput& Output = DefaultExpression->Outputs[OutputIndex];
						const int32 ComponentCount = GetExpressionOutputComponentCount(Output);

						TSharedRef<FJsonObject> OutputObject = MakeShared<FJsonObject>();
						OutputObject->SetNumberField(TEXT("index"), OutputIndex);
						OutputObject->SetStringField(TEXT("name"), Output.OutputName.ToString());
						OutputObject->SetNumberField(TEXT("componentCount"), ComponentCount);
						OutputObject->SetStringField(TEXT("outputType"), GetOutputTypeNameFromComponentCount(ComponentCount));
						OutputValues.Add(MakeShared<FJsonValueObject>(OutputObject));
					}
				}
				ExpressionObject->SetArrayField(TEXT("outputs"), OutputValues);
				ExpressionObject->SetStringField(
					TEXT("defaultOutputType"),
					OutputValues.IsEmpty()
						? TEXT("float1")
						: OutputValues[0]->AsObject()->GetStringField(TEXT("outputType")));

				ExpressionValues.Add(MakeShared<FJsonValueObject>(ExpressionObject));
			}

			ExpressionValues.Sort([](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
			{
				const TSharedPtr<FJsonObject> LeftObject = Left.IsValid() ? Left->AsObject() : nullptr;
				const TSharedPtr<FJsonObject> RightObject = Right.IsValid() ? Right->AsObject() : nullptr;
				return LeftObject.IsValid()
					&& RightObject.IsValid()
					&& LeftObject->GetStringField(TEXT("name")) < RightObject->GetStringField(TEXT("name"));
			});

			TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
			RootObject->SetStringField(TEXT("schema"), TEXT("DreamShader.MaterialExpressions"));
			RootObject->SetNumberField(TEXT("version"), 1);
			RootObject->SetStringField(TEXT("generatedAt"), FDateTime::UtcNow().ToIso8601());
			RootObject->SetArrayField(TEXT("expressions"), ExpressionValues);

			FString ManifestText;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ManifestText);
			FJsonSerializer::Serialize(RootObject, Writer);

			if (FFileHelper::SaveStringToFile(ManifestText, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				UE_LOG(LogDreamShader, Display, TEXT("Wrote DreamShader MaterialExpression manifest: %s"), *ManifestPath);
			}
			else
			{
				UE_LOG(LogDreamShader, Warning, TEXT("Failed to write DreamShader MaterialExpression manifest: %s"), *ManifestPath);
			}
		}

		bool WriteDreamShaderWorkspaceFile(FString& OutWorkspaceFilePath, FString& OutError)
		{
			const FString SourceDirectory = UE::DreamShader::NormalizeSourceFilePath(UE::DreamShader::GetSourceShaderDirectory());
			if (SourceDirectory.IsEmpty())
			{
				OutError = TEXT("DreamShader source directory is empty.");
				return false;
			}

			if (!IFileManager::Get().MakeDirectory(*SourceDirectory, true))
			{
				OutError = FString::Printf(TEXT("Failed to create DreamShader source directory '%s'."), *SourceDirectory);
				return false;
			}

			const FString WorkspaceFilePath = UE::DreamShader::NormalizeSourceFilePath(
				FPaths::Combine(SourceDirectory, TEXT("DreamShader.code-workspace")));

			FString WorkspaceText;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&WorkspaceText);
			Writer->WriteObjectStart();
			Writer->WriteArrayStart(TEXT("folders"));

			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("name"), TEXT("DreamShader Source"));
			Writer->WriteValue(TEXT("path"), TEXT("."));
			Writer->WriteObjectEnd();

			Writer->WriteArrayEnd();
			Writer->WriteObjectStart(TEXT("settings"));
			Writer->WriteObjectStart(TEXT("files.associations"));
			Writer->WriteValue(TEXT("*.dsm"), TEXT("dreamshaderlang"));
			Writer->WriteValue(TEXT("*.dsh"), TEXT("dreamshaderlang"));
			Writer->WriteValue(TEXT("*.dsf"), TEXT("dreamshaderlang"));
			Writer->WriteObjectEnd();
			Writer->WriteObjectEnd();
			Writer->WriteObjectEnd();
			Writer->Close();

			if (!FFileHelper::SaveStringToFile(WorkspaceText, *WorkspaceFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = FString::Printf(TEXT("Failed to write DreamShader workspace file '%s'."), *WorkspaceFilePath);
				return false;
			}

			OutWorkspaceFilePath = WorkspaceFilePath;
			return true;
		}

		void AddExistingFileCandidate(TArray<FString>& OutCandidates, const FString& Candidate)
		{
			if (!Candidate.IsEmpty() && FPaths::FileExists(Candidate))
			{
				OutCandidates.AddUnique(UE::DreamShader::NormalizeSourceFilePath(Candidate));
			}
		}

		TArray<FString> FindVSCodeExecutableCandidates()
		{
			TArray<FString> Candidates;

			auto AddFromEnvironmentDirectory = [&Candidates](const TCHAR* VariableName, const TCHAR* RelativePath)
			{
				const FString Directory = FPlatformMisc::GetEnvironmentVariable(VariableName);
				if (!Directory.IsEmpty())
				{
					AddExistingFileCandidate(Candidates, FPaths::Combine(Directory, RelativePath));
				}
			};

			AddFromEnvironmentDirectory(TEXT("LOCALAPPDATA"), TEXT("Programs/Microsoft VS Code/Code.exe"));
			AddFromEnvironmentDirectory(TEXT("LOCALAPPDATA"), TEXT("Programs/Microsoft VS Code/bin/code.cmd"));
			AddFromEnvironmentDirectory(TEXT("LOCALAPPDATA"), TEXT("Programs/Microsoft VS Code Insiders/Code - Insiders.exe"));
			AddFromEnvironmentDirectory(TEXT("LOCALAPPDATA"), TEXT("Programs/Microsoft VS Code Insiders/bin/code-insiders.cmd"));
			AddFromEnvironmentDirectory(TEXT("ProgramFiles"), TEXT("Microsoft VS Code/Code.exe"));
			AddFromEnvironmentDirectory(TEXT("ProgramFiles"), TEXT("Microsoft VS Code/bin/code.cmd"));
			AddFromEnvironmentDirectory(TEXT("ProgramFiles(x86)"), TEXT("Microsoft VS Code/Code.exe"));
			AddFromEnvironmentDirectory(TEXT("ProgramFiles(x86)"), TEXT("Microsoft VS Code/bin/code.cmd"));

			const FString PathEnvironment = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
			TArray<FString> PathEntries;
			PathEnvironment.ParseIntoArray(PathEntries, TEXT(";"), true);
			for (FString PathEntry : PathEntries)
			{
				PathEntry.TrimStartAndEndInline();
				if (PathEntry.IsEmpty())
				{
					continue;
				}

				AddExistingFileCandidate(Candidates, FPaths::Combine(PathEntry, TEXT("code.cmd")));
				AddExistingFileCandidate(Candidates, FPaths::Combine(PathEntry, TEXT("code.exe")));
				AddExistingFileCandidate(Candidates, FPaths::Combine(PathEntry, TEXT("Code.exe")));
				AddExistingFileCandidate(Candidates, FPaths::Combine(PathEntry, TEXT("code-insiders.cmd")));
				AddExistingFileCandidate(Candidates, FPaths::Combine(PathEntry, TEXT("Code - Insiders.exe")));
			}

			return Candidates;
		}

		bool LaunchVSCodeWorkspace(const FString& WorkspaceFilePath)
		{
			for (const FString& Candidate : FindVSCodeExecutableCandidates())
			{
				const UDreamShaderSettings* Settings = GetDefault<UDreamShaderSettings>();
				
				FProcHandle ProcessHandle;
				if (Candidate.EndsWith(TEXT(".cmd"), ESearchCase::IgnoreCase)
					|| Candidate.EndsWith(TEXT(".bat"), ESearchCase::IgnoreCase))
				{
					FString CmdExe = FPlatformMisc::GetEnvironmentVariable(TEXT("ComSpec"));
					if (CmdExe.IsEmpty())
					{
						CmdExe = TEXT("C:/Windows/System32/cmd.exe");
					}
					
					FString Parameters = FString::Printf(
						TEXT("/C \"\"%s\" %s %s\""),
						*Candidate,
						((Settings && !Settings->bOpenInNewWindow) ? TEXT(" --reuse-window") : TEXT("")),
						*QuoteProcessArgument(WorkspaceFilePath));
					ProcessHandle = FPlatformProcess::CreateProc(*CmdExe, *Parameters, true, true, true, nullptr, 0, nullptr, nullptr);
				}
				else
				{
					const FString Parameters = FString::Printf(TEXT("%s %s"), 
					((Settings && !Settings->bOpenInNewWindow) ? TEXT(" --reuse-window") : TEXT("")),
					*QuoteProcessArgument(WorkspaceFilePath));
					ProcessHandle = FPlatformProcess::CreateProc(*Candidate, *Parameters, true, false, false, nullptr, 0, nullptr, nullptr);
				}

				if (ProcessHandle.IsValid())
				{
					FPlatformProcess::CloseProc(ProcessHandle);
					return true;
				}
			}

			return false;
		}

		bool LaunchVSCodeFile(const FString& FilePath, int32 Line = 1, int32 Column = 1)
		{
			const FString GotoArgument = FString::Printf(
				TEXT("%s:%d:%d"),
				*FilePath,
				FMath::Max(1, Line),
				FMath::Max(1, Column));

			for (const FString& Candidate : FindVSCodeExecutableCandidates())
			{
				FProcHandle ProcessHandle;
				if (Candidate.EndsWith(TEXT(".cmd"), ESearchCase::IgnoreCase)
					|| Candidate.EndsWith(TEXT(".bat"), ESearchCase::IgnoreCase))
				{
					FString CmdExe = FPlatformMisc::GetEnvironmentVariable(TEXT("ComSpec"));
					if (CmdExe.IsEmpty())
					{
						CmdExe = TEXT("C:/Windows/System32/cmd.exe");
					}

					const FString Parameters = FString::Printf(
						TEXT("/C \"\"%s\" --reuse-window -g %s\""),
						*Candidate,
						*QuoteProcessArgument(GotoArgument));
					ProcessHandle = FPlatformProcess::CreateProc(*CmdExe, *Parameters, true, true, true, nullptr, 0, nullptr, nullptr);
				}
				else
				{
					const FString Parameters = FString::Printf(TEXT("--reuse-window -g %s"), *QuoteProcessArgument(GotoArgument));
					ProcessHandle = FPlatformProcess::CreateProc(*Candidate, *Parameters, true, false, false, nullptr, 0, nullptr, nullptr);
				}

				if (ProcessHandle.IsValid())
				{
					FPlatformProcess::CloseProc(ProcessHandle);
					return true;
				}
			}

			return false;
		}

		bool LaunchTextFileWithNotepad(const FString& FilePath)
		{
			TArray<FString> Candidates;
			const FString SystemRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot"));
			AddExistingFileCandidate(Candidates, FPaths::Combine(SystemRoot, TEXT("System32/notepad.exe")));
			Candidates.Add(TEXT("notepad.exe"));

			for (const FString& Candidate : Candidates)
			{
				const FString Parameters = QuoteProcessArgument(FilePath);
				FProcHandle ProcessHandle = FPlatformProcess::CreateProc(*Candidate, *Parameters, true, false, false, nullptr, 0, nullptr, nullptr);
				if (ProcessHandle.IsValid())
				{
					FPlatformProcess::CloseProc(ProcessHandle);
					return true;
				}
			}

			return false;
		}

		bool LaunchTextFileInPreferredEditor(const FString& FilePath, int32 Line = 1, int32 Column = 1)
		{
			if (LaunchVSCodeFile(FilePath, Line, Column))
			{
				return true;
			}
			if (FPlatformProcess::LaunchFileInDefaultExternalApplication(*FilePath, nullptr, ELaunchVerb::Edit, false))
			{
				return true;
			}
			return LaunchTextFileWithNotepad(FilePath);
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

		bool TryExtractImportPathFromLine(const FString& Line, FString& OutPath)
		{
			FString TrimmedLine = Line.TrimStartAndEnd();
			if (TrimmedLine.StartsWith(TEXT("//"))
				|| !TrimmedLine.StartsWith(TEXT("import"), ESearchCase::CaseSensitive))
			{
				return false;
			}

			TrimmedLine.RightChopInline(6, EAllowShrinking::No);
			TrimmedLine.TrimStartInline();
			if (TrimmedLine.Len() < 2 || (TrimmedLine[0] != TCHAR('"') && TrimmedLine[0] != TCHAR('\'')))
			{
				return false;
			}

			const TCHAR Quote = TrimmedLine[0];
			int32 ClosingQuoteIndex = INDEX_NONE;
			for (int32 Index = 1; Index < TrimmedLine.Len(); ++Index)
			{
				if (TrimmedLine[Index] == Quote)
				{
					ClosingQuoteIndex = Index;
					break;
				}
			}

			if (ClosingQuoteIndex == INDEX_NONE)
			{
				return false;
			}

			OutPath = TrimmedLine.Mid(1, ClosingQuoteIndex - 1).TrimStartAndEnd();
			return !OutPath.IsEmpty();
		}

		FString NormalizeDreamShaderImportSpecifier(const FString& ImportSpecifier)
		{
			FString Normalized = ImportSpecifier.TrimStartAndEnd();
			Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
			while (Normalized.StartsWith(TEXT("./")))
			{
				Normalized.RightChopInline(2, EAllowShrinking::No);
			}

			if (FPaths::GetExtension(Normalized, true).IsEmpty())
			{
				Normalized += TEXT(".dsh");
			}

			return Normalized;
		}

		bool ResolveDreamShaderImportPath(const FString& CurrentFilePath, const FString& ImportSpecifier, FString& OutResolvedPath)
		{
			const FString NormalizedImport = NormalizeDreamShaderImportSpecifier(ImportSpecifier);
			if (NormalizedImport.IsEmpty())
			{
				return false;
			}

			const TArray<FString> Candidates =
			{
				FPaths::Combine(FPaths::GetPath(CurrentFilePath), NormalizedImport),
				FPaths::Combine(UE::DreamShader::GetSourceShaderDirectory(), NormalizedImport),
				FPaths::Combine(UE::DreamShader::GetPackageShaderDirectory(), NormalizedImport),
				FPaths::Combine(UE::DreamShader::GetBuiltinShaderLibraryDirectory(), NormalizedImport)
			};

			for (const FString& Candidate : Candidates)
			{
				const FString NormalizedCandidate = UE::DreamShader::NormalizeSourceFilePath(Candidate);
				if (IFileManager::Get().FileExists(*NormalizedCandidate))
				{
					OutResolvedPath = NormalizedCandidate;
					return true;
				}
			}

			return false;
		}

		void FindProjectMaterialSourceFiles(TArray<FString>& OutSourceFiles)
		{
			TArray<FString> MaterialFiles;
			TArray<FString> FunctionFiles;
			IFileManager::Get().FindFilesRecursive(
				MaterialFiles,
				*UE::DreamShader::GetSourceShaderDirectory(),
				TEXT("*.dsm"),
				true,
				false,
				false);
			IFileManager::Get().FindFilesRecursive(
				FunctionFiles,
				*UE::DreamShader::GetSourceShaderDirectory(),
				TEXT("*.dsf"),
				true,
				false,
				false);

			OutSourceFiles.Reset();
			OutSourceFiles.Append(MaterialFiles);
			OutSourceFiles.Append(FunctionFiles);

			for (FString& SourceFile : OutSourceFiles)
			{
				SourceFile = UE::DreamShader::NormalizeSourceFilePath(SourceFile);
			}

			OutSourceFiles.RemoveAll([](const FString& SourceFile)
			{
				return IsPackageMaterialFile(SourceFile);
			});
		}

		void CollectHeaderDependenciesRecursive(
			const FString& SourceFilePath,
			TSet<FString>& OutHeaders,
			TSet<FString>& InOutVisitedFiles)
		{
			const FString NormalizedPath = UE::DreamShader::NormalizeSourceFilePath(SourceFilePath);
			if (InOutVisitedFiles.Contains(NormalizedPath))
			{
				return;
			}
			InOutVisitedFiles.Add(NormalizedPath);

			FString SourceText;
			if (!FFileHelper::LoadFileToString(SourceText, *NormalizedPath))
			{
				return;
			}

			TArray<FString> Lines;
			SourceText.ParseIntoArrayLines(Lines, false);
			for (const FString& Line : Lines)
			{
				FString ImportPath;
				if (!TryExtractImportPathFromLine(Line, ImportPath))
				{
					continue;
				}

				FString ResolvedImportPath;
				if (!ResolveDreamShaderImportPath(NormalizedPath, ImportPath, ResolvedImportPath))
				{
					continue;
				}

				if (UE::DreamShader::IsDreamShaderHeaderFile(ResolvedImportPath) || UE::DreamShader::IsDreamShaderFunctionFile(ResolvedImportPath))
				{
					OutHeaders.Add(ResolvedImportPath);
				}

				CollectHeaderDependenciesRecursive(ResolvedImportPath, OutHeaders, InOutVisitedFiles);
			}
		}

		bool TryParseErrorLocation(
			const FString& Line,
			FString& OutFilePath,
			int32& OutLine,
			int32& OutColumn,
			FString& OutMessage)
		{
			const int32 CloseMarkerIndex = Line.Find(TEXT("): "));
			if (CloseMarkerIndex == INDEX_NONE)
			{
				return false;
			}

			const int32 OpenMarkerIndex = Line.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromEnd, CloseMarkerIndex);
			if (OpenMarkerIndex == INDEX_NONE || OpenMarkerIndex >= CloseMarkerIndex)
			{
				return false;
			}

			const FString LocationText = Line.Mid(OpenMarkerIndex + 1, CloseMarkerIndex - OpenMarkerIndex - 1);
			FString LineText;
			FString ColumnText;
			if (!LocationText.Split(TEXT(","), &LineText, &ColumnText))
			{
				return false;
			}

			LineText.TrimStartAndEndInline();
			ColumnText.TrimStartAndEndInline();
			if (!LineText.IsNumeric() || !ColumnText.IsNumeric())
			{
				return false;
			}

			OutLine = FMath::Max(1, FCString::Atoi(*LineText));
			OutColumn = FMath::Max(1, FCString::Atoi(*ColumnText));

			OutFilePath = UE::DreamShader::NormalizeSourceFilePath(Line.Left(OpenMarkerIndex));
			OutMessage = Line.Mid(CloseMarkerIndex + 3).TrimStartAndEnd();
			return !OutFilePath.IsEmpty() && !OutMessage.IsEmpty();
		}

		void AddDiagnosticJson(TArray<TSharedPtr<FJsonValue>>& OutDiagnostics, const FDreamShaderEditorBridge::FDiagnosticRecord& Diagnostic)
		{
			TSharedRef<FJsonObject> DiagnosticObject = MakeShared<FJsonObject>();
			DiagnosticObject->SetStringField(TEXT("message"), Diagnostic.Message);
			if (!Diagnostic.Detail.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("detail"), Diagnostic.Detail);
			}
			if (!Diagnostic.Stage.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("stage"), Diagnostic.Stage);
			}
			if (!Diagnostic.AssetPath.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("assetPath"), Diagnostic.AssetPath);
			}
			if (!Diagnostic.ShaderPlatform.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("shaderPlatform"), Diagnostic.ShaderPlatform);
			}
			if (!Diagnostic.QualityLevel.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("qualityLevel"), Diagnostic.QualityLevel);
			}
			if (!Diagnostic.Code.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("code"), Diagnostic.Code);
			}
			DiagnosticObject->SetNumberField(TEXT("line"), Diagnostic.Line);
			DiagnosticObject->SetNumberField(TEXT("column"), Diagnostic.Column);
			DiagnosticObject->SetStringField(TEXT("severity"), Diagnostic.Severity);
			DiagnosticObject->SetStringField(TEXT("source"), Diagnostic.Source);
			OutDiagnostics.Add(MakeShared<FJsonValueObject>(DiagnosticObject));
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

		ExportMaterialExpressionManifest();
		ExportDreamShaderSettingsManifest();
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
		DiagnosticsByFile.Reset();
	}

	void FDreamShaderEditorBridge::QueueFullScan()
	{
		TArray<FString> SourceFiles;
		FindProjectMaterialSourceFiles(SourceFiles);
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
		TSet<FString> SourcesToQueue;

		if (const TSet<FString>* ExistingDependents = HeaderDependentsByFile.Find(NormalizedImportPath))
		{
			for (const FString& Dependent : *ExistingDependents)
			{
				SourcesToQueue.Add(Dependent);
			}
		}

		RebuildDependencyGraph();

		if (const TSet<FString>* RebuiltDependents = HeaderDependentsByFile.Find(NormalizedImportPath))
		{
			for (const FString& Dependent : *RebuiltDependents)
			{
				SourcesToQueue.Add(Dependent);
			}
		}

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
						if (!IsPackageMaterialFile(FileChange.Filename))
						{
							Bridge->QueueSourceFile(FileChange.Filename);
						}
						Bridge->QueueDependentSourcesForImport(FileChange.Filename);
					}
					else if (IsPackageMaterialFile(FileChange.Filename))
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
					else if (IsPackageMaterialFile(FileChange.Filename))
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
		FString Message;
		if (FMaterialGenerator::GenerateAssetsFromFile(SourceFilePath, Message))
		{
			ClearDiagnosticsForSourceAndDependencies(SourceFilePath);
			UpdateDiagnosticsFile();
			UE_LOG(LogDreamShader, Display, TEXT("%s"), *Message);
			return;
		}

		TArray<FDiagnosticRecord> Diagnostics = BuildErrorDiagnostics(SourceFilePath, Message);
		ClearDiagnosticsForSourceAndDependencies(SourceFilePath);
		SetDiagnostics(SourceFilePath, MoveTemp(Diagnostics));
		UpdateDiagnosticsFile();
		UE_LOG(LogDreamShader, Error, TEXT("%s"), *Message);
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

		TArray<FDiagnosticRecord> Diagnostics;
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

					FString ParsedFilePath;
					int32 ParsedLine = 1;
					int32 ParsedColumn = 1;
					FString ParsedMessage;
					const bool bHasParsedLocation = TryParseErrorLocation(RawError, ParsedFilePath, ParsedLine, ParsedColumn, ParsedMessage);
					const bool bMapsToDreamShaderSource = bHasParsedLocation && UE::DreamShader::IsDreamShaderSourceFile(ParsedFilePath);

					const FString DisplayMessage = FString::Printf(
						TEXT("[%s / %s] %s"),
						*ShaderPlatformLabel,
						*QualityLabel,
						*(bHasParsedLocation ? ParsedMessage : GetFirstMeaningfulErrorLine(RawError)));

					const FString DeduplicationKey = FString::Printf(
						TEXT("%s|%s|%s|%s|%d|%d"),
						*SourceFilePath,
						*ShaderPlatformLabel,
						*QualityLabel,
						*DisplayMessage,
						bMapsToDreamShaderSource ? ParsedLine : 1,
						bMapsToDreamShaderSource ? ParsedColumn : 1);
					if (SeenDiagnosticKeys.Contains(DeduplicationKey))
					{
						continue;
					}
					SeenDiagnosticKeys.Add(DeduplicationKey);

					FDiagnosticRecord& Diagnostic = Diagnostics.AddDefaulted_GetRef();
					Diagnostic.FilePath = bMapsToDreamShaderSource ? ParsedFilePath : SourceFilePath;
					Diagnostic.Message = DisplayMessage;
					Diagnostic.Detail = RawError;
					Diagnostic.Stage = TEXT("materialCompile");
					Diagnostic.AssetPath = MaterialAssetPath;
					Diagnostic.ShaderPlatform = ShaderPlatformLabel;
					Diagnostic.QualityLevel = QualityLabel;
					Diagnostic.Code = TEXT("material-compile");
					Diagnostic.Source = TEXT("DreamShader Material Compile");
					Diagnostic.Line = bMapsToDreamShaderSource ? ParsedLine : 1;
					Diagnostic.Column = bMapsToDreamShaderSource ? ParsedColumn : 1;
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
				TEXT("DreamShader.VirtualFunctionToolbarActions"),
				FNewToolMenuSectionDelegate::CreateSP(AsShared(), &FDreamShaderEditorBridge::PopulateMaterialFunctionEditorToolbar));
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

	void FDreamShaderEditorBridge::PopulateMaterialFunctionEditorToolbar(FToolMenuSection& InSection)
	{
		const UMaterialEditorMenuContext* Context = InSection.FindContext<UMaterialEditorMenuContext>();
		TSharedPtr<IMaterialEditor> MaterialEditor = Context ? Context->MaterialEditor.Pin() : nullptr;
		if (!MaterialEditor.IsValid())
		{
			return;
		}

		UMaterialFunction* MaterialFunction = nullptr;
		const TArray<UObject*>* EditingObjects = MaterialEditor->GetObjectsCurrentlyBeingEdited();
		if (EditingObjects)
		{
			for (UObject* EditingObject : *EditingObjects)
			{
				MaterialFunction = Cast<UMaterialFunction>(EditingObject);
				if (MaterialFunction)
				{
					break;
				}
			}
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

		ExportMaterialExpressionManifest();
		ExportDreamShaderSettingsManifest();

		FString WorkspaceFilePath;
		FString Error;
		if (!WriteDreamShaderWorkspaceFile(WorkspaceFilePath, Error))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to create workspace: %s"), *Error)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to create DreamShader workspace: %s"), *Error);
			return;
		}

		if (LaunchVSCodeWorkspace(WorkspaceFilePath))
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

		if (LaunchTextFileWithNotepad(WorkspaceFilePath))
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

		FDreamShaderGraphDecompiler Decompiler;
		FString SourceText;
		FString Error;
		if (!Decompiler.DecompileMaterial(MaterialAsset, MakeDecompiledAssetName(MaterialAsset, TEXT("Materials")), SourceText, Error))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to export DSM: %s"), *Error)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to export Material '%s' to DSM: %s"), *MaterialAsset->GetPathName(), *Error);
			return;
		}

		const FString SourceFilePath = MakeDecompiledMaterialFilePath(MaterialAsset);
		const FString SourceDirectory = FPaths::GetPath(SourceFilePath);
		if (!IFileManager::Get().MakeDirectory(*SourceDirectory, true))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to create directory: %s"), *SourceDirectory)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to create decompiled Material directory '%s'."), *SourceDirectory);
			return;
		}

		if (!FFileHelper::SaveStringToFile(SourceText, *SourceFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to write DSM file: %s"), *SourceFilePath)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to write decompiled Material DSM file '%s'."), *SourceFilePath);
			return;
		}

		if (!LaunchTextFileInPreferredEditor(SourceFilePath))
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

		FDreamShaderGraphDecompiler Decompiler;
		FString SourceText;
		FString Error;
		if (!Decompiler.DecompileFunction(Function, MakeDecompiledAssetName(Function, TEXT("Functions")), SourceText, Error))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to export DSF: %s"), *Error)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to export MaterialFunction '%s' to DSF: %s"), *Function->GetPathName(), *Error);
			return;
		}

		const FString SourceFilePath = MakeDecompiledFunctionFilePath(Function);
		const FString SourceDirectory = FPaths::GetPath(SourceFilePath);
		if (!IFileManager::Get().MakeDirectory(*SourceDirectory, true))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to create directory: %s"), *SourceDirectory)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to create decompiled MaterialFunction directory '%s'."), *SourceDirectory);
			return;
		}

		if (!FFileHelper::SaveStringToFile(SourceText, *SourceFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			ShowDreamShaderNotification(
				FText::FromString(FString::Printf(TEXT("DreamShader failed to write DSF file: %s"), *SourceFilePath)),
				SNotificationItem::CS_Fail);
			UE_LOG(LogDreamShader, Warning, TEXT("Failed to write decompiled MaterialFunction DSF file '%s'."), *SourceFilePath);
			return;
		}

		if (!LaunchTextFileInPreferredEditor(SourceFilePath))
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

		const FString DefinitionFilePath = MakeVirtualFunctionDefinitionFilePath(Function);
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

		if (!LaunchTextFileInPreferredEditor(DefinitionFilePath))
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

		if (!LaunchTextFileInPreferredEditor(ExistingDefinition.SourceFilePath, ExistingDefinition.Line, ExistingDefinition.Column))
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
		if (!BuildVirtualFunctionCallTextFromSignature(
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
		HeaderDependentsByFile.Reset();

		TArray<FString> MaterialFiles;
		FindProjectMaterialSourceFiles(MaterialFiles);
		for (const FString& MaterialFile : MaterialFiles)
		{
			TSet<FString> Dependencies;
			TSet<FString> VisitedFiles;
			CollectHeaderDependenciesRecursive(MaterialFile, Dependencies, VisitedFiles);
			for (const FString& HeaderFile : Dependencies)
			{
				HeaderDependentsByFile.FindOrAdd(HeaderFile).Add(MaterialFile);
			}
		}
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
		FindProjectDreamShaderSourceFiles(SourceFiles);

		int32 ScannedDefinitionCount = 0;
		int32 UpdatedDefinitionCount = 0;
		int32 ErrorCount = 0;

		for (const FString& SourceFile : SourceFiles)
		{
			FString SourceText;
			TArray<FVirtualFunctionDefinitionLocation> Locations;
			TArray<FDiagnosticRecord> Diagnostics;
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

	void FDreamShaderEditorBridge::SetDiagnostics(const FString& SourceFilePath, TArray<FDiagnosticRecord>&& Diagnostics)
	{
		const FString NormalizedPath = UE::DreamShader::NormalizeSourceFilePath(SourceFilePath);
		DiagnosticsByFile.Remove(NormalizedPath);
		if (Diagnostics.IsEmpty())
		{
			return;
		}

		for (FDiagnosticRecord& Diagnostic : Diagnostics)
		{
			const FString DiagnosticFilePath = Diagnostic.FilePath.IsEmpty()
				? NormalizedPath
				: UE::DreamShader::NormalizeSourceFilePath(Diagnostic.FilePath);
			Diagnostic.FilePath.Reset();
			DiagnosticsByFile.FindOrAdd(DiagnosticFilePath).Add(MoveTemp(Diagnostic));
		}
	}

	void FDreamShaderEditorBridge::ClearDiagnostics(const FString& SourceFilePath)
	{
		DiagnosticsByFile.Remove(UE::DreamShader::NormalizeSourceFilePath(SourceFilePath));
	}

	void FDreamShaderEditorBridge::ClearDiagnosticsForSourceAndDependencies(const FString& SourceFilePath)
	{
		ClearDiagnostics(SourceFilePath);

		TSet<FString> Dependencies;
		TSet<FString> VisitedFiles;
		CollectHeaderDependenciesRecursive(SourceFilePath, Dependencies, VisitedFiles);
		for (const FString& HeaderFile : Dependencies)
		{
			ClearDiagnostics(HeaderFile);
		}
	}

	void FDreamShaderEditorBridge::UpdateDiagnosticsFile() const
	{
		TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetNumberField(TEXT("version"), 1);
		RootObject->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());

		TArray<TSharedPtr<FJsonValue>> FileEntries;
		for (const TPair<FString, TArray<FDiagnosticRecord>>& Pair : DiagnosticsByFile)
		{
			TSharedRef<FJsonObject> FileObject = MakeShared<FJsonObject>();
			FileObject->SetStringField(TEXT("path"), Pair.Key);

			TArray<TSharedPtr<FJsonValue>> DiagnosticValues;
			for (const FDiagnosticRecord& Diagnostic : Pair.Value)
			{
				AddDiagnosticJson(DiagnosticValues, Diagnostic);
			}

			FileObject->SetArrayField(TEXT("diagnostics"), DiagnosticValues);
			FileEntries.Add(MakeShared<FJsonValueObject>(FileObject));
		}

		RootObject->SetArrayField(TEXT("files"), FileEntries);

		FString OutputText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputText);
		FJsonSerializer::Serialize(RootObject, Writer);
		FFileHelper::SaveStringToFile(OutputText, *GetDiagnosticsFilePath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	TArray<FDreamShaderEditorBridge::FDiagnosticRecord> FDreamShaderEditorBridge::BuildErrorDiagnostics(
		const FString& SourceFilePath,
		const FString& ErrorMessage) const
	{
		TArray<FDiagnosticRecord> Diagnostics;

		TArray<FString> Lines;
		ErrorMessage.ParseIntoArrayLines(Lines, false);
		if (Lines.IsEmpty())
		{
			Lines.Add(ErrorMessage);
		}

		const FString PathPrefix = UE::DreamShader::NormalizeSourceFilePath(SourceFilePath) + TEXT(": ");
		for (FString Line : Lines)
		{
			FString DiagnosticFilePath;
			int32 DiagnosticLine = 1;
			int32 DiagnosticColumn = 1;
			FString DiagnosticMessage;
			if (TryParseErrorLocation(Line, DiagnosticFilePath, DiagnosticLine, DiagnosticColumn, DiagnosticMessage))
			{
				FDiagnosticRecord& Diagnostic = Diagnostics.AddDefaulted_GetRef();
				Diagnostic.FilePath = DiagnosticFilePath;
				Diagnostic.Message = DiagnosticMessage;
				Diagnostic.Detail = Line;
				Diagnostic.Stage = TEXT("generate");
				Diagnostic.Code = TEXT("generate-error");
				Diagnostic.Source = TEXT("DreamShader Generate");
				Diagnostic.Line = DiagnosticLine;
				Diagnostic.Column = DiagnosticColumn;
				continue;
			}

			if (Line.StartsWith(PathPrefix))
			{
				Line.RightChopInline(PathPrefix.Len(), EAllowShrinking::No);
			}

			FDiagnosticRecord& Diagnostic = Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Message = Line;
			Diagnostic.Detail = Line;
			Diagnostic.Stage = TEXT("generate");
			Diagnostic.Code = TEXT("generate-error");
			Diagnostic.Source = TEXT("DreamShader Generate");
		}

		return Diagnostics;
	}
}

#undef LOCTEXT_NAMESPACE
