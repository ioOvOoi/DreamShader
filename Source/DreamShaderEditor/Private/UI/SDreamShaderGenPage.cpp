#include "UI/SDreamShaderGenPage.h"

#include "UI/DreamShaderGeneratedAssetPath.h"
#include "UI/DreamShaderInstanceFactory.h"

#include "DependencyGraph/DreamShaderDependencyGraphService.h"
#include "Materials/MaterialInterface.h"

#include "DreamShaderModule.h"
#include "MaterialAssetGeneration/DreamShaderMaterialGenerator.h"
#include "MaterialAssetGeneration/DreamShaderMaterialGeneratorPrivate.h"
#include "MaterialAssetGeneration/DreamShaderMaterialGeneratorSourceLoading.h"
#include "SourceFiles/DreamShaderSourceFileUtils.h"
#include "Workspace/DreamShaderWorkspaceService.h"

#include "AssetThumbnail.h"
#include "Editor.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "DreamShaderMaterialBrowser"

namespace UE::DreamShader::Editor::Private
{
	namespace
	{
		void NotifyGen(const FText& Message, bool bSuccess)
		{
			FNotificationInfo Info(Message);
			Info.ExpireDuration = 3.5f;
			Info.bFireAndForget = true;
			TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
			if (Item.IsValid())
			{
				Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
			}
		}

		struct FStatusVisual
		{
			FText Glyph;
			FLinearColor Color;
			FText Label;
		};

		FStatusVisual GetStatusVisual(FDreamShaderSourceItem::EStatus Status)
		{
			switch (Status)
			{
			case FDreamShaderSourceItem::EStatus::UpToDate:
				return { FText::FromString(TEXT("●")), FLinearColor(0.10f, 0.62f, 0.20f), LOCTEXT("StatusUpToDate", "up to date") };
			case FDreamShaderSourceItem::EStatus::Stale:
				return { FText::FromString(TEXT("●")), FLinearColor(0.90f, 0.62f, 0.12f), LOCTEXT("StatusStale", "stale") };
			case FDreamShaderSourceItem::EStatus::NeverCompiled:
				return { FText::FromString(TEXT("○")), FLinearColor(0.50f, 0.50f, 0.50f), LOCTEXT("StatusNever", "not compiled") };
			case FDreamShaderSourceItem::EStatus::Function:
				return { FText::FromString(TEXT("◆")), FLinearColor(0.30f, 0.52f, 0.82f), LOCTEXT("StatusFunction", "function / header") };
			default:
				return { FText::FromString(TEXT("▲")), FLinearColor(0.82f, 0.24f, 0.22f), LOCTEXT("StatusUnresolved", "unresolved") };
			}
		}
	}

	void SDreamShaderGenPage::Construct(const FArguments& InArgs)
	{
		ThumbnailPool = MakeShared<FAssetThumbnailPool>(8);
		RegisterActiveTimer(0.05f, FWidgetActiveTimerDelegate::CreateLambda(
			[WeakPool = TWeakPtr<FAssetThumbnailPool>(ThumbnailPool)](double, float DeltaTime)
			{
				if (TSharedPtr<FAssetThumbnailPool> Pool = WeakPool.Pin())
				{
					Pool->Tick(DeltaTime);
				}
				return EActiveTimerReturnType::Continue;
			}));

		ChildSlot
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::Get().GetBrush("Brushes.Header"))
				.Padding(FMargin(8.0f, 5.0f))
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SButton)
						.Text(LOCTEXT("Refresh", "Refresh"))
						.ToolTipText(LOCTEXT("RefreshTip", "Rescan the source directory and recompute status."))
						.OnClicked_Lambda([this]() { Refresh(); return FReply::Handled(); })
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(10.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text_Lambda([this]()
						{
							return FText::Format(LOCTEXT("SourceCount", "{0} source file(s)"), FText::AsNumber(Items.Num()));
						})
					]
				]
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Horizontal)

				+ SSplitter::Slot()
				.Value(0.4f)
				[
					SAssignNew(PreviewContainer, SBorder)
					.BorderImage(FAppStyle::Get().GetBrush("Brushes.Recessed"))
					.Padding(FMargin(12.0f))
				]

				+ SSplitter::Slot()
				.Value(0.6f)
				[
					SAssignNew(ListView, SListView<TSharedPtr<FDreamShaderSourceItem>>)
					.ListItemsSource(&Items)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SDreamShaderGenPage::OnGenerateRow)
					.OnSelectionChanged(this, &SDreamShaderGenPage::OnSelectionChanged)
				]
			]
		];

		Refresh();
	}

	void SDreamShaderGenPage::Refresh()
	{
		Items.Reset();

		TArray<FString> SourceFiles;
		FDreamShaderSourceFileUtils::FindProjectDreamShaderSourceFiles(SourceFiles);
		SourceFiles.Sort();

		// Which materials import each header/function file -- for the "used by N materials" column.
		TMap<FString, TSet<FString>> DependentsByFile;
		FDreamShaderDependencyGraphService::RebuildMaterialDependencyGraph(DependentsByFile);

		for (const FString& SourceFile : SourceFiles)
		{
			TSharedPtr<FDreamShaderSourceItem> Item = MakeShared<FDreamShaderSourceItem>();
			Item->SourceFilePath = UE::DreamShader::NormalizeSourceFilePath(SourceFile);
			Item->DisplayName = FPaths::GetCleanFilename(Item->SourceFilePath);
			Item->bIsFunction = UE::DreamShader::IsDreamShaderFunctionFile(Item->SourceFilePath)
				|| UE::DreamShader::IsDreamShaderHeaderFile(Item->SourceFilePath);
			if (Item->bIsFunction)
			{
				if (const TSet<FString>* Dependents = DependentsByFile.Find(Item->SourceFilePath))
				{
					Item->DependentCount = Dependents->Num();
				}
			}
			RefreshItemStatus(Item);
			Items.Add(Item);
		}

		// The old selected item is no longer in the rebuilt list.
		SelectedItem.Reset();
		if (ListView.IsValid())
		{
			ListView->RequestListRefresh();
		}
		RebuildPreview();
	}

	void SDreamShaderGenPage::RefreshItemStatus(const TSharedPtr<FDreamShaderSourceItem>& Item) const
	{
		if (!Item.IsValid())
		{
			return;
		}

		if (Item->bIsFunction)
		{
			Item->Status = FDreamShaderSourceItem::EStatus::Function;
			Item->StatusDetail = LOCTEXT("FunctionDetail", "Function library / header. Recompiles the materials that import it.").ToString();
			return;
		}

		FString Error;
		if (!ResolveGeneratedAssetObjectPath(Item->SourceFilePath, Item->ObjectPath, Error))
		{
			Item->Status = FDreamShaderSourceItem::EStatus::Unresolved;
			Item->StatusDetail = Error;
			return;
		}

		UObject* Asset = FindObject<UObject>(nullptr, *Item->ObjectPath);
		if (!Asset)
		{
			Item->Status = FDreamShaderSourceItem::EStatus::NeverCompiled;
			Item->StatusDetail = FString::Printf(TEXT("No generated asset at %s"), *Item->ObjectPath);
			return;
		}

		FString PreparedText;
		FString LoadError;
		if (!UE::DreamShader::Editor::LoadPreparedDreamShaderSource(Item->SourceFilePath, PreparedText, LoadError))
		{
			Item->Status = FDreamShaderSourceItem::EStatus::Unresolved;
			Item->StatusDetail = LoadError;
			return;
		}

		const FString SourceHash = BuildSourceHash(PreparedText);
		const bool bCurrent = IsGeneratedAssetSourceCurrent(Asset, Item->SourceFilePath, SourceHash);
		Item->Status = bCurrent ? FDreamShaderSourceItem::EStatus::UpToDate : FDreamShaderSourceItem::EStatus::Stale;
		Item->StatusDetail = Item->ObjectPath;
	}

	void SDreamShaderGenPage::OnSelectionChanged(TSharedPtr<FDreamShaderSourceItem> Item, ESelectInfo::Type)
	{
		SelectedItem = Item;
		RebuildPreview();
	}

	void SDreamShaderGenPage::RebuildPreview()
	{
		if (PreviewContainer.IsValid())
		{
			PreviewContainer->SetContent(BuildPreview(SelectedItem));
		}
	}

	TSharedRef<SWidget> SDreamShaderGenPage::BuildPreview(TSharedPtr<FDreamShaderSourceItem> Item)
	{
		if (!Item.IsValid())
		{
			return SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PreviewNone", "Select a source file to preview its material."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
		}

		UMaterialInterface* Material = (!Item->bIsFunction && !Item->ObjectPath.IsEmpty())
			? LoadObject<UMaterialInterface>(nullptr, *Item->ObjectPath)
			: nullptr;

		const FStatusVisual Visual = GetStatusVisual(Item->Status);

		// Thumbnail, or a placeholder tile when there is no compiled material to render.
		TSharedRef<SWidget> ThumbWidget = SNullWidget::NullWidget;
		if (Material)
		{
			PreviewThumbnail = MakeShared<FAssetThumbnail>(Material, 160, 160, ThumbnailPool);
			ThumbWidget = PreviewThumbnail->MakeThumbnailWidget();
		}
		else
		{
			PreviewThumbnail.Reset();
			ThumbWidget = SNew(SBorder)
				.BorderImage(FAppStyle::Get().GetBrush("Brushes.Header"))
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Item->bIsFunction ? LOCTEXT("PreviewFunction", "function library") : LOCTEXT("PreviewNoMaterial", "not compiled yet"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				];
		}

		const TSharedPtr<FDreamShaderSourceItem> ItemRef = Item;
		const auto MakeActionButton = [this, ItemRef](const FText& Label, const FText& Tip, bool bEnabled, TFunction<void()> Action) -> TSharedRef<SWidget>
		{
			return SNew(SButton)
				.Text(Label)
				.ToolTipText(Tip)
				.IsEnabled(bEnabled)
				.OnClicked_Lambda([Action]() { Action(); return FReply::Handled(); });
		};

		return SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				SNew(SBox).WidthOverride(160.0f).HeightOverride(160.0f)[ ThumbWidget ]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock).Text(FText::FromString(Item->DisplayName)).TextStyle(FAppStyle::Get(), "LargeText").AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(STextBlock).Text(Visual.Glyph).ColorAndOpacity(FSlateColor(Visual.Color))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(Visual.Label).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock).Text(FText::FromString(Item->SourceFilePath)).TextStyle(FAppStyle::Get(), "SmallText")
				.ColorAndOpacity(FSlateColor::UseSubduedForeground()).AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				SNew(STextBlock)
				.Visibility(Item->bIsFunction ? EVisibility::Visible : EVisibility::Collapsed)
				.Text(FText::Format(LOCTEXT("PreviewUsedBy", "used by {0} material(s)"), FText::AsNumber(Item->DependentCount)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 4.0f))
				+ SWrapBox::Slot()[ MakeActionButton(LOCTEXT("PComp", "Compile"), LOCTEXT("PCompTip", "Force-recompile this source (in memory)."), true, [this, ItemRef]() { CompileItem(ItemRef); }) ]
				+ SWrapBox::Slot()[ MakeActionButton(LOCTEXT("PInst", "Create instance"), LOCTEXT("PInstTip", "Create a material instance of this material."), !ItemRef->bIsFunction, [this, ItemRef]() { CreateInstanceForItem(ItemRef); }) ]
				+ SWrapBox::Slot()[ MakeActionButton(LOCTEXT("POpenMat", "Open material"), LOCTEXT("POpenMatTip", "Open the generated material asset."), Material != nullptr, [this, ItemRef]() { OpenItemMaterial(ItemRef); }) ]
				+ SWrapBox::Slot()[ MakeActionButton(LOCTEXT("POpenSrc", "Open source"), LOCTEXT("POpenSrcTip", "Open the .dsm/.dsf in your preferred editor."), true, [this, ItemRef]() { OpenItemSource(ItemRef); }) ]
			];
	}

	void SDreamShaderGenPage::OpenItemMaterial(TSharedPtr<FDreamShaderSourceItem> Item)
	{
		if (!Item.IsValid() || Item->ObjectPath.IsEmpty())
		{
			return;
		}
		if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Item->ObjectPath))
		{
			if (GEditor)
			{
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Material);
			}
		}
	}

	TSharedRef<ITableRow> SDreamShaderGenPage::OnGenerateRow(TSharedPtr<FDreamShaderSourceItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
	{
		const FStatusVisual Visual = GetStatusVisual(Item->Status);
		const FText SubLabel = Item->bIsFunction
			? FText::Format(LOCTEXT("FunctionUsedBy", "function · used by {0} material(s)"), FText::AsNumber(Item->DependentCount))
			: Visual.Label;

		return SNew(STableRow<TSharedPtr<FDreamShaderSourceItem>>, OwnerTable)
			.Padding(FMargin(4.0f, 3.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(Visual.Glyph)
					.ColorAndOpacity(FSlateColor(Visual.Color))
					.ToolTipText(FText::FromString(Item->StatusDetail))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString(Item->DisplayName))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.TextStyle(FAppStyle::Get(), "SmallText")
						.Text(SubLabel)
					]
				]
			];
	}

	void SDreamShaderGenPage::CompileItem(TSharedPtr<FDreamShaderSourceItem> Item)
	{
		if (!Item.IsValid())
		{
			return;
		}

		FString Message;
		const bool bSuccess = UE::DreamShader::Editor::FMaterialGenerator::GenerateAssetsFromFile(
			Item->SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);

		NotifyGen(
			FText::Format(
				bSuccess ? LOCTEXT("CompileOk", "Compiled {0}") : LOCTEXT("CompileFail", "Failed to compile {0}"),
				FText::FromString(Item->DisplayName)),
			bSuccess);

		if (!bSuccess)
		{
			UE_LOG(LogDreamShader, Error, TEXT("Material Content Browser compile failed: %s"), *Message);
		}

		RefreshItemStatus(Item);
		if (ListView.IsValid())
		{
			ListView->RequestListRefresh();
		}
		if (SelectedItem == Item)
		{
			RebuildPreview();
		}
	}

	void SDreamShaderGenPage::OpenItemSource(TSharedPtr<FDreamShaderSourceItem> Item)
	{
		if (Item.IsValid())
		{
			FDreamShaderEditorLaunchUtils::LaunchTextFileInPreferredEditor(Item->SourceFilePath);
		}
	}

	void SDreamShaderGenPage::CreateInstanceForItem(TSharedPtr<FDreamShaderSourceItem> Item)
	{
		if (!Item.IsValid() || Item->bIsFunction)
		{
			return;
		}

		// Make sure the source has been generated so there is a parent to instance from.
		UMaterialInterface* Material = Item->ObjectPath.IsEmpty()
			? nullptr
			: LoadObject<UMaterialInterface>(nullptr, *Item->ObjectPath);
		if (!Material)
		{
			FString Message;
			if (UE::DreamShader::Editor::FMaterialGenerator::GenerateAssetsFromFile(Item->SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true))
			{
				RefreshItemStatus(Item);
				Material = Item->ObjectPath.IsEmpty() ? nullptr : LoadObject<UMaterialInterface>(nullptr, *Item->ObjectPath);
			}
		}

		if (Material)
		{
			OpenCreateInstanceDialog(Material);
		}
		else
		{
			NotifyGen(FText::Format(LOCTEXT("InstanceNeedsCompile", "Compile {0} first."), FText::FromString(Item->DisplayName)), false);
		}
	}
}

#undef LOCTEXT_NAMESPACE
