#include "UI/SDreamShaderProjectPage.h"

#include "UI/DreamShaderInstanceFactory.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "IContentBrowserSingleton.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "DreamShaderMaterialBrowser"

namespace UE::DreamShader::Editor::Private
{
	namespace
	{
		void OpenAssetForEditing(const FAssetData& AssetData)
		{
			if (!GEditor)
			{
				return;
			}
			if (UObject* Asset = AssetData.GetAsset())
			{
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
			}
		}
	}

	void SDreamShaderProjectPage::Construct(const FArguments& InArgs)
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

		FAssetPickerConfig PickerConfig;
		PickerConfig.Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
		PickerConfig.Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
		// Recursive so UDreamShaderMaterialInstance (a UMaterialInstanceConstant subclass) is included.
		PickerConfig.Filter.bRecursiveClasses = true;
		PickerConfig.Filter.PackagePaths.Add("/Game");
		PickerConfig.Filter.bRecursivePaths = true;
		PickerConfig.InitialAssetViewType = EAssetViewType::Tile;
		PickerConfig.bAllowDragging = true;
		PickerConfig.bCanShowClasses = false;
		PickerConfig.bForceShowEngineContent = false;
		PickerConfig.bShowPathInColumnView = true;
		PickerConfig.SelectionMode = ESelectionMode::Single;
		PickerConfig.OnAssetSelected = FOnAssetSelected::CreateSP(this, &SDreamShaderProjectPage::OnAssetSelected);
		PickerConfig.OnAssetDoubleClicked = FOnAssetDoubleClicked::CreateStatic(&OpenAssetForEditing);
		PickerConfig.AssetShowWarningText = LOCTEXT("EmptyProject", "No materials found under /Game.");

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
					[
						SNew(SButton)
						.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
						.Text(LOCTEXT("CreateInstance", "Create instance"))
						.ToolTipText(LOCTEXT("CreateInstanceTip", "Create a material instance of the selected material (shares its compiled shader map)."))
						.OnClicked(this, &SDreamShaderProjectPage::OnCreateInstanceClicked)
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
							return SelectedAsset.IsValid()
								? FText::Format(LOCTEXT("SelectedFmt", "Selected: {0}"), FText::FromName(SelectedAsset.AssetName))
								: LOCTEXT("NoneSelected", "Select a material, then create an instance.");
						})
					]
				]
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				ContentBrowserModule.Get().CreateAssetPicker(PickerConfig)
			]
		];
	}

	void SDreamShaderProjectPage::OnAssetSelected(const FAssetData& AssetData)
	{
		SelectedAsset = AssetData;
	}

	FReply SDreamShaderProjectPage::OnCreateInstanceClicked()
	{
		if (UMaterialInterface* Material = Cast<UMaterialInterface>(SelectedAsset.GetAsset()))
		{
			OpenCreateInstanceDialog(Material);
		}
		else
		{
			OpenCreateInstanceDialog(nullptr); // shows the "select a material first" toast
		}
		return FReply::Handled();
	}
}

#undef LOCTEXT_NAMESPACE
