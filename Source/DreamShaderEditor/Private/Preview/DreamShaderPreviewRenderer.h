#pragma once

#include "CoreMinimal.h"

namespace UE::DreamShader::Editor::Private
{
	struct FDreamShaderPreviewRequest
	{
		FString SourceFilePath;
		int32 Width = 512;
		int32 Height = 512;
		FString Mesh = TEXT("sphere");
	};

	struct FDreamShaderPreviewResult
	{
		bool bSucceeded = false;
		FString SourceFilePath;
		FString AssetPath;
		FString ImagePath;
		FString Mesh;
		FString Message;
	};

	class FDreamShaderPreviewRenderer
	{
	public:
		static FString GetPreviewDirectory();
		static FString GetPreviewManifestPath();
		static bool ResolvePreviewMaterial(const FDreamShaderPreviewRequest& Request, FDreamShaderPreviewResult& OutResult, UMaterial*& OutMaterial);
		static bool RenderMaterialPreviewFrame(UMaterial* Material, int32 Width, int32 Height, TArray64<uint8>& OutPngData, FString& OutError);
		static bool SaveMaterialPreviewFrame(UMaterial* Material, const FString& SourceFilePath, int32 Width, int32 Height, FString& OutImagePath, FString& OutError);
		static bool RenderMaterialPreview(const FDreamShaderPreviewRequest& Request, FDreamShaderPreviewResult& OutResult);
		static void WritePreviewResult(const FDreamShaderPreviewResult& Result, const FString& Status, const FString& RequestId = FString());
	};
}
