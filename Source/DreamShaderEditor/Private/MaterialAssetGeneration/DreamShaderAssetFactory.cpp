#include "DreamShaderMaterialGeneratorPrivate.h"

#include "DreamShaderMaterialInstance.h"
#include "DreamShaderVersionCompat.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

namespace UE::DreamShader::Editor::Private
{
	namespace
	{
		bool AppendSanitizedPackageSegment(
			const FString& RawSegment,
			const FString& ErrorContext,
			FString& InOutPackagePath,
			FString& OutError)
		{
			FString Segment = RawSegment.TrimStartAndEnd();
			if (Segment.IsEmpty())
			{
				return true;
			}

			const FString FolderName = ObjectTools::SanitizeObjectName(Segment);
			if (FolderName.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s contains an invalid folder segment."), *ErrorContext);
				return false;
			}

			InOutPackagePath += TEXT("/");
			InOutPackagePath += FolderName;
			return true;
		}

		bool ResolveProjectContentPluginPackageRoot(
			const FString& Root,
			const FString& PluginName,
			FString& OutPackagePath,
			FString& OutError)
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
			if (!Plugin.IsValid())
			{
				OutError = FString::Printf(TEXT("DreamShader Root '%s' references project plugin '%s', but no enabled plugin with that name was found."), *Root, *PluginName);
				return false;
			}

			const FString PluginBaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
			const FString ProjectPluginsDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir());
			if (Plugin->GetType() != EPluginType::Project || !FPaths::IsUnderDirectory(PluginBaseDir, ProjectPluginsDir))
			{
				OutError = FString::Printf(TEXT("DreamShader Root '%s' must reference a project plugin under '%s'."), *Root, *ProjectPluginsDir);
				return false;
			}

			if (!Plugin->IsEnabled())
			{
				OutError = FString::Printf(TEXT("DreamShader Root '%s' references project plugin '%s', but the plugin is not enabled."), *Root, *PluginName);
				return false;
			}

			if (!Plugin->CanContainContent())
			{
				OutError = FString::Printf(TEXT("DreamShader Root '%s' references project plugin '%s', but the plugin cannot contain content."), *Root, *PluginName);
				return false;
			}

			const FString ContentDir = FPaths::ConvertRelativePathToFull(Plugin->GetContentDir());
			if (!IFileManager::Get().DirectoryExists(*ContentDir))
			{
				OutError = FString::Printf(TEXT("DreamShader Root '%s' references project plugin '%s', but its Content directory does not exist: '%s'."), *Root, *PluginName, *ContentDir);
				return false;
			}

#if DREAMSHADER_UE_VERSION_AT_LEAST(5, 6)
			if (!Plugin->IsMounted())
			{
				OutError = FString::Printf(TEXT("DreamShader Root '%s' references project plugin '%s', but the plugin content is not mounted."), *Root, *PluginName);
				return false;
			}
#endif

			FString MountedAssetPath = Plugin->GetMountedAssetPath();
			MountedAssetPath.TrimStartAndEndInline();
			MountedAssetPath.ReplaceInline(TEXT("\\"), TEXT("/"));
			while (MountedAssetPath.EndsWith(TEXT("/")))
			{
				MountedAssetPath.LeftChopInline(1, DREAMSHADER_ALLOW_SHRINKING_NO);
			}
			if (!MountedAssetPath.StartsWith(TEXT("/")))
			{
				MountedAssetPath = TEXT("/") + MountedAssetPath;
			}

			if (MountedAssetPath.IsEmpty() || MountedAssetPath == TEXT("/"))
			{
				MountedAssetPath = TEXT("/") + Plugin->GetName();
			}

			OutPackagePath = MountedAssetPath;
			return true;
		}

		bool ResolveDreamShaderRootPackagePath(
			const FString& Root,
			FString& OutPackagePath,
			FString& OutError)
		{
			FString Normalized = Root;
			Normalized.TrimStartAndEndInline();
			Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));

			if (Normalized.IsEmpty())
			{
				OutPackagePath = TEXT("/Game");
				return true;
			}

			const bool bHadLeadingSlash = Normalized.StartsWith(TEXT("/"));
			while (Normalized.StartsWith(TEXT("/")))
			{
				Normalized.RightChopInline(1, DREAMSHADER_ALLOW_SHRINKING_NO);
			}
			while (Normalized.EndsWith(TEXT("/")))
			{
				Normalized.LeftChopInline(1, DREAMSHADER_ALLOW_SHRINKING_NO);
			}

			if (Normalized.IsEmpty())
			{
				OutPackagePath = TEXT("/Game");
				return true;
			}

			TArray<FString> Segments;
			Normalized.ParseIntoArray(Segments, TEXT("/"), true);
			if (Segments.IsEmpty())
			{
				OutPackagePath = TEXT("/Game");
				return true;
			}

			const FString RootSegment = Segments[0].TrimStartAndEnd();
			int32 FirstFolderSegmentIndex = 1;
			if (RootSegment.Equals(TEXT("Game"), ESearchCase::IgnoreCase))
			{
				OutPackagePath = TEXT("/Game");
			}
			else if (RootSegment.StartsWith(TEXT("Plugin."), ESearchCase::IgnoreCase)
				|| RootSegment.StartsWith(TEXT("Plugins."), ESearchCase::IgnoreCase))
			{
				const int32 PluginPrefixLength = RootSegment.StartsWith(TEXT("Plugins."), ESearchCase::IgnoreCase) ? 8 : 7;
				FString PluginName = RootSegment.Mid(PluginPrefixLength).TrimStartAndEnd();
				if (PluginName.IsEmpty() || ObjectTools::SanitizeObjectName(PluginName) != PluginName)
				{
					OutError = FString::Printf(TEXT("DreamShader Root '%s' has an invalid plugin name."), *Root);
					return false;
				}

				if (!ResolveProjectContentPluginPackageRoot(Root, PluginName, OutPackagePath, OutError))
				{
					return false;
				}
			}
			else if ((RootSegment.Equals(TEXT("Plugin"), ESearchCase::IgnoreCase)
				|| RootSegment.Equals(TEXT("Plugins"), ESearchCase::IgnoreCase)) && Segments.IsValidIndex(1))
			{
				FString PluginName = Segments[1].TrimStartAndEnd();
				if (PluginName.IsEmpty() || ObjectTools::SanitizeObjectName(PluginName) != PluginName)
				{
					OutError = FString::Printf(TEXT("DreamShader Root '%s' has an invalid plugin name."), *Root);
					return false;
				}

				if (!ResolveProjectContentPluginPackageRoot(Root, PluginName, OutPackagePath, OutError))
				{
					return false;
				}
				FirstFolderSegmentIndex = 2;
			}
			else if (bHadLeadingSlash)
			{
				if (RootSegment.IsEmpty() || ObjectTools::SanitizeObjectName(RootSegment) != RootSegment)
				{
					OutError = FString::Printf(TEXT("DreamShader Root '%s' has an invalid package root."), *Root);
					return false;
				}

				OutPackagePath = TEXT("/") + RootSegment;
			}
			else
			{
				OutPackagePath = TEXT("/Game");
				FirstFolderSegmentIndex = 0;
			}

			for (int32 Index = FirstFolderSegmentIndex; Index < Segments.Num(); ++Index)
			{
				if (!AppendSanitizedPackageSegment(Segments[Index], FString::Printf(TEXT("DreamShader Root '%s'"), *Root), OutPackagePath, OutError))
				{
					return false;
				}
			}

			return true;
		}
	}

	bool ResolveDreamShaderAssetDestination(
		const FString& AssetName,
		const FString& Root,
		FString& OutPackageName,
		FString& OutObjectPath,
		FString& OutAssetLeafName,
		FString& OutError)
	{
		FString Normalized = AssetName;
		Normalized.TrimStartAndEndInline();
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Normalized.StartsWith(TEXT("/")))
		{
			Normalized.RightChopInline(1, DREAMSHADER_ALLOW_SHRINKING_NO);
		}
		while (Normalized.EndsWith(TEXT("/")))
		{
			Normalized.LeftChopInline(1, DREAMSHADER_ALLOW_SHRINKING_NO);
		}

		if (Normalized.IsEmpty())
		{
			OutError = TEXT("DreamShader asset name must resolve to a non-empty asset path.");
			return false;
		}

		TArray<FString> Segments;
		Normalized.ParseIntoArray(Segments, TEXT("/"), true);
		if (Segments.IsEmpty())
		{
			OutError = TEXT("DreamShader asset name must resolve to a non-empty asset path.");
			return false;
		}

		OutAssetLeafName = ObjectTools::SanitizeObjectName(Segments.Last());
		if (OutAssetLeafName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("DreamShader asset name '%s' produced an invalid asset name."), *AssetName);
			return false;
		}

		FString PackagePath;
		if (!ResolveDreamShaderRootPackagePath(Root, PackagePath, OutError))
		{
			return false;
		}

		for (int32 Index = 0; Index < Segments.Num() - 1; ++Index)
		{
			if (!AppendSanitizedPackageSegment(
				Segments[Index],
				FString::Printf(TEXT("DreamShader asset name '%s'"), *AssetName),
				PackagePath,
				OutError))
			{
				return false;
			}
		}

		OutPackageName = PackagePath + TEXT("/") + OutAssetLeafName;
		OutObjectPath = FString::Printf(TEXT("%s.%s"), *OutPackageName, *OutAssetLeafName);
		FText PathError;
		if (!FPackageName::IsValidObjectPath(OutObjectPath, &PathError))
		{
			const FString PathErrorText = PathError.ToString();
			OutError = PathErrorText.IsEmpty()
				? FString::Printf(TEXT("DreamShader asset path '%s' is not a valid Unreal object path."), *OutObjectPath)
				: PathErrorText;
			return false;
		}

		return true;
	}

	bool CreateOrReuseMaterial(const FTextShaderDefinition& Definition, UMaterial*& OutMaterial, FString& OutError, const bool bTransient)
	{
		FString PackageName;
		FString ObjectPath;
		FString AssetName;
		if (!ResolveDreamShaderAssetDestination(Definition.Name, Definition.Root, PackageName, ObjectPath, AssetName, OutError))
		{
			return false;
		}

		if (UObject* ExistingObject = LoadObject<UObject>(nullptr, *ObjectPath))
		{
			OutMaterial = Cast<UMaterial>(ExistingObject);
			if (!OutMaterial)
			{
				OutError = FString::Printf(TEXT("Asset '%s' already exists and is not a Material."), *ObjectPath);
				return false;
			}

			return true;
		}

		UPackage* MaterialPackage = CreatePackage(*PackageName);
		if (!MaterialPackage)
		{
			OutError = FString::Printf(TEXT("Failed to create package '%s'."), *PackageName);
			return false;
		}

		if (bTransient)
		{
			MaterialPackage->SetPackageFlags(PKG_NewlyCreated);
			OutMaterial = NewObject<UMaterial>(MaterialPackage, FName(*AssetName), RF_Public | RF_Standalone);
		}
		else
		{
			UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
			OutMaterial = Cast<UMaterial>(Factory->FactoryCreateNew(
				UMaterial::StaticClass(),
				MaterialPackage,
				FName(*AssetName),
				RF_Public | RF_Standalone,
				nullptr,
				GWarn));
		}

		if (!OutMaterial)
		{
			OutError = FString::Printf(TEXT("Failed to create material '%s'."), *ObjectPath);
			return false;
		}

		if (!bTransient)
		{
			FAssetRegistryModule::AssetCreated(OutMaterial);
		}
		return true;
	}

	bool CreateOrReuseInstanceMaterial(const FTextShaderDefinition& Definition, UDreamShaderMaterialInstance*& OutInstance, FString& OutError, const bool bTransient)
	{
		FString PackageName;
		FString ObjectPath;
		FString AssetName;
		if (!ResolveDreamShaderAssetDestination(Definition.Name, Definition.Root, PackageName, ObjectPath, AssetName, OutError))
		{
			return false;
		}

		if (UObject* ExistingObject = LoadObject<UObject>(nullptr, *ObjectPath))
		{
			OutInstance = Cast<UDreamShaderMaterialInstance>(ExistingObject);
			if (!OutInstance)
			{
				OutError = FString::Printf(
					TEXT("Asset '%s' already exists and is not a DreamShader instance material. Delete it (or remove Backend=\"Instance\") before switching backends."),
					*ObjectPath);
				return false;
			}

			return true;
		}

		UPackage* InstancePackage = CreatePackage(*PackageName);
		if (!InstancePackage)
		{
			OutError = FString::Printf(TEXT("Failed to create package '%s'."), *PackageName);
			return false;
		}

		if (bTransient)
		{
			InstancePackage->SetPackageFlags(PKG_NewlyCreated);
		}

		// Material instances don't support undo/redo (the shader map desyncs), so no RF_Transactional.
		OutInstance = NewObject<UDreamShaderMaterialInstance>(InstancePackage, FName(*AssetName), RF_Public | RF_Standalone);
		if (!OutInstance)
		{
			OutError = FString::Printf(TEXT("Failed to create instance material '%s'."), *ObjectPath);
			return false;
		}

		if (!bTransient)
		{
			FAssetRegistryModule::AssetCreated(OutInstance);
		}
		return true;
	}

	bool CreateOrReuseMaterialFunction(const FTextShaderMaterialFunctionDefinition& Definition, UMaterialFunction*& OutFunction, FString& OutError, const bool bTransient)
	{
		FString PackageName;
		FString ObjectPath;
		FString AssetName;
		if (!ResolveDreamShaderAssetDestination(Definition.Name, Definition.Root, PackageName, ObjectPath, AssetName, OutError))
		{
			return false;
		}

		UClass* ExpectedClass = UMaterialFunction::StaticClass();
		const TCHAR* ExpectedKindText = UE::DreamShader::LexToString(Definition.Kind);
		if (Definition.Kind == ETextShaderMaterialFunctionKind::MaterialLayer)
		{
			ExpectedClass = UMaterialFunctionMaterialLayer::StaticClass();
		}
		else if (Definition.Kind == ETextShaderMaterialFunctionKind::MaterialLayerBlend)
		{
			ExpectedClass = UMaterialFunctionMaterialLayerBlend::StaticClass();
		}

		if (UObject* ExistingObject = LoadObject<UObject>(nullptr, *ObjectPath))
		{
			const bool bClassMatches = Definition.Kind == ETextShaderMaterialFunctionKind::ShaderFunction
				? ExistingObject->GetClass() == ExpectedClass
				: ExistingObject->IsA(ExpectedClass);
			if (!bClassMatches)
			{
				OutError = FString::Printf(
					TEXT("Asset '%s' already exists as '%s', but %s generation requires '%s'. Delete or move the existing asset and regenerate it."),
					*ObjectPath,
					*ExistingObject->GetClass()->GetName(),
					ExpectedKindText,
					*ExpectedClass->GetName());
				return false;
			}

			OutFunction = Cast<UMaterialFunction>(ExistingObject);
			if (!OutFunction)
			{
				OutError = FString::Printf(TEXT("Asset '%s' already exists and is not a MaterialFunction asset."), *ObjectPath);
				return false;
			}

			return true;
		}

		UPackage* FunctionPackage = CreatePackage(*PackageName);
		if (!FunctionPackage)
		{
			OutError = FString::Printf(TEXT("Failed to create package '%s'."), *PackageName);
			return false;
		}

		const EObjectFlags ObjectFlags = RF_Public | RF_Standalone;

		if (bTransient)
		{
			FunctionPackage->SetPackageFlags(PKG_NewlyCreated);
		}

		OutFunction = Cast<UMaterialFunction>(NewObject<UObject>(
			FunctionPackage,
			ExpectedClass,
			FName(*AssetName),
			ObjectFlags));

		if (!OutFunction)
		{
			OutError = FString::Printf(TEXT("Failed to create material function '%s'."), *ObjectPath);
			return false;
		}

		if (!bTransient)
		{
			FAssetRegistryModule::AssetCreated(OutFunction);
		}
		return true;
	}
}
