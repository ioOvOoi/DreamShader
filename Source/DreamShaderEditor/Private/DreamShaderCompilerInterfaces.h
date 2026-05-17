#pragma once

#include "CoreMinimal.h"

class UObject;
class UMaterial;
class UMaterialFunction;

namespace UE::DreamShader::Editor
{
	struct FDreamShaderCompileRequest
	{
		FString SourceFilePath;
		bool bForce = false;
	};

	struct FDreamShaderCompileResult
	{
		bool bSucceeded = false;
		FString Message;
	};

	class IDreamShaderCompiler
	{
	public:
		virtual ~IDreamShaderCompiler() = default;

		virtual FDreamShaderCompileResult CompileAssets(const FDreamShaderCompileRequest& Request) = 0;
		virtual FDreamShaderCompileResult CompileMaterial(const FDreamShaderCompileRequest& Request) = 0;
	};

	struct FDreamShaderDecompileRequest
	{
		UObject* Asset = nullptr;
		FString OutputFilePath;
	};

	struct FDreamShaderDecompileResult
	{
		bool bSucceeded = false;
		FString SourceText;
		FString OutputFilePath;
		FString Error;
	};

	class IDreamShaderDecompiler
	{
	public:
		virtual ~IDreamShaderDecompiler() = default;

		virtual bool DecompileMaterial(UMaterial* Material, const FString& DecompiledName, FString& OutSourceText, FString& OutError) = 0;
		virtual bool DecompileFunction(UMaterialFunction* MaterialFunction, const FString& DecompiledName, FString& OutSourceText, FString& OutError) = 0;
	};
}
