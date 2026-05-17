#include "DreamShaderSourceFileUtils.h"

#include "DreamShaderModule.h"

#include "HAL/FileManager.h"

namespace UE::DreamShader::Editor::Private
{
	bool FDreamShaderSourceFileUtils::IsPathUnderDirectory(const FString& InPath, const FString& InDirectory)
	{
		const FString Path = UE::DreamShader::NormalizeSourceFilePath(InPath);
		FString Directory = UE::DreamShader::NormalizeSourceFilePath(InDirectory);
		Directory.RemoveFromEnd(TEXT("/"));

		return Path.Equals(Directory, ESearchCase::IgnoreCase)
			|| Path.StartsWith(Directory + TEXT("/"), ESearchCase::IgnoreCase);
	}

	bool FDreamShaderSourceFileUtils::IsPackageMaterialFile(const FString& InPath)
	{
		return UE::DreamShader::IsDreamShaderMaterialFile(InPath)
			&& IsPathUnderDirectory(InPath, UE::DreamShader::GetPackageShaderDirectory());
	}

	void FDreamShaderSourceFileUtils::FindProjectDreamShaderSourceFiles(TArray<FString>& OutSourceFiles)
	{
		TArray<FString> MaterialFiles;
		TArray<FString> HeaderFiles;
		TArray<FString> FunctionFiles;
		IFileManager::Get().FindFilesRecursive(
			MaterialFiles,
			*UE::DreamShader::GetSourceShaderDirectory(),
			TEXT("*.dsm"),
			true,
			false,
			false);
		IFileManager::Get().FindFilesRecursive(
			HeaderFiles,
			*UE::DreamShader::GetSourceShaderDirectory(),
			TEXT("*.dsh"),
			true,
			false,
			false);
		IFileManager::Get().FindFilesRecursive(
			FunctionFiles,
			*UE::DreamShader::GetSourceShaderDirectory(),
			TEXT("*.dsf"),
			true,
			false,
			false);

		OutSourceFiles.Reset();
		OutSourceFiles.Append(MaterialFiles);
		OutSourceFiles.Append(HeaderFiles);
		OutSourceFiles.Append(FunctionFiles);

		for (FString& SourceFile : OutSourceFiles)
		{
			SourceFile = UE::DreamShader::NormalizeSourceFilePath(SourceFile);
		}

		OutSourceFiles.RemoveAll([](const FString& SourceFile)
		{
			return FDreamShaderSourceFileUtils::IsPathUnderDirectory(SourceFile, UE::DreamShader::GetPackageShaderDirectory());
		});
		OutSourceFiles.Sort();
	}

	void FDreamShaderSourceFileUtils::FindProjectMaterialSourceFiles(TArray<FString>& OutSourceFiles)
	{
		TArray<FString> MaterialFiles;
		TArray<FString> FunctionFiles;
		IFileManager::Get().FindFilesRecursive(
			MaterialFiles,
			*UE::DreamShader::GetSourceShaderDirectory(),
			TEXT("*.dsm"),
			true,
			false,
			false);
		IFileManager::Get().FindFilesRecursive(
			FunctionFiles,
			*UE::DreamShader::GetSourceShaderDirectory(),
			TEXT("*.dsf"),
			true,
			false,
			false);

		OutSourceFiles.Reset();
		OutSourceFiles.Append(MaterialFiles);
		OutSourceFiles.Append(FunctionFiles);

		for (FString& SourceFile : OutSourceFiles)
		{
			SourceFile = UE::DreamShader::NormalizeSourceFilePath(SourceFile);
		}

		OutSourceFiles.RemoveAll([](const FString& SourceFile)
		{
			return FDreamShaderSourceFileUtils::IsPackageMaterialFile(SourceFile);
		});
	}
}
