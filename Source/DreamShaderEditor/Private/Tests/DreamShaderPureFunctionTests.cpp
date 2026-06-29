// Copyright (c) 2026 TypeDreamMoon. All rights reserved.
//
// Pure-function quick layer: zero editor/world/asset dependency, runs in milliseconds. Covers the
// header-declared pure helpers used by diagnostics, import resolution, and the compile commandlet.
// These gate PRs alongside the parse corpus (DreamShader.Lang.* / DreamShader.Commandlet.Args.*).

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Commandlet/DreamShaderCommandletRunner.h"
#include "DependencyGraph/DreamShaderDependencyGraphService.h"
#include "Diagnostics/DreamShaderDiagnosticsStore.h"

#include "Misc/AutomationTest.h"

// ---------------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderParseErrorLocationTest,
	"DreamShader.Lang.Diagnostics.ParseErrorLocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderParseErrorLocationTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor::Private;

	// Standard "File(Line,Col): message" form.
	{
		FDreamShaderDiagnosticLocation Location;
		const bool bParsed = FDreamShaderDiagnosticsStore::TryParseErrorLocation(
			TEXT("C:/Proj/DShader/M_Foo.dsm(17,9): Unexpected token 'f' in Graph expression."), Location);
		TestTrue(TEXT("located error parses"), bParsed);
		TestEqual(TEXT("line"), Location.Line, 17);
		TestEqual(TEXT("column"), Location.Column, 9);
		TestTrue(TEXT("file path retained"), Location.FilePath.Contains(TEXT("M_Foo.dsm")));
		TestEqual(TEXT("message"), Location.Message, FString(TEXT("Unexpected token 'f' in Graph expression.")));
	}

	// Line/Column are clamped to a minimum of 1.
	{
		FDreamShaderDiagnosticLocation Location;
		const bool bParsed = FDreamShaderDiagnosticsStore::TryParseErrorLocation(TEXT("X.dsm(0,0): boom"), Location);
		TestTrue(TEXT("zero location parses"), bParsed);
		TestEqual(TEXT("line clamped"), Location.Line, 1);
		TestEqual(TEXT("column clamped"), Location.Column, 1);
	}

	// A line without the "): " marker, or with non-numeric coordinates, is not a located error.
	{
		FDreamShaderDiagnosticLocation Location;
		TestFalse(TEXT("plain text is not located"), FDreamShaderDiagnosticsStore::TryParseErrorLocation(TEXT("just a message"), Location));
		TestFalse(TEXT("non-numeric coords rejected"), FDreamShaderDiagnosticsStore::TryParseErrorLocation(TEXT("X.dsm(a,b): boom"), Location));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderBuildGenerateDiagnosticsTest,
	"DreamShader.Lang.Diagnostics.BuildGenerateDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderBuildGenerateDiagnosticsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor::Private;

	const FString SourceFilePath = TEXT("C:/Proj/DShader/M_Foo.dsm");
	const FString Message =
		FString(TEXT("C:/Proj/DShader/M_Foo.dsm(12,5): Unsupported swizzle '.q'.")) + LINE_TERMINATOR
		+ TEXT("Generation aborted.");

	const TArray<FDreamShaderDiagnosticRecord> Records =
		FDreamShaderDiagnosticsStore::BuildGenerateErrorDiagnostics(SourceFilePath, Message);

	if (!TestEqual(TEXT("two diagnostic records"), Records.Num(), 2))
	{
		return false;
	}

	TestEqual(TEXT("located record line"), Records[0].Line, 12);
	TestEqual(TEXT("located record column"), Records[0].Column, 5);
	TestEqual(TEXT("located record message"), Records[0].Message, FString(TEXT("Unsupported swizzle '.q'.")));
	TestEqual(TEXT("located record stage"), Records[0].Stage, FString(TEXT("generate")));
	return true;
}

// ---------------------------------------------------------------------------------------------
// Import resolution
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderExtractImportPathTest,
	"DreamShader.Lang.Import.ExtractImportPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderExtractImportPathTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor::Private;

	auto Extract = [](const TCHAR* Line, FString& OutPath)
	{
		return FDreamShaderDependencyGraphService::TryExtractImportPathFromLine(Line, OutPath);
	};

	FString Path;
	TestTrue(TEXT("double-quoted with semicolon"), Extract(TEXT("import \"Shared/Common.dsh\";"), Path));
	TestEqual(TEXT("path value"), Path, FString(TEXT("Shared/Common.dsh")));

	TestTrue(TEXT("no trailing semicolon"), Extract(TEXT("import \"Functions/F.dsf\""), Path));
	TestEqual(TEXT("dsf path"), Path, FString(TEXT("Functions/F.dsf")));

	TestTrue(TEXT("single-quoted"), Extract(TEXT("import 'X.dsh';"), Path));
	TestEqual(TEXT("single-quoted path"), Path, FString(TEXT("X.dsh")));

	TestTrue(TEXT("trailing comment allowed"), Extract(TEXT("import \"A.dsh\"; // note"), Path));

	TestFalse(TEXT("commented import ignored"), Extract(TEXT("// import \"X.dsh\";"), Path));
	TestFalse(TEXT("no space after keyword"), Extract(TEXT("importX \"Y.dsh\";"), Path));
	TestFalse(TEXT("unquoted path rejected"), Extract(TEXT("import Shared/Common.dsh;"), Path));
	TestFalse(TEXT("non-import line"), Extract(TEXT("Shader(Name=\"M\")"), Path));
	TestFalse(TEXT("trailing junk rejected"), Extract(TEXT("import \"X.dsh\" garbage"), Path));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderNormalizeImportSpecifierTest,
	"DreamShader.Lang.Import.NormalizeSpecifier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderNormalizeImportSpecifierTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor::Private;

	auto Normalize = [](const TCHAR* Spec)
	{
		return FDreamShaderDependencyGraphService::NormalizeImportSpecifier(Spec);
	};

	TestEqual(TEXT("extensionless defaults to .dsh"), Normalize(TEXT("Shared/Common")), FString(TEXT("Shared/Common.dsh")));
	TestEqual(TEXT("explicit .dsf preserved"), Normalize(TEXT("Functions/F.dsf")), FString(TEXT("Functions/F.dsf")));
	TestEqual(TEXT("backslashes and ./ stripped"), Normalize(TEXT("./Shared\\Common.dsh")), FString(TEXT("Shared/Common.dsh")));
	TestEqual(TEXT("bare name gets .dsh"), Normalize(TEXT("Noise")), FString(TEXT("Noise.dsh")));
	return true;
}

// ---------------------------------------------------------------------------------------------
// Commandlet argument parsing
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderCommandletArgParsingTest,
	"DreamShader.Commandlet.Args.SplitAndGet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderCommandletArgParsingTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader::Editor::Private;

	// NormalizeCommandletKey strips leading dashes; NormalizeCommandletValue strips quotes/whitespace.
	TestEqual(TEXT("key strips dashes"), NormalizeCommandletKey(TEXT("--Force")), FString(TEXT("Force")));
	TestEqual(TEXT("value strips quotes"), NormalizeCommandletValue(TEXT("  \"a value\"  ")), FString(TEXT("a value")));

	// TrySplitCommandletAssignment.
	{
		FString Key, Value;
		TestTrue(TEXT("assignment splits"), TrySplitCommandletAssignment(TEXT("-Source=C:/x.dsm"), Key, Value));
		TestEqual(TEXT("split key"), Key, FString(TEXT("Source")));
		TestEqual(TEXT("split value"), Value, FString(TEXT("C:/x.dsm")));

		TestFalse(TEXT("no equals -> no split"), TrySplitCommandletAssignment(TEXT("Force"), Key, Value));
	}

	// TryGetCommandletParam searches Params, then Switches, then Tokens.
	{
		TArray<FString> Tokens = { TEXT("Mode=compile") };
		TArray<FString> Switches = { TEXT("-Force=true") };
		TMap<FString, FString> Params;
		Params.Add(TEXT("Source"), TEXT("C:/x.dsm"));

		FString Value;
		TestTrue(TEXT("param from map"), TryGetCommandletParam(Tokens, Switches, Params, TEXT("Source"), Value));
		TestEqual(TEXT("map value"), Value, FString(TEXT("C:/x.dsm")));

		TestTrue(TEXT("param from switch"), TryGetCommandletParam(Tokens, Switches, Params, TEXT("Force"), Value));
		TestEqual(TEXT("switch value"), Value, FString(TEXT("true")));

		TestTrue(TEXT("param from token"), TryGetCommandletParam(Tokens, Switches, Params, TEXT("Mode"), Value));
		TestEqual(TEXT("token value"), Value, FString(TEXT("compile")));

		TestFalse(TEXT("missing param"), TryGetCommandletParam(Tokens, Switches, Params, TEXT("Nope"), Value));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
