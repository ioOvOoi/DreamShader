#include "Commandlet/DreamShaderCommandletRunner.h"
#include "DreamShaderMaterialInstance.h"
#include "DreamShaderModule.h"
#include "DreamShaderParser.h"
#include "DreamShaderSettings.h"
#include "DreamShaderTypes.h"
#include "DreamShaderVersionCompat.h"
#include "MaterialAssetGeneration/DreamShaderMaterialGenerator.h"

#include "HAL/FileManager.h"
#include "Materials/Material.h"
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
	// user opts in via bShowVirtualMaterialsInContentBrowser; object-path references still resolve.
	if (!GetDefault<UDreamShaderSettings>()->bShowVirtualMaterialsInContentBrowser)
	{
		TestFalse(TEXT("Virtual instance is not an enumerable asset (hidden from the Content Browser)."), Instance->IsAsset());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
