#include "DreamShaderCompileService.h"

#include "DreamShaderMaterialGenerator.h"

namespace UE::DreamShader::Editor
{
	FDreamShaderCompileResult FMaterialGeneratorCompiler::CompileAssets(const FDreamShaderCompileRequest& Request)
	{
		FDreamShaderCompileResult Result;
		Result.bSucceeded = FMaterialGenerator::GenerateAssetsFromFile(Request.SourceFilePath, Result.Message, Request.bForce);
		return Result;
	}

	FDreamShaderCompileResult FMaterialGeneratorCompiler::CompileMaterial(const FDreamShaderCompileRequest& Request)
	{
		FDreamShaderCompileResult Result;
		Result.bSucceeded = FMaterialGenerator::GenerateMaterialFromFile(Request.SourceFilePath, Result.Message, Request.bForce);
		return Result;
	}

	FDreamShaderCompileResult FDreamShaderCompileService::CompileAssets(const FString& SourceFilePath, const bool bForce)
	{
		FDreamShaderCompileRequest Request;
		Request.SourceFilePath = SourceFilePath;
		Request.bForce = bForce;
		return Compiler.CompileAssets(Request);
	}

	FDreamShaderCompileResult FDreamShaderCompileService::CompileMaterial(const FString& SourceFilePath, const bool bForce)
	{
		FDreamShaderCompileRequest Request;
		Request.SourceFilePath = SourceFilePath;
		Request.bForce = bForce;
		return Compiler.CompileMaterial(Request);
	}

	FMaterialGeneratorCompiler& GetMaterialGeneratorCompiler()
	{
		static FMaterialGeneratorCompiler Compiler;
		return Compiler;
	}
}
