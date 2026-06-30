// Copyright (c) 2026 TypeDreamMoon. All rights reserved.
//
// Diagnostic-formatting helpers for the DreamShader material generator: map a prepared-source byte
// index back to a (file, line, column) and format parse / generate / code-block errors with that
// location. Pure string processing — no asset or graph state. Extracted from
// DreamShaderMaterialGenerator.cpp (where they lived in an anonymous namespace) and promoted to
// external linkage so the generator entry points can call them across translation units.

#pragma once

#include "CoreMinimal.h"

namespace UE::DreamShader::Editor
{
	bool TryExtractPreparedSourceIndexFromError(const FString& Error, int32& OutIndex);

	bool TryMapPreparedSourceIndexToLocation(
		const FString& PreparedSource,
		const int32 SourceIndex,
		FString& OutFilePath,
		int32& OutLine,
		int32& OutColumn);

	FString FormatParseErrorWithSourceLocation(
		const FString& FallbackSourceFilePath,
		const FString& PreparedSource,
		const FString& ParseError);

	bool LooksLikeLocatedDiagnostic(const FString& Error);

	FString FormatGenerateError(const FString& SourceFilePath, const FString& Error);

	void ResolveCodeBlockLocation(
		const FString& FallbackSourceFilePath,
		const FString& PreparedSource,
		const int32 CodeStartIndex,
		FString& OutFilePath,
		int32& OutLine,
		int32& OutColumn);

	FString FormatCodeBlockError(
		const FString& FallbackSourceFilePath,
		const FString& CodeSourceFilePath,
		const int32 CodeStartLine,
		const int32 CodeStartColumn,
		const FString& Error,
		const int32 ErrorLine,
		const int32 ErrorColumn);
}
