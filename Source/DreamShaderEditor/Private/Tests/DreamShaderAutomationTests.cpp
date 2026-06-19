#include "Commandlet/DreamShaderCommandletRunner.h"
#include "DreamShaderModule.h"
#include "DreamShaderParser.h"
#include "DreamShaderTypes.h"
#include "DreamShaderVersionCompat.h"
#include "MaterialAssetGeneration/DreamShaderMaterialGenerator.h"

#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamShader::Editor::Private::Tests
{
	FString MakeUniqueTestAssetName(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	FString GetAutomationSourceDirectory()
	{
		return FPaths::Combine(UE::DreamShader::GetSourceShaderDirectory(), TEXT("Tests"), TEXT("Automation"));
	}

	FString MakeAutomationSourcePath(const FString& FileName)
	{
		return UE::DreamShader::NormalizeSourceFilePath(FPaths::Combine(GetAutomationSourceDirectory(), FileName));
	}

	FString MakeAutomationObjectPath(const FString& AssetName)
	{
		return FString::Printf(TEXT("/Game/DreamShaderTests/Automation/%s.%s"), *AssetName, *AssetName);
	}

	bool WriteAutomationSourceFile(
		FAutomationTestBase& Test,
		const FString& FileName,
		const FString& SourceText,
		FString& OutSourceFilePath)
	{
		OutSourceFilePath = MakeAutomationSourcePath(FileName);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutSourceFilePath), true);
		if (!FFileHelper::SaveStringToFile(SourceText, *OutSourceFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Test.AddError(FString::Printf(TEXT("Failed to write DreamShader automation source file '%s'."), *OutSourceFilePath));
			return false;
		}

		return true;
	}

	void AddExpectedNewAssetProbeWarnings(FAutomationTestBase& Test, const FString& ObjectPath)
	{
		Test.AddExpectedError(
			FString::Printf(TEXT("SkipPackage: %s"), *FPackageName::ObjectPathToPackageName(ObjectPath)),
			EAutomationExpectedErrorFlags::Contains);
		Test.AddExpectedError(
			FString::Printf(TEXT("%s"), *ObjectPath),
			EAutomationExpectedErrorFlags::Contains);
	}

	void AddExpectedAutomationCleanupWarnings(FAutomationTestBase& Test)
	{
		Test.AddExpectedError(TEXT("package was marked as deleted in editor, but has been modified on disk"), EAutomationExpectedErrorFlags::Contains);
	}

	void DeleteSourceFileForAutomation(const FString& SourceFilePath)
	{
		if (!SourceFilePath.IsEmpty())
		{
			IFileManager::Get().Delete(*SourceFilePath, false, true);
		}
	}

	void DeleteAssetForAutomation(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return;
		}

		UObject* Object = LoadObject<UObject>(nullptr, *ObjectPath);
		if (!Object)
		{
			return;
		}

		TArray<UObject*> ObjectsToDelete;
		ObjectsToDelete.Add(Object);
		ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
	}

	class FScopedDreamShaderAutomationArtifacts
	{
	public:
		void AddSourceFile(const FString& SourceFilePath)
		{
			SourceFilePaths.Add(SourceFilePath);
		}

		void AddObjectPath(const FString& ObjectPath)
		{
			ObjectPaths.Add(ObjectPath);
		}

		~FScopedDreamShaderAutomationArtifacts()
		{
			for (const FString& ObjectPath : ObjectPaths)
			{
				DeleteAssetForAutomation(ObjectPath);
			}

			for (const FString& SourceFilePath : SourceFilePaths)
			{
				DeleteSourceFileForAutomation(SourceFilePath);
			}
		}

	private:
		TArray<FString> SourceFilePaths;
		TArray<FString> ObjectPaths;
	};

	FString MakeMinimalMaterialSource(const FString& AssetName)
	{
		return FString::Printf(TEXT(R"(
Shader(Name="DreamShaderTests/Automation/%s")
{
    Properties = {
        vec3 Tint = vec3(1.0, 0.2, 0.2);
    }

    Settings = {
        Domain = "UI";
        ShadingModel = "Unlit";
    }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        Color = Tint;
    }
}
)"), *AssetName);
	}

	FString MakeSharedHeaderSource()
	{
		return TEXT(R"(
Function ApplyAutomationTint(in vec3 color, in vec3 tint, out vec3 result) {
    result = color * tint;
}
)");
	}

	FString MakeImportedFunctionSource(const FString& HeaderFileName, const FString& AssetName)
	{
		return FString::Printf(TEXT(R"(
import "%s";

ShaderFunction(Name="DreamShaderTests/Automation/%s")
{
    Inputs = {
        vec3 InColor;
        vec3 InTint;
    }

    Outputs = {
        vec3 OutColor;
    }

    Graph = {
        ApplyAutomationTint(InColor, InTint, OutColor);
    }
}
)"), *HeaderFileName, *AssetName);
	}

	FString MakeSubstrateMaterialSource(const FString& AssetName)
	{
		return FString::Printf(TEXT(R"(
Shader(Name="DreamShaderTests/Automation/%s")
{
    Outputs = {
        Substrate Surface;
        Base.FrontMaterial = Surface;
    }

    Graph = {
        Surface = Substrate.Unlit(EmissiveColor=vec3(0.1, 0.6, 1.0));
    }
}
)"), *AssetName);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderParserMinimalMaterialTest,
	"DreamShader.Compiler.Parser.MinimalMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderParserMinimalMaterialTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FTextShaderDefinition Definition;
	FString ParseError;
	const bool bParsed = FTextShaderParser::Parse(MakeMinimalMaterialSource(TEXT("M_ParserMinimal")), Definition, ParseError);
	if (!TestTrue(FString::Printf(TEXT("Parser succeeds: %s"), *ParseError), bParsed))
	{
		return false;
	}

	TestEqual(TEXT("Shader name"), Definition.Name, FString(TEXT("DreamShaderTests/Automation/M_ParserMinimal")));
	FString Domain;
	TestTrue(TEXT("Domain setting exists"), Definition.TryGetSetting(TEXT("Domain"), Domain));
	TestEqual(TEXT("Domain setting"), Domain, FString(TEXT("UI")));
	TestEqual(TEXT("Output declaration count"), Definition.OutputDeclarations.Num(), 1);
	TestEqual(TEXT("Output binding count"), Definition.Outputs.Num(), 1);
	TestFalse(TEXT("Graph code is captured"), Definition.Code.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateMinimalMaterialTest,
	"DreamShader.Compiler.Generate.MinimalMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateMinimalMaterialTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoMinimal"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), MakeMinimalMaterialSource(AssetName), SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, true);
	if (!TestTrue(FString::Printf(TEXT("Material generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UMaterial* GeneratedMaterial = LoadObject<UMaterial>(nullptr, *ObjectPath);
	TestNotNull(FString::Printf(TEXT("Generated material loads from '%s'."), *ObjectPath), GeneratedMaterial);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateDsfWithImportTest,
	"DreamShader.Compiler.Generate.DsfWithImport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateDsfWithImportTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString FunctionName = MakeUniqueTestAssetName(TEXT("F_AutoImport"));
	const FString HeaderFileName = FunctionName + TEXT("_Shared.dsh");
	const FString ObjectPath = MakeAutomationObjectPath(FunctionName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	FString HeaderFilePath;
	if (!WriteAutomationSourceFile(*this, HeaderFileName, MakeSharedHeaderSource(), HeaderFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(HeaderFilePath);

	FString FunctionFilePath;
	if (!WriteAutomationSourceFile(*this, FunctionName + TEXT(".dsf"), MakeImportedFunctionSource(HeaderFileName, FunctionName), FunctionFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(FunctionFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateAssetsFromFile(FunctionFilePath, Message, true);
	if (!TestTrue(FString::Printf(TEXT("Imported .dsf generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UMaterialFunction* GeneratedFunction = LoadObject<UMaterialFunction>(nullptr, *ObjectPath);
	TestNotNull(FString::Printf(TEXT("Generated material function loads from '%s'."), *ObjectPath), GeneratedFunction);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateSubstrateMaterialTest,
	"DreamShader.Compiler.Generate.SubstrateMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateSubstrateMaterialTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

#if DREAMSHADER_WITH_SUBSTRATE_BUILTINS
	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoSubstrate"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), MakeSubstrateMaterialSource(AssetName), SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, true);
	if (!TestTrue(FString::Printf(TEXT("Substrate material generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UMaterial* GeneratedMaterial = LoadObject<UMaterial>(nullptr, *ObjectPath);
	TestNotNull(FString::Printf(TEXT("Generated Substrate material loads from '%s'."), *ObjectPath), GeneratedMaterial);
#else
	AddInfo(TEXT("DreamShader Substrate builtins are not available for this Unreal Engine version; skipping generation test."));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderSourceHashSkipTest,
	"DreamShader.Compiler.SourceHash.SkipUnchangedMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderSourceHashSkipTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoHash"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), MakeMinimalMaterialSource(AssetName), SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString FirstMessage;
	if (!TestTrue(
		FString::Printf(TEXT("Initial material generation succeeds: %s"), *FirstMessage),
		FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, FirstMessage, true)))
	{
		return false;
	}

	FString SecondMessage;
	const bool bSkipped = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, SecondMessage, false);
	if (!TestTrue(FString::Printf(TEXT("Unchanged material generation succeeds: %s"), *SecondMessage), bSkipped))
	{
		return false;
	}
	TestTrue(
		FString::Printf(TEXT("Unchanged source is skipped: %s"), *SecondMessage),
		SecondMessage.Contains(TEXT("source hash is unchanged"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderCommandletCompileSingleSourceSmokeTest,
	"DreamShader.Commandlet.Compile.SingleSourceSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderCommandletCompileSingleSourceSmokeTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor::Private;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoCommandlet"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), MakeMinimalMaterialSource(AssetName), SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	TArray<FString> Tokens;
	TArray<FString> Switches;
	Switches.Add(TEXT("Force"));

	TMap<FString, FString> Params;
	Params.Add(TEXT("Source"), SourceFilePath);

	const bool bCompiled = RunDreamShaderCompileCommandlet(Tokens, Switches, Params);
	if (!TestTrue(TEXT("DreamShader compile commandlet runner succeeds."), bCompiled))
	{
		return false;
	}

	UMaterial* GeneratedMaterial = LoadObject<UMaterial>(nullptr, *ObjectPath);
	TestNotNull(FString::Printf(TEXT("Commandlet generated material loads from '%s'."), *ObjectPath), GeneratedMaterial);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
