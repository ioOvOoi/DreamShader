#pragma once

#include "DreamShaderCompilerInterfaces.h"

namespace UE::DreamShader::Editor
{
	class FMaterialGenerator;

	class FMaterialGeneratorCompiler final : public IDreamShaderCompiler
	{
	public:
		virtual FDreamShaderCompileResult CompileAssets(const FDreamShaderCompileRequest& Request) override;
		virtual FDreamShaderCompileResult CompileMaterial(const FDreamShaderCompileRequest& Request) override;
	};

	class FDreamShaderCompileService
	{
	public:
		explicit FDreamShaderCompileService(IDreamShaderCompiler& InCompiler)
			: Compiler(InCompiler)
		{
		}

		FDreamShaderCompileResult CompileAssets(const FString& SourceFilePath, bool bForce = false);
		FDreamShaderCompileResult CompileMaterial(const FString& SourceFilePath, bool bForce = false);

	private:
		IDreamShaderCompiler& Compiler;
	};

	FMaterialGeneratorCompiler& GetMaterialGeneratorCompiler();
}
