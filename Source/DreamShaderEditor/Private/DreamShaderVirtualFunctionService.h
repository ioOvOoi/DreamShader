#pragma once

#include "CoreMinimal.h"
#include "DreamShaderTypes.h"

class UMaterialExpressionFunctionOutput;
class UMaterialFunction;

namespace UE::DreamShader::Editor::Private
{
	struct FDreamShaderVirtualFunctionService
	{
		using FOutputTypeResolver = TFunctionRef<FString(const UMaterialExpressionFunctionOutput*)>;

		static bool BuildDefinition(
			const UMaterialFunction* MaterialFunction,
			FOutputTypeResolver OutputTypeResolver,
			FString& OutDefinition,
			FString& OutError);
		static bool BuildCallTextFromSignature(
			const FString& FunctionName,
			const TArray<FTextShaderFunctionParameter>& Inputs,
			const TArray<FTextShaderFunctionParameter>& Outputs,
			FString& OutCallText,
			FString& OutError);
		static bool BuildCallText(const UMaterialFunction* MaterialFunction, FString& OutCallText, FString& OutError);
		static FString MakeDefinitionFilePath(const UMaterialFunction* MaterialFunction);
	};
}
