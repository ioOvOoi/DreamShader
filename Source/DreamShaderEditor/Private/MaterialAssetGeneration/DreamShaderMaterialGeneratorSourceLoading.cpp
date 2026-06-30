// Copyright (c) 2026 TypeDreamMoon. All rights reserved.
//
// Source loading for the DreamShader material generator (recursive import inlining + cycle
// detection + header/function-file rule checks). Extracted byte-for-byte from
// DreamShaderMaterialGenerator.cpp's anonymous namespace; LoadPreparedDreamShaderSource is promoted
// to external linkage (DreamShaderMaterialGeneratorSourceLoading.h) for the generator entry points.

#include "DreamShaderMaterialGeneratorSourceLoading.h"

#include "DependencyGraph/DreamShaderDependencyGraphService.h"
#include "DreamShaderModule.h"
#include "Misc/FileHelper.h"

namespace UE::DreamShader::Editor
{
	static bool ResolveDreamShaderImportPath(
		const FString& CurrentFilePath,
		const FString& ImportSpecifier,
		FString& OutResolvedPath,
		FString& OutError)
	{
		if (Private::FDreamShaderDependencyGraphService::ResolveImportPath(CurrentFilePath, ImportSpecifier, OutResolvedPath))
		{
			return true;
		}

		OutError = FString::Printf(
			TEXT("DreamShader import '%s' referenced from '%s' could not be resolved."),
			*ImportSpecifier,
			*CurrentFilePath);
		return false;
	}

	static bool LoadPreparedDreamShaderSourceRecursive(
		const FString& SourceFilePath,
		TSet<FString>& InOutVisitedFiles,
		TSet<FString>& InOutActiveStack,
		FString& OutSourceText,
		FString& OutError)
	{
		const FString NormalizedPath = UE::DreamShader::NormalizeSourceFilePath(SourceFilePath);
		if (InOutVisitedFiles.Contains(NormalizedPath))
		{
			return true;
		}

		if (InOutActiveStack.Contains(NormalizedPath))
		{
			OutError = FString::Printf(TEXT("DreamShader import cycle detected at '%s'."), *NormalizedPath);
			return false;
		}

		FString SourceText;
		if (!FFileHelper::LoadFileToString(SourceText, *NormalizedPath))
		{
			OutError = FString::Printf(TEXT("DreamShader could not read '%s'."), *NormalizedPath);
			return false;
		}

		InOutActiveStack.Add(NormalizedPath);

		TArray<FString> Lines;
		SourceText.ParseIntoArrayLines(Lines, false);

		FString SanitizedSourceText;
		SanitizedSourceText.Reserve(SourceText.Len());

		for (const FString& Line : Lines)
		{
			FString ImportPath;
			if (Private::FDreamShaderDependencyGraphService::TryExtractImportPathFromLine(Line, ImportPath))
			{
				FString ResolvedImportPath;
				if (!ResolveDreamShaderImportPath(NormalizedPath, ImportPath, ResolvedImportPath, OutError))
				{
					return false;
				}

				if (!LoadPreparedDreamShaderSourceRecursive(ResolvedImportPath, InOutVisitedFiles, InOutActiveStack, OutSourceText, OutError))
				{
					return false;
				}
				continue;
			}

			SanitizedSourceText += Line;
			SanitizedSourceText += TEXT("\n");
		}

		if (UE::DreamShader::IsDreamShaderHeaderFile(NormalizedPath)
			&& (SanitizedSourceText.Contains(TEXT("Shader("), ESearchCase::IgnoreCase)
				|| SanitizedSourceText.Contains(TEXT("ShaderFunction("), ESearchCase::IgnoreCase)
				|| SanitizedSourceText.Contains(TEXT("ShaderLayer("), ESearchCase::IgnoreCase)
				|| SanitizedSourceText.Contains(TEXT("ShaderLayerBlend("), ESearchCase::IgnoreCase)
				|| SanitizedSourceText.Contains(TEXT("MaterialLayer("), ESearchCase::IgnoreCase)
				|| SanitizedSourceText.Contains(TEXT("MaterialLayerBlend("), ESearchCase::IgnoreCase)))
		{
			OutError = FString::Printf(TEXT("DreamShader header '%s' may only declare Function/Namespace/GraphFunction/VirtualFunction blocks and imports."), *NormalizedPath);
			return false;
		}

		if (UE::DreamShader::IsDreamShaderFunctionFile(NormalizedPath)
			&& SanitizedSourceText.Contains(TEXT("Shader("), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("DreamShader function file '%s' may only declare imports, Function/Namespace/GraphFunction/VirtualFunction blocks, and ShaderFunction/ShaderLayer/ShaderLayerBlend blocks."), *NormalizedPath);
			return false;
		}

		OutSourceText += FString::Printf(TEXT("// Begin DreamShader source: %s\n"), *NormalizedPath);
		OutSourceText += SanitizedSourceText;
		OutSourceText += FString::Printf(TEXT("\n// End DreamShader source: %s\n\n"), *NormalizedPath);

		InOutActiveStack.Remove(NormalizedPath);
		InOutVisitedFiles.Add(NormalizedPath);
		return true;
	}

	bool LoadPreparedDreamShaderSource(const FString& SourceFilePath, FString& OutSourceText, FString& OutError)
	{
		OutSourceText.Reset();
		TSet<FString> VisitedFiles;
		TSet<FString> ActiveStack;
		return LoadPreparedDreamShaderSourceRecursive(SourceFilePath, VisitedFiles, ActiveStack, OutSourceText, OutError);
	}
}
