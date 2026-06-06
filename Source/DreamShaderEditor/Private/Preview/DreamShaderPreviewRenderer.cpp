#include "Preview/DreamShaderPreviewRenderer.h"

#include "DreamShaderCompileService.h"
#include "DreamShaderModule.h"
#include "DreamShaderParser.h"
#include "Compile/DreamShaderEditorCompileAdapter.h"
#include "DependencyGraph/DreamShaderDependencyGraphService.h"
#include "MaterialAssetGeneration/DreamShaderMaterialGeneratorPrivate.h"

#include "AssetCompilingManager.h"
#include "CanvasTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineModule.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "LegacyScreenPercentageDriver.h"
#include "Materials/Material.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "SceneView.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ShaderCompiler.h"
#include "ThumbnailHelpers.h"

namespace UE::DreamShader::Editor::Private
{
	namespace
	{
		FString BuildPreviewImagePath(const FString& SourceFilePath)
		{
			FString RelativePath = SourceFilePath;
			FPaths::MakePathRelativeTo(RelativePath, *FPaths::ProjectDir());
			RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
			const FString StableKey = RelativePath.IsEmpty() ? SourceFilePath : RelativePath;
			const FString FileStem = ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(SourceFilePath));
			const FString Hash = FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*StableKey));
			return FPaths::Combine(
				FDreamShaderPreviewRenderer::GetPreviewDirectory(),
				FString::Printf(TEXT("%s-%s.png"), FileStem.IsEmpty() ? TEXT("DreamShaderPreview") : *FileStem, *Hash));
		}

		bool ResolveGeneratedMaterialPath(const FString& SourceFilePath, FString& OutObjectPath, FString& OutError)
		{
			FString SourceText;
			if (!FFileHelper::LoadFileToString(SourceText, *SourceFilePath))
			{
				OutError = FString::Printf(TEXT("Failed to read DreamShader source '%s'."), *SourceFilePath);
				return false;
			}

			TArray<FString> Lines;
			SourceText.ParseIntoArrayLines(Lines, false);
			SourceText.Reset();
			for (const FString& Line : Lines)
			{
				FString ImportPath;
				if (FDreamShaderDependencyGraphService::TryExtractImportPathFromLine(Line, ImportPath))
				{
					continue;
				}
				SourceText += Line;
				SourceText += TEXT("\n");
			}

			FTextShaderDefinition Definition;
			FString ParseError;
			if (!FTextShaderParser::Parse(SourceText, Definition, ParseError))
			{
				OutError = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *ParseError);
				return false;
			}

			if (Definition.Name.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s: This file does not define a top-level Shader block."), *SourceFilePath);
				return false;
			}

			FString PackageName;
			FString AssetName;
			return ResolveDreamShaderAssetDestination(Definition.Name, Definition.Root, PackageName, OutObjectPath, AssetName, OutError);
		}

		void WaitForPreviewMaterialCompilation(UMaterial* Material)
		{
			TArray<UObject*> MaterialObjects;
			MaterialObjects.Add(Material);
			FAssetCompilingManager::Get().FinishCompilationForObjects(MaterialObjects);
			if (GShaderCompilingManager)
			{
				GShaderCompilingManager->FinishAllCompilation();
			}
			FlushRenderingCommands();
		}

		bool BuildThumbnailPng(UMaterial* Material, const int32 Width, const int32 Height, const bool bWaitForCompilation, TArray64<uint8>& OutPngData, FString& OutError)
		{
			OutPngData.Reset();
			if (!Material)
			{
				OutError = TEXT("Preview material is not valid.");
				return false;
			}

			if (bWaitForCompilation)
			{
				WaitForPreviewMaterialCompilation(Material);
			}

			const int32 PreviewWidth = FMath::Clamp(Width, 64, 2048);
			const int32 PreviewHeight = FMath::Clamp(Height, 64, 2048);
			UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
			if (!RenderTarget)
			{
				OutError = TEXT("Failed to create preview render target.");
				return false;
			}
			RenderTarget->ClearColor = FLinearColor(0.025f, 0.025f, 0.03f, 1.0f);
			RenderTarget->InitCustomFormat(PreviewWidth, PreviewHeight, PF_B8G8R8A8, false);
			RenderTarget->UpdateResourceImmediate(true);

			FTextureRenderTargetResource* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
			if (!RenderTargetResource)
			{
				OutError = TEXT("Failed to access preview render target resource.");
				return false;
			}

			FMaterialThumbnailScene ThumbnailScene;
			ThumbnailScene.SetMaterialInterface(Material);
			FCanvas Canvas(RenderTargetResource, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
			Canvas.Clear(RenderTarget->ClearColor);

			FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
				RenderTargetResource,
				ThumbnailScene.GetScene(),
				FEngineShowFlags(ESFIM_Game))
				.SetTime(FGameTime::GetTimeSinceAppStart()));
			ViewFamily.EngineShowFlags.MotionBlur = 0;
			ViewFamily.EngineShowFlags.AntiAliasing = 0;
			ViewFamily.EngineShowFlags.SetSeparateTranslucency(ThumbnailScene.ShouldSetSeparateTranslucency(Material));

			FSceneView* View = ThumbnailScene.CreateView(&ViewFamily, 0, 0, PreviewWidth, PreviewHeight);
			if (!View)
			{
				OutError = FString::Printf(TEXT("Failed to create preview view for '%s'."), *Material->GetPathName());
				return false;
			}

			ViewFamily.EngineShowFlags.ScreenPercentage = false;
			ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
				ViewFamily,
				1.0f));
			GetRendererModule().BeginRenderingViewFamily(&Canvas, &ViewFamily);
			Canvas.Flush_GameThread();
			FlushRenderingCommands();

			TArray<FColor> Colors;
			if (!RenderTargetResource->ReadPixels(Colors) || Colors.Num() != PreviewWidth * PreviewHeight)
			{
				OutError = FString::Printf(TEXT("Failed to read preview pixels for '%s'."), *Material->GetPathName());
				return false;
			}
			for (FColor& Color : Colors)
			{
				Color.A = 255;
			}
			ThumbnailScene.SetMaterialInterface(nullptr);

			FImageUtils::PNGCompressImageArray(
				PreviewWidth,
				PreviewHeight,
				TArrayView64<const FColor>(Colors.GetData(), Colors.Num()),
				OutPngData);
			if (OutPngData.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Failed to encode preview thumbnail for '%s'."), *Material->GetPathName());
				return false;
			}

			return true;
		}

		bool SaveThumbnailPng(UMaterial* Material, const int32 Width, const int32 Height, const FString& ImagePath, FString& OutError)
		{
			TArray64<uint8> PngData;
			if (!BuildThumbnailPng(Material, Width, Height, true, PngData, OutError))
			{
				return false;
			}

			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ImagePath), true);
			if (!FFileHelper::SaveArrayToFile(TArrayView64<const uint8>(PngData.GetData(), PngData.Num()), *ImagePath))
			{
				OutError = FString::Printf(TEXT("Failed to write preview image '%s'."), *ImagePath);
				return false;
			}

			return true;
		}
	}

	FString FDreamShaderPreviewRenderer::GetPreviewDirectory()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamShader"), TEXT("Bridge"), TEXT("Preview"));
	}

	FString FDreamShaderPreviewRenderer::GetPreviewManifestPath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamShader"), TEXT("Bridge"), TEXT("preview.json"));
	}

	bool FDreamShaderPreviewRenderer::ResolvePreviewMaterial(const FDreamShaderPreviewRequest& Request, FDreamShaderPreviewResult& OutResult, UMaterial*& OutMaterial)
	{
		OutMaterial = nullptr;
		const FString SourceFilePath = UE::DreamShader::NormalizeSourceFilePath(Request.SourceFilePath);
		OutResult = FDreamShaderPreviewResult();
		OutResult.SourceFilePath = SourceFilePath;
		OutResult.Mesh = Request.Mesh.IsEmpty() ? TEXT("sphere") : Request.Mesh;

		if (SourceFilePath.IsEmpty() || !IFileManager::Get().FileExists(*SourceFilePath))
		{
			OutResult.Message = FString::Printf(TEXT("DreamShader source '%s' does not exist."), *SourceFilePath);
			return false;
		}

		if (!UE::DreamShader::IsDreamShaderMaterialFile(SourceFilePath))
		{
			OutResult.Message = FString::Printf(TEXT("DreamShader preview only supports .dsm material files: '%s'."), *SourceFilePath);
			return false;
		}

		FString ObjectPath;
		FString ResolveError;
		if (!ResolveGeneratedMaterialPath(SourceFilePath, ObjectPath, ResolveError))
		{
			OutResult.Message = ResolveError;
			return false;
		}
		OutResult.AssetPath = ObjectPath;

		UE::DreamShader::Compiler::FDreamShaderCompileService CompileService(UE::DreamShader::Editor::GetEditorCompileAdapter());
		const UE::DreamShader::Compiler::FDreamShaderCompileResult CompileResult = CompileService.CompileMaterial(SourceFilePath, true);
		if (!CompileResult.bSucceeded)
		{
			OutResult.Message = CompileResult.Message;
			return false;
		}

		OutMaterial = LoadObject<UMaterial>(nullptr, *ObjectPath);
		if (!OutMaterial)
		{
			OutResult.Message = FString::Printf(TEXT("Generated material '%s' could not be loaded."), *ObjectPath);
			return false;
		}

		OutResult.bSucceeded = true;
		OutResult.Message = FString::Printf(TEXT("Compiled preview material for %s."), *ObjectPath);
		if (!CompileResult.Message.IsEmpty())
		{
			OutResult.Message += TEXT(" ");
			OutResult.Message += CompileResult.Message;
		}
		return true;
	}

	bool FDreamShaderPreviewRenderer::RenderMaterialPreviewFrame(UMaterial* Material, int32 Width, int32 Height, TArray64<uint8>& OutPngData, FString& OutError)
	{
		return BuildThumbnailPng(Material, Width, Height, false, OutPngData, OutError);
	}

	bool FDreamShaderPreviewRenderer::SaveMaterialPreviewFrame(UMaterial* Material, const FString& SourceFilePath, int32 Width, int32 Height, FString& OutImagePath, FString& OutError)
	{
		OutImagePath = BuildPreviewImagePath(SourceFilePath);
		return SaveThumbnailPng(Material, Width, Height, OutImagePath, OutError);
	}

	bool FDreamShaderPreviewRenderer::RenderMaterialPreview(const FDreamShaderPreviewRequest& Request, FDreamShaderPreviewResult& OutResult)
	{
		UMaterial* Material = nullptr;
		if (!ResolvePreviewMaterial(Request, OutResult, Material))
		{
			return false;
		}

		FString RenderError;
		FString ImagePath;
		if (!SaveMaterialPreviewFrame(Material, OutResult.SourceFilePath, Request.Width, Request.Height, ImagePath, RenderError))
		{
			OutResult.Message = RenderError;
			return false;
		}

		OutResult.bSucceeded = true;
		OutResult.ImagePath = ImagePath;
		OutResult.Message = FString::Printf(TEXT("Rendered preview for %s."), *OutResult.AssetPath);
		return true;
	}

	void FDreamShaderPreviewRenderer::WritePreviewResult(const FDreamShaderPreviewResult& Result, const FString& Status, const FString& RequestId)
	{
		TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetNumberField(TEXT("version"), 1);
		if (!RequestId.IsEmpty())
		{
			RootObject->SetStringField(TEXT("requestId"), RequestId);
		}
		RootObject->SetStringField(TEXT("status"), Status);
		RootObject->SetStringField(TEXT("sourceFile"), Result.SourceFilePath);
		RootObject->SetStringField(TEXT("assetPath"), Result.AssetPath);
		RootObject->SetStringField(TEXT("imagePath"), Result.ImagePath);
		RootObject->SetStringField(TEXT("mesh"), Result.Mesh);
		RootObject->SetStringField(TEXT("message"), Result.Message);
		RootObject->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());

		FString OutputText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputText);
		if (FJsonSerializer::Serialize(RootObject, Writer))
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(GetPreviewManifestPath()), true);
			FFileHelper::SaveStringToFile(OutputText, *GetPreviewManifestPath());
		}
	}
}
