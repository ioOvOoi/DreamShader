#include "Bridge/DreamShaderEditorBridge.h"
#include "MaterialAssetGeneration/DreamShaderMaterialGenerator.h"
#include "SourceFiles/DreamShaderSourceFileUtils.h"

#include "CoreGlobals.h"
#include "DreamShaderModule.h"
#include "DreamShaderSettings.h"
#include "Misc/CommandLine.h"
#include "Modules/ModuleManager.h"
#include "Templates/SharedPointer.h"

namespace
{
	bool ShouldSkipDreamShaderEditorBridge()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("NoDreamShaderEditorBridge"));
	}

	bool IsCookCommandlet()
	{
		FString CommandletName;
		return FParse::Value(FCommandLine::Get(), TEXT("-run="), CommandletName) && CommandletName.Contains(TEXT("Cook"));
	}
}

class FDreamShaderEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (IsRunningCommandlet())
		{
			if (IsCookCommandlet())
			{
				const UDreamShaderSettings* Settings = GetDefault<UDreamShaderSettings>();
				if (Settings && Settings->bInMemoryMaterialMode)
				{
					GenerateAllAssetsForCook();
				}
			}
			return;
		}

		if (ShouldSkipDreamShaderEditorBridge())
		{
			return;
		}

		Bridge = MakeShared<UE::DreamShader::Editor::Private::FDreamShaderEditorBridge, ESPMode::ThreadSafe>();
		Bridge->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Bridge)
		{
			Bridge->Shutdown();
			Bridge.Reset();
		}
	}

private:
	void GenerateAllAssetsForCook()
	{
		TArray<FString> SourceFiles;
		UE::DreamShader::Editor::Private::FDreamShaderSourceFileUtils::FindProjectDreamShaderSourceFiles(SourceFiles);

		if (SourceFiles.IsEmpty())
		{
			return;
		}

		UE_LOG(LogDreamShader, Display, TEXT("DreamShader cook: generating %d source file(s) as persistent assets..."), SourceFiles.Num());

		for (const FString& SourceFile : SourceFiles)
		{
			const FString NormalizedPath = UE::DreamShader::NormalizeSourceFilePath(SourceFile);
			if (UE::DreamShader::IsDreamShaderHeaderFile(NormalizedPath))
			{
				continue;
			}

			FString Message;
			const bool bSuccess = UE::DreamShader::Editor::FMaterialGenerator::GenerateAssetsFromFile(NormalizedPath, Message, true, false);
			if (bSuccess)
			{
				UE_LOG(LogDreamShader, Display, TEXT("  [Cook] %s"), *Message);
			}
			else
			{
				UE_LOG(LogDreamShader, Error, TEXT("  [Cook] Failed: %s"), *Message);
			}
		}

		UE_LOG(LogDreamShader, Display, TEXT("DreamShader cook asset generation complete."));
	}

	TSharedPtr<UE::DreamShader::Editor::Private::FDreamShaderEditorBridge, ESPMode::ThreadSafe> Bridge;
};

IMPLEMENT_MODULE(FDreamShaderEditorModule, DreamShaderEditor)
