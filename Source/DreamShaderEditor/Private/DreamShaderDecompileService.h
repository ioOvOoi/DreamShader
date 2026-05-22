#pragma once

#include "DreamShaderCompilerInterfaces.h"

namespace UE::DreamShader::Editor::Private
{
	struct FDecompiledAssetNaming
	{
		static FString MakeMaterialFilePath(const UMaterial* Material);
		static FString MakeFunctionFilePath(const UMaterialFunction* MaterialFunction);
		static const TCHAR* GetFunctionCategory(EDreamShaderDecompiledFunctionKind FunctionKind);
		static EDreamShaderDecompiledFunctionKind GetFunctionKind(const UMaterialFunction* MaterialFunction);
		static FString MakeAssetName(const UObject* Asset, const TCHAR* Category);
	};

	struct FDecompiledSourceWriter
	{
		static bool Save(const FDreamShaderDecompileResult& Result, FString& OutError);
	};

	class FDreamShaderDecompileService
	{
	public:
		explicit FDreamShaderDecompileService(IDreamShaderDecompiler& InDecompiler)
			: Decompiler(InDecompiler)
		{
		}

		FDreamShaderDecompileResult DecompileAsset(const FDreamShaderDecompileRequest& Request);

	private:
		IDreamShaderDecompiler& Decompiler;
	};
}
