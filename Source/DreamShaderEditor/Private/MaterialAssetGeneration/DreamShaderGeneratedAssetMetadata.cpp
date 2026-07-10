#include "DreamShaderMaterialGeneratorPrivate.h"

#include "DreamShaderModule.h"
#include "DreamShaderVersionCompat.h"

#include "FileHelpers.h"
#include "Misc/Crc.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

namespace UE::DreamShader::Editor::Private
{
	namespace
	{
		FString GetSourceMetadataValue(UObject* Asset, const TCHAR* Key)
		{
			if (!Asset)
			{
				return FString();
			}

			UPackage* Package = Asset->GetOutermost();
			if (!Package)
			{
				return FString();
			}

#if DREAMSHADER_UE_VERSION_AT_LEAST(5, 6)
			return Package->GetMetaData().GetValue(Asset, Key);
#else
			if (UMetaData* MetaData = Package->GetMetaData())
			{
				return MetaData->GetValue(Asset, Key);
			}
			return FString();
#endif
		}
	}

	FString BuildSourceHash(const FString& SourceText)
	{
		return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*SourceText));
	}

	bool IsGeneratedAssetSourceCurrent(UObject* Asset, const FString& SourceFilePath, const FString& SourceHash)
	{
		if (!Asset || SourceHash.IsEmpty())
		{
			return false;
		}

		const FString ExistingSourceFileRaw = GetSourceMetadataValue(Asset, TEXT("DreamShader.SourceFile"));
		if (ExistingSourceFileRaw.IsEmpty())
		{
			return false;
		}

		const FString ExistingSourceFile = UE::DreamShader::NormalizeSourceFilePath(ExistingSourceFileRaw);
		const FString ExistingSourceHash = GetSourceMetadataValue(Asset, TEXT("DreamShader.SourceHash"));

		return ExistingSourceFile.Equals(UE::DreamShader::NormalizeSourceFilePath(SourceFilePath), ESearchCase::IgnoreCase)
			&& ExistingSourceHash.Equals(SourceHash, ESearchCase::CaseSensitive);
	}

	bool HasDreamShaderSourceMetadata(UObject* Asset)
	{
		// DreamShader stamps DreamShader.SourceFile on every asset it generates; its presence is the
		// ownership marker used to decide whether an existing asset is safe to regenerate over.
		return !GetSourceMetadataValue(Asset, TEXT("DreamShader.SourceFile")).IsEmpty();
	}

	void ApplySourceMetadata(UObject* Asset, const FString& SourceFilePath)
	{
		ApplySourceMetadata(Asset, SourceFilePath, FString());
	}

	void ApplySourceMetadata(UObject* Asset, const FString& SourceFilePath, const FString& SourceHash)
	{
		if (!Asset)
		{
			return;
		}

		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			return;
		}

#if DREAMSHADER_UE_VERSION_AT_LEAST(5, 6)
		FMetaData& MetaData = Package->GetMetaData();
		MetaData.SetValue(Asset, TEXT("DreamShader.SourceFile"), *UE::DreamShader::NormalizeSourceFilePath(SourceFilePath));
		if (!SourceHash.IsEmpty())
		{
			MetaData.SetValue(Asset, TEXT("DreamShader.SourceHash"), *SourceHash);
			MetaData.SetValue(Asset, TEXT("DreamShader.GeneratedAtUtc"), *FDateTime::UtcNow().ToIso8601());
		}
#else
		UMetaData* MetaData = Package->GetMetaData();
		if (!MetaData)
		{
			return;
		}
		MetaData->SetValue(Asset, TEXT("DreamShader.SourceFile"), *UE::DreamShader::NormalizeSourceFilePath(SourceFilePath));
		if (!SourceHash.IsEmpty())
		{
			MetaData->SetValue(Asset, TEXT("DreamShader.SourceHash"), *SourceHash);
			MetaData->SetValue(Asset, TEXT("DreamShader.GeneratedAtUtc"), *FDateTime::UtcNow().ToIso8601());
		}
#endif
	}

	bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		check(Asset);

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true))
		{
			OutError = FString::Printf(TEXT("Generated DreamShader asset '%s' could not be saved."), *Asset->GetPathName());
			return false;
		}

		return true;
	}
}
