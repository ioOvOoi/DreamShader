#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UMaterialInstanceConstant;

namespace UE::DreamShader::Editor::Private
{
	struct FCreateInstanceResult
	{
		bool bSucceeded = false;
		FString Error;
		UMaterialInstanceConstant* Instance = nullptr;
	};

	// Creates a persisted UMaterialInstanceConstant parented to Parent. If Parent is a memory-only
	// DreamShader material, it is first materialized to disk (its transient base can't be a parent import),
	// so the child never references a transient object. The child shares the root's compiled shader map
	// (UDreamShaderMaterialInstance::HasOverridenBaseProperties), so many variants cost one shader map.
	// PackagePath is the destination folder (e.g. "/Game/Foo/Instances"); AssetName the leaf.
	FCreateInstanceResult CreateDreamShaderMaterialInstance(
		UMaterialInterface* Parent,
		const FString& AssetName,
		const FString& PackagePath,
		bool bOpenAfterCreate);

	// Computes the default destination for a new instance of Parent: <parentDir>/<InstanceSubfolder> and a
	// unique MI_<parentLeaf>_<n> asset name.
	void GetDefaultInstanceDestination(UMaterialInterface* Parent, FString& OutPackagePath, FString& OutAssetName);

	// Persists a memory-only DreamShader material (and its hidden base) to disk by re-generating it from
	// its source with the persistent backend, and returns the on-disk material. Already-persisted
	// materials are returned unchanged. Returns null with OutError on failure.
	UMaterialInterface* MaterializeDreamShaderMaterial(UMaterialInterface* Material, FString& OutError);

	// True when Material is a memory-only (transient / PKG_NewlyCreated) instance that materialization
	// would move to disk.
	bool IsMemoryOnlyMaterial(UMaterialInterface* Material);

	// Opens a modal dialog to configure and create a new instance of Parent.
	void OpenCreateInstanceDialog(UMaterialInterface* Parent);
}
