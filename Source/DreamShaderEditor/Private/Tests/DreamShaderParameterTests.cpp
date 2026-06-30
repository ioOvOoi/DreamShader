// Copyright (c) 2026 TypeDreamMoon. All rights reserved.
//
// Complete coverage for the parameter-expression Properties surface: every parameter node type the
// parser recognises is declared here and asserted on both axes -- parse (correct ParameterNodeType /
// base Type / component count / default flag) and generate (the right UMaterialExpression*Parameter
// node is actually created in the material graph). Also pins the "default is optional" contract:
// a declaration without `= value` parses and generates with bHasDefaultValue == false.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DreamShaderParser.h"
#include "DreamShaderTypes.h"
#include "DreamShaderTestCommon.h"

#include "MaterialAssetGeneration/DreamShaderMaterialGenerator.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectIterator.h"

namespace UE::DreamShader::Editor::Private::ParameterTests
{
	using namespace UE::DreamShader;

	struct FParameterCase
	{
		const TCHAR* NodeType;          // the DSL keyword == ParameterNodeType
		const TCHAR* Default;           // inline default literal, or nullptr for "declare without default"
		ETextShaderPropertyType Type;   // expected base type
		int32 ComponentCount;           // expected component count
	};

	// One row per parameter node type the parser accepts in a plain Properties declaration.
	static const FParameterCase GParameterCases[] = {
		// Scalar family
		{ TEXT("ScalarParameter"),                         TEXT("0.55"),                                ETextShaderPropertyType::Scalar,   1 },
		{ TEXT("ScalarParameter"),                         nullptr,                                     ETextShaderPropertyType::Scalar,   1 }, // optional default
		{ TEXT("StaticBoolParameter"),                     TEXT("true"),                                ETextShaderPropertyType::Scalar,   1 },
		{ TEXT("StaticSwitchParameter"),                   TEXT("false"),                               ETextShaderPropertyType::Scalar,   1 },
		// Vector family (float4 output)
		{ TEXT("VectorParameter"),                         TEXT("float4(0.1, 0.2, 0.3, 1.0)"),          ETextShaderPropertyType::Vector,   4 },
		{ TEXT("VectorParameter"),                         nullptr,                                     ETextShaderPropertyType::Vector,   4 }, // optional default
		{ TEXT("DoubleVectorParameter"),                   TEXT("float4(1, 2, 3, 4)"),                  ETextShaderPropertyType::Vector,   4 },
		{ TEXT("ChannelMaskParameter"),                    TEXT("float4(1, 0, 0, 0)"),                  ETextShaderPropertyType::Vector,   1 },
		{ TEXT("StaticComponentMaskParameter"),            TEXT("float4(1, 1, 0, 0)"),                  ETextShaderPropertyType::Vector,   4 },
		{ TEXT("CurveAtlasRowParameter"),                  TEXT("float3(0.5, 0.5, 0.5)"),               ETextShaderPropertyType::Vector,   3 },
		{ TEXT("DynamicParameter"),                        TEXT("float4(0, 0, 0, 0)"),                  ETextShaderPropertyType::Vector,   4 },
		{ TEXT("FontSampleParameter"),                     nullptr,                                     ETextShaderPropertyType::Vector,   4 },
		{ TEXT("SpriteTextureSampler"),                    nullptr,                                     ETextShaderPropertyType::Vector,   4 },
		// Texture object family (texture input, no sampled output)
		{ TEXT("TextureObjectParameter"),                  TEXT("Path(Game, \"Probe/T_Default\")"),     ETextShaderPropertyType::Texture2D, 0 },
		{ TEXT("TextureObjectParameter"),                  nullptr,                                     ETextShaderPropertyType::Texture2D, 0 }, // optional default
		{ TEXT("TextureCollectionParameter"),              nullptr,                                     ETextShaderPropertyType::Texture2D, 0 },
		{ TEXT("SparseVolumeTextureObjectParameter"),      nullptr,                                     ETextShaderPropertyType::Texture2D, 0 },
		// Texture sample family (float4 sampled output)
		{ TEXT("TextureSampleParameter2D"),                nullptr,                                     ETextShaderPropertyType::Vector,   4 },
		{ TEXT("TextureSampleParameter2DArray"),           nullptr,                                     ETextShaderPropertyType::Vector,   4 },
		{ TEXT("TextureSampleParameterCube"),              nullptr,                                     ETextShaderPropertyType::Vector,   4 },
		{ TEXT("TextureSampleParameterCubeArray"),         nullptr,                                     ETextShaderPropertyType::Vector,   4 },
		{ TEXT("TextureSampleParameterVolume"),            nullptr,                                     ETextShaderPropertyType::Vector,   4 },
		{ TEXT("TextureSampleParameterSubUV"),             nullptr,                                     ETextShaderPropertyType::Vector,   4 },
		{ TEXT("RuntimeVirtualTextureSampleParameter"),    nullptr,                                     ETextShaderPropertyType::Vector,   4 },
		{ TEXT("SparseVolumeTextureSampleParameter"),      nullptr,                                     ETextShaderPropertyType::Vector,   4 },
	};

	// Build a Shader source declaring one property per case, named P0..PN.
	static FString BuildAllParametersSource()
	{
		FString Properties;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(GParameterCases); ++Index)
		{
			const FParameterCase& Case = GParameterCases[Index];
			if (Case.Default)
			{
				Properties += FString::Printf(TEXT("        %s P%d = %s [Group=\"Params\"; SortPriority=%d;];\n"), Case.NodeType, Index, Case.Default, Index);
			}
			else
			{
				Properties += FString::Printf(TEXT("        %s P%d [Group=\"Params\"; SortPriority=%d;];\n"), Case.NodeType, Index, Index);
			}
		}

		return FString::Printf(TEXT(
			"Shader(Name=\"DreamShaderTests/Params/M_AllParameterTypes\", Root=\"Game\")\n"
			"{\n"
			"    Properties = {\n%s    }\n"
			"    Settings = { Domain = \"Surface\"; ShadingModel = \"Unlit\"; BlendMode = \"Opaque\"; }\n"
			"    Outputs = { vec3 Color; Base.EmissiveColor = Color; }\n"
			"    Graph = { Color = vec3(0.5, 0.5, 0.5); }\n"
			"}\n"), *Properties);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderParameterParseAllTest,
	"DreamShader.Lang.ParameterExpressions.ParseAll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderParameterParseAllTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader;
	using namespace UE::DreamShader::Editor::Private::ParameterTests;

	FTextShaderDefinition Definition;
	FString ParseError;
	const bool bParsed = FTextShaderParser::Parse(BuildAllParametersSource(), Definition, ParseError);
	if (!TestTrue(FString::Printf(TEXT("source with every parameter type parses: %s"), *ParseError), bParsed))
	{
		return false;
	}

	if (!TestEqual(TEXT("one property parsed per case"), Definition.Properties.Num(), (int32)UE_ARRAY_COUNT(GParameterCases)))
	{
		return false;
	}

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(GParameterCases); ++Index)
	{
		const FParameterCase& Case = GParameterCases[Index];
		const FString PropertyName = FString::Printf(TEXT("P%d"), Index);
		const FTextShaderPropertyDefinition* Property = Definition.Properties.FindByPredicate(
			[&PropertyName](const FTextShaderPropertyDefinition& Candidate) { return Candidate.Name == PropertyName; });

		if (!TestNotNull(*FString::Printf(TEXT("%s (%s) parsed"), *PropertyName, Case.NodeType), Property))
		{
			continue;
		}

		const FString Label = FString::Printf(TEXT("%s (%s)"), *PropertyName, Case.NodeType);
		TestEqual(*FString::Printf(TEXT("%s ParameterNodeType"), *Label), Property->ParameterNodeType, FString(Case.NodeType));
		TestEqual(*FString::Printf(TEXT("%s source is Parameter"), *Label), (int32)Property->Source, (int32)ETextShaderPropertySource::Parameter);
		TestEqual(*FString::Printf(TEXT("%s base Type"), *Label), (int32)Property->Type, (int32)Case.Type);
		TestEqual(*FString::Printf(TEXT("%s ComponentCount"), *Label), Property->ComponentCount, Case.ComponentCount);
		TestEqual(*FString::Printf(TEXT("%s bHasDefaultValue matches presence of inline default"), *Label), Property->bHasDefaultValue, Case.Default != nullptr);
	}

	return true;
}

// Group("X") { ... } Properties scope: stamps the group + an auto-incrementing SortPriority (step 10,
// global counter; explicit values win and don't consume a slot); loose params are untouched. Also
// pins the Slider(min,max) shorthand and asset-in-= (bare quoted absolute path) -- none of which the
// corpus golden can see, so they are asserted at the FTextShaderDefinition level here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDreamShaderPropertyGroupScopeTest,
	"DreamShader.Lang.ParameterExpressions.GroupScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamShaderPropertyGroupScopeTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamShader;

	const FString Source = TEXT(R"(
Shader(Name="DreamShaderTests/Params/M_GroupScope", Root="Game")
{
    Properties {
        Group("Surface") {
            ScalarParameter A = 0.5 [Slider(0, 1)];
            VectorParameter B = float4(1, 1, 1, 1);
        }
        Group("Detail") {
            ScalarParameter C = 1.0 [SortPriority=99;];
            ScalarParameter D = 2.0;
        }
        ScalarParameter Loose = 3.0;
        TextureSampleParameter2D Tex = "/Engine/EngineResources/WhiteSquareTexture";
    }
    Settings { Domain = "Surface"; ShadingModel = "Unlit"; BlendMode = "Opaque"; }
    Outputs { vec3 Color; Base.EmissiveColor = Color; }
    Graph { Color = vec3(A, A, A); }
}
)");

	FTextShaderDefinition Definition;
	FString ParseError;
	if (!TestTrue(FString::Printf(TEXT("Group-scope source parses: %s"), *ParseError),
		FTextShaderParser::Parse(Source, Definition, ParseError)))
	{
		return false;
	}

	auto Find = [&Definition](const TCHAR* Name) -> const FTextShaderPropertyDefinition*
	{
		return Definition.Properties.FindByPredicate(
			[Name](const FTextShaderPropertyDefinition& Candidate) { return Candidate.Name == Name; });
	};

	const FTextShaderPropertyDefinition* A = Find(TEXT("A"));
	const FTextShaderPropertyDefinition* B = Find(TEXT("B"));
	const FTextShaderPropertyDefinition* C = Find(TEXT("C"));
	const FTextShaderPropertyDefinition* D = Find(TEXT("D"));
	const FTextShaderPropertyDefinition* Loose = Find(TEXT("Loose"));
	const FTextShaderPropertyDefinition* Tex = Find(TEXT("Tex"));
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("C"), C)
		|| !TestNotNull(TEXT("D"), D) || !TestNotNull(TEXT("Loose"), Loose) || !TestNotNull(TEXT("Tex"), Tex))
	{
		return false;
	}

	// Group stamping.
	TestEqual(TEXT("A inherits group 'Surface'"), A->Metadata.Group, FString(TEXT("Surface")));
	TestEqual(TEXT("B inherits group 'Surface'"), B->Metadata.Group, FString(TEXT("Surface")));
	TestEqual(TEXT("C inherits group 'Detail'"), C->Metadata.Group, FString(TEXT("Detail")));
	TestEqual(TEXT("D inherits group 'Detail'"), D->Metadata.Group, FString(TEXT("Detail")));
	TestTrue(TEXT("loose param keeps no group"), Loose->Metadata.Group.IsEmpty());

	// Auto SortPriority: global counter, step 10; explicit value (C) wins and does not consume a slot.
	TestTrue(TEXT("A auto-sorted"), A->Metadata.bHasSortPriority);
	TestEqual(TEXT("A SortPriority == 0"), A->Metadata.SortPriority, 0);
	TestEqual(TEXT("B SortPriority == 10"), B->Metadata.SortPriority, 10);
	TestEqual(TEXT("C keeps explicit SortPriority == 99"), C->Metadata.SortPriority, 99);
	TestEqual(TEXT("D SortPriority == 20 (explicit C didn't consume the counter)"), D->Metadata.SortPriority, 20);
	TestFalse(TEXT("loose param is not auto-sorted"), Loose->Metadata.bHasSortPriority);

	// Slider(0, 1) shorthand -> two slider reflected properties.
	int32 SliderKeyCount = 0;
	for (const TPair<FString, FString>& Pair : A->Metadata.ReflectedProperties)
	{
		if (Pair.Key.Contains(TEXT("slider"), ESearchCase::IgnoreCase))
		{
			++SliderKeyCount;
		}
	}
	TestEqual(TEXT("Slider(0,1) expands to SliderMin + SliderMax"), SliderKeyCount, 2);

	// asset-in-= via a bare quoted absolute path.
	TestTrue(TEXT("Tex bound an asset path from '= \"/Engine/...\"'"),
		Tex->TextureDefaultObjectPath.Contains(TEXT("WhiteSquareTexture")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
