#pragma once

#include "AssetRegistry/AssetData.h"
#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "Widgets/SCompoundWidget.h"

namespace UE::DreamShader::Editor::Private
{
	// Project tab: a browsable, filterable grid of every material / material instance under /Game, with
	// thumbnails, backed by the engine content-browser asset picker, plus a "create instance" action on
	// the selected material. The inheritance detail panel is layered on in a later milestone.
	class SDreamShaderProjectPage : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SDreamShaderProjectPage) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs);

	private:
		FAssetData SelectedAsset;

		void OnAssetSelected(const FAssetData& AssetData);
		FReply OnCreateInstanceClicked();
	};
}
