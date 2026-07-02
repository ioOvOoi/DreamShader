#include "DreamShaderModule.h"

#include "DreamShaderSettings.h"

#include "HAL/FileManager.h"
#include "CoreGlobals.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Char.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "UObject/UObjectBase.h"

DEFINE_LOG_CATEGORY(LogDreamShader);

namespace UE::DreamShader
{
	namespace Private
	{
		static const FString GeneratedShaderVirtualDirectory = TEXT("/DreamShaderGenerated");

		struct FConfiguredDirectories
		{
			FString Source;
			FString Package;
			FString Generated;
			bool bInitialized = false;
		};

		static FConfiguredDirectories ConfiguredDirectories;

		static FString ResolveProjectDirectory(const FString& ConfiguredPath, const FString& DefaultPath)
		{
			FString PathText = ConfiguredPath;
			PathText.TrimStartAndEndInline();
			if (PathText.IsEmpty())
			{
				PathText = DefaultPath;
			}

			FString ResolvedPath = FPaths::IsRelative(PathText)
				? FPaths::Combine(FPaths::ProjectDir(), PathText)
				: PathText;
			FPaths::NormalizeFilename(ResolvedPath);
			FPaths::MakeStandardFilename(ResolvedPath);
			return ResolvedPath;
		}

		static bool CanReadSettingsObject()
		{
			return UObjectInitialized() && !GExitPurge && !IsEngineExitRequested();
		}

		static void RefreshConfiguredDirectories()
		{
			const UDreamShaderSettings* Settings = CanReadSettingsObject()
				? GetDefault<UDreamShaderSettings>()
				: nullptr;

			ConfiguredDirectories.Source = ResolveProjectDirectory(
				Settings ? Settings->SourceDirectory.Path : FString(),
				TEXT("DShader"));
			ConfiguredDirectories.Package = FPaths::Combine(ConfiguredDirectories.Source, TEXT("Packages"));
			ConfiguredDirectories.Generated = ResolveProjectDirectory(
				Settings ? Settings->GeneratedShaderDirectory.Path : FString(),
				TEXT("Intermediate/DreamShader/GeneratedShaders"));
			ConfiguredDirectories.bInitialized = true;
		}

		static const FConfiguredDirectories& GetConfiguredDirectories()
		{
			if (!ConfiguredDirectories.bInitialized || CanReadSettingsObject())
			{
				RefreshConfiguredDirectories();
			}

			return ConfiguredDirectories;
		}
	}

	FString GetSourceShaderDirectory()
	{
		return Private::GetConfiguredDirectories().Source;
	}

	FString GetPackageShaderDirectory()
	{
		return Private::GetConfiguredDirectories().Package;
	}

	FString GetGeneratedShaderDirectory()
	{
		const FString VirtualDirectory = GetGeneratedShaderVirtualDirectory();
		if (const FString* MappedDirectory = AllShaderSourceDirectoryMappings().Find(VirtualDirectory))
		{
			return *MappedDirectory;
		}

		return Private::GetConfiguredDirectories().Generated;
	}

	FString GetGeneratedShaderVirtualDirectory()
	{
		return Private::GeneratedShaderVirtualDirectory;
	}

	FString SanitizeIdentifier(const FString& InText)
	{
		FString Result;
		Result.Reserve(InText.Len() + 1);

		for (TCHAR Char : InText)
		{
			if ((Char >= TCHAR('A') && Char <= TCHAR('Z'))
				|| (Char >= TCHAR('a') && Char <= TCHAR('z'))
				|| (Char >= TCHAR('0') && Char <= TCHAR('9'))
				|| Char == TCHAR('_'))
			{
				Result.AppendChar(Char);
			}
			else
			{
				Result.AppendChar(TEXT('_'));
			}
		}

		if (Result.IsEmpty())
		{
			Result = TEXT("DreamShaderSymbol");
		}

		bool bOnlyUnderscores = true;
		for (int32 Index = 0; Index < Result.Len(); ++Index)
		{
			if (Result[Index] != TCHAR('_'))
			{
				bOnlyUnderscores = false;
				break;
			}
		}
		if (bOnlyUnderscores)
		{
			Result = TEXT("DreamShaderSymbol");
		}

		if (!((Result[0] >= TCHAR('A') && Result[0] <= TCHAR('Z'))
			|| (Result[0] >= TCHAR('a') && Result[0] <= TCHAR('z'))
			|| Result[0] == TCHAR('_')))
		{
			Result.InsertAt(0, TCHAR('_'));
		}

		for (int32 Index = Result.Len() - 1; Index > 0; --Index)
		{
			if (Result[Index] == TCHAR('_') && Result[Index - 1] == TCHAR('_'))
			{
				Result.RemoveAt(Index, 1, DREAMSHADER_ALLOW_SHRINKING_NO);
			}
		}

		return Result;
	}

	FString NormalizeSourceFilePath(const FString& InPath)
	{
		FString Result = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Result);
		FPaths::MakeStandardFilename(Result);
		return Result;
	}

	bool IsDreamShaderMaterialFile(const FString& InPath)
	{
		return FPaths::GetExtension(InPath, true).Equals(TEXT(".dsm"), ESearchCase::IgnoreCase);
	}

	bool IsDreamShaderHeaderFile(const FString& InPath)
	{
		return FPaths::GetExtension(InPath, true).Equals(TEXT(".dsh"), ESearchCase::IgnoreCase);
	}

	bool IsDreamShaderFunctionFile(const FString& InPath)
	{
		return FPaths::GetExtension(InPath, true).Equals(TEXT(".dsf"), ESearchCase::IgnoreCase);
	}

	bool IsDreamShaderSourceFile(const FString& InPath)
	{
		return IsDreamShaderMaterialFile(InPath) || IsDreamShaderHeaderFile(InPath) || IsDreamShaderFunctionFile(InPath);
	}
}

void FDreamShaderModule::StartupModule()
{
	IFileManager::Get().MakeDirectory(*UE::DreamShader::GetSourceShaderDirectory(), true);
	IFileManager::Get().MakeDirectory(*UE::DreamShader::GetPackageShaderDirectory(), true);
	IFileManager::Get().MakeDirectory(*UE::DreamShader::Private::GetConfiguredDirectories().Generated, true);

	const FString VirtualDirectory = UE::DreamShader::GetGeneratedShaderVirtualDirectory();
	const FString GeneratedShaderDirectory = UE::DreamShader::Private::GetConfiguredDirectories().Generated;
	if (!AllShaderSourceDirectoryMappings().Contains(VirtualDirectory))
	{
		AddShaderSourceDirectoryMapping(VirtualDirectory, GeneratedShaderDirectory);
	}

	// Static plugin shaders (instance-backend builtin support header).
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamShader")))
	{
		const FString PluginShaderVirtualDirectory = TEXT("/Plugin/DreamShader");
		if (!AllShaderSourceDirectoryMappings().Contains(PluginShaderVirtualDirectory))
		{
			AddShaderSourceDirectoryMapping(PluginShaderVirtualDirectory, FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
	}
}

void FDreamShaderModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FDreamShaderModule, DreamShader);
