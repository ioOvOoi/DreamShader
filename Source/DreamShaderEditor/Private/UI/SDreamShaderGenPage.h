#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

template <typename ItemType> class SListView;
class ITableRow;
class STableViewBase;

namespace UE::DreamShader::Editor::Private
{
	// One source-file row in the Dream Shader Gen page.
	struct FDreamShaderSourceItem
	{
		enum class EStatus : uint8
		{
			UpToDate,      // generated asset exists and its stored source hash matches the current source
			Stale,         // generated asset exists but the source changed since it was compiled
			NeverCompiled, // no generated asset found (never run, or generation failed)
			Function,      // .dsf/.dsh -- a function/header, not a top-level material
			Unresolved,    // could not read/parse the source to determine a target
		};

		FString SourceFilePath;   // absolute, normalized
		FString DisplayName;      // clean filename with extension
		FString ObjectPath;       // resolved /Game object path (empty if unresolved / a function)
		FString StatusDetail;     // tooltip detail (error/parse message, or object path)
		EStatus Status = EStatus::NeverCompiled;
		bool bIsFunction = false; // .dsf or .dsh
		int32 DependentCount = 0; // for functions/headers: how many materials import this file
	};

	// Source-file-centric view of the DreamShader generation pipeline: every .dsm/.dsf/.dsh under the
	// source directory, its compile status, and per-row compile / open-source actions.
	class SDreamShaderGenPage : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SDreamShaderGenPage) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs);

	private:
		TArray<TSharedPtr<FDreamShaderSourceItem>> Items;
		TSharedPtr<SListView<TSharedPtr<FDreamShaderSourceItem>>> ListView;

		void Refresh();
		void RefreshItemStatus(const TSharedPtr<FDreamShaderSourceItem>& Item) const;

		TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FDreamShaderSourceItem> Item, const TSharedRef<STableViewBase>& OwnerTable);

		void CompileItem(TSharedPtr<FDreamShaderSourceItem> Item);
		void OpenItemSource(TSharedPtr<FDreamShaderSourceItem> Item);
		void CreateInstanceForItem(TSharedPtr<FDreamShaderSourceItem> Item);
	};
}
