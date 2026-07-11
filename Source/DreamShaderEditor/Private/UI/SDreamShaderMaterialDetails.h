#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class SBorder;
class UMaterialInterface;
class FAssetThumbnail;
class FAssetThumbnailPool;

namespace UE::DreamShader::Editor::Private
{
	// The Project page's right-hand detail panel: for the selected material it shows the base surface
	// settings, the DreamShader source (if any), the full inheritance chain (hidden base -> root instance
	// -> the selection -> its own children), and the create-instance / materialize actions. This is the
	// view that makes the ThinCustom base <-> thin MIC <-> variant relationship legible.
	class SDreamShaderMaterialDetails : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SDreamShaderMaterialDetails) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs);

		void SetMaterial(UMaterialInterface* InMaterial);

	private:
		TWeakObjectPtr<UMaterialInterface> CurrentMaterial;
		TSharedPtr<SBorder> ContentContainer;
		TSharedPtr<FAssetThumbnailPool> ThumbnailPool;
		TSharedPtr<FAssetThumbnail> CurrentThumbnail;

		void Rebuild();
		TSharedRef<SWidget> BuildContent(UMaterialInterface* Material);
	};
}
