#pragma once

#include "CoreMinimal.h"

namespace UE::DreamShader::Editor::Private
{
	// Resolves the /Game object path a .dsm/.dsf source file compiles to, by reading + parsing the
	// source's top-level Shader/Function block Name+Root (imports stripped) and running it through
	// ResolveDreamShaderAssetDestination -- the same recipe the compiler and preview renderer use. This
	// is the source -> generated-asset link the Material Content Browser needs to compute per-file status
	// (never-compiled vs up-to-date vs stale) without an on-disk registry. Returns false with OutError if
	// the file can't be read, doesn't parse, or declares no top-level block.
	bool ResolveGeneratedAssetObjectPath(const FString& SourceFilePath, FString& OutObjectPath, FString& OutError);
}
