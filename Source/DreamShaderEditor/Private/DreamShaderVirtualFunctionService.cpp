#include "DreamShaderVirtualFunctionService.h"

#include "DreamShaderModule.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"

namespace UE::DreamShader::Editor::Private
{
	namespace
	{
		FString EscapeDreamShaderString(const FString& InText)
		{
			FString Result = InText;
			Result.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			Result.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return Result;
		}

		FString GetDreamShaderTypeForFunctionInput(const EFunctionInputType InputType)
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
			case FunctionInput_VolumeTexture:
				return TEXT("VolumeTexture");
			case FunctionInput_StaticBool:
			case FunctionInput_Bool:
				return TEXT("bool");
			default:
				return TEXT("float4");
			}
		}

		FString MakeDreamShaderDeclarationName(const FString& InName, const TCHAR* FallbackPrefix, const int32 Index)
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

		FString MakePreviewValueText(const EFunctionInputType InputType, const FVector4f& PreviewValue)
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
				OutLiteral = FString::Printf(TEXT("Path(Plugins.%s, %s)"), *BestPluginName, *RelativePath);
				return true;
			}

			OutLiteral = MaterialFunction->GetPathName();
			return true;
		}
	}

	bool FDreamShaderVirtualFunctionService::BuildDefinition(
		const UMaterialFunction* MaterialFunction,
		FOutputTypeResolver OutputTypeResolver,
		FString& OutDefinition,
		FString& OutError)
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
			const FString MetadataSuffix = OutputExpression
				? MakeFunctionParameterMetadataSuffix(OutputExpression->Description, OutputExpression->SortPriority, OutputIndex)
				: FString();
			Lines.Add(FString::Printf(
				TEXT("\t\t%s %s%s;"),
				*OutputTypeResolver(OutputExpression),
				*MakeDreamShaderDeclarationName(OutputName, TEXT("Output"), OutputIndex),
				*MetadataSuffix));
		}
		Lines.Add(TEXT("\t}"));
		Lines.Add(TEXT("}"));

		OutDefinition = FString::Join(Lines, TEXT("\n"));
		return true;
	}

	bool FDreamShaderVirtualFunctionService::BuildCallTextFromSignature(
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

	bool FDreamShaderVirtualFunctionService::BuildCallText(const UMaterialFunction* MaterialFunction, FString& OutCallText, FString& OutError)
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

		return BuildCallTextFromSignature(MaterialFunction->GetName(), Inputs, Outputs, OutCallText, OutError);
	}

	FString FDreamShaderVirtualFunctionService::MakeDefinitionFilePath(const UMaterialFunction* MaterialFunction)
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
}
