#include "DreamShaderDependencyGraphService.h"

#include "DreamShaderModule.h"
#include "DreamShaderSourceFileUtils.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::DreamShader::Editor::Private
{
	namespace
	{
		bool IsPathUnderDirectory(const FString& InPath, const FString& InDirectory)
		{
			if (InPath.IsEmpty() || InDirectory.IsEmpty())
			{
				return false;
			}

			const FString Path = UE::DreamShader::NormalizeSourceFilePath(InPath);
			FString Directory = UE::DreamShader::NormalizeSourceFilePath(InDirectory);
			Directory.RemoveFromEnd(TEXT("/"));

			return Path.Equals(Directory, ESearchCase::IgnoreCase)
				|| Path.StartsWith(Directory + TEXT("/"), ESearchCase::IgnoreCase);
		}

		FString GetImportRootDirectoryForFile(const FString& CurrentFilePath)
		{
			const TArray<FString> RootDirectories =
			{
				UE::DreamShader::GetSourceShaderDirectory(),
				UE::DreamShader::GetPackageShaderDirectory(),
				UE::DreamShader::GetBuiltinShaderLibraryDirectory()
			};

			FString BestRootDirectory;
			for (const FString& RootDirectory : RootDirectories)
			{
				if (IsPathUnderDirectory(CurrentFilePath, RootDirectory) && RootDirectory.Len() > BestRootDirectory.Len())
				{
					BestRootDirectory = RootDirectory;
				}
			}

			return BestRootDirectory.IsEmpty() ? FPaths::GetPath(CurrentFilePath) : BestRootDirectory;
		}
	}

	bool FDreamShaderDependencyGraphService::TryExtractImportPathFromLine(const FString& Line, FString& OutPath)
	{
		FString TrimmedLine = Line.TrimStartAndEnd();
		if (TrimmedLine.StartsWith(TEXT("//"))
			|| !TrimmedLine.StartsWith(TEXT("import"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		const int32 ImportKeywordLength = 6;
		if (TrimmedLine.Len() > ImportKeywordLength
			&& !FChar::IsWhitespace(TrimmedLine[ImportKeywordLength]))
		{
			return false;
		}

		TrimmedLine.RightChopInline(ImportKeywordLength, EAllowShrinking::No);
		TrimmedLine.TrimStartAndEndInline();
		if (TrimmedLine.Len() < 2 || (TrimmedLine[0] != TCHAR('"') && TrimmedLine[0] != TCHAR('\'')))
		{
			return false;
		}

		const TCHAR Quote = TrimmedLine[0];
		int32 ClosingQuoteIndex = INDEX_NONE;
		bool bEscaped = false;
		for (int32 Index = 1; Index < TrimmedLine.Len(); ++Index)
		{
			const TCHAR Character = TrimmedLine[Index];
			if (bEscaped)
			{
				bEscaped = false;
				continue;
			}
			if (Character == TCHAR('\\'))
			{
				bEscaped = true;
				continue;
			}
			if (Character == Quote)
			{
				ClosingQuoteIndex = Index;
				break;
			}
		}

		if (ClosingQuoteIndex == INDEX_NONE)
		{
			return false;
		}

		FString TrailingText = TrimmedLine.Mid(ClosingQuoteIndex + 1).TrimStartAndEnd();
		if (TrailingText.StartsWith(TEXT(";")))
		{
			TrailingText.RightChopInline(1, EAllowShrinking::No);
			TrailingText.TrimStartAndEndInline();
		}
		if (!TrailingText.IsEmpty() && !TrailingText.StartsWith(TEXT("//")))
		{
			return false;
		}

		OutPath = TrimmedLine.Mid(1, ClosingQuoteIndex - 1).TrimStartAndEnd();
		return !OutPath.IsEmpty();
	}

	FString FDreamShaderDependencyGraphService::NormalizeImportSpecifier(const FString& ImportSpecifier)
	{
		FString Normalized = ImportSpecifier.TrimStartAndEnd();
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Normalized.StartsWith(TEXT("./")))
		{
			Normalized.RightChopInline(2, EAllowShrinking::No);
		}

		if (FPaths::GetExtension(Normalized, true).IsEmpty())
		{
			Normalized += TEXT(".dsh");
		}

		return Normalized;
	}

	bool FDreamShaderDependencyGraphService::ResolveImportPath(
		const FString& CurrentFilePath,
		const FString& ImportSpecifier,
		FString& OutResolvedPath)
	{
		const FString NormalizedImport = NormalizeImportSpecifier(ImportSpecifier);
		if (NormalizedImport.IsEmpty())
		{
			return false;
		}

		struct FImportCandidate
		{
			FString Path;
			FString RootDirectory;
		};

		const TArray<FImportCandidate> Candidates =
		{
			{ FPaths::Combine(FPaths::GetPath(CurrentFilePath), NormalizedImport), GetImportRootDirectoryForFile(CurrentFilePath) },
			{ FPaths::Combine(UE::DreamShader::GetSourceShaderDirectory(), NormalizedImport), UE::DreamShader::GetSourceShaderDirectory() },
			{ FPaths::Combine(UE::DreamShader::GetPackageShaderDirectory(), NormalizedImport), UE::DreamShader::GetPackageShaderDirectory() },
			{ FPaths::Combine(UE::DreamShader::GetBuiltinShaderLibraryDirectory(), NormalizedImport), UE::DreamShader::GetBuiltinShaderLibraryDirectory() }
		};

		for (const FImportCandidate& Candidate : Candidates)
		{
			const FString NormalizedCandidate = UE::DreamShader::NormalizeSourceFilePath(Candidate.Path);
			if (!IsPathUnderDirectory(NormalizedCandidate, Candidate.RootDirectory))
			{
				continue;
			}

			if (IFileManager::Get().FileExists(*NormalizedCandidate))
			{
				OutResolvedPath = NormalizedCandidate;
				return true;
			}
		}

		return false;
	}

	void FDreamShaderDependencyGraphService::CollectHeaderDependenciesRecursive(
		const FString& SourceFilePath,
		TSet<FString>& OutHeaders,
		TSet<FString>& InOutVisitedFiles)
	{
		const FString NormalizedPath = UE::DreamShader::NormalizeSourceFilePath(SourceFilePath);
		if (InOutVisitedFiles.Contains(NormalizedPath))
		{
			return;
		}
		InOutVisitedFiles.Add(NormalizedPath);

		FString SourceText;
		if (!FFileHelper::LoadFileToString(SourceText, *NormalizedPath))
		{
			return;
		}

		TArray<FString> Lines;
		SourceText.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			FString ImportPath;
			if (!TryExtractImportPathFromLine(Line, ImportPath))
			{
				continue;
			}

			FString ResolvedImportPath;
			if (!ResolveImportPath(NormalizedPath, ImportPath, ResolvedImportPath))
			{
				continue;
			}

			if (UE::DreamShader::IsDreamShaderHeaderFile(ResolvedImportPath) || UE::DreamShader::IsDreamShaderFunctionFile(ResolvedImportPath))
			{
				OutHeaders.Add(ResolvedImportPath);
			}

			CollectHeaderDependenciesRecursive(ResolvedImportPath, OutHeaders, InOutVisitedFiles);
		}
	}

	void FDreamShaderDependencyGraphService::RebuildMaterialDependencyGraph(
		TMap<FString, TSet<FString>>& OutHeaderDependentsByFile)
	{
		OutHeaderDependentsByFile.Reset();

		TArray<FString> MaterialFiles;
		FDreamShaderSourceFileUtils::FindProjectMaterialSourceFiles(MaterialFiles);
		for (const FString& MaterialFile : MaterialFiles)
		{
			TSet<FString> Dependencies;
			TSet<FString> VisitedFiles;
			CollectHeaderDependenciesRecursive(MaterialFile, Dependencies, VisitedFiles);
			for (const FString& HeaderFile : Dependencies)
			{
				OutHeaderDependentsByFile.FindOrAdd(HeaderFile).Add(MaterialFile);
			}
		}
	}

	TSet<FString> FDreamShaderDependencyGraphService::RebuildAndCollectDependentsForImport(
		const FString& ImportFilePath,
		TMap<FString, TSet<FString>>& InOutHeaderDependentsByFile)
	{
		const FString NormalizedImportPath = UE::DreamShader::NormalizeSourceFilePath(ImportFilePath);
		TSet<FString> SourcesToQueue;

		if (const TSet<FString>* ExistingDependents = InOutHeaderDependentsByFile.Find(NormalizedImportPath))
		{
			for (const FString& Dependent : *ExistingDependents)
			{
				SourcesToQueue.Add(Dependent);
			}
		}

		RebuildMaterialDependencyGraph(InOutHeaderDependentsByFile);

		if (const TSet<FString>* RebuiltDependents = InOutHeaderDependentsByFile.Find(NormalizedImportPath))
		{
			for (const FString& Dependent : *RebuiltDependents)
			{
				SourcesToQueue.Add(Dependent);
			}
		}

		return SourcesToQueue;
	}
}
