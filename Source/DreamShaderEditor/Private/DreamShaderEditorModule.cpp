#include "Bridge/DreamShaderEditorBridge.h"

#include "CoreGlobals.h"
#include "Misc/CommandLine.h"
#include "Modules/ModuleManager.h"
#include "Templates/SharedPointer.h"

namespace
{
	bool ShouldSkipDreamShaderEditorBridge()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("NoDreamShaderEditorBridge"));
	}
}

class FDreamShaderEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (IsRunningCommandlet() || ShouldSkipDreamShaderEditorBridge())
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
	TSharedPtr<UE::DreamShader::Editor::Private::FDreamShaderEditorBridge, ESPMode::ThreadSafe> Bridge;
};

IMPLEMENT_MODULE(FDreamShaderEditorModule, DreamShaderEditor)
