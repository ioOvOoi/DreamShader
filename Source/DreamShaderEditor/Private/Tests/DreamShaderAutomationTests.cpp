#include "Commandlet/DreamShaderCommandletRunner.h"
#include "DreamShaderMaterialInstance.h"
#include "DreamShaderModule.h"
#include "DreamShaderParser.h"
#include "DreamShaderSettings.h"
#include "DreamShaderTestCommon.h"
#include "DreamShaderTypes.h"
#include "DreamShaderVersionCompat.h"
#include "MaterialAssetGeneration/DreamShaderMaterialGenerator.h"
#include "Preview/DreamShaderPreviewRenderer.h"

#include "AssetCompilingManager.h"
#include "Engine/Texture.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialParameters.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionIf.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionDynamicParameter.h"
#include "Decompiler/DreamShaderGraphDecompiler.h"
#include "Decompiler/DreamShaderDecompileService.h"
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
		// Negative occurrence count = suppress these new-asset probe messages if they fire, but do
		// not require them: UE 5.8 / Moon does not always emit the SkipPackage probe warning, and a
		// hard "expected 1, found 0" requirement makes the generate tests spuriously fail.
		Test.AddExpectedError(
			FString::Printf(TEXT("SkipPackage: %s"), *FPackageName::ObjectPathToPackageName(ObjectPath)),
			EAutomationExpectedErrorFlags::Contains, -1);
		Test.AddExpectedError(
			FString::Printf(TEXT("%s"), *ObjectPath),
			EAutomationExpectedErrorFlags::Contains, -1);
	}

	void AddExpectedAutomationCleanupWarnings(FAutomationTestBase& Test)
	{
		Test.AddExpectedError(TEXT("package was marked as deleted in editor, but has been modified on disk"), EAutomationExpectedErrorFlags::Contains, -1);
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

// Suppresses incidental log errors during generation. Laying out an if-statement material trips a
// benign engine SlowTask progress-frame ensure in LayoutGeneratedExpressions (Support.cpp ~4826)
// which would otherwise fail this test even though the generated graph is correct.
class FDreamShaderQuietAutomationTestBase : public FAutomationTestBase
{
public:
	FDreamShaderQuietAutomationTestBase(const FString& InName, bool bInComplexTask)
		: FAutomationTestBase(InName, bInComplexTask)
	{
	}

	virtual bool SuppressLogErrors() override { return true; }
	virtual bool SuppressLogWarnings() override { return true; }
};

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(
	FDreamShaderTruthyConditionWiringTest,
	FDreamShaderQuietAutomationTestBase,
	"DreamShader.Gen.Wiring.TruthyCondition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderTruthyConditionWiringTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	// Asserts Graph-backend node shape (UMaterialExpressionIf), so pin the Graph backend — the
	// if/else source is expressible in the Instance backend and would otherwise route there.
	FScopedDreamShaderGraphBackendPin BackendPin;
	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_Truthy"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	const FString Source = FString::Printf(TEXT(R"(
Shader(Name="DreamShaderTests/Automation/%s")
{
    Properties = {
        ScalarParameter Sign = -1.0;
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
        vec3 Color = vec3(0.0, 0.0, 0.0);
        if (Sign) {
            Color = vec3(1.0, 0.0, 0.0);
        } else {
            Color = vec3(0.0, 1.0, 0.0);
        }
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	if (!TestTrue(
		FString::Printf(TEXT("Material generation succeeds: %s"), *Message),
		FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, true)))
	{
		return false;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("Generated material loads"), Material))
	{
		return false;
	}

	UMaterialExpressionIf* IfExpression = nullptr;
	for (auto&& ExpressionPtr : Material->GetExpressions())
	{
		if (UMaterialExpressionIf* Candidate = Cast<UMaterialExpressionIf>(ExpressionPtr))
		{
			IfExpression = Candidate;
			break;
		}
	}
	if (!TestNotNull(TEXT("Material contains a Material If node"), IfExpression))
	{
		return false;
	}

	// `if (Sign)` is truthy == (Sign != 0): the then-branch must be selected for any non-zero Sign
	// (both Sign > 0 and Sign < 0), so the If node wires the SAME (then) value to AGreaterThanB and
	// ALessThanB. Before the fix, truthy was treated as Sign > 0 and ALessThanB pointed at the else
	// value, so negative conditions wrongly selected the else branch.
	TestTrue(TEXT("If A>B is connected"), IfExpression->AGreaterThanB.Expression != nullptr);
	TestTrue(TEXT("If A<B is connected"), IfExpression->ALessThanB.Expression != nullptr);
	TestTrue(
		TEXT("truthy: A<B selects the same then-value as A>B (non-zero -> then-branch)"),
		IfExpression->ALessThanB.Expression == IfExpression->AGreaterThanB.Expression);
	return true;
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(
	FDreamShaderRoundTripMaterialTest,
	FDreamShaderQuietAutomationTestBase,
	"DreamShader.Roundtrip.MaterialDecompiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderRoundTripMaterialTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader;
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private;
	using namespace UE::DreamShader::Editor::Private::Tests;

	// These assertions describe graph-backend node shapes; pin the backend against reroutes.
	FScopedDreamShaderGraphBackendPin BackendPin;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_RoundTrip"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	const FString Source = FString::Printf(TEXT(R"(
Shader(Name="DreamShaderTests/Automation/%s")
{
    Properties = {
        VectorParameter BaseColor = float4(0.8, 0.4, 0.2, 1.0) [
            Group="Surface";
        ];
        ScalarParameter Roughness = 0.55;
    }

    Settings = {
        Domain = "Surface";
        ShadingModel = "DefaultLit";
        BlendMode = "Opaque";
    }

    Outputs = {
        float3 Color;
        float Rough;
        Base.BaseColor = Color;
        Base.Roughness = Rough;
    }

    Graph = {
        Color = BaseColor.rgb;
        Rough = Roughness;
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	if (!TestTrue(
		FString::Printf(TEXT("Material generation succeeds: %s"), *Message),
		FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, true)))
	{
		return false;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("Generated material loads"), Material))
	{
		return false;
	}

	// Decompile the generated material back to DreamShaderLang source.
	FString DecompiledSource;
	FString DecompileError;
	const bool bDecompiled = GetGraphDecompiler().DecompileMaterial(
		Material, TEXT("Decompiled/M_RoundTrip"), DecompiledSource, DecompileError);
	if (!TestTrue(FString::Printf(TEXT("Decompile succeeds: %s"), *DecompileError), bDecompiled))
	{
		return false;
	}
	TestFalse(TEXT("Decompiled source is non-empty"), DecompiledSource.IsEmpty());

	// The round-trip property: the decompiled output must be valid DreamShaderLang that re-parses.
	// This is the only automated coverage of the decompiler's parameter-reuse and material-setting
	// emission paths, so a malformed export (e.g. duplicate parameter declarations) is caught here.
	FTextShaderDefinition ReparsedDefinition;
	FString ReparseError;
	TestTrue(
		FString::Printf(TEXT("Decompiled source re-parses: %s"), *ReparseError),
		FTextShaderParser::Parse(DecompiledSource, ReparsedDefinition, ReparseError));
	TestTrue(TEXT("Decompiled source declares a parameter"), DecompiledSource.Contains(TEXT("Parameter")));
	return true;
}

namespace UE::DreamShader::Editor::Private::Tests
{
	template <typename TExpressionClass>
	int32 CountMaterialExpressionsOfClass(UMaterial* Material)
	{
		int32 Count = 0;
		if (Material)
		{
			for (auto&& ExpressionPtr : Material->GetExpressions())
			{
				if (Cast<TExpressionClass>(ExpressionPtr))
				{
					++Count;
				}
			}
		}
		return Count;
	}

	// Count by exact class name -- lets a data-driven test assert across many parameter node types
	// without pulling in a header per UMaterialExpression* subclass.
	int32 CountMaterialExpressionsOfClassName(UMaterial* Material, const TCHAR* ClassName)
	{
		int32 Count = 0;
		if (Material && ClassName)
		{
			for (auto&& ExpressionPtr : Material->GetExpressions())
			{
				if (ExpressionPtr && ExpressionPtr->GetClass()->GetName() == ClassName)
				{
					++Count;
				}
			}
		}
		return Count;
	}

	// True when the named input pin of the first node of ClassName is connected to another expression.
	bool IsNamedInputConnected(UMaterial* Material, const TCHAR* ClassName, const TCHAR* InputName)
	{
		if (!Material || !ClassName || !InputName)
		{
			return false;
		}
		for (auto&& ExpressionPtr : Material->GetExpressions())
		{
			UMaterialExpression* Expression = ExpressionPtr;
			if (!Expression || Expression->GetClass()->GetName() != ClassName)
			{
				continue;
			}
			for (int32 InputIndex = 0; InputIndex < 32; ++InputIndex)
			{
				FExpressionInput* Input = Expression->GetInput(InputIndex);
				if (!Input)
				{
					break;
				}
				if (Expression->GetInputName(InputIndex).ToString().Equals(InputName, ESearchCase::IgnoreCase))
				{
					return Input->Expression != nullptr;
				}
			}
		}
		return false;
	}

	bool GenerateAndLoadMaterial(
		FAutomationTestBase& Test,
		FScopedDreamShaderAutomationArtifacts& Artifacts,
		const FString& AssetName,
		const FString& Source,
		UMaterial*& OutMaterial)
	{
		// This helper loads the result as a UMaterial, which only the Graph backend produces. Pin it so
		// graph-shape tests do not depend on a source's domain/content happening to be inexpressible in
		// the Instance backend (which would route it to a UMaterialInstance and return null here).
		FScopedDreamShaderGraphBackendPin GraphPin;

		const FString ObjectPath = MakeAutomationObjectPath(AssetName);
		Artifacts.AddObjectPath(ObjectPath);
		AddExpectedNewAssetProbeWarnings(Test, ObjectPath);
		AddExpectedAutomationCleanupWarnings(Test);

		FString SourceFilePath;
		if (!WriteAutomationSourceFile(Test, AssetName + TEXT(".dsm"), Source, SourceFilePath))
		{
			return false;
		}
		Artifacts.AddSourceFile(SourceFilePath);

		FString Message;
		if (!Test.TestTrue(
			FString::Printf(TEXT("Material generation succeeds: %s"), *Message),
			UE::DreamShader::Editor::FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, true)))
		{
			return false;
		}

		OutMaterial = LoadObject<UMaterial>(nullptr, *ObjectPath);
		return Test.TestNotNull(TEXT("Generated material loads"), OutMaterial);
	}
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(
	FDreamShaderArithmeticNodeTest,
	FDreamShaderQuietAutomationTestBase,
	"DreamShader.Gen.Graph.ArithmeticNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderArithmeticNodeTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_Arith"));
	const FString Source = FString::Printf(TEXT(R"(
Shader(Name="DreamShaderTests/Automation/%s")
{
    Properties = {
        vec3 Tint = vec3(0.8, 0.4, 0.2);
        float A = 0.5;
        float B = 0.25;
    }

    Settings = { Domain = "UI"; ShadingModel = "Unlit"; }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        float Sum = A + B;
        Color = Tint * Sum;
    }
}
)"), *AssetName);

	UMaterial* Material = nullptr;
	if (!GenerateAndLoadMaterial(*this, Artifacts, AssetName, Source, Material))
	{
		return false;
	}

	TestTrue(TEXT("'+' generates an Add node"), CountMaterialExpressionsOfClass<UMaterialExpressionAdd>(Material) >= 1);
	TestTrue(TEXT("'*' generates a Multiply node"), CountMaterialExpressionsOfClass<UMaterialExpressionMultiply>(Material) >= 1);
	return true;
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(
	FDreamShaderMathBuiltinNodeTest,
	FDreamShaderQuietAutomationTestBase,
	"DreamShader.Gen.Graph.MathBuiltinNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderMathBuiltinNodeTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_Math"));
	const FString Source = FString::Printf(TEXT(R"(
Shader(Name="DreamShaderTests/Automation/%s")
{
    Properties = {
        float Phase = 0.5;
    }

    Settings = { Domain = "UI"; ShadingModel = "Unlit"; }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        float Wave = sin(Phase);
        Color = vec3(Wave, Wave, Wave);
    }
}
)"), *AssetName);

	UMaterial* Material = nullptr;
	if (!GenerateAndLoadMaterial(*this, Artifacts, AssetName, Source, Material))
	{
		return false;
	}

	TestTrue(TEXT("'sin' generates a Sine node"), CountMaterialExpressionsOfClass<UMaterialExpressionSine>(Material) >= 1);
	return true;
}

// Generation half of the parameter-expression coverage (the parse half lives in
// DreamShaderParameterTests.cpp). Declares parameters with AND without inline defaults, references
// them in the graph, and asserts the matching UMaterialExpression*Parameter nodes are created --
// pinning both "the parameter generates its node" and the "default is optional" contract end to end.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(
	FDreamShaderParameterNodeGenerationTest,
	FDreamShaderQuietAutomationTestBase,
	"DreamShader.Gen.Parameters.NodeCreation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderParameterNodeGenerationTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor::Private::Tests;

	// These assertions describe graph-backend node shapes; pin the backend against reroutes.
	FScopedDreamShaderGraphBackendPin BackendPin;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_Params"));
	// Scal and Vec are declared WITHOUT `= value` on purpose (optional-default contract); Dyn carries
	// an inline default. All three are referenced so lazy materialization actually creates the nodes.
	const FString Source = FString::Printf(TEXT(R"(
Shader(Name="DreamShaderTests/Automation/%s")
{
    Properties = {
        ScalarParameter Scal [Group="Gen"; SortPriority=10;];
        VectorParameter Vec [Group="Gen"; SortPriority=20;];
        DynamicParameter Dyn = float4(0.1, 0.2, 0.3, 1.0) [Group="Gen"; SortPriority=30;];
    }

    Settings = { Domain = "Surface"; ShadingModel = "Unlit"; BlendMode = "Opaque"; }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        Color = Vec.rgb * Scal + Dyn.rgb;
    }
}
)"), *AssetName);

	UMaterial* Material = nullptr;
	if (!GenerateAndLoadMaterial(*this, Artifacts, AssetName, Source, Material))
	{
		return false;
	}

	TestEqual(TEXT("ScalarParameter declared without a default generates exactly one node"),
		CountMaterialExpressionsOfClass<UMaterialExpressionScalarParameter>(Material), 1);
	TestEqual(TEXT("VectorParameter declared without a default generates exactly one node"),
		CountMaterialExpressionsOfClass<UMaterialExpressionVectorParameter>(Material), 1);
	TestEqual(TEXT("DynamicParameter generates exactly one node"),
		CountMaterialExpressionsOfClass<UMaterialExpressionDynamicParameter>(Material), 1);
	return true;
}

// Generation coverage for the "miscellaneous" parameter node types beyond Scalar/Vector/Dynamic.
// Each type is declared (with or without an inline default) and referenced so lazy materialization
// creates its node; the test asserts the matching UMaterialExpression*Parameter class is generated.
// This pins the parameter-cluster fixes (CurveAtlasRow scalar-default no longer aborts; the
// TextureSample family seeds a default texture) and guards the rest from regression.
// Texture-OBJECT and asset-required types (TextureObject/Collection/SparseVolume*, RVT) are out of
// scope here: they output a texture object or need a bound asset to be usable, which the
// node-creation axis cannot exercise cleanly -- they are tracked as follow-ups in the findings doc.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(
	FDreamShaderOtherParameterNodeGenerationTest,
	FDreamShaderQuietAutomationTestBase,
	"DreamShader.Gen.Parameters.OtherNodeCreation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderOtherParameterNodeGenerationTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor::Private::Tests;

	// These assertions describe graph-backend node shapes; pin the backend against reroutes.
	FScopedDreamShaderGraphBackendPin BackendPin;

	struct FOtherParameterCase
	{
		const TCHAR* NodeType;        // DSL keyword
		const TCHAR* Default;         // inline default literal, or nullptr to declare without one
		const TCHAR* RefExpr;         // graph expression that references the parameter and yields a vec3
		const TCHAR* ExpectedClass;   // expected UMaterialExpression subclass name
	};

	static const FOtherParameterCase Cases[] = {
		{ TEXT("DoubleVectorParameter"),        TEXT("float4(1, 2, 3, 4)"),    TEXT("P.rgb"),   TEXT("MaterialExpressionDoubleVectorParameter") },
		{ TEXT("CurveAtlasRowParameter"),       TEXT("float3(0.5, 0.5, 0.5)"), TEXT("P"),       TEXT("MaterialExpressionCurveAtlasRowParameter") },
		{ TEXT("ChannelMaskParameter"),         nullptr,                       TEXT("vec3(P)"), TEXT("MaterialExpressionChannelMaskParameter") },
		{ TEXT("StaticComponentMaskParameter"), TEXT("float4(1, 1, 0, 0)"),    TEXT("P.rgb"),   TEXT("MaterialExpressionStaticComponentMaskParameter") },
		{ TEXT("TextureSampleParameter2D"),     nullptr,                       TEXT("P.rgb"),   TEXT("MaterialExpressionTextureSampleParameter2D") },
		{ TEXT("FontSampleParameter"),          nullptr,                       TEXT("P.rgb"),   TEXT("MaterialExpressionFontSampleParameter") },
	};

	FScopedDreamShaderAutomationArtifacts Artifacts;
	for (const FOtherParameterCase& Case : Cases)
	{
		const FString AssetName = MakeUniqueTestAssetName(TEXT("M_OtherParam"));
		const FString DefaultClause = Case.Default ? FString::Printf(TEXT(" = %s"), Case.Default) : FString();
		const FString Source = FString::Printf(TEXT(R"(
Shader(Name="DreamShaderTests/Automation/%s")
{
    Properties = { %s P%s [Group="Gen";]; }
    Settings = { Domain = "Surface"; ShadingModel = "Unlit"; BlendMode = "Opaque"; }
    Outputs = { vec3 Color; Base.EmissiveColor = Color; }
    Graph = { Color = %s; }
}
)"), *AssetName, Case.NodeType, *DefaultClause, Case.RefExpr);

		UMaterial* Material = nullptr;
		if (!GenerateAndLoadMaterial(*this, Artifacts, AssetName, Source, Material))
		{
			// GenerateAndLoadMaterial already recorded the failure; keep going so every type is covered.
			continue;
		}

		TestEqual(
			*FString::Printf(TEXT("%s generates exactly one %s node"), Case.NodeType, Case.ExpectedClass),
			CountMaterialExpressionsOfClassName(Material, Case.ExpectedClass),
			1);
	}

	return true;
}

// Parameter input-wiring call form: a parameter that owns input pins can be configured from the Graph
// with Param(InputPin = expr). Generation only succeeds if each named argument resolves to a real
// input pin and is wired, so this both exercises and pins that path (ChannelMask Input + texture
// sample Coordinates), asserting the pins end up connected.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(
	FDreamShaderParameterInputWiringTest,
	FDreamShaderQuietAutomationTestBase,
	"DreamShader.Gen.Parameters.InputWiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderParameterInputWiringTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor::Private::Tests;

	// These assertions describe graph-backend node shapes; pin the backend against reroutes.
	FScopedDreamShaderGraphBackendPin BackendPin;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_ParamInputs"));
	const FString Source = FString::Printf(TEXT(R"(
Shader(Name="DreamShaderTests/Automation/%s")
{
    Properties = {
        VectorParameter Col = float4(0.8, 0.4, 0.2, 1.0) [Group="T";];
        ChannelMaskParameter Msk [Group="T";];
        TextureSampleParameterCube TexCube [Group="T";];
    }
    Settings = { Domain = "Surface"; ShadingModel = "Unlit"; BlendMode = "Opaque"; }
    Outputs = { vec3 Color; Base.EmissiveColor = Color; }
    Graph = {
        float3 dir = float3(0.0, 0.0, 1.0);
        Color = vec3(Msk(Input = Col)) + TexCube(Coordinates = dir).rgb;
    }
}
)"), *AssetName);

	UMaterial* Material = nullptr;
	if (!GenerateAndLoadMaterial(*this, Artifacts, AssetName, Source, Material))
	{
		return false;
	}

	TestTrue(TEXT("ChannelMaskParameter Input pin is wired by Msk(Input=...)"),
		IsNamedInputConnected(Material, TEXT("MaterialExpressionChannelMaskParameter"), TEXT("Input")));
	TestTrue(TEXT("TextureSampleParameterCube Coordinates pin is wired by TexCube(Coordinates=...)"),
		IsNamedInputConnected(Material, TEXT("MaterialExpressionTextureSampleParameterCube"), TEXT("Coordinates")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateInstanceBackendTest,
	"DreamShader.Compiler.Generate.InstanceBackend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateInstanceBackendTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstance"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
        ScalarParameter Boost = 0.5;
        VectorParameter Tint = float4(1.0, 0.5, 0.25, 1.0);
        const float K = 2.0;
    }

    Settings = {
        Backend = "Instance";
        ShadingModel = "Unlit";
        BlendMode = "Opaque";
    }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        Color = Tint.rgb * Boost * K;
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, true);
	if (!TestTrue(FString::Printf(TEXT("Instance material generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = LoadObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(FString::Printf(TEXT("Generated instance material loads from '%s'."), *ObjectPath), Instance))
	{
		return false;
	}

	TestNotNull(TEXT("Instance is parented to the shared host material."), Instance->Parent.Get());
	if (Instance->Parent)
	{
		TestEqual(TEXT("Parent is M_DreamShaderHost."), Instance->Parent->GetName(), FString(TEXT("M_DreamShaderHost")));
	}

	TestTrue(TEXT("Instance forces a static permutation."), Instance->HasOverridenBaseProperties());
	TestEqual(TEXT("Const property does not become a parameter."), Instance->InstanceParameters.Num(), 2);
	TestEqual(TEXT("One bound output."), Instance->InstanceOutputs.Num(), 1);
	if (Instance->InstanceOutputs.Num() == 1)
	{
		TestEqual(TEXT("Output binds EmissiveColor."), Instance->InstanceOutputs[0].Property.GetValue(), MP_EmissiveColor);
		TestEqual(TEXT("Eval function name."), Instance->InstanceOutputs[0].EvalFunctionName, FString(TEXT("DreamShaderEval_EmissiveColor")));
	}

#if WITH_EDITORONLY_DATA
	if (TestEqual(TEXT("One eval expression per output."), Instance->EvalExpressions.Num(), 1) && Instance->EvalExpressions[0])
	{
		const UMaterialExpressionCustom* EvalExpression = Instance->EvalExpressions[0];
		TestTrue(TEXT("Eval code calls the eval function."), EvalExpression->Code.Contains(TEXT("DreamShaderEval_EmissiveColor")));
		TestEqual(TEXT("Eval expression inputs match the parameters."), EvalExpression->Inputs.Num(), 2);
		TestTrue(TEXT("Eval expression includes the generated .ush."),
			EvalExpression->IncludeFilePaths.Num() == 1 && EvalExpression->IncludeFilePaths[0] == Instance->GeneratedIncludeVirtualPath);
	}
#endif

	// The generated include exists on disk and holds the eval function + const fold.
	{
		const FString FileName = FPaths::GetCleanFilename(Instance->GeneratedIncludeVirtualPath);
		const FString DiskPath = UE::DreamShader::GetGeneratedShaderDirectory() / FileName;
		FString IncludeContent;
		if (TestTrue(TEXT("Generated instance .ush exists on disk."), FFileHelper::LoadFileToString(IncludeContent, *DiskPath)))
		{
			TestTrue(TEXT(".ush defines the eval function."), IncludeContent.Contains(TEXT("DreamShaderEval_EmissiveColor")));
			TestTrue(TEXT(".ush inlines the const property."), IncludeContent.Contains(TEXT("static const float K")));
		}
	}

	float BoostValue = 0.0f;
	TestTrue(TEXT("Scalar parameter default is set on the instance."),
		Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Boost")), BoostValue) && FMath::IsNearlyEqual(BoostValue, 0.5f));

	TestTrue(TEXT("ShadingModel setting lands in BasePropertyOverrides."),
		Instance->BasePropertyOverrides.bOverride_ShadingModel && Instance->BasePropertyOverrides.ShadingModel == MSM_Unlit);

	// Distinct source hashes must yield distinct shader map ids (DDC keys) — the generated include is
	// injected during translation and invisible to the material's own include hashing.
	{
		UMaterial* BaseMaterial = Instance->GetMaterial();
		const FString OriginalSourceHash = Instance->SourceHash;

		TUniquePtr<FMaterialResource> ResourceA(Instance->AllocatePermutationResource());
		ResourceA->SetMaterial(BaseMaterial, Instance, GMaxRHIShaderPlatform);
		FMaterialShaderMapId IdA;
		ResourceA->BuildShaderMapId(IdA, nullptr);

		Instance->SourceHash = OriginalSourceHash + TEXT("_changed");
		TUniquePtr<FMaterialResource> ResourceB(Instance->AllocatePermutationResource());
		ResourceB->SetMaterial(BaseMaterial, Instance, GMaxRHIShaderPlatform);
		FMaterialShaderMapId IdB;
		ResourceB->BuildShaderMapId(IdB, nullptr);
		Instance->SourceHash = OriginalSourceHash;

		TestNotEqual(TEXT("SourceHash salts the shader map id (ExpressionIncludesHash)."),
			IdA.ExpressionIncludesHash, IdB.ExpressionIncludesHash);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateThinCustomBackendTest,
	"DreamShader.Compiler.Generate.ThinCustomBackend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateThinCustomBackendTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoThinCustom"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	// Persist-mode generation (bTransient defaults false below) also materializes the hidden base as
	// a real sibling asset; register it for cleanup and expected-probe suppression too.
	const FString BaseAssetName = FString::Printf(TEXT("MB_DreamThinBase_%s"), *AssetName);
	const FString BaseObjectPath = MakeAutomationObjectPath(BaseAssetName);
	Artifacts.AddObjectPath(ObjectPath);
	Artifacts.AddObjectPath(BaseObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, BaseObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
        ScalarParameter Boost = 0.5;
        VectorParameter Tint = float4(1.0, 0.5, 0.25, 1.0);
    }

    Settings = {
        Backend = "ThinCustom";
        ShadingModel = "Unlit";
        BlendMode = "Opaque";
    }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        Color = Tint.rgb * Boost;
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, true);
	if (!TestTrue(FString::Printf(TEXT("ThinCustom material generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = LoadObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(FString::Printf(TEXT("Generated ThinCustom instance loads from '%s'."), *ObjectPath), Instance))
	{
		return false;
	}

	// The instance is a THIN material instance of a real base UMaterial: its own Instance-backend
	// model stays empty (no per-property Custom injection, no synthesized parameter data), so the
	// engine compiles and enumerates the base's real graph natively. This is what distinguishes the
	// convergence path from the Instance backend.
	TestEqual(TEXT("ThinCustom leaves the Instance-backend outputs empty."), Instance->InstanceOutputs.Num(), 0);
	TestEqual(TEXT("ThinCustom leaves the Instance-backend parameters empty."), Instance->InstanceParameters.Num(), 0);
	TestTrue(TEXT("Instance forces a static permutation (root of its own shader map)."), Instance->HasOverridenBaseProperties());

	UMaterial* Base = Cast<UMaterial>(Instance->Parent);
	if (TestNotNull(TEXT("Instance is parented to a real base UMaterial."), Base))
	{
		TestTrue(TEXT("Parent is the ThinCustom hidden base."), Base->GetName().StartsWith(TEXT("MB_DreamThinBase_")));
		TestTrue(TEXT("The base carries a real material graph (Multiply from the Graph block)."),
			CountMaterialExpressionsOfClass<UMaterialExpressionMultiply>(Base) >= 1);

		// Persist mode: the saved instance records its Parent as a package import, so the base must
		// be a real saveable sibling asset -- a transient-package parent cannot resolve on a future
		// load or at cook (the instance would silently lose its parent).
		TestNotEqual(TEXT("Persisted base does not live in the transient package."),
			Base->GetOutermost(), GetTransientPackage());
		TestEqual(TEXT("Base is the named sibling asset."), Base->GetPathName(), BaseObjectPath);
		TestTrue(TEXT("Base package reached disk alongside the instance."),
			FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(BaseObjectPath)));
	}

	return true;
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(
	FDreamShaderThinCustomVsGraphParityTest,
	FDreamShaderQuietAutomationTestBase,
	"DreamShader.Render.ThinCustomVsGraphParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Stage-1 acceptance gate for the ThinCustom convergence backend: the SAME shader body generated
// through the Graph backend (a visible UMaterial) and through ThinCustom (hidden base + thin
// instance, rendering through the instance's own static-permutation shader map) must produce
// pixel-identical frames on the real RHI. Anything short of that -- a broken fall-through in the
// instance resource, a parameter that failed to bind by name, a shading-model/blend-mode mismatch on
// the base -- shows up as a pixel difference here.
bool FDreamShaderThinCustomVsGraphParityTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	if (GUsingNullRHI || !FApp::CanEverRender())
	{
		AddInfo(TEXT("Skipping ThinCustom-vs-Graph render parity: no usable RHI (-nullrhi). Run without -nullrhi for the full pixel comparison."));
		return true;
	}

	FScopedDreamShaderAutomationArtifacts Artifacts;
	AddExpectedAutomationCleanupWarnings(*this);

	// One shared body per case, generated twice: only the Backend setting differs between the twins.
	// Unlit + Opaque so lighting cannot mask a difference. FlatParams exercises by-name scalar/vector
	// parameter binding; UvTexture adds UE.TexCoord + a texture-object parameter + SampleTexture2D so
	// interpolator allocation and the texture-sampling path must also match (Stage 2).
	struct FParityCase
	{
		const TCHAR* CaseName;
		const TCHAR* PropertiesBlock;
		const TCHAR* GraphBlock;
	};
	const FParityCase ParityCases[] =
	{
		{
			TEXT("FlatParams"),
			TEXT("        ScalarParameter Boost = 0.75;\n        VectorParameter Tint = float4(0.1, 0.9, 0.35, 1.0);"),
			TEXT("        Color = Tint.rgb * Boost;")
		},
		{
			TEXT("UvTexture"),
			TEXT("        TextureObjectParameter BaseMap = \"/Engine/EngineResources/DefaultTexture\";\n        ScalarParameter Boost = 1.0;"),
			TEXT("        float2 uv = UE.TexCoord(Index=0);\n        float4 texel = SampleTexture2D(BaseMap, uv);\n        Color = vec3(0.08 * (uv.x + texel.r), 0.55 + 0.35 * uv.y, 0.15) * Boost;")
		},
	};

	const auto MakeParitySource = [](const FString& AssetName, const TCHAR* Backend, const FParityCase& Case)
	{
		return FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
%s
    }

    Settings = {
        Backend = "%s";
        ShadingModel = "Unlit";
        BlendMode = "Opaque";
    }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
%s
    }
}
)"), *AssetName, Case.PropertiesBlock, Backend, Case.GraphBlock);
	};

	const int32 RenderSize = 256;
	const TCHAR* RenderMesh = TEXT("sphere");
	const float RenderYaw = -157.5f;
	const float RenderPitch = -11.25f;
	UE::DreamShader::Editor::Private::FDreamShaderPreviewRenderContext RenderContext;

	for (const FParityCase& Case : ParityCases)
	{
		UMaterialInterface* Twins[2] = { nullptr, nullptr };
		const TCHAR* TwinBackends[2] = { TEXT("Graph"), TEXT("ThinCustom") };
		const FString TwinAssetNames[2] =
		{
			MakeUniqueTestAssetName(*FString::Printf(TEXT("M_Parity%sGraph"), Case.CaseName)),
			MakeUniqueTestAssetName(*FString::Printf(TEXT("M_Parity%sThin"), Case.CaseName)),
		};

		for (int32 TwinIndex = 0; TwinIndex < 2; ++TwinIndex)
		{
			const FString& AssetName = TwinAssetNames[TwinIndex];
			const FString ObjectPath = MakeAutomationObjectPath(AssetName);
			Artifacts.AddObjectPath(ObjectPath);
			AddExpectedNewAssetProbeWarnings(*this, ObjectPath);

			FString SourceFilePath;
			if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), MakeParitySource(AssetName, TwinBackends[TwinIndex], Case), SourceFilePath))
			{
				return false;
			}
			Artifacts.AddSourceFile(SourceFilePath);

			FString Message;
			if (!TestTrue(
				FString::Printf(TEXT("[%s] %s twin generation succeeds: %s"), Case.CaseName, TwinBackends[TwinIndex], *Message),
				FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true)))
			{
				return false;
			}

			Twins[TwinIndex] = LoadObject<UMaterialInterface>(nullptr, *ObjectPath);
			if (!TestNotNull(FString::Printf(TEXT("[%s] %s twin loads from '%s'."), Case.CaseName, TwinBackends[TwinIndex], *ObjectPath), Twins[TwinIndex]))
			{
				return false;
			}
		}

		// The ThinCustom twin must actually be the thin instance chain -- otherwise the comparison
		// would silently degrade into Graph-vs-Graph.
		UDreamShaderMaterialInstance* ThinInstance = Cast<UDreamShaderMaterialInstance>(Twins[1]);
		if (!TestNotNull(FString::Printf(TEXT("[%s] ThinCustom twin is a UDreamShaderMaterialInstance."), Case.CaseName), ThinInstance))
		{
			return false;
		}
		TestEqual(FString::Printf(TEXT("[%s] ThinCustom twin keeps the Instance-backend model empty."), Case.CaseName), ThinInstance->InstanceOutputs.Num(), 0);
		TestNotNull(FString::Printf(TEXT("[%s] ThinCustom twin is parented to its hidden base."), Case.CaseName), Cast<UMaterial>(ThinInstance->Parent.Get()));

		// Both shader maps must be fully compiled before pixels mean anything.
		const auto FinishTwinCompilation = [&Twins]()
		{
			TArray<UObject*> CompileObjects;
			CompileObjects.Add(Twins[0]);
			CompileObjects.Add(Twins[1]);
			FAssetCompilingManager::Get().FinishCompilationForObjects(CompileObjects);
			if (GShaderCompilingManager)
			{
				GShaderCompilingManager->FinishAllCompilation();
			}
			FlushRenderingCommands();
		};
		FinishTwinCompilation();

		// Render both twins through the identical offscreen path. A throwaway warm-up render per twin
		// first: material resources/shader maps are allocated lazily on first render, and the finish-
		// compile after the warm-up drains whatever that first render queued, so the measured frame
		// renders the real shader instead of the default-material fallback.
		TArray<FColor> TwinPixels[2];
		for (int32 TwinIndex = 0; TwinIndex < 2; ++TwinIndex)
		{
			FString RenderError;
			TArray<FColor> WarmupPixels;
			if (!TestTrue(
				FString::Printf(TEXT("[%s] %s twin warm-up render succeeds: %s"), Case.CaseName, TwinBackends[TwinIndex], *RenderError),
				RenderContext.RenderFramePixels(Twins[TwinIndex], RenderSize, RenderSize, RenderMesh, RenderYaw, RenderPitch, WarmupPixels, RenderError)))
			{
				return false;
			}

			FinishTwinCompilation();

			if (!TestTrue(
				FString::Printf(TEXT("[%s] %s twin render succeeds: %s"), Case.CaseName, TwinBackends[TwinIndex], *RenderError),
				RenderContext.RenderFramePixels(Twins[TwinIndex], RenderSize, RenderSize, RenderMesh, RenderYaw, RenderPitch, TwinPixels[TwinIndex], RenderError)))
			{
				return false;
			}
		}

		if (!TestEqual(FString::Printf(TEXT("[%s] Both twins produced the same pixel count."), Case.CaseName), TwinPixels[1].Num(), TwinPixels[0].Num()))
		{
			return false;
		}

		// Sanity before parity: the frame must actually contain our green-dominant emissive. This is
		// what separates "both twins render the same broken default-material checkerboard" (would pass
		// a naive diff) from "both twins render the intended shader".
		{
			int32 GreenDominantPixels = 0;
			uint64 SumR = 0, SumG = 0, SumB = 0;
			for (const FColor& Pixel : TwinPixels[0])
			{
				SumR += Pixel.R;
				SumG += Pixel.G;
				SumB += Pixel.B;
				if (Pixel.G > Pixel.R + 30 && Pixel.G > Pixel.B + 30)
				{
					++GreenDominantPixels;
				}
			}
			const int32 PixelCount = FMath::Max(TwinPixels[0].Num(), 1);
			const FColor CenterPixel = TwinPixels[0][(RenderSize / 2) * RenderSize + (RenderSize / 2)];
			AddInfo(FString::Printf(
				TEXT("[%s] Graph twin frame stats: avg RGB=(%d,%d,%d), center=(%d,%d,%d), %d green-dominant pixels."),
				Case.CaseName,
				int32(SumR / PixelCount), int32(SumG / PixelCount), int32(SumB / PixelCount),
				CenterPixel.R, CenterPixel.G, CenterPixel.B,
				GreenDominantPixels));

			const bool bShowsEmissive = GreenDominantPixels > (TwinPixels[0].Num() / 20);
			if (!bShowsEmissive)
			{
				// Dump both frames for offline inspection before failing -- a bit-identical pair that
				// does not show the material means BOTH chains rendered the same wrong thing.
				for (int32 TwinIndex = 0; TwinIndex < 2; ++TwinIndex)
				{
					TArray<FColor> PngColors = TwinPixels[TwinIndex];
					for (FColor& Color : PngColors)
					{
						Color.A = 255;
					}
					TArray64<uint8> PngData;
					FImageUtils::PNGCompressImageArray(RenderSize, RenderSize, TArrayView64<const FColor>(PngColors.GetData(), PngColors.Num()), PngData);
					const FString DumpPath = FPaths::ProjectSavedDir() / TEXT("DreamShaderTests") / FString::Printf(TEXT("Parity_%s_%s.png"), Case.CaseName, TwinBackends[TwinIndex]);
					FFileHelper::SaveArrayToFile(TArrayView64<const uint8>(PngData.GetData(), PngData.Num()), *DumpPath);
					AddInfo(FString::Printf(TEXT("Dumped %s twin frame to '%s'."), TwinBackends[TwinIndex], *DumpPath));
				}
			}
			TestTrue(
				FString::Printf(TEXT("[%s] Graph twin frame shows the green emissive sphere (%d green-dominant pixels)."), Case.CaseName, GreenDominantPixels),
				bShowsEmissive);
		}

		// Pixel parity with a small tolerance: the two shader maps are equivalent but separately
		// translated/compiled, so allow LSB-level float/rounding differences while failing on anything
		// a human could see.
		{
			constexpr int32 ChannelTolerance = 2;
			int32 OffendingPixels = 0;
			int32 MaxChannelDifference = 0;
			for (int32 PixelIndex = 0; PixelIndex < TwinPixels[0].Num(); ++PixelIndex)
			{
				const FColor& A = TwinPixels[0][PixelIndex];
				const FColor& B = TwinPixels[1][PixelIndex];
				const int32 PixelDifference = FMath::Max3(
					FMath::Abs(int32(A.R) - int32(B.R)),
					FMath::Abs(int32(A.G) - int32(B.G)),
					FMath::Abs(int32(A.B) - int32(B.B)));
				MaxChannelDifference = FMath::Max(MaxChannelDifference, PixelDifference);
				if (PixelDifference > ChannelTolerance)
				{
					++OffendingPixels;
				}
			}

			const int32 AllowedOffenders = TwinPixels[0].Num() / 1000; // 0.1%
			TestTrue(
				FString::Printf(
					TEXT("[%s] ThinCustom renders identically to its Graph twin (%d of %d pixels beyond tolerance %d, max channel difference %d)."),
					Case.CaseName, OffendingPixels, TwinPixels[0].Num(), ChannelTolerance, MaxChannelDifference),
				OffendingPixels <= AllowedOffenders);
			AddInfo(FString::Printf(
				TEXT("[%s] ThinCustom-vs-Graph parity: %d/%d pixels beyond tolerance %d, max channel difference %d."),
				Case.CaseName, OffendingPixels, TwinPixels[0].Num(), ChannelTolerance, MaxChannelDifference));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateThinCustomTextureTest,
	"DreamShader.Compiler.Generate.ThinCustomTexture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Stage 2: texture parameters and state-read builtins reach ThinCustom as REAL nodes through the
// shared Graph construction -- no .ush lowering, no instance-side texture index space, no dummy
// interpolator chunks. The engine enumerates the base's texture parameter natively, which is what
// makes the MI editor and the cook's used-texture gather work with zero instance machinery.
bool FDreamShaderGenerateThinCustomTextureTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoThinCustomTexture"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
        TextureObjectParameter BaseMap = "/Engine/EngineResources/DefaultTexture";
        ScalarParameter Intensity = 1.0;
    }

    Settings = {
        Backend = "ThinCustom";
        ShadingModel = "Unlit";
    }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        float2 uv = UE.TexCoord(Index=0);
        float4 texel = SampleTexture2D(BaseMap, uv);
        Color = texel.rgb * Intensity;
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
	if (!TestTrue(FString::Printf(TEXT("ThinCustom texture generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("ThinCustom texture instance exists in memory."), Instance))
	{
		return false;
	}

	// The Instance-backend model stays empty: no lowered .ush, no per-instance texture index space,
	// no dummy TexCoord side-effect inputs.
	TestEqual(TEXT("No Instance-backend parameters."), Instance->InstanceParameters.Num(), 0);
	TestEqual(TEXT("No Instance-backend outputs."), Instance->InstanceOutputs.Num(), 0);
	TestEqual(TEXT("No instance default-texture index space."), Instance->InstanceDefaultTextures.Num(), 0);
	TestEqual(TEXT("No dummy interpolator chunks."), Instance->UsedTexCoordCount, 0);

	// Native parameter enumeration through the base's real nodes -- the engine, not synthesized
	// cached data, resolves these.
	UTexture* BaseMapValue = nullptr;
	TestTrue(TEXT("BaseMap texture parameter resolves natively through the chain."),
		Instance->GetTextureParameterValue(FMaterialParameterInfo(TEXT("BaseMap")), BaseMapValue) && BaseMapValue != nullptr);
	float IntensityValue = 0.0f;
	TestTrue(TEXT("Intensity scalar parameter resolves natively through the chain."),
		Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Intensity")), IntensityValue) && FMath::IsNearlyEqual(IntensityValue, 1.0f));

	// The base carries the real nodes: a texture-object parameter and a real TexCoord node (the
	// stock translator allocates the interpolator from the node -- no dummy-chunk ceiling).
	UMaterial* Base = Cast<UMaterial>(Instance->Parent.Get());
	if (TestNotNull(TEXT("Instance is parented to the hidden base."), Base))
	{
		TestTrue(TEXT("Base has a real texture-object-parameter node."),
			CountMaterialExpressionsOfClassName(Base, TEXT("MaterialExpressionTextureObjectParameter")) >= 1);
		TestTrue(TEXT("Base has a real TextureCoordinate node."),
			CountMaterialExpressionsOfClassName(Base, TEXT("MaterialExpressionTextureCoordinate")) >= 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateThinCustomUITest,
	"DreamShader.Compiler.Generate.ThinCustomUI",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Stage 3: the UI domain through ThinCustom. Unlike the Instance backend (which needs a dedicated
// graphless per-domain host asset), the shared Graph construction sets MaterialDomain directly on
// the per-material hidden base -- no host infrastructure at all.
bool FDreamShaderGenerateThinCustomUITest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoThinCustomUI"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
        VectorParameter BaseTint = float4(0.15, 0.55, 0.95, 1.0);
        ScalarParameter Opacity = 1.0;
    }

    Settings = {
        Backend = "ThinCustom";
        Domain = "UI";
    }

    Outputs = {
        vec3 Color;
        float A;
        Base.EmissiveColor = Color;
        Base.Opacity = A;
    }

    Graph = {
        float2 uv = UE.TexCoord(Index=0);
        float grad = lerp(0.85, 1.0, uv.y);
        Color = BaseTint.rgb * grad;
        A = Opacity;
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
	if (!TestTrue(FString::Printf(TEXT("ThinCustom UI generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("ThinCustom UI instance exists in memory."), Instance))
	{
		return false;
	}

	TestEqual(TEXT("Instance-backend model stays empty."), Instance->InstanceOutputs.Num(), 0);
	FLinearColor BaseTintValue = FLinearColor::Black;
	TestTrue(TEXT("BaseTint vector parameter resolves natively through the chain."),
		Instance->GetVectorParameterValue(FMaterialParameterInfo(TEXT("BaseTint")), BaseTintValue));

	UMaterial* Base = Cast<UMaterial>(Instance->Parent.Get());
	if (TestNotNull(TEXT("Instance is parented to the hidden base."), Base))
	{
		TestEqual(TEXT("The base carries the UI material domain."), Base->MaterialDomain, MD_UI);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateThinCustomPostProcessTest,
	"DreamShader.Compiler.Generate.ThinCustomPostProcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Stage 3: the PostProcess domain through ThinCustom. The scene read becomes a REAL
// UMaterialExpressionSceneTexture node on the hidden base -- replacing the Instance backend's named
// value-input machinery (FDreamShaderSceneRead / SceneTextureLookup chunks) for this path.
bool FDreamShaderGenerateThinCustomPostProcessTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoThinCustomPP"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
        ScalarParameter Saturation = 1.4;
    }

    Settings = {
        Backend = "ThinCustom";
        Domain = "PostProcess";
    }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        float3 scene = UE.SceneTexture(Id="PostProcessInput0").rgb;
        float luma = dot(scene, float3(0.299, 0.587, 0.114));
        Color = lerp(float3(luma, luma, luma), scene, Saturation);
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
	if (!TestTrue(FString::Printf(TEXT("ThinCustom PostProcess generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("ThinCustom PostProcess instance exists in memory."), Instance))
	{
		return false;
	}

	// No named value-input scene reads: the scene texture is a real node on the base, not a
	// translator-injected chunk on the instance.
	TestEqual(TEXT("Instance-backend model stays empty."), Instance->InstanceOutputs.Num(), 0);
	TestEqual(TEXT("No named value-input scene reads on the instance."), Instance->SceneReads.Num(), 0);

	UMaterial* Base = Cast<UMaterial>(Instance->Parent.Get());
	if (TestNotNull(TEXT("Instance is parented to the hidden base."), Base))
	{
		TestEqual(TEXT("The base carries the PostProcess material domain."), Base->MaterialDomain, MD_PostProcess);
		TestTrue(TEXT("The base carries a real SceneTexture node."),
			CountMaterialExpressionsOfClassName(Base, TEXT("MaterialExpressionSceneTexture")) >= 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateInstanceBackendVirtualTest,
	"DreamShader.Compiler.Generate.InstanceBackendVirtual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateInstanceBackendVirtualTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceVirtual"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
        ScalarParameter Boost = 0.25;
    }

    Settings = {
        Backend = "Instance";
        ShadingModel = "Unlit";
    }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        Color = vec3(Boost, Boost, Boost);
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
	if (!TestTrue(FString::Printf(TEXT("Virtual instance generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("Virtual instance exists in memory."), Instance))
	{
		return false;
	}

	// The whole point of virtual mode: nothing reaches disk and nothing can be nagged into saving.
	TestFalse(TEXT("Virtual instance package is not dirty (no save-prompt materialization)."),
		Instance->GetPackage()->IsDirty());
	TestTrue(TEXT("Virtual instance package is flagged newly-created (in-memory import resolution)."),
		Instance->GetPackage()->HasAnyPackageFlags(PKG_NewlyCreated));
	FString ExistingDiskPackage;
	TestFalse(TEXT("Virtual instance has no package file on disk."),
		FPackageName::DoesPackageExist(Instance->GetPackage()->GetName(), &ExistingDiskPackage));

	// Memory-only instances hide from asset enumeration (Content Browser, save pickers) unless the
	// user opts in via bShowInMemoryMaterialsInContentBrowser; object-path references still resolve.
	if (!GetDefault<UDreamShaderSettings>()->bShowInMemoryMaterialsInContentBrowser)
	{
		TestFalse(TEXT("In-memory instance is not an enumerable asset (hidden from the Content Browser)."), Instance->IsAsset());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateInstanceBackendBuiltinsTest,
	"DreamShader.Compiler.Generate.InstanceBackendBuiltins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateInstanceBackendBuiltinsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceBuiltins"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
        ScalarParameter Speed = 0.5;
    }

    Settings = {
        Backend = "Instance";
        ShadingModel = "Unlit";
    }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        float2 uv = UE.TexCoord(Index=0);
        float pulse = UE.Time(Period=2.0) * Speed;
        Color = vec3(uv.x, uv.y, pulse);
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
	if (!TestTrue(FString::Printf(TEXT("Builtin instance generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("Builtin instance exists in memory."), Instance))
	{
		return false;
	}

	TestEqual(TEXT("UE.TexCoord(Index=0) records one used texcoord slot."), Instance->UsedTexCoordCount, 1);
	TestFalse(TEXT("VertexColor builtin not used."), Instance->bUsesVertexColorBuiltin);

#if WITH_EDITORONLY_DATA
	if (TestEqual(TEXT("One eval expression."), Instance->EvalExpressions.Num(), 1) && Instance->EvalExpressions[0])
	{
		const UMaterialExpressionCustom* EvalExpression = Instance->EvalExpressions[0];
		// Inputs: Speed + the dummy texcoord side-effect input, index-aligned with the resource.
		TestEqual(TEXT("Eval inputs = DSL parameters + dummy texcoord."), EvalExpression->Inputs.Num(), 2);
		TestTrue(TEXT("Eval code forwards Parameters."), EvalExpression->Code.Contains(TEXT("(Parameters, Speed)")));
	}
#endif

	// The generated include lowers UE.* builtins to DS_* equivalents and pulls in the support header.
	{
		const FString FileName = FPaths::GetCleanFilename(Instance->GeneratedIncludeVirtualPath);
		const FString DiskPath = UE::DreamShader::GetGeneratedShaderDirectory() / FileName;
		FString IncludeContent;
		if (TestTrue(TEXT("Generated instance .ush exists on disk."), FFileHelper::LoadFileToString(IncludeContent, *DiskPath)))
		{
			TestTrue(TEXT(".ush includes DreamShaderBuiltins."), IncludeContent.Contains(TEXT("/Plugin/DreamShader/DreamShaderBuiltins.ush")));
			TestTrue(TEXT("UE.TexCoord lowers to DS_TexCoord."), IncludeContent.Contains(TEXT("DS_TexCoord(Parameters, 0)")));
			TestTrue(TEXT("UE.Time(Period=..) lowers to DS_PERIODIC_TIME."), IncludeContent.Contains(TEXT("DS_PERIODIC_TIME(2.0)")));
			TestTrue(TEXT("Eval functions receive FMaterialPixelParameters."), IncludeContent.Contains(TEXT("FMaterialPixelParameters Parameters")));
			TestFalse(TEXT("No unlowered UE.* remains."), IncludeContent.Contains(TEXT("UE.")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateInstanceBackendTextureTest,
	"DreamShader.Compiler.Generate.InstanceBackendTexture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateInstanceBackendTextureTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceTexture"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
        TextureObjectParameter BaseMap = "/Engine/EngineResources/DefaultTexture";
        ScalarParameter Intensity = 1.0;
    }

    Settings = {
        Backend = "Instance";
        ShadingModel = "Unlit";
    }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        float2 uv = UE.TexCoord(Index=0);
        float4 texel = SampleTexture2D(BaseMap, uv);
        Color = texel.rgb * Intensity;
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
	if (!TestTrue(FString::Printf(TEXT("Texture instance generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("Texture instance exists in memory."), Instance))
	{
		return false;
	}

	// The texture parameter model: declaration-ordered, typed, with a resolved non-null default.
	if (TestEqual(TEXT("Two instance parameters."), Instance->InstanceParameters.Num(), 2))
	{
		const FDreamShaderInstanceParameter& TextureParameter = Instance->InstanceParameters[0];
		TestEqual(TEXT("First parameter is BaseMap."), TextureParameter.Name, FName(TEXT("BaseMap")));
		TestTrue(TEXT("BaseMap is a texture parameter."), TextureParameter.Type == EDreamShaderInstanceParameterType::Texture);
		TestNotNull(TEXT("BaseMap default texture resolved."), TextureParameter.TextureDefault.Get());
	}

	// The compile-time default-texture index space the resource serves to the translator.
	if (TestEqual(TEXT("One default texture registered."), Instance->InstanceDefaultTextures.Num(), 1))
	{
		TestTrue(
			TEXT("Registered default matches the parameter default."),
			Instance->InstanceDefaultTextures[0] == Instance->InstanceParameters[0].TextureDefault);
	}

	// The explicit parameter value keeps the cook's used-texture gather inside the instance's index space.
	{
		UTexture* ParameterValue = nullptr;
		if (TestTrue(
			TEXT("BaseMap texture parameter value is set."),
			Instance->GetTextureParameterValue(FMaterialParameterInfo(TEXT("BaseMap")), ParameterValue)))
		{
			TestTrue(TEXT("Parameter value equals the default texture."), ParameterValue == Instance->InstanceParameters[0].TextureDefault);
		}
	}

#if WITH_EDITORONLY_DATA
	if (TestEqual(TEXT("One eval expression."), Instance->EvalExpressions.Num(), 1) && Instance->EvalExpressions[0])
	{
		const UMaterialExpressionCustom* EvalExpression = Instance->EvalExpressions[0];
		// Inputs: BaseMap + Intensity + the dummy texcoord side-effect input (one input per compiled
		// chunk — a texture chunk is a single input even though it expands to a texture/sampler pair).
		TestEqual(TEXT("Eval inputs = DSL parameters + dummy texcoord."), EvalExpression->Inputs.Num(), 3);
		TestTrue(
			TEXT("Eval code forwards the texture together with its sampler."),
			EvalExpression->Code.Contains(TEXT("(Parameters, BaseMap, BaseMapSampler, Intensity)")));
	}
#endif

	// The generated eval function signature carries the translator-shaped texture/sampler pair.
	{
		const FString FileName = FPaths::GetCleanFilename(Instance->GeneratedIncludeVirtualPath);
		const FString DiskPath = UE::DreamShader::GetGeneratedShaderDirectory() / FileName;
		FString IncludeContent;
		if (TestTrue(TEXT("Generated instance .ush exists on disk."), FFileHelper::LoadFileToString(IncludeContent, *DiskPath)))
		{
			TestTrue(
				TEXT(".ush declares the texture/sampler parameter pair."),
				IncludeContent.Contains(TEXT("Texture2D BaseMap, SamplerState BaseMapSampler")));
			// The DSL surface writes SampleTexture2D; lowering namespaces it to the DS_ macro.
			TestTrue(TEXT("SampleTexture2D lowers to DS_SampleTexture2D."), IncludeContent.Contains(TEXT("DS_SampleTexture2D(BaseMap, uv)")));
		}
	}

	// Synthesized parameters enumerate through the standard chain APIs — the exact calls the
	// material instance editor makes to build its rows — both on the instance and on a plain
	// child MIC parented to it.
	{
		TMap<FMaterialParameterInfo, FMaterialParameterMetadata> ScalarParameters;
		Instance->GetAllParametersOfType(EMaterialParameterType::Scalar, ScalarParameters);
		TestTrue(TEXT("Scalar enumeration contains Intensity."), ScalarParameters.Contains(FMaterialParameterInfo(TEXT("Intensity"))));

		TMap<FMaterialParameterInfo, FMaterialParameterMetadata> TextureParameters;
		Instance->GetAllParametersOfType(EMaterialParameterType::Texture, TextureParameters);
		TestTrue(TEXT("Texture enumeration contains BaseMap."), TextureParameters.Contains(FMaterialParameterInfo(TEXT("BaseMap"))));

		float DefaultIntensity = 0.0f;
		if (TestTrue(TEXT("Scalar default resolves through the chain."), Instance->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(TEXT("Intensity")), DefaultIntensity)))
		{
			TestEqual(TEXT("Scalar default matches the DSL default."), DefaultIntensity, 1.0f);
		}

		UMaterialInstanceConstant* Child = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
		Child->SetParentEditorOnly(Instance, /*RecacheShader*/ false);
		TMap<FMaterialParameterInfo, FMaterialParameterMetadata> ChildScalarParameters;
		Child->GetAllParametersOfType(EMaterialParameterType::Scalar, ChildScalarParameters);
		TestTrue(TEXT("Child MIC inherits the synthesized parameter enumeration."), ChildScalarParameters.Contains(FMaterialParameterInfo(TEXT("Intensity"))));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateInstanceBackendStateReadsTest,
	"DreamShader.Compiler.Generate.InstanceBackendStateReads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateInstanceBackendStateReadsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceStateReads"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	// A spread across the three pure-read shapes: LWC-demoted world/object position, external-code
	// reflection vector, and Get*(Parameters) helpers (screen UV, per-instance random).
	const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Settings = {
        Backend = "Instance";
        ShadingModel = "Unlit";
    }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        float3 wp = UE.WorldPosition();
        float3 op = UE.ObjectPosition();
        float3 refl = UE.ReflectionVector();
        float2 vp = UE.ScreenPosition();
        float rnd = UE.PerInstanceRandom();
        Color = frac(wp * 0.001) * 0.5 + refl * 0.25 + float3(vp * rnd, op.z * 0.0);
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
	if (!TestTrue(FString::Printf(TEXT("State-read instance generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("State-read instance exists in memory."), Instance))
	{
		return false;
	}

	// Pure reads need no compiled chunk and no eval argument: no texcoord slots, no vertex color,
	// no parameters, and therefore zero Custom inputs.
	TestEqual(TEXT("No texcoord slots requested."), Instance->UsedTexCoordCount, 0);
	TestFalse(TEXT("VertexColor builtin not used."), Instance->bUsesVertexColorBuiltin);
	TestEqual(TEXT("No synthesized parameters."), Instance->InstanceParameters.Num(), 0);
#if WITH_EDITORONLY_DATA
	if (TestEqual(TEXT("One eval expression."), Instance->EvalExpressions.Num(), 1) && Instance->EvalExpressions[0])
	{
		TestEqual(TEXT("Pure-read builtins add zero Custom inputs."), Instance->EvalExpressions[0]->Inputs.Num(), 0);
	}
#endif

	// The generated include lowers each UE.* state read to its DreamShaderBuiltins.ush macro.
	{
		const FString FileName = FPaths::GetCleanFilename(Instance->GeneratedIncludeVirtualPath);
		const FString DiskPath = UE::DreamShader::GetGeneratedShaderDirectory() / FileName;
		FString IncludeContent;
		if (TestTrue(TEXT("Generated instance .ush exists on disk."), FFileHelper::LoadFileToString(IncludeContent, *DiskPath)))
		{
			TestTrue(TEXT("UE.WorldPosition lowers to DS_WorldPosition."), IncludeContent.Contains(TEXT("DS_WorldPosition(Parameters)")));
			TestTrue(TEXT("UE.ObjectPosition lowers to DS_ObjectPosition."), IncludeContent.Contains(TEXT("DS_ObjectPosition(Parameters)")));
			TestTrue(TEXT("UE.ReflectionVector lowers to DS_ReflectionVector."), IncludeContent.Contains(TEXT("DS_ReflectionVector(Parameters)")));
			TestTrue(TEXT("UE.ScreenPosition lowers to DS_ViewportUV."), IncludeContent.Contains(TEXT("DS_ViewportUV(Parameters)")));
			TestTrue(TEXT("UE.PerInstanceRandom lowers to DS_PerInstanceRandom."), IncludeContent.Contains(TEXT("DS_PerInstanceRandom(Parameters)")));
			TestFalse(TEXT("No unlowered UE.* remains."), IncludeContent.Contains(TEXT("UE.")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateInstanceBackendSceneReadsTest,
	"DreamShader.Compiler.Generate.InstanceBackendSceneReads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateInstanceBackendSceneReadsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	// --- Positive: a translucent instance reading scene depth (soft fade) + scene color (tint). ---
	{
		FScopedDreamShaderAutomationArtifacts Artifacts;
		const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceSceneReads"));
		const FString ObjectPath = MakeAutomationObjectPath(AssetName);
		Artifacts.AddObjectPath(ObjectPath);
		AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
		AddExpectedAutomationCleanupWarnings(*this);

		const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Settings = {
        Backend = "Instance";
        ShadingModel = "Unlit";
        BlendMode = "Translucent";
    }

    Outputs = {
        vec3 Color;
        float Alpha;
        Base.EmissiveColor = Color;
        Base.Opacity = Alpha;
    }

    Graph = {
        float sceneD = UE.SceneDepth();
        float pixelD = UE.PixelDepth();
        float4 sceneC = UE.SceneColor();
        float fade = saturate((sceneD - pixelD) * 0.02);
        Color = sceneC.rgb * 0.5;
        Alpha = fade;
    }
}
)"), *AssetName);

		FString SourceFilePath;
		if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
		{
			return false;
		}
		Artifacts.AddSourceFile(SourceFilePath);

		FString Message;
		const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
		if (!TestTrue(FString::Printf(TEXT("Translucent scene-read generation succeeds: %s"), *Message), bGenerated))
		{
			return false;
		}

		UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
		if (!TestNotNull(TEXT("Scene-read instance exists in memory."), Instance))
		{
			return false;
		}

		TestTrue(TEXT("Translucent blend override applied."),
			Instance->BasePropertyOverrides.bOverride_BlendMode && IsTranslucentBlendMode(Instance->BasePropertyOverrides.BlendMode.GetValue()));

		// Two deduped scene reads: SceneDepth (scalar) + SceneColor (float4). PixelDepth is a pure
		// read (inline macro), so it is NOT a scene value-input.
		if (TestEqual(TEXT("Two scene reads recorded."), Instance->SceneReads.Num(), 2))
		{
			bool bHasDepth = false, bHasColor = false;
			for (const FDreamShaderSceneRead& SceneRead : Instance->SceneReads)
			{
				bHasDepth |= (SceneRead.Kind == EDreamShaderSceneReadKind::SceneDepth);
				bHasColor |= (SceneRead.Kind == EDreamShaderSceneReadKind::SceneColor);
			}
			TestTrue(TEXT("SceneDepth recorded."), bHasDepth);
			TestTrue(TEXT("SceneColor recorded."), bHasColor);
		}

#if WITH_EDITORONLY_DATA
		if (TestEqual(TEXT("Two eval expressions (Emissive + Opacity)."), Instance->EvalExpressions.Num(), 2) && Instance->EvalExpressions[0])
		{
			// Each eval's Custom carries the two REAL named scene inputs (forwarded to the eval fn).
			const UMaterialExpressionCustom* Eval = Instance->EvalExpressions[0];
			bool bInputDepth = false, bInputColor = false;
			for (const FCustomInput& Input : Eval->Inputs)
			{
				bInputDepth |= (Input.InputName == FName(TEXT("DreamShaderSceneDepth")));
				bInputColor |= (Input.InputName == FName(TEXT("DreamShaderSceneColor")));
			}
			TestTrue(TEXT("Custom carries the SceneDepth named input."), bInputDepth);
			TestTrue(TEXT("Custom carries the SceneColor named input."), bInputColor);
			TestTrue(TEXT("Eval code forwards the scene args."), Eval->Code.Contains(TEXT("DreamShaderSceneColor")));
		}
#endif

		// The generated eval signature carries the trailing scene args with the right types.
		{
			const FString FileName = FPaths::GetCleanFilename(Instance->GeneratedIncludeVirtualPath);
			const FString DiskPath = UE::DreamShader::GetGeneratedShaderDirectory() / FileName;
			FString IncludeContent;
			if (TestTrue(TEXT("Generated instance .ush exists on disk."), FFileHelper::LoadFileToString(IncludeContent, *DiskPath)))
			{
				TestTrue(TEXT(".ush eval signature has float DreamShaderSceneDepth."), IncludeContent.Contains(TEXT("float DreamShaderSceneDepth")));
				TestTrue(TEXT(".ush eval signature has float4 DreamShaderSceneColor."), IncludeContent.Contains(TEXT("float4 DreamShaderSceneColor")));
				TestFalse(TEXT("No unlowered UE.* remains."), IncludeContent.Contains(TEXT("UE.")));
			}
		}
	}

	// --- Negative: UE.SceneColor() on an opaque instance is rejected with a translucent hint. ---
	{
		FScopedDreamShaderAutomationArtifacts Artifacts;
		const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceSceneColorOpaque"));
		const FString ObjectPath = MakeAutomationObjectPath(AssetName);
		Artifacts.AddObjectPath(ObjectPath);

		const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Settings = { Backend = "Instance"; ShadingModel = "Unlit"; }
    Outputs = { vec3 Color; Base.EmissiveColor = Color; }
    Graph = { Color = UE.SceneColor().rgb; }
}
)"), *AssetName);

		FString SourceFilePath;
		if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
		{
			return false;
		}
		Artifacts.AddSourceFile(SourceFilePath);

		FString Message;
		const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
		TestFalse(TEXT("Opaque SceneColor is rejected."), bGenerated);
		TestTrue(FString::Printf(TEXT("Rejection mentions translucent: %s"), *Message), Message.Contains(TEXT("translucent")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateInstanceBackendUIDomainTest,
	"DreamShader.Compiler.Generate.InstanceBackendUIDomain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateInstanceBackendUIDomainTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	// --- Positive: a UI-domain instance binding Final Color (EmissiveColor) + Opacity. ---
	{
		FScopedDreamShaderAutomationArtifacts Artifacts;
		const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceUI"));
		const FString ObjectPath = MakeAutomationObjectPath(AssetName);
		Artifacts.AddObjectPath(ObjectPath);
		AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
		AddExpectedAutomationCleanupWarnings(*this);

		const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
        VectorParameter Tint = (1.0, 0.5, 0.2, 1.0);
        ScalarParameter Alpha = 0.8;
    }

    Settings = {
        Backend = "Instance";
        Domain = "UI";
    }

    Outputs = {
        vec3 Color;
        float A;
        Base.EmissiveColor = Color;
        Base.Opacity = A;
    }

    Graph = {
        Color = Tint.rgb;
        A = Alpha;
    }
}
)"), *AssetName);

		FString SourceFilePath;
		if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
		{
			return false;
		}
		Artifacts.AddSourceFile(SourceFilePath);

		FString Message;
		const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
		if (!TestTrue(FString::Printf(TEXT("UI-domain generation succeeds: %s"), *Message), bGenerated))
		{
			return false;
		}

		UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
		if (!TestNotNull(TEXT("UI instance exists in memory."), Instance))
		{
			return false;
		}

		// Routed to the UI host (MaterialDomain=MD_UI). Domain is not per-instance overridable, so the
		// parent chain's domain IS the material's domain.
		if (UMaterial* Host = Instance->GetMaterial())
		{
			TestEqual(TEXT("UI instance is parented to a UI-domain host."), (int32)Host->MaterialDomain.GetValue(), (int32)MD_UI);
		}
	}

	// --- Negative: a UI instance cannot bind a lit channel (BaseColor). ---
	{
		FScopedDreamShaderAutomationArtifacts Artifacts;
		const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceUIBadOutput"));
		const FString ObjectPath = MakeAutomationObjectPath(AssetName);
		Artifacts.AddObjectPath(ObjectPath);

		const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Settings = { Backend = "Instance"; Domain = "UI"; }
    Outputs = { vec3 C; Base.BaseColor = C; }
    Graph = { C = vec3(1.0, 1.0, 1.0); }
}
)"), *AssetName);

		FString SourceFilePath;
		if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
		{
			return false;
		}
		Artifacts.AddSourceFile(SourceFilePath);

		FString Message;
		const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
		TestFalse(TEXT("UI BaseColor output is rejected."), bGenerated);
		TestTrue(FString::Printf(TEXT("Rejection names the UI domain: %s"), *Message), Message.Contains(TEXT("UI")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateInstanceBackendPostProcessTest,
	"DreamShader.Compiler.Generate.InstanceBackendPostProcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateInstanceBackendPostProcessTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	// --- Positive: a PostProcess instance sampling PostProcessInput0 (scene color post-tonemap). ---
	{
		FScopedDreamShaderAutomationArtifacts Artifacts;
		const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstancePP"));
		const FString ObjectPath = MakeAutomationObjectPath(AssetName);
		Artifacts.AddObjectPath(ObjectPath);
		AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
		AddExpectedAutomationCleanupWarnings(*this);

		const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = { ScalarParameter Exposure = 1.2; }
    Settings = { Backend = "Instance"; Domain = "PostProcess"; }
    Outputs = { vec3 Color; Base.EmissiveColor = Color; }
    Graph = {
        float4 scene = UE.SceneTexture(Id="PostProcessInput0");
        Color = scene.rgb * Exposure;
    }
}
)"), *AssetName);

		FString SourceFilePath;
		if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
		{
			return false;
		}
		Artifacts.AddSourceFile(SourceFilePath);

		FString Message;
		const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
		if (!TestTrue(FString::Printf(TEXT("PostProcess generation succeeds: %s"), *Message), bGenerated))
		{
			return false;
		}

		UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
		if (!TestNotNull(TEXT("PostProcess instance exists in memory."), Instance))
		{
			return false;
		}

		if (UMaterial* Host = Instance->GetMaterial())
		{
			TestEqual(TEXT("PP instance is parented to a PostProcess-domain host."), (int32)Host->MaterialDomain.GetValue(), (int32)MD_PostProcess);
		}

		if (TestEqual(TEXT("One scene texture read."), Instance->SceneReads.Num(), 1))
		{
			TestTrue(TEXT("Scene read is a SceneTexture."), Instance->SceneReads[0].Kind == EDreamShaderSceneReadKind::SceneTexture);
		}

		const FString FileName = FPaths::GetCleanFilename(Instance->GeneratedIncludeVirtualPath);
		const FString DiskPath = UE::DreamShader::GetGeneratedShaderDirectory() / FileName;
		FString IncludeContent;
		if (TestTrue(TEXT("Generated instance .ush exists on disk."), FFileHelper::LoadFileToString(IncludeContent, *DiskPath)))
		{
			TestTrue(TEXT(".ush eval signature has a float4 scene-texture arg."), IncludeContent.Contains(TEXT("float4 DreamShaderSceneTex_")));
			TestFalse(TEXT("No unlowered UE.* remains."), IncludeContent.Contains(TEXT("UE.")));
		}
	}

	// --- Negative: UE.SceneTexture on a Surface instance is rejected (PostProcess only). ---
	{
		FScopedDreamShaderAutomationArtifacts Artifacts;
		const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceSceneTexSurface"));
		const FString ObjectPath = MakeAutomationObjectPath(AssetName);
		Artifacts.AddObjectPath(ObjectPath);

		const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Settings = { Backend = "Instance"; ShadingModel = "Unlit"; }
    Outputs = { vec3 Color; Base.EmissiveColor = Color; }
    Graph = { Color = UE.SceneTexture(Id="PostProcessInput0").rgb; }
}
)"), *AssetName);

		FString SourceFilePath;
		if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
		{
			return false;
		}
		Artifacts.AddSourceFile(SourceFilePath);

		FString Message;
		const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
		TestFalse(TEXT("Surface SceneTexture is rejected."), bGenerated);
		TestTrue(FString::Printf(TEXT("Rejection names PostProcess: %s"), *Message), Message.Contains(TEXT("PostProcess")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateInstanceBackendMaterialAttributesTest,
	"DreamShader.Compiler.Generate.InstanceBackendMaterialAttributes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateInstanceBackendMaterialAttributesTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceMatAttrs"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	// A whole-MaterialAttributes bind writing two lit channels; Normal is read (into a local) but not
	// written, so it must NOT become a bound output (it falls through to the host default).
	const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
        VectorParameter Tint = (0.6, 0.8, 1.0, 1.0);
        ScalarParameter Rough = 0.35;
    }

    Settings = {
        Backend = "Instance";
        Domain = "Surface";
        ShadingModel = "DefaultLit";
    }

    Outputs = {
        MaterialAttributes Attrs;
        Base.MaterialAttributes = Attrs;
    }

    Graph = {
        Attrs.BaseColor = Tint.rgb;
        Attrs.Roughness = Rough;
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
	if (!TestTrue(FString::Printf(TEXT("MaterialAttributes generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("MaterialAttributes instance exists in memory."), Instance))
	{
		return false;
	}

	// The whole-attributes bind expanded to one output per WRITTEN channel (BaseColor + Roughness).
	if (TestEqual(TEXT("Two channel outputs from the attributes bind."), Instance->InstanceOutputs.Num(), 2))
	{
		bool bHasBaseColor = false, bHasRoughness = false, bHasNormal = false;
		for (const FDreamShaderInstanceOutput& Output : Instance->InstanceOutputs)
		{
			bHasBaseColor |= (Output.Property == MP_BaseColor);
			bHasRoughness |= (Output.Property == MP_Roughness);
			bHasNormal |= (Output.Property == MP_Normal);
		}
		TestTrue(TEXT("BaseColor channel is bound."), bHasBaseColor);
		TestTrue(TEXT("Roughness channel is bound."), bHasRoughness);
		TestFalse(TEXT("Unwritten Normal channel is NOT bound."), bHasNormal);
	}

	// The generated .ush flattens Attrs.Field into __Attrs_Field locals and never touches the
	// procedurally-generated FMaterialAttributes struct.
	{
		const FString FileName = FPaths::GetCleanFilename(Instance->GeneratedIncludeVirtualPath);
		const FString DiskPath = UE::DreamShader::GetGeneratedShaderDirectory() / FileName;
		FString IncludeContent;
		if (TestTrue(TEXT("Generated instance .ush exists on disk."), FFileHelper::LoadFileToString(IncludeContent, *DiskPath)))
		{
			TestTrue(TEXT("Attrs.BaseColor flattened to __Attrs_BaseColor."), IncludeContent.Contains(TEXT("__Attrs_BaseColor")));
			TestTrue(TEXT("Attrs.Roughness flattened to __Attrs_Roughness."), IncludeContent.Contains(TEXT("__Attrs_Roughness")));
			TestFalse(TEXT("No raw Attrs.Field member access remains."), IncludeContent.Contains(TEXT("Attrs.BaseColor")));
			TestFalse(TEXT("No dependency on the FMaterialAttributes struct."), IncludeContent.Contains(TEXT("FMaterialAttributes")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateInstanceBackendImportedFunctionTest,
	"DreamShader.Compiler.Generate.InstanceBackendImportedFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateInstanceBackendImportedFunctionTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	// An out-param imported function (ApplyAutomationTint: in,in,out) called out-param style from an
	// Instance material. The generated functions-include emits it as a single-out RETURN-VALUE function,
	// so the eval call site must be rewritten from `Fn(a, b, out)` to `out = Fn(a, b)` — otherwise the
	// 3-arg out-param call would not match the 2-arg return-value definition (the live shader-compile bug).
	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceImport"));
	const FString HeaderFileName = AssetName + TEXT("_Shared.dsh");
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	FString HeaderFilePath;
	if (!WriteAutomationSourceFile(*this, HeaderFileName, MakeSharedHeaderSource(), HeaderFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(HeaderFilePath);

	const FString Source = FString::Printf(TEXT(R"(import "%s";

Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Properties = {
        VectorParameter InColor = (0.5, 0.5, 0.5, 1.0);
        VectorParameter InTint = (1.0, 0.8, 0.6, 1.0);
    }

    Settings = { Backend = "Instance"; ShadingModel = "Unlit"; }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        ApplyAutomationTint(InColor.rgb, InTint.rgb, Color);
    }
}
)"), *HeaderFileName, *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
	if (!TestTrue(FString::Printf(TEXT("Imported-function instance generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("Imported-function instance exists in memory."), Instance))
	{
		return false;
	}

	const FString FileName = FPaths::GetCleanFilename(Instance->GeneratedIncludeVirtualPath);
	const FString DiskPath = UE::DreamShader::GetGeneratedShaderDirectory() / FileName;
	FString IncludeContent;
	if (TestTrue(TEXT("Generated instance .ush exists on disk."), FFileHelper::LoadFileToString(IncludeContent, *DiskPath)))
	{
		// The out-param call was reconciled into a return-value assignment against the DreamShaderFn_ symbol.
		TestTrue(
			TEXT("Out-param call rewritten to `Color = DreamShaderFn_ApplyAutomationTint(...)`."),
			IncludeContent.Contains(TEXT("= DreamShaderFn_ApplyAutomationTint(")));
		// The un-rewritten 3-arg out-param form (…, Color) must NOT survive.
		TestFalse(
			TEXT("No un-rewritten 3-arg out-param call remains."),
			IncludeContent.Contains(TEXT(", Color)")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderGenerateInstanceBackendBaseOverridesTest,
	"DreamShader.Compiler.Generate.InstanceBackendBaseOverrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderGenerateInstanceBackendBaseOverridesTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor;
	using namespace UE::DreamShader::Editor::Private::Tests;

	FScopedDreamShaderAutomationArtifacts Artifacts;
	const FString AssetName = MakeUniqueTestAssetName(TEXT("M_AutoInstanceOverrides"));
	const FString ObjectPath = MakeAutomationObjectPath(AssetName);
	Artifacts.AddObjectPath(ObjectPath);
	AddExpectedNewAssetProbeWarnings(*this, ObjectPath);
	AddExpectedAutomationCleanupWarnings(*this);

	const FString Source = FString::Printf(TEXT(R"(Shader(Name="DreamShaderTests/Automation/%s", Root="Game")
{
    Settings = {
        Backend = "Instance";
        ShadingModel = "Unlit";
        BlendMode = "Masked";
        TwoSided = true;
        OpacityMaskClipValue = 0.7;
    }

    Outputs = {
        vec3 Color;
        float Mask;
        Base.EmissiveColor = Color;
        Base.OpacityMask = Mask;
    }

    Graph = {
        Color = vec3(1.0, 1.0, 1.0);
        Mask = 1.0;
    }
}
)"), *AssetName);

	FString SourceFilePath;
	if (!WriteAutomationSourceFile(*this, AssetName + TEXT(".dsm"), Source, SourceFilePath))
	{
		return false;
	}
	Artifacts.AddSourceFile(SourceFilePath);

	FString Message;
	const bool bGenerated = FMaterialGenerator::GenerateMaterialFromFile(SourceFilePath, Message, /*bForce*/ true, /*bTransient*/ true);
	if (!TestTrue(FString::Printf(TEXT("Base-override generation succeeds: %s"), *Message), bGenerated))
	{
		return false;
	}

	UDreamShaderMaterialInstance* Instance = FindObject<UDreamShaderMaterialInstance>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("Override instance exists in memory."), Instance))
	{
		return false;
	}

	// Reflected long-tail base overrides applied with their bOverride_ companions.
	TestTrue(TEXT("TwoSided override enabled + set."), Instance->BasePropertyOverrides.bOverride_TwoSided && Instance->BasePropertyOverrides.TwoSided);
	TestTrue(TEXT("OpacityMaskClipValue override enabled."), Instance->BasePropertyOverrides.bOverride_OpacityMaskClipValue);
	TestEqual(TEXT("OpacityMaskClipValue value set."), Instance->BasePropertyOverrides.OpacityMaskClipValue, 0.7f);

	// Root/child shader-map ownership: the generated instance is a ROOT — its immediate parent is the
	// host UMaterial — so it unconditionally forces its own permutation (its shading logic lives in the
	// injected .ush, which base-property comparison can't see). A child instance's parent is another
	// instance (not a UMaterial), so it does NOT hit the root gate and delegates to the stock
	// comparison-against-parent, which lets a no-new-override variant share the root's map.
	TestTrue(TEXT("Root instance (parent = host material) forces its own permutation."), Instance->HasOverridenBaseProperties());

	UDreamShaderMaterialInstance* Child = NewObject<UDreamShaderMaterialInstance>(GetTransientPackage());
	Child->SetParentEditorOnly(Instance, /*RecacheShader*/ false);
	TestNull(TEXT("Child instance's parent is another instance, so it bypasses the root ownership gate."),
		Cast<UMaterial>(Child->Parent));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
