#include "DreamShaderDiagnosticsStore.h"

#include "DreamShaderModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::DreamShader::Editor::Private
{
	namespace
	{
		void AddDiagnosticJson(TArray<TSharedPtr<FJsonValue>>& OutDiagnostics, const FDreamShaderDiagnosticRecord& Diagnostic)
		{
			TSharedRef<FJsonObject> DiagnosticObject = MakeShared<FJsonObject>();
			DiagnosticObject->SetStringField(TEXT("message"), Diagnostic.Message);
			if (!Diagnostic.Detail.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("detail"), Diagnostic.Detail);
			}
			if (!Diagnostic.Stage.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("stage"), Diagnostic.Stage);
			}
			if (!Diagnostic.AssetPath.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("assetPath"), Diagnostic.AssetPath);
			}
			if (!Diagnostic.ShaderPlatform.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("shaderPlatform"), Diagnostic.ShaderPlatform);
			}
			if (!Diagnostic.QualityLevel.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("qualityLevel"), Diagnostic.QualityLevel);
			}
			if (!Diagnostic.Code.IsEmpty())
			{
				DiagnosticObject->SetStringField(TEXT("code"), Diagnostic.Code);
			}
			DiagnosticObject->SetNumberField(TEXT("line"), Diagnostic.Line);
			DiagnosticObject->SetNumberField(TEXT("column"), Diagnostic.Column);
			DiagnosticObject->SetStringField(TEXT("severity"), Diagnostic.Severity);
			DiagnosticObject->SetStringField(TEXT("source"), Diagnostic.Source);
			OutDiagnostics.Add(MakeShared<FJsonValueObject>(DiagnosticObject));
		}

		void RemoveDiagnosticsOwnedBySource(
			TMap<FString, TArray<FDreamShaderDiagnosticRecord>>& InOutDiagnosticsByFile,
			const FString& OwnerSourceFilePath)
		{
			TArray<FString> EmptyFiles;
			for (TPair<FString, TArray<FDreamShaderDiagnosticRecord>>& Pair : InOutDiagnosticsByFile)
			{
				Pair.Value.RemoveAll([&OwnerSourceFilePath](const FDreamShaderDiagnosticRecord& Diagnostic)
				{
					return Diagnostic.OwnerSourceFilePath.Equals(OwnerSourceFilePath, ESearchCase::IgnoreCase);
				});

				if (Pair.Value.IsEmpty())
				{
					EmptyFiles.Add(Pair.Key);
				}
			}

			for (const FString& EmptyFile : EmptyFiles)
			{
				InOutDiagnosticsByFile.Remove(EmptyFile);
			}
		}
	}

	void FDreamShaderDiagnosticsStore::Reset()
	{
		DiagnosticsByFile.Reset();
	}

	void FDreamShaderDiagnosticsStore::SetDiagnostics(
		const FString& SourceFilePath,
		TArray<FDreamShaderDiagnosticRecord>&& Diagnostics)
	{
		const FString NormalizedPath = UE::DreamShader::NormalizeSourceFilePath(SourceFilePath);
		RemoveDiagnosticsOwnedBySource(DiagnosticsByFile, NormalizedPath);
		if (Diagnostics.IsEmpty())
		{
			return;
		}

		for (FDreamShaderDiagnosticRecord& Diagnostic : Diagnostics)
		{
			const FString DiagnosticFilePath = Diagnostic.FilePath.IsEmpty()
				? NormalizedPath
				: UE::DreamShader::NormalizeSourceFilePath(Diagnostic.FilePath);
			Diagnostic.FilePath.Reset();
			Diagnostic.OwnerSourceFilePath = NormalizedPath;
			DiagnosticsByFile.FindOrAdd(DiagnosticFilePath).Add(MoveTemp(Diagnostic));
		}
	}

	void FDreamShaderDiagnosticsStore::ClearDiagnostics(const FString& SourceFilePath)
	{
		const FString NormalizedPath = UE::DreamShader::NormalizeSourceFilePath(SourceFilePath);
		DiagnosticsByFile.Remove(NormalizedPath);
		RemoveDiagnosticsOwnedBySource(DiagnosticsByFile, NormalizedPath);
	}

	void FDreamShaderDiagnosticsStore::WriteToFile(const FString& OutputFilePath) const
	{
		TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetNumberField(TEXT("version"), 1);
		RootObject->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());

		TArray<TSharedPtr<FJsonValue>> FileEntries;
		for (const TPair<FString, TArray<FDreamShaderDiagnosticRecord>>& Pair : DiagnosticsByFile)
		{
			TSharedRef<FJsonObject> FileObject = MakeShared<FJsonObject>();
			FileObject->SetStringField(TEXT("path"), Pair.Key);

			TArray<TSharedPtr<FJsonValue>> DiagnosticValues;
			for (const FDreamShaderDiagnosticRecord& Diagnostic : Pair.Value)
			{
				AddDiagnosticJson(DiagnosticValues, Diagnostic);
			}

			FileObject->SetArrayField(TEXT("diagnostics"), DiagnosticValues);
			FileEntries.Add(MakeShared<FJsonValueObject>(FileObject));
		}

		RootObject->SetArrayField(TEXT("files"), FileEntries);

		FString OutputText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputText);
		FJsonSerializer::Serialize(RootObject, Writer);
		FFileHelper::SaveStringToFile(OutputText, *OutputFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool FDreamShaderDiagnosticsStore::TryParseErrorLocation(
		const FString& Line,
		FDreamShaderDiagnosticLocation& OutLocation)
	{
		const int32 CloseMarkerIndex = Line.Find(TEXT("): "));
		if (CloseMarkerIndex == INDEX_NONE)
		{
			return false;
		}

		const int32 OpenMarkerIndex = Line.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromEnd, CloseMarkerIndex);
		if (OpenMarkerIndex == INDEX_NONE || OpenMarkerIndex >= CloseMarkerIndex)
		{
			return false;
		}

		const FString LocationText = Line.Mid(OpenMarkerIndex + 1, CloseMarkerIndex - OpenMarkerIndex - 1);
		FString LineText;
		FString ColumnText;
		if (!LocationText.Split(TEXT(","), &LineText, &ColumnText))
		{
			return false;
		}

		LineText.TrimStartAndEndInline();
		ColumnText.TrimStartAndEndInline();
		if (!LineText.IsNumeric() || !ColumnText.IsNumeric())
		{
			return false;
		}

		OutLocation.Line = FMath::Max(1, FCString::Atoi(*LineText));
		OutLocation.Column = FMath::Max(1, FCString::Atoi(*ColumnText));
		OutLocation.FilePath = UE::DreamShader::NormalizeSourceFilePath(Line.Left(OpenMarkerIndex));
		OutLocation.Message = Line.Mid(CloseMarkerIndex + 3).TrimStartAndEnd();
		return !OutLocation.FilePath.IsEmpty() && !OutLocation.Message.IsEmpty();
	}

	TArray<FDreamShaderDiagnosticRecord> FDreamShaderDiagnosticsStore::BuildGenerateErrorDiagnostics(
		const FString& SourceFilePath,
		const FString& ErrorMessage)
	{
		TArray<FDreamShaderDiagnosticRecord> Diagnostics;

		TArray<FString> Lines;
		ErrorMessage.ParseIntoArrayLines(Lines, false);
		if (Lines.IsEmpty())
		{
			Lines.Add(ErrorMessage);
		}

		const FString PathPrefix = UE::DreamShader::NormalizeSourceFilePath(SourceFilePath) + TEXT(": ");
		for (FString Line : Lines)
		{
			FDreamShaderDiagnosticLocation Location;
			if (TryParseErrorLocation(Line, Location))
			{
				FDreamShaderDiagnosticRecord& Diagnostic = Diagnostics.AddDefaulted_GetRef();
				Diagnostic.FilePath = Location.FilePath;
				Diagnostic.Message = Location.Message;
				Diagnostic.Detail = Line;
				Diagnostic.Stage = TEXT("generate");
				Diagnostic.Code = TEXT("generate-error");
				Diagnostic.Source = TEXT("DreamShader Generate");
				Diagnostic.Line = Location.Line;
				Diagnostic.Column = Location.Column;
				continue;
			}

			if (Line.StartsWith(PathPrefix))
			{
				Line.RightChopInline(PathPrefix.Len(), DREAMSHADER_ALLOW_SHRINKING_NO);
			}

			FDreamShaderDiagnosticRecord& Diagnostic = Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Message = Line;
			Diagnostic.Detail = Line;
			Diagnostic.Stage = TEXT("generate");
			Diagnostic.Code = TEXT("generate-error");
			Diagnostic.Source = TEXT("DreamShader Generate");
		}

		return Diagnostics;
	}
}
