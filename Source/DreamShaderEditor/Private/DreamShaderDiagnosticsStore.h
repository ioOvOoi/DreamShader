#pragma once

#include "CoreMinimal.h"

namespace UE::DreamShader::Editor::Private
{
	struct FDreamShaderDiagnosticRecord
	{
		FString FilePath;
		FString Message;
		FString Detail;
		FString Stage;
		FString AssetPath;
		FString ShaderPlatform;
		FString QualityLevel;
		FString Code;
		int32 Line = 1;
		int32 Column = 1;
		FString Severity = TEXT("error");
		FString Source = TEXT("DreamShader");
	};

	struct FDreamShaderDiagnosticLocation
	{
		FString FilePath;
		FString Message;
		int32 Line = 1;
		int32 Column = 1;
	};

	class FDreamShaderDiagnosticsStore
	{
	public:
		void Reset();
		void SetDiagnostics(const FString& SourceFilePath, TArray<FDreamShaderDiagnosticRecord>&& Diagnostics);
		void ClearDiagnostics(const FString& SourceFilePath);
		void WriteToFile(const FString& OutputFilePath) const;

		static bool TryParseErrorLocation(const FString& Line, FDreamShaderDiagnosticLocation& OutLocation);
		static TArray<FDreamShaderDiagnosticRecord> BuildGenerateErrorDiagnostics(
			const FString& SourceFilePath,
			const FString& ErrorMessage);

	private:
		TMap<FString, TArray<FDreamShaderDiagnosticRecord>> DiagnosticsByFile;
	};
}
