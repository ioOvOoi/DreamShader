#pragma once

#include "CoreMinimal.h"

namespace UE::DreamShader::Editor::Private
{
	struct FDreamShaderDependencyGraphService
	{
		static bool TryExtractImportPathFromLine(const FString& Line, FString& OutPath);
		static FString NormalizeImportSpecifier(const FString& ImportSpecifier);
		static bool ResolveImportPath(const FString& CurrentFilePath, const FString& ImportSpecifier, FString& OutResolvedPath);
		static void CollectHeaderDependenciesRecursive(
			const FString& SourceFilePath,
			TSet<FString>& OutHeaders,
			TSet<FString>& InOutVisitedFiles);
		static void RebuildMaterialDependencyGraph(TMap<FString, TSet<FString>>& OutHeaderDependentsByFile);
		static TSet<FString> RebuildAndCollectDependentsForImport(
			const FString& ImportFilePath,
			TMap<FString, TSet<FString>>& InOutHeaderDependentsByFile);
	};
}
