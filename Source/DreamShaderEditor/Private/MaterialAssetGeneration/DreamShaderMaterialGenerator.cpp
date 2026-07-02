#include "DreamShaderMaterialGenerator.h"

#include "DependencyGraph/DreamShaderDependencyGraphService.h"
#include "DreamShaderMaterialGeneratorCodeShared.h"
#include "DreamShaderMaterialGeneratorDiagnostics.h"
#include "DreamShaderMaterialGeneratorPrivate.h"
#include "DreamShaderMaterialGeneratorSourceLoading.h"

#include "DreamShaderMaterialInstance.h"
#include "DreamShaderModule.h"
#include "DreamShaderParser.h"
#include "DreamShaderSettings.h"
#include "DreamShaderVersionCompat.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreGlobals.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "MaterialShared.h"
#include "Misc/PackageName.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"

namespace UE::DreamShader::Editor
{
	namespace
	{
		static bool IsIdentifierBoundary(const FString& Text, const int32 Index)
		{
			if (!Text.IsValidIndex(Index))
			{
				return true;
			}

			const TCHAR Char = Text[Index];
			return !(FChar::IsAlnum(Char) || Char == TCHAR('_'));
		}

		static bool TryConsumeKeywordAt(const FString& Text, const int32 Index, const TCHAR* Keyword)
		{
			const int32 KeywordLength = FCString::Strlen(Keyword);
			if (!Text.Mid(Index, KeywordLength).Equals(Keyword, ESearchCase::CaseSensitive))
			{
				return false;
			}

			return IsIdentifierBoundary(Text, Index - 1) && IsIdentifierBoundary(Text, Index + KeywordLength);
		}

		static bool ContainsIdentifierReference(const FString& Text, const FString& Identifier)
		{
			if (Identifier.IsEmpty())
			{
				return false;
			}

			bool bInString = false;
			bool bInLineComment = false;
			bool bInBlockComment = false;
			for (int32 Index = 0; Index < Text.Len(); ++Index)
			{
				const TCHAR Char = Text[Index];
				const TCHAR Next = Text.IsValidIndex(Index + 1) ? Text[Index + 1] : TCHAR('\0');

				if (bInLineComment)
				{
					if (Char == TCHAR('\n'))
					{
						bInLineComment = false;
					}
					continue;
				}

				if (bInBlockComment)
				{
					if (Char == TCHAR('*') && Next == TCHAR('/'))
					{
						bInBlockComment = false;
						++Index;
					}
					continue;
				}

				if (bInString)
				{
					if (Char == TCHAR('\\') && Text.IsValidIndex(Index + 1))
					{
						++Index;
					}
					else if (Char == TCHAR('"'))
					{
						bInString = false;
					}
					continue;
				}

				if (Char == TCHAR('"'))
				{
					bInString = true;
					continue;
				}
				if (Char == TCHAR('/') && Next == TCHAR('/'))
				{
					bInLineComment = true;
					++Index;
					continue;
				}
				if (Char == TCHAR('/') && Next == TCHAR('*'))
				{
					bInBlockComment = true;
					++Index;
					continue;
				}

				if ((FChar::IsAlpha(Char) || Char == TCHAR('_')) && IsIdentifierBoundary(Text, Index - 1))
				{
					const int32 Start = Index++;
					while (Text.IsValidIndex(Index) && (FChar::IsAlnum(Text[Index]) || Text[Index] == TCHAR('_')))
					{
						++Index;
					}

					if (Text.Mid(Start, Index - Start).Equals(Identifier, ESearchCase::CaseSensitive))
					{
						return true;
					}
					--Index;
				}
			}

			return false;
		}

		static FString FindRegionNameForStatement(
			const TArray<FTextShaderGraphRegion>& Regions,
			const Private::FCodeStatement& Statement)
		{
			if (!Statement.bHasSourceLocation)
			{
				return FString();
			}

			for (const FTextShaderGraphRegion& Region : Regions)
			{
				if (Statement.SourceLine >= Region.StartLine && Statement.SourceLine <= Region.EndLine)
				{
					return Region.Name;
				}
			}

			return FString();
		}

		static void ApplyStatementRegionsRecursive(
			TArray<Private::FCodeStatement>& Statements,
			const TArray<FTextShaderGraphRegion>& Regions)
		{
			for (Private::FCodeStatement& Statement : Statements)
			{
				Statement.RegionName = FindRegionNameForStatement(Regions, Statement);
				ApplyStatementRegionsRecursive(Statement.ThenStatements, Regions);
				ApplyStatementRegionsRecursive(Statement.ElseStatements, Regions);
			}
		}

		static bool TryParseFunctionInputPreviewLiteral(
			const FString& InText,
			const int32 ComponentCount,
			FVector4f& OutPreviewValue)
		{
			if (ComponentCount <= 1)
			{
				double ScalarValue = 0.0;
				if (!Private::ParseScalarLiteral(InText, ScalarValue))
				{
					return false;
				}

				const float Value = static_cast<float>(ScalarValue);
				OutPreviewValue = FVector4f(Value, Value, Value, Value);
				return true;
			}

			FString Candidate = InText.TrimStartAndEnd();
			const int32 OpenParenIndex = Candidate.Find(TEXT("("));
			const int32 CloseParenIndex = Candidate.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (OpenParenIndex == INDEX_NONE || CloseParenIndex == INDEX_NONE || CloseParenIndex <= OpenParenIndex)
			{
				return false;
			}

			const FString ValueBlock = Candidate.Mid(OpenParenIndex + 1, CloseParenIndex - OpenParenIndex - 1);
			TArray<FString> Parts;
			ValueBlock.ParseIntoArray(Parts, TEXT(","), true);
			if (Parts.IsEmpty() || Parts.Num() > 4)
			{
				return false;
			}

			float Parsed[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			for (int32 Index = 0; Index < Parts.Num(); ++Index)
			{
				double ParsedValue = 0.0;
				if (!Private::ParseScalarLiteral(Parts[Index], ParsedValue))
				{
					return false;
				}
				Parsed[Index] = static_cast<float>(ParsedValue);
			}

			if (Parts.Num() == 1)
			{
				Parsed[1] = Parsed[0];
				Parsed[2] = Parsed[0];
				Parsed[3] = Parsed[0];
			}

			OutPreviewValue = FVector4f(Parsed[0], Parsed[1], Parsed[2], Parsed[3]);
			return true;
		}

		static bool ApplyFunctionInputPreviewDefault(
			UMaterialFunction* MaterialFunction,
			const FString& SourceFilePath,
			const FTextShaderDefinition& RootDefinition,
			const FTextShaderFunctionParameter& InputDefinition,
			UMaterialExpressionFunctionInput* InputExpression,
			const int32 ComponentCount,
			const bool bIsTextureObject,
			const ETextShaderTextureType TextureType,
			const TArray<FTextShaderPropertyDefinition>* LocalProperties,
			TMap<FString, Private::FCodeValue>& GeneratedValues,
			FString& OutError)
		{
			if (!InputExpression || (!InputDefinition.bOptional && !InputDefinition.bHasDefaultValue))
			{
				return true;
			}

			InputExpression->bUsePreviewValueAsDefault = InputDefinition.bOptional ? 1U : 0U;
			if (!InputDefinition.bHasDefaultValue)
			{
				return true;
			}

			const bool bIsSubstrateMaterial = Private::IsSubstrateMaterialType(InputDefinition.Type);
			const bool bIsMaterialAttributes = ComponentCount == 0 && !bIsTextureObject && !bIsSubstrateMaterial;
			FVector4f PreviewValue;
			if (!bIsTextureObject && !bIsMaterialAttributes && !bIsSubstrateMaterial && TryParseFunctionInputPreviewLiteral(InputDefinition.DefaultValueText, ComponentCount, PreviewValue))
			{
				InputExpression->PreviewValue = PreviewValue;
				return true;
			}

			Private::FCodeGraphBuilder PreviewGraphBuilder(
				nullptr,
				MaterialFunction,
				RootDefinition,
				SourceFilePath,
				Private::BuildGeneratedIncludeVirtualPath(SourceFilePath),
				LocalProperties);
			TArray<Private::FCodeStatement> EmptyStatements;
			FString BuildError;
			if (!PreviewGraphBuilder.Build(EmptyStatements, GeneratedValues, BuildError))
			{
				OutError = BuildError;
				return false;
			}

			Private::FCodeValue PreviewExpressionValue;
			if (!PreviewGraphBuilder.EvaluateOutputExpression(InputDefinition.DefaultValueText, PreviewExpressionValue, OutError))
			{
				return false;
			}

			if (PreviewExpressionValue.bIsTextureObject != bIsTextureObject
				|| (bIsTextureObject && PreviewExpressionValue.TextureType != TextureType)
				|| PreviewExpressionValue.bIsMaterialAttributes != bIsMaterialAttributes
				|| PreviewExpressionValue.bIsSubstrateMaterial != bIsSubstrateMaterial
				|| PreviewExpressionValue.ComponentCount != ComponentCount)
			{
				OutError = FString::Printf(
					TEXT("Input '%s' default expression '%s' does not match declared type '%s'."),
					*InputDefinition.Name,
					*InputDefinition.DefaultValueText,
					*InputDefinition.Type);
				return false;
			}

			Private::ConnectCodeValueToInput(InputExpression->Preview, PreviewExpressionValue);
			return true;
		}

		static bool SeedMaterialAttributesGraphValue(
			UMaterial* Material,
			UMaterialFunction* MaterialFunction,
			const FString& ValueName,
			TMap<FString, Private::FCodeValue>& InOutGeneratedValues,
			int32& InOutPositionY,
			FString& OutError)
		{
			if (ValueName.IsEmpty() || InOutGeneratedValues.Contains(ValueName))
			{
				return true;
			}

			auto* Expression = Cast<UMaterialExpressionMakeMaterialAttributes>(
				UMaterialEditingLibrary::CreateMaterialExpressionEx(
					Material,
					MaterialFunction,
					UMaterialExpressionMakeMaterialAttributes::StaticClass(),
					nullptr,
					120,
					InOutPositionY,
					false));
			if (!Expression)
			{
				OutError = FString::Printf(TEXT("Failed to create a MakeMaterialAttributes node for '%s'."), *ValueName);
				return false;
			}

			Private::FCodeValue Value;
			Value.Expression = Expression;
			Value.OutputIndex = 0;
			Value.ComponentCount = 0;
			Value.bIsTextureObject = false;
			Value.bIsMaterialAttributes = true;
			Value.bIsSubstrateMaterial = false;
			InOutGeneratedValues.Add(ValueName, Value);
			InOutPositionY += 220;
			return true;
		}

		static const FTextShaderPropertyDefinition* FindPropertyByName(
			const TArray<FTextShaderPropertyDefinition>& Properties,
			const FString& Name)
		{
			for (const FTextShaderPropertyDefinition& Property : Properties)
			{
				if (Property.Name.Equals(Name, ESearchCase::IgnoreCase))
				{
					return &Property;
				}
			}

			return nullptr;
		}

		static bool CreateReferencedPropertyExpression(
			UMaterial* Material,
			UMaterialFunction* MaterialFunction,
			const TArray<FTextShaderPropertyDefinition>& Properties,
			const FTextShaderPropertyDefinition& Property,
			TMap<FString, UMaterialExpression*>& InOutGeneratedPropertyExpressions,
			TSet<FString>& InOutCreatingPropertyNames,
			int32& InOutPositionY,
			UMaterialExpression*& OutExpression,
			FString& OutError)
		{
			if (UMaterialExpression* const* ExistingExpression = InOutGeneratedPropertyExpressions.Find(Property.Name))
			{
				OutExpression = *ExistingExpression;
				return true;
			}

			for (const FString& CreatingName : InOutCreatingPropertyNames)
			{
				if (CreatingName.Equals(Property.Name, ESearchCase::IgnoreCase))
				{
					OutError = FString::Printf(TEXT("Property '%s' has a recursive UE builtin dependency."), *Property.Name);
					return false;
				}
			}

			InOutCreatingPropertyNames.Add(Property.Name);
			if (Property.Source == ETextShaderPropertySource::UEBuiltin)
			{
				for (const TPair<FString, FString>& Argument : Property.UEBuiltinArguments)
				{
					const FTextShaderPropertyDefinition* Dependency = FindPropertyByName(Properties, Argument.Value.TrimStartAndEnd());
					if (!Dependency)
					{
						continue;
					}

					UMaterialExpression* IgnoredDependencyExpression = nullptr;
					if (!CreateReferencedPropertyExpression(
						Material,
						MaterialFunction,
						Properties,
						*Dependency,
						InOutGeneratedPropertyExpressions,
						InOutCreatingPropertyNames,
						InOutPositionY,
						IgnoredDependencyExpression,
						OutError))
					{
						InOutCreatingPropertyNames.Remove(Property.Name);
						return false;
					}
				}
			}

			FString PropertyExpressionError;
			OutExpression = Private::CreatePropertyExpression(
				Material,
				MaterialFunction,
				Property,
				InOutGeneratedPropertyExpressions,
				InOutPositionY,
				PropertyExpressionError);
			if (!OutExpression)
			{
				OutError = PropertyExpressionError;
				InOutCreatingPropertyNames.Remove(Property.Name);
				return false;
			}

			InOutGeneratedPropertyExpressions.Add(Property.Name, OutExpression);
			InOutPositionY += 220;
			InOutCreatingPropertyNames.Remove(Property.Name);
			return true;
		}

		static bool FindMatchingDelimiter(
			const FString& Text,
			const int32 OpenIndex,
			const TCHAR OpenChar,
			const TCHAR CloseChar,
			int32& OutCloseIndex)
		{
			if (!Text.IsValidIndex(OpenIndex) || Text[OpenIndex] != OpenChar)
			{
				return false;
			}

			int32 Depth = 1;
			bool bInString = false;
			bool bInLineComment = false;
			bool bInBlockComment = false;

			for (int32 Index = OpenIndex + 1; Index < Text.Len(); ++Index)
			{
				const TCHAR Char = Text[Index];
				const TCHAR Next = Text.IsValidIndex(Index + 1) ? Text[Index + 1] : TCHAR('\0');

				if (bInLineComment)
				{
					if (Char == TCHAR('\n'))
					{
						bInLineComment = false;
					}
					continue;
				}

				if (bInBlockComment)
				{
					if (Char == TCHAR('*') && Next == TCHAR('/'))
					{
						bInBlockComment = false;
						++Index;
					}
					continue;
				}

				if (bInString)
				{
					if (Char == TCHAR('\\') && Text.IsValidIndex(Index + 1))
					{
						++Index;
					}
					else if (Char == TCHAR('"'))
					{
						bInString = false;
					}
					continue;
				}

				if (Char == TCHAR('"'))
				{
					bInString = true;
					continue;
				}

				if (Char == TCHAR('/') && Next == TCHAR('/'))
				{
					bInLineComment = true;
					++Index;
					continue;
				}

				if (Char == TCHAR('/') && Next == TCHAR('*'))
				{
					bInBlockComment = true;
					++Index;
					continue;
				}

				if (Char == OpenChar)
				{
					++Depth;
				}
				else if (Char == CloseChar)
				{
					--Depth;
					if (Depth == 0)
					{
						OutCloseIndex = Index;
						return true;
					}
				}
			}

			return false;
		}

		static TArray<FString> SplitTopLevelParameters(const FString& ParameterBlock)
		{
			TArray<FString> Parameters;
			FString Current;
			int32 ParenthesisDepth = 0;
			bool bInString = false;

			for (int32 Index = 0; Index < ParameterBlock.Len(); ++Index)
			{
				const TCHAR Char = ParameterBlock[Index];

				if (bInString)
				{
					Current.AppendChar(Char);
					if (Char == TCHAR('\\') && ParameterBlock.IsValidIndex(Index + 1))
					{
						Current.AppendChar(ParameterBlock[++Index]);
					}
					else if (Char == TCHAR('"'))
					{
						bInString = false;
					}
					continue;
				}

				if (Char == TCHAR('"'))
				{
					bInString = true;
					Current.AppendChar(Char);
					continue;
				}

				if (Char == TCHAR('('))
				{
					++ParenthesisDepth;
					Current.AppendChar(Char);
					continue;
				}

				if (Char == TCHAR(')'))
				{
					ParenthesisDepth = FMath::Max(0, ParenthesisDepth - 1);
					Current.AppendChar(Char);
					continue;
				}

				if (Char == TCHAR(',') && ParenthesisDepth == 0)
				{
					Current.TrimStartAndEndInline();
					if (!Current.IsEmpty())
					{
						Parameters.Add(Current);
					}
					Current.Reset();
					continue;
				}

				Current.AppendChar(Char);
			}

			Current.TrimStartAndEndInline();
			if (!Current.IsEmpty())
			{
				Parameters.Add(Current);
			}

			return Parameters;
		}

		static FString NormalizeShaderTypeToken(const FString& InTypeToken)
		{
			FString TypeToken = InTypeToken.TrimStartAndEnd();
			FString Lower = TypeToken;
			Lower.ToLowerInline();

			if (Lower == TEXT("vec2")) return TEXT("float2");
			if (Lower == TEXT("vec3")) return TEXT("float3");
			if (Lower == TEXT("vec4")) return TEXT("float4");
			if (Lower == TEXT("ivec2")) return TEXT("int2");
			if (Lower == TEXT("ivec3")) return TEXT("int3");
			if (Lower == TEXT("ivec4")) return TEXT("int4");
			if (Lower == TEXT("uvec2")) return TEXT("uint2");
			if (Lower == TEXT("uvec3")) return TEXT("uint3");
			if (Lower == TEXT("uvec4")) return TEXT("uint4");
			if (Lower == TEXT("bvec2")) return TEXT("bool2");
			if (Lower == TEXT("bvec3")) return TEXT("bool3");
			if (Lower == TEXT("bvec4")) return TEXT("bool4");
			if (Lower == TEXT("mat2")) return TEXT("float2x2");
			if (Lower == TEXT("mat3")) return TEXT("float3x3");
			if (Lower == TEXT("mat4")) return TEXT("float4x4");

			return TypeToken;
		}

		static FString NormalizeShaderLanguageText(const FString& InCode)
		{
			static const TMap<FString, FString> IdentifierMap = {
				{ TEXT("vec2"), TEXT("float2") },
				{ TEXT("vec3"), TEXT("float3") },
				{ TEXT("vec4"), TEXT("float4") },
				{ TEXT("ivec2"), TEXT("int2") },
				{ TEXT("ivec3"), TEXT("int3") },
				{ TEXT("ivec4"), TEXT("int4") },
				{ TEXT("uvec2"), TEXT("uint2") },
				{ TEXT("uvec3"), TEXT("uint3") },
				{ TEXT("uvec4"), TEXT("uint4") },
				{ TEXT("bvec2"), TEXT("bool2") },
				{ TEXT("bvec3"), TEXT("bool3") },
				{ TEXT("bvec4"), TEXT("bool4") },
				{ TEXT("mat2"), TEXT("float2x2") },
				{ TEXT("mat3"), TEXT("float3x3") },
				{ TEXT("mat4"), TEXT("float4x4") },
				{ TEXT("mix"), TEXT("lerp") },
				{ TEXT("fract"), TEXT("frac") },
				{ TEXT("mod"), TEXT("fmod") }
			};

			FString OutCode;
			OutCode.Reserve(InCode.Len() + 32);

			bool bInString = false;
			bool bInLineComment = false;
			bool bInBlockComment = false;

			for (int32 Index = 0; Index < InCode.Len();)
			{
				const TCHAR Char = InCode[Index];
				const TCHAR Next = InCode.IsValidIndex(Index + 1) ? InCode[Index + 1] : TCHAR('\0');

				if (bInLineComment)
				{
					OutCode.AppendChar(Char);
					if (Char == TCHAR('\n'))
					{
						bInLineComment = false;
					}
					++Index;
					continue;
				}

				if (bInBlockComment)
				{
					OutCode.AppendChar(Char);
					if (Char == TCHAR('*') && Next == TCHAR('/'))
					{
						OutCode.AppendChar(Next);
						bInBlockComment = false;
						Index += 2;
					}
					else
					{
						++Index;
					}
					continue;
				}

				if (bInString)
				{
					OutCode.AppendChar(Char);
					if (Char == TCHAR('\\') && InCode.IsValidIndex(Index + 1))
					{
						OutCode.AppendChar(InCode[Index + 1]);
						Index += 2;
					}
					else
					{
						if (Char == TCHAR('"'))
						{
							bInString = false;
						}
						++Index;
					}
					continue;
				}

				if (Char == TCHAR('"'))
				{
					bInString = true;
					OutCode.AppendChar(Char);
					++Index;
					continue;
				}

				if (Char == TCHAR('/') && Next == TCHAR('/'))
				{
					bInLineComment = true;
					OutCode.AppendChar(Char);
					OutCode.AppendChar(Next);
					Index += 2;
					continue;
				}

				if (Char == TCHAR('/') && Next == TCHAR('*'))
				{
					bInBlockComment = true;
					OutCode.AppendChar(Char);
					OutCode.AppendChar(Next);
					Index += 2;
					continue;
				}

				if (FChar::IsAlpha(Char) || Char == TCHAR('_'))
				{
					const int32 Start = Index++;
					while (InCode.IsValidIndex(Index) && (FChar::IsAlnum(InCode[Index]) || InCode[Index] == TCHAR('_')))
					{
						++Index;
					}

					const FString Identifier = InCode.Mid(Start, Index - Start);
					if (const FString* Replacement = IdentifierMap.Find(Identifier.ToLower()))
					{
						OutCode += *Replacement;
					}
					else
					{
						OutCode += Identifier;
					}
					continue;
				}

				OutCode.AppendChar(Char);
				++Index;
			}

			return OutCode;
		}

		static bool ParseModernFunctionSignature(
			const FString& FunctionName,
			const FString& ParameterBlock,
			FString& OutInputsBlock,
			FString& OutResultsBlock,
			FString& OutError)
		{
			OutInputsBlock.Reset();
			OutResultsBlock.Reset();

			int32 ResultCount = 0;
			for (const FString& RawParameter : SplitTopLevelParameters(ParameterBlock))
			{
				const FString Parameter = RawParameter.TrimStartAndEnd();
				if (Parameter.IsEmpty())
				{
					continue;
				}

				TArray<FString> Parts;
				Parameter.ParseIntoArrayWS(Parts);
				if (Parts.Num() < 2 || Parts.Num() > 3)
				{
					OutError = FString::Printf(TEXT("Function '%s' has an invalid parameter declaration '%s'."), *FunctionName, *Parameter);
					return false;
				}

				FString Qualifier = TEXT("in");
				FString TypeToken;
				FString NameToken;
				if (Parts.Num() == 2)
				{
					TypeToken = Parts[0];
					NameToken = Parts[1];
				}
				else
				{
					Qualifier = Parts[0];
					TypeToken = Parts[1];
					NameToken = Parts[2];
				}

				Qualifier = Qualifier.TrimStartAndEnd();
				Qualifier.ToLowerInline();
				TypeToken = NormalizeShaderTypeToken(TypeToken);
				NameToken = NameToken.TrimStartAndEnd();

				if (!Qualifier.Equals(TEXT("in")) && !Qualifier.Equals(TEXT("out")))
				{
					OutError = FString::Printf(
						TEXT("Function '%s' parameter '%s' uses unsupported qualifier '%s'. Supported qualifiers are in and out."),
						*FunctionName,
						*Parameter,
						*Qualifier);
					return false;
				}

				if (TypeToken.IsEmpty() || NameToken.IsEmpty())
				{
					OutError = FString::Printf(TEXT("Function '%s' has an invalid parameter declaration '%s'."), *FunctionName, *Parameter);
					return false;
				}

				const FString Statement = FString::Printf(TEXT("        %s %s;\n"), *TypeToken, *NameToken);
				if (Qualifier.Equals(TEXT("out")))
				{
					OutResultsBlock += Statement;
					++ResultCount;
				}
				else
				{
					OutInputsBlock += Statement;
				}
			}

			if (ResultCount == 0)
			{
				OutError = FString::Printf(TEXT("Function '%s' must declare at least one out parameter."), *FunctionName);
				return false;
			}

			return true;
		}

		static bool TransformModernFunctionSyntax(const FString& InSourceText, FString& OutSourceText, FString& OutError)
		{
			OutError.Reset();
			OutSourceText = InSourceText;
			return true;
		}


		bool TryResolveExpressionOutputIndexByName(const UMaterialExpression* Expression, const FString& OutputSpecifier, int32& OutIndex)
		{
			if (!Expression || Expression->Outputs.Num() == 0)
			{
				return false;
			}

			const FName DesiredOutput(*OutputSpecifier.TrimStartAndEnd());
			if (DesiredOutput.IsNone())
			{
				OutIndex = 0;
				return true;
			}

			for (int32 OutputIndex = 0; OutputIndex < Expression->Outputs.Num(); ++OutputIndex)
			{
				const FExpressionOutput& Output = Expression->Outputs[OutputIndex];
				if (!Output.OutputName.IsNone())
				{
					if (Output.OutputName == DesiredOutput)
					{
						OutIndex = OutputIndex;
						return true;
					}
					continue;
				}

				if (Output.MaskR && Output.MaskG && Output.MaskB && !Output.MaskA && DesiredOutput == FName(TEXT("RGB")))
				{
					OutIndex = OutputIndex;
					return true;
				}
				if (Output.MaskR && Output.MaskG && !Output.MaskB && !Output.MaskA && DesiredOutput == FName(TEXT("RG")))
				{
					OutIndex = OutputIndex;
					return true;
				}
				if (Output.MaskR && Output.MaskG && Output.MaskB && Output.MaskA && DesiredOutput == FName(TEXT("RGBA")))
				{
					OutIndex = OutputIndex;
					return true;
				}
				if (Output.MaskR && !Output.MaskG && !Output.MaskB && !Output.MaskA && DesiredOutput == FName(TEXT("R")))
				{
					OutIndex = OutputIndex;
					return true;
				}
				if (!Output.MaskR && Output.MaskG && !Output.MaskB && !Output.MaskA && DesiredOutput == FName(TEXT("G")))
				{
					OutIndex = OutputIndex;
					return true;
				}
				if (!Output.MaskR && !Output.MaskG && Output.MaskB && !Output.MaskA && DesiredOutput == FName(TEXT("B")))
				{
					OutIndex = OutputIndex;
					return true;
				}
				if (!Output.MaskR && !Output.MaskG && !Output.MaskB && Output.MaskA && DesiredOutput == FName(TEXT("A")))
				{
					OutIndex = OutputIndex;
					return true;
				}
			}

			return false;
		}

		int32 GetPreferredOutputIndexForProperty(const FTextShaderPropertyDefinition& Property, const UMaterialExpression* Expression)
		{
			if (Property.Type == ETextShaderPropertyType::Vector && !Property.bConst)
			{
				static const TCHAR* ComponentOutputs[] = { TEXT(""), TEXT("R"), TEXT("RG"), TEXT("RGB"), TEXT("RGBA") };
				int32 OutputIndex = 0;
				if (Property.ComponentCount > 0
					&& Property.ComponentCount < UE_ARRAY_COUNT(ComponentOutputs)
					&& TryResolveExpressionOutputIndexByName(Expression, ComponentOutputs[Property.ComponentCount], OutputIndex))
				{
					return OutputIndex;
				}
			}

			return 0;
		}

		FString BuildOutputTargetCacheKey(const FTextShaderOutputBinding& Binding)
		{
			TArray<FString> Parts;
			Parts.Reserve(Binding.ExpressionArguments.Num() + 1);
			Parts.Add(UE::DreamShader::NormalizeSettingKey(Binding.ExpressionClass));

			TArray<FString> ArgumentKeys;
			Binding.ExpressionArguments.GetKeys(ArgumentKeys);
			ArgumentKeys.Sort();
			for (const FString& Key : ArgumentKeys)
			{
				Parts.Add(Key + TEXT("=") + Binding.ExpressionArguments.FindChecked(Key));
			}

			return FString::Join(Parts, TEXT("|"));
		}

		bool CreateOrReuseOutputTargetExpression(
			UMaterial* Material,
			const FTextShaderOutputBinding& Binding,
			TMap<FString, UMaterialExpression*>& InOutExpressions,
			int32& InOutPositionY,
			UMaterialExpression*& OutExpression,
			FString& OutError)
		{
			const FString CacheKey = BuildOutputTargetCacheKey(Binding);
			if (UMaterialExpression* const* ExistingExpression = InOutExpressions.Find(CacheKey))
			{
				OutExpression = *ExistingExpression;
				return true;
			}

			UClass* ExpressionClass = Private::ResolveMaterialExpressionClass(Binding.ExpressionClass);
			if (!ExpressionClass)
			{
				OutError = FString::Printf(TEXT("Output target '%s' could not resolve MaterialExpression class '%s'."), *Binding.TargetText, *Binding.ExpressionClass);
				return false;
			}

			OutExpression = Cast<UMaterialExpression>(
				UMaterialEditingLibrary::CreateMaterialExpression(Material, ExpressionClass, 1200, InOutPositionY));
			if (!OutExpression)
			{
				OutError = FString::Printf(TEXT("Output target '%s' failed to create '%s'."), *Binding.TargetText, *ExpressionClass->GetName());
				return false;
			}
			InOutPositionY += 220;

			for (const TPair<FString, FString>& Argument : Binding.ExpressionArguments)
			{
				if (Argument.Key == UE::DreamShader::NormalizeSettingKey(TEXT("Class")))
				{
					continue;
				}

				FProperty* BoundProperty = Private::FindMaterialExpressionArgumentProperty(ExpressionClass, Argument.Key);
				if (!BoundProperty)
				{
					OutError = FString::Printf(TEXT("Output target '%s': '%s' is not a property on '%s'."), *Binding.TargetText, *Argument.Key, *ExpressionClass->GetName());
					return false;
				}

				if (Private::IsMaterialExpressionInputProperty(BoundProperty))
				{
					OutError = FString::Printf(TEXT("Output target '%s': inline input property '%s' is not supported yet. Bind through .Pin[index] instead."), *Binding.TargetText, *Argument.Key);
					return false;
				}

				FString LiteralError;
				if (!Private::SetMaterialExpressionLiteralProperty(OutExpression, BoundProperty, Argument.Value, LiteralError))
				{
					OutError = FString::Printf(TEXT("Output target '%s': %s"), *Binding.TargetText, *LiteralError);
					return false;
				}
			}

			InOutExpressions.Add(CacheKey, OutExpression);
			return true;
		}

		bool ConnectExpressionSourceToTargetPin(
			UMaterialExpression* SourceExpression,
			int32 SourceOutputIndex,
			const FString& SourceDebugName,
			const FTextShaderOutputBinding& Binding,
			UMaterialExpression* TargetExpression,
			TSet<FString>& BoundPins,
			FString& OutError)
		{
			if (!SourceExpression || !TargetExpression)
			{
				OutError = TEXT("Invalid output source or target expression.");
				return false;
			}

			const FString PinKey = BuildOutputTargetCacheKey(Binding) + FString::Printf(TEXT("#%d"), Binding.ExpressionPinIndex);
			if (BoundPins.Contains(PinKey))
			{
				OutError = FString::Printf(TEXT("Output target pin '%s' is bound more than once."), *Binding.TargetText);
				return false;
			}

			FExpressionInput* TargetInput = TargetExpression->GetInput(Binding.ExpressionPinIndex);
			if (!TargetInput)
			{
				OutError = FString::Printf(TEXT("Output target '%s' does not have Pin[%d]."), *Binding.TargetText, Binding.ExpressionPinIndex);
				return false;
			}

			Private::FCodeValue SourceValue;
			SourceValue.Expression = SourceExpression;
			SourceValue.OutputIndex = SourceOutputIndex;
			Private::FCodeValue RoutedValue = Private::CreateOutputRerouteValue(
				SourceExpression->Material,
				SourceExpression->Function,
				SourceValue,
				Binding.TargetText,
				Binding.ExpressionPinIndex);
			Private::ConnectCodeValueToInput(*TargetInput, RoutedValue);
			BoundPins.Add(PinKey);
			return true;
		}

		const TCHAR* GetMaterialFunctionBlockKindText(const ETextShaderMaterialFunctionKind Kind)
		{
			return UE::DreamShader::LexToString(Kind);
		}

		EMaterialFunctionUsage GetUnrealMaterialFunctionUsage(const ETextShaderMaterialFunctionKind Kind)
		{
			switch (Kind)
			{
			case ETextShaderMaterialFunctionKind::MaterialLayer:
				return EMaterialFunctionUsage::MaterialLayer;
			case ETextShaderMaterialFunctionKind::MaterialLayerBlend:
				return EMaterialFunctionUsage::MaterialLayerBlend;
			case ETextShaderMaterialFunctionKind::ShaderFunction:
			default:
				return EMaterialFunctionUsage::Default;
			}
		}

		static constexpr int32 DreamShaderAcceptableLayerMaterialAttributesInputs = 1;
		static constexpr int32 DreamShaderAcceptableBlendMaterialAttributesInputs = 2;

#if DREAMSHADER_UE_VERSION_AT_LEAST(5, 7)
		EBlendInputRelevance ResolveMaterialLayerBlendInputRelevance(
			const FTextShaderMaterialFunctionDefinition& FunctionDefinition,
			const FTextShaderFunctionParameter& InputDefinition,
			const int32 MaterialAttributesInputIndex)
		{
			if (FunctionDefinition.Kind != ETextShaderMaterialFunctionKind::MaterialLayerBlend
				|| !Private::IsMaterialAttributesType(InputDefinition.Type))
			{
				return EBlendInputRelevance::General;
			}

			FString NormalizedInputName = InputDefinition.Name;
			NormalizedInputName.ReplaceInline(TEXT(" "), TEXT(""));
			NormalizedInputName.ReplaceInline(TEXT("_"), TEXT(""));
			NormalizedInputName.ReplaceInline(TEXT("-"), TEXT(""));
			if (NormalizedInputName.Equals(TEXT("Top"), ESearchCase::IgnoreCase)
				|| NormalizedInputName.Equals(TEXT("TopLayer"), ESearchCase::IgnoreCase)
				|| InputDefinition.Name.Equals(TopMaterialBlendInputName, ESearchCase::IgnoreCase))
			{
				return EBlendInputRelevance::Top;
			}
			if (NormalizedInputName.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase)
				|| NormalizedInputName.Equals(TEXT("BottomLayer"), ESearchCase::IgnoreCase)
				|| NormalizedInputName.Equals(TEXT("Base"), ESearchCase::IgnoreCase)
				|| NormalizedInputName.Equals(TEXT("BaseLayer"), ESearchCase::IgnoreCase)
				|| InputDefinition.Name.Equals(BottomMaterialBlendInputName, ESearchCase::IgnoreCase))
			{
				return EBlendInputRelevance::Bottom;
			}

			return MaterialAttributesInputIndex == 0
				? EBlendInputRelevance::Bottom
				: EBlendInputRelevance::Top;
		}
#endif

		bool ValidateMaterialLayerFunctionDefinition(const FTextShaderMaterialFunctionDefinition& FunctionDefinition, FString& OutError)
		{
			const TCHAR* BlockKind = GetMaterialFunctionBlockKindText(FunctionDefinition.Kind);
			if (FunctionDefinition.Kind == ETextShaderMaterialFunctionKind::ShaderFunction)
			{
				return true;
			}

			if (FunctionDefinition.Outputs.Num() != 1 || !Private::IsMaterialAttributesType(FunctionDefinition.Outputs[0].Type))
			{
				OutError = FString::Printf(TEXT("%s '%s' must declare exactly one MaterialAttributes output."), BlockKind, *FunctionDefinition.Name);
				return false;
			}

			int32 MaterialAttributesInputCount = 0;
			for (const FTextShaderFunctionParameter& InputDefinition : FunctionDefinition.Inputs)
			{
				if (Private::IsMaterialAttributesType(InputDefinition.Type))
				{
					++MaterialAttributesInputCount;
				}
			}

			if (FunctionDefinition.Kind == ETextShaderMaterialFunctionKind::MaterialLayer
				&& (FunctionDefinition.Inputs.Num() > DreamShaderAcceptableLayerMaterialAttributesInputs
					|| MaterialAttributesInputCount != FunctionDefinition.Inputs.Num()))
			{
				OutError = FString::Printf(TEXT("ShaderLayer '%s' must declare at most one input, and it must be MaterialAttributes. Use Properties for layer controls."), *FunctionDefinition.Name);
				return false;
			}

			if (FunctionDefinition.Kind == ETextShaderMaterialFunctionKind::MaterialLayerBlend)
			{
				if (FunctionDefinition.Inputs.Num() != DreamShaderAcceptableBlendMaterialAttributesInputs
					|| MaterialAttributesInputCount != DreamShaderAcceptableBlendMaterialAttributesInputs)
				{
					OutError = FString::Printf(TEXT("ShaderLayerBlend '%s' must declare exactly two inputs, both MaterialAttributes. Use Properties for blend controls."), *FunctionDefinition.Name);
					return false;
				}
			}

			return true;
		}

		void CacheMaterialFunctionInterfaceIds(
			const UMaterialFunction* MaterialFunction,
			TMap<FName, FGuid>& OutInputIdsByName,
			TMap<FName, FGuid>& OutOutputIdsByName)
		{
			OutInputIdsByName.Reset();
			OutOutputIdsByName.Reset();
			if (!MaterialFunction)
			{
				return;
			}

			for (UMaterialExpression* Expression : MaterialFunction->GetExpressions())
			{
				if (const UMaterialExpressionFunctionInput* InputExpression = Cast<UMaterialExpressionFunctionInput>(Expression))
				{
					if (!InputExpression->InputName.IsNone() && InputExpression->Id.IsValid())
					{
						OutInputIdsByName.Add(InputExpression->InputName, InputExpression->Id);
					}
				}
				else if (const UMaterialExpressionFunctionOutput* OutputExpression = Cast<UMaterialExpressionFunctionOutput>(Expression))
				{
					if (!OutputExpression->OutputName.IsNone() && OutputExpression->Id.IsValid())
					{
						OutOutputIdsByName.Add(OutputExpression->OutputName, OutputExpression->Id);
					}
				}
			}
		}

		void RestoreOrGenerateFunctionInputId(
			UMaterialExpressionFunctionInput* InputExpression,
			const TMap<FName, FGuid>& InputIdsByName)
		{
			if (!InputExpression)
			{
				return;
			}

			if (const FGuid* ExistingId = InputIdsByName.Find(InputExpression->InputName))
			{
				InputExpression->Id = *ExistingId;
			}

			InputExpression->ConditionallyGenerateId(false);
		}

		void RestoreOrGenerateFunctionOutputId(
			UMaterialExpressionFunctionOutput* OutputExpression,
			const TMap<FName, FGuid>& OutputIdsByName)
		{
			if (!OutputExpression)
			{
				return;
			}

			if (const FGuid* ExistingId = OutputIdsByName.Find(OutputExpression->OutputName))
			{
				OutputExpression->Id = *ExistingId;
			}

			OutputExpression->ConditionallyGenerateId(false);
		}

		bool AppendInitializedOutputStatements(
			const TArray<FTextShaderVariableDeclaration>& OutputDeclarations,
			TArray<Private::FCodeStatement>& InOutStatements,
			FString& OutError)
		{
			for (const FTextShaderVariableDeclaration& OutputDeclaration : OutputDeclarations)
			{
				if (!OutputDeclaration.bHasDefaultValue)
				{
					continue;
				}

				Private::FCodeStatement Statement;
				if (!Private::MakeCodeDeclarationStatement(
					OutputDeclaration.Type,
					OutputDeclaration.Name,
					OutputDeclaration.DefaultValueText,
					Statement,
					OutError))
				{
					OutError = FString::Printf(TEXT("Output '%s': %s"), *OutputDeclaration.Name, *OutError);
					return false;
				}

				InOutStatements.Add(Statement);
			}

			return true;
		}

		bool GenerateMaterialFunctionAsset(
			const FString& SourceFilePath,
			const FString& PreparedSource,
			const FString& SourceHash,
			const FTextShaderDefinition& RootDefinition,
			const FTextShaderMaterialFunctionDefinition& FunctionDefinition,
			const bool bForce,
			const bool bTransient,
			FString& OutGeneratedAssetPath,
			FString& OutError)
		{
			FScopedSlowTask FunctionSlowTask(
				10.0f,
				FText::FromString(FString::Printf(TEXT("Generating DreamShader function '%s'..."), *FunctionDefinition.Name)));
			if (!IsRunningCommandlet())
			{
				FunctionSlowTask.MakeDialogDelayed(0.25f);
			}

			const TCHAR* BlockKind = GetMaterialFunctionBlockKindText(FunctionDefinition.Kind);
			FunctionSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Validating %s '%s'..."), BlockKind, *FunctionDefinition.Name)));
			if (FunctionDefinition.Outputs.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s '%s' must declare at least one output."), BlockKind, *FunctionDefinition.Name);
				return false;
			}

			if (!ValidateMaterialLayerFunctionDefinition(FunctionDefinition, OutError))
			{
				return false;
			}

			if (FunctionDefinition.Code.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s '%s' must provide a Graph block."), BlockKind, *FunctionDefinition.Name);
				return false;
			}

			UMaterialFunction* MaterialFunction = nullptr;
			if (!Private::CreateOrReuseMaterialFunction(FunctionDefinition, MaterialFunction, OutError, bTransient) || !MaterialFunction)
			{
				return false;
			}

			const EMaterialFunctionUsage ExpectedUsage = GetUnrealMaterialFunctionUsage(FunctionDefinition.Kind);
			if (!bForce
				&& Private::IsGeneratedAssetSourceCurrent(MaterialFunction, SourceFilePath, SourceHash)
				&& MaterialFunction->GetMaterialFunctionUsage() == ExpectedUsage)
			{
				OutGeneratedAssetPath = MaterialFunction->GetPathName();
				return true;
			}

			MaterialFunction->Modify();
			MaterialFunction->SetMaterialFunctionUsage(ExpectedUsage);
			TMap<FName, FGuid> ExistingInputIdsByName;
			TMap<FName, FGuid> ExistingOutputIdsByName;
			CacheMaterialFunctionInterfaceIds(MaterialFunction, ExistingInputIdsByName, ExistingOutputIdsByName);
			FunctionSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Clearing old function graph '%s'..."), *MaterialFunction->GetName())));
			Private::ClearMaterialFunctionExpressions(MaterialFunction);

			if (const FString* Description = FunctionDefinition.Settings.Find(UE::DreamShader::NormalizeSettingKey(TEXT("Description"))))
			{
				MaterialFunction->Description = *Description;
			}
			else
			{
				MaterialFunction->Description.Reset();
			}

			if (const FString* Caption = FunctionDefinition.Settings.Find(UE::DreamShader::NormalizeSettingKey(TEXT("UserExposedCaption"))))
			{
				MaterialFunction->UserExposedCaption = *Caption;
			}
			else
			{
				MaterialFunction->UserExposedCaption.Reset();
			}

			if (const FString* ExposeToLibraryText = FunctionDefinition.Settings.Find(UE::DreamShader::NormalizeSettingKey(TEXT("ExposeToLibrary"))))
			{
				bool bExposeToLibrary = false;
				if (!Private::ParseBooleanLiteral(*ExposeToLibraryText, bExposeToLibrary))
				{
					OutError = FString::Printf(TEXT("%s '%s': ExposeToLibrary must be true or false."), BlockKind, *FunctionDefinition.Name);
					return false;
				}
				MaterialFunction->bExposeToLibrary = bExposeToLibrary ? 1U : 0U;
			}
			else
			{
				MaterialFunction->bExposeToLibrary = 0U;
			}

			MaterialFunction->LibraryCategoriesText.Reset();
			if (const FString* CategoriesText = FunctionDefinition.Settings.Find(UE::DreamShader::NormalizeSettingKey(TEXT("LibraryCategories"))))
			{
				TArray<FString> Categories;
				CategoriesText->ParseIntoArray(Categories, TEXT(","), true);
				for (const FString& Category : Categories)
				{
					const FString TrimmedCategory = Category.TrimStartAndEnd();
					if (!TrimmedCategory.IsEmpty())
					{
						MaterialFunction->LibraryCategoriesText.Add(FText::FromString(TrimmedCategory));
					}
				}
			}

			TMap<FString, Private::FCodeValue> GeneratedValues;
			TMap<FString, UMaterialExpression*> GeneratedExpressionsByVariable;
			TMap<FString, FString> RegionByVariable;
			TSet<FString> SeenPropertyNames;
			for (const FTextShaderPropertyDefinition& Property : FunctionDefinition.Properties)
			{
				bool bNameConflict = false;
				for (const FString& ExistingPropertyName : SeenPropertyNames)
				{
					if (ExistingPropertyName.Equals(Property.Name, ESearchCase::IgnoreCase))
					{
						bNameConflict = true;
						break;
					}
				}

				for (const FTextShaderFunctionParameter& InputDefinition : FunctionDefinition.Inputs)
				{
					if (InputDefinition.Name.Equals(Property.Name, ESearchCase::IgnoreCase))
					{
						bNameConflict = true;
						break;
					}
				}

				if (bNameConflict)
				{
					OutError = FString::Printf(
						TEXT("%s '%s' property '%s' conflicts with another property or input name."),
						BlockKind,
						*FunctionDefinition.Name,
						*Property.Name);
					return false;
				}

				SeenPropertyNames.Add(Property.Name);
			}

			TMap<FString, UMaterialExpressionFunctionInput*> GeneratedInputExpressions;
			int32 InputPositionY = -260;
			int32 MaterialAttributesInputIndex = 0;
			FunctionSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Creating inputs for '%s'..."), *FunctionDefinition.Name)));
			for (int32 InputIndex = 0; InputIndex < FunctionDefinition.Inputs.Num(); ++InputIndex)
			{
				const FTextShaderFunctionParameter& InputDefinition = FunctionDefinition.Inputs[InputIndex];

				int32 ComponentCount = 0;
				bool bIsTextureObject = false;
				bool bIsSubstrateMaterial = false;
				ETextShaderTextureType TextureType = ETextShaderTextureType::Texture2D;
				int32 FunctionInputTypeValue = 0;
				if (!Private::TryResolveMaterialFunctionParameterType(
					InputDefinition.Type,
					ComponentCount,
					bIsTextureObject,
					FunctionInputTypeValue,
					bIsSubstrateMaterial))
				{
					if (Private::IsSubstrateMaterialType(InputDefinition.Type))
					{
						OutError = FString::Printf(TEXT("%s '%s' input '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), BlockKind, *FunctionDefinition.Name, *InputDefinition.Name);
						return false;
					}
					OutError = FString::Printf(TEXT("%s '%s' input '%s' uses unsupported type '%s'."), BlockKind, *FunctionDefinition.Name, *InputDefinition.Name, *InputDefinition.Type);
					return false;
				}
				if (bIsTextureObject)
				{
					verify(Private::TryResolveCodeDeclaredType(InputDefinition.Type, ComponentCount, bIsTextureObject, TextureType));
				}

				auto* InputExpression = Cast<UMaterialExpressionFunctionInput>(
					UMaterialEditingLibrary::CreateMaterialExpressionInFunction(MaterialFunction, UMaterialExpressionFunctionInput::StaticClass(), -800, InputPositionY));
				if (!InputExpression)
				{
					OutError = FString::Printf(TEXT("%s '%s' failed to create input '%s'."), BlockKind, *FunctionDefinition.Name, *InputDefinition.Name);
					return false;
				}

				InputExpression->InputName = FName(*InputDefinition.Name);
				InputExpression->InputType = static_cast<EFunctionInputType>(FunctionInputTypeValue);
#if DREAMSHADER_UE_VERSION_AT_LEAST(5, 7)
				InputExpression->BlendInputRelevance = ResolveMaterialLayerBlendInputRelevance(
					FunctionDefinition,
					InputDefinition,
					MaterialAttributesInputIndex);
#endif
				InputExpression->Description = InputDefinition.Metadata.Description;
				InputExpression->SortPriority = InputDefinition.Metadata.bHasSortPriority
					? InputDefinition.Metadata.SortPriority
					: InputIndex;
				RestoreOrGenerateFunctionInputId(InputExpression, ExistingInputIdsByName);
				if (Private::IsMaterialAttributesType(InputDefinition.Type))
				{
					++MaterialAttributesInputIndex;
				}

				Private::FCodeValue InputValue;
				InputValue.Expression = InputExpression;
				InputValue.ComponentCount = ComponentCount;
				InputValue.bIsTextureObject = bIsTextureObject;
				InputValue.TextureType = TextureType;
				InputValue.bIsMaterialAttributes = ComponentCount == 0 && !bIsTextureObject && !bIsSubstrateMaterial;
				InputValue.bIsSubstrateMaterial = bIsSubstrateMaterial;
				GeneratedValues.Add(InputDefinition.Name, InputValue);
				GeneratedInputExpressions.Add(InputDefinition.Name, InputExpression);
				GeneratedExpressionsByVariable.Add(InputDefinition.Name, InputExpression);
				InputPositionY += 180;
			}

			for (const FTextShaderFunctionParameter& InputDefinition : FunctionDefinition.Inputs)
			{
				UMaterialExpressionFunctionInput** InputExpressionPtr = GeneratedInputExpressions.Find(InputDefinition.Name);
				const Private::FCodeValue* InputValue = GeneratedValues.Find(InputDefinition.Name);
				if (!InputExpressionPtr || !*InputExpressionPtr || !InputValue)
				{
					OutError = FString::Printf(TEXT("%s '%s' failed to resolve generated input '%s'."), BlockKind, *FunctionDefinition.Name, *InputDefinition.Name);
					return false;
				}

				FString PreviewError;
				if (!ApplyFunctionInputPreviewDefault(
					MaterialFunction,
					SourceFilePath,
					RootDefinition,
					InputDefinition,
					*InputExpressionPtr,
					InputValue->ComponentCount,
					InputValue->bIsTextureObject,
					InputValue->TextureType,
					&FunctionDefinition.Properties,
					GeneratedValues,
					PreviewError))
				{
					OutError = FString::Printf(TEXT("%s '%s' input '%s': %s"), BlockKind, *FunctionDefinition.Name, *InputDefinition.Name, *PreviewError);
					return false;
				}
			}

			int32 MaterialAttributesSeedPositionY = 260;
			for (const FTextShaderFunctionParameter& OutputDefinition : FunctionDefinition.Outputs)
			{
				if (Private::IsMaterialAttributesType(OutputDefinition.Type))
				{
					FString SeedError;
					if (!SeedMaterialAttributesGraphValue(
						nullptr,
						MaterialFunction,
						OutputDefinition.Name,
						GeneratedValues,
						MaterialAttributesSeedPositionY,
						SeedError))
					{
						OutError = FString::Printf(TEXT("%s '%s' output '%s': %s"), BlockKind, *FunctionDefinition.Name, *OutputDefinition.Name, *SeedError);
						return false;
					}
				}
				else if (Private::IsSubstrateMaterialType(OutputDefinition.Type) && !OutputDefinition.bHasDefaultValue)
				{
					continue;
				}
			}

			if (!FunctionDefinition.Code.IsEmpty())
			{
				FunctionSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Parsing Graph block for '%s'..."), *FunctionDefinition.Name)));
				FString CodeSourceFilePath;
				int32 CodeStartLine = 1;
				int32 CodeStartColumn = 1;
				ResolveCodeBlockLocation(SourceFilePath, PreparedSource, FunctionDefinition.CodeStartIndex, CodeSourceFilePath, CodeStartLine, CodeStartColumn);
				TArray<Private::FCodeStatement> CodeStatements;
				FString CodeParseError;
				int32 CodeParseErrorLine = 0;
				int32 CodeParseErrorColumn = 0;
				if (!Private::ParseCodeStatements(FunctionDefinition.Code, CodeStatements, CodeParseError, &CodeParseErrorLine, &CodeParseErrorColumn))
				{
					OutError = FormatCodeBlockError(
						SourceFilePath,
						CodeSourceFilePath,
						CodeStartLine,
						CodeStartColumn,
						FString::Printf(TEXT("%s '%s': %s"), BlockKind, *FunctionDefinition.Name, *CodeParseError),
						CodeParseErrorLine,
						CodeParseErrorColumn);
					return false;
				}
				ApplyStatementRegionsRecursive(CodeStatements, FunctionDefinition.GraphRegions);

				Private::FCodeGraphBuilder CodeGraphBuilder(
					nullptr,
					MaterialFunction,
					RootDefinition,
					SourceFilePath,
					Private::BuildGeneratedIncludeVirtualPath(SourceFilePath),
					&FunctionDefinition.Properties,
					CodeSourceFilePath,
					CodeStartLine,
					CodeStartColumn);
				FString CodeBuildError;
				FunctionSlowTask.EnterProgressFrame(2.0f, FText::FromString(FString::Printf(TEXT("Creating Graph nodes for '%s'..."), *FunctionDefinition.Name)));
				if (!CodeGraphBuilder.Build(CodeStatements, GeneratedValues, CodeBuildError))
				{
					OutError = CodeBuildError;
					return false;
				}
				GeneratedExpressionsByVariable.Append(CodeGraphBuilder.GetGeneratedExpressionsByVariable());
				RegionByVariable = CodeGraphBuilder.GetRegionByVariable();
			}
			else
			{
				const FTextShaderFunctionParameter& PrimaryOutput = FunctionDefinition.Outputs[0];
				ECustomMaterialOutputType OutputType = CMOT_Float1;
				if (Private::IsSubstrateMaterialType(PrimaryOutput.Type))
				{
					OutError = Private::IsSubstrateMaterialTypeSupported()
						? FString::Printf(TEXT("%s '%s' output '%s' uses Substrate, which is not supported by HLSL Custom node functions. Use a Graph block and Substrate.* nodes."), BlockKind, *FunctionDefinition.Name, *PrimaryOutput.Name)
						: FString::Printf(TEXT("%s '%s' output '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), BlockKind, *FunctionDefinition.Name, *PrimaryOutput.Name);
					return false;
				}
				if (!Private::TryResolveCustomOutputType(PrimaryOutput.Type, OutputType))
				{
					OutError = FString::Printf(TEXT("%s '%s' output '%s' uses unsupported type '%s'."), BlockKind, *FunctionDefinition.Name, *PrimaryOutput.Name, *PrimaryOutput.Type);
					return false;
				}

				auto* CustomExpression = Cast<UMaterialExpressionCustom>(
					UMaterialEditingLibrary::CreateMaterialExpressionInFunction(MaterialFunction, UMaterialExpressionCustom::StaticClass(), 120, 0));
				if (!CustomExpression)
				{
					OutError = FString::Printf(TEXT("%s '%s' failed to create the function Custom node."), BlockKind, *FunctionDefinition.Name);
					return false;
				}

				CustomExpression->Description = FunctionDefinition.Name;
				CustomExpression->OutputType = OutputType;
#if DREAMSHADER_UE_VERSION_AT_LEAST(5, 4)
				CustomExpression->ShowCode = false;
#endif
				CustomExpression->Inputs.Reset();
				CustomExpression->AdditionalOutputs.Reset();
				CustomExpression->IncludeFilePaths.Reset();

				FString PreparedCustomCode;
				bool bUsesGeneratedInclude = false;
				if (!Private::PrepareCustomNodeCode(
					RootDefinition,
					FunctionDefinition.HLSL,
					TArray<FString>(),
					FunctionDefinition.Name,
					PreparedCustomCode,
					bUsesGeneratedInclude,
					OutError))
				{
					OutError = FString::Printf(TEXT("%s '%s': %s"), BlockKind, *FunctionDefinition.Name, *OutError);
					return false;
				}
				CustomExpression->Code = Private::EnsureTopLevelReturn(PreparedCustomCode);

				if (bUsesGeneratedInclude)
				{
					CustomExpression->IncludeFilePaths.Add(Private::BuildGeneratedIncludeVirtualPath(SourceFilePath));
				}

				TMap<FString, UMaterialExpression*> GeneratedPropertyExpressions;
				TSet<FString> CreatingPropertyNames;
				int32 PropertyPositionY = -620;
				for (const FTextShaderFunctionParameter& InputDefinition : FunctionDefinition.Inputs)
				{
					const Private::FCodeValue* InputValue = GeneratedValues.Find(InputDefinition.Name);
					if (!InputValue || !InputValue->Expression)
					{
						OutError = FString::Printf(TEXT("%s '%s' failed to resolve generated input '%s'."), BlockKind, *FunctionDefinition.Name, *InputDefinition.Name);
						return false;
					}

					FCustomInput Input;
					Input.InputName = FName(*InputDefinition.Name);
					CustomExpression->Inputs.Add(Input);
					Private::ConnectCodeValueToInput(CustomExpression->Inputs.Last().Input, *InputValue);
				}

				for (const FTextShaderPropertyDefinition& Property : FunctionDefinition.Properties)
				{
					if (!ContainsIdentifierReference(PreparedCustomCode, Property.Name))
					{
						continue;
					}

					FString PropertyExpressionError;
					UMaterialExpression* PropertyExpression = nullptr;
					if (!CreateReferencedPropertyExpression(
						nullptr,
						MaterialFunction,
						FunctionDefinition.Properties,
						Property,
						GeneratedPropertyExpressions,
						CreatingPropertyNames,
						PropertyPositionY,
						PropertyExpression,
						PropertyExpressionError))
					{
						OutError = FString::Printf(TEXT("%s '%s' property '%s': %s"), BlockKind, *FunctionDefinition.Name, *Property.Name, *PropertyExpressionError);
						return false;
					}

					FCustomInput Input;
					Input.InputName = FName(*Property.Name);
					CustomExpression->Inputs.Add(Input);
					CustomExpression->Inputs.Last().Input.Connect(GetPreferredOutputIndexForProperty(Property, PropertyExpression), PropertyExpression);
				}

				for (int32 OutputIndex = 1; OutputIndex < FunctionDefinition.Outputs.Num(); ++OutputIndex)
				{
					const FTextShaderFunctionParameter& OutputDefinition = FunctionDefinition.Outputs[OutputIndex];
					ECustomMaterialOutputType AdditionalOutputType = CMOT_Float1;
					if (Private::IsSubstrateMaterialType(OutputDefinition.Type))
					{
						OutError = Private::IsSubstrateMaterialTypeSupported()
							? FString::Printf(TEXT("%s '%s' output '%s' uses Substrate, which is not supported by HLSL Custom node functions. Use a Graph block and Substrate.* nodes."), BlockKind, *FunctionDefinition.Name, *OutputDefinition.Name)
							: FString::Printf(TEXT("%s '%s' output '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), BlockKind, *FunctionDefinition.Name, *OutputDefinition.Name);
						return false;
					}
					if (!Private::TryResolveCustomOutputType(OutputDefinition.Type, AdditionalOutputType))
					{
						OutError = FString::Printf(TEXT("%s '%s' output '%s' uses unsupported type '%s'."), BlockKind, *FunctionDefinition.Name, *OutputDefinition.Name, *OutputDefinition.Type);
						return false;
					}

					FCustomOutput Output;
					Output.OutputName = FName(*OutputDefinition.Name);
					Output.OutputType = AdditionalOutputType;
					CustomExpression->AdditionalOutputs.Add(Output);
				}

				Private::RebuildDreamShaderCustomOutputs(CustomExpression);

				Private::FCodeValue PrimaryOutputValue;
				PrimaryOutputValue.Expression = CustomExpression;
				PrimaryOutputValue.ComponentCount = 0;
				PrimaryOutputValue.bIsTextureObject = false;
				verify(Private::TryResolveCodeDeclaredType(PrimaryOutput.Type, PrimaryOutputValue.ComponentCount, PrimaryOutputValue.bIsTextureObject, PrimaryOutputValue.TextureType, PrimaryOutputValue.bIsSubstrateMaterial));
				PrimaryOutputValue.bIsMaterialAttributes = PrimaryOutputValue.ComponentCount == 0 && !PrimaryOutputValue.bIsTextureObject && !PrimaryOutputValue.bIsSubstrateMaterial;
				GeneratedValues.Add(PrimaryOutput.Name, PrimaryOutputValue);

				for (int32 OutputIndex = 1; OutputIndex < FunctionDefinition.Outputs.Num(); ++OutputIndex)
				{
					const FTextShaderFunctionParameter& OutputDefinition = FunctionDefinition.Outputs[OutputIndex];
					Private::FCodeValue OutputValue;
					OutputValue.Expression = CustomExpression;
					OutputValue.OutputIndex = OutputIndex;
					verify(Private::TryResolveCodeDeclaredType(OutputDefinition.Type, OutputValue.ComponentCount, OutputValue.bIsTextureObject, OutputValue.TextureType, OutputValue.bIsSubstrateMaterial));
					OutputValue.bIsMaterialAttributes = OutputValue.ComponentCount == 0 && !OutputValue.bIsTextureObject && !OutputValue.bIsSubstrateMaterial;
					GeneratedValues.Add(OutputDefinition.Name, OutputValue);
				}
			}

			int32 OutputPositionY = -120;
			FunctionSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Connecting outputs for '%s'..."), *FunctionDefinition.Name)));
			for (int32 OutputIndex = 0; OutputIndex < FunctionDefinition.Outputs.Num(); ++OutputIndex)
			{
				const FTextShaderFunctionParameter& OutputDefinition = FunctionDefinition.Outputs[OutputIndex];
				const Private::FCodeValue* OutputValue = GeneratedValues.Find(OutputDefinition.Name);
				if (!OutputValue || !OutputValue->Expression)
				{
					OutError = FString::Printf(TEXT("%s '%s' output '%s' was never assigned an expression."), BlockKind, *FunctionDefinition.Name, *OutputDefinition.Name);
					return false;
				}

				int32 ExpectedComponentCount = 0;
				bool bExpectedTexture = false;
				bool bExpectedSubstrate = false;
				int32 IgnoredFunctionInputType = 0;
				if (!Private::TryResolveMaterialFunctionParameterType(
					OutputDefinition.Type,
					ExpectedComponentCount,
					bExpectedTexture,
					IgnoredFunctionInputType,
					bExpectedSubstrate))
				{
					if (Private::IsSubstrateMaterialType(OutputDefinition.Type) && !Private::IsSubstrateMaterialTypeSupported())
					{
						OutError = FString::Printf(TEXT("%s '%s' output '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), BlockKind, *FunctionDefinition.Name, *OutputDefinition.Name);
						return false;
					}
					OutError = FString::Printf(TEXT("%s '%s' output '%s' uses unsupported type '%s'."), BlockKind, *FunctionDefinition.Name, *OutputDefinition.Name, *OutputDefinition.Type);
					return false;
				}

				ETextShaderTextureType ExpectedTextureType = ETextShaderTextureType::Texture2D;
				if (bExpectedTexture)
				{
					verify(Private::TryResolveCodeDeclaredType(OutputDefinition.Type, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType));
				}

				if (bExpectedTexture != OutputValue->bIsTextureObject
					|| (bExpectedTexture && ExpectedTextureType != OutputValue->TextureType)
					|| bExpectedSubstrate != OutputValue->bIsSubstrateMaterial
					|| ((ExpectedComponentCount == 0 && !bExpectedTexture && !bExpectedSubstrate) != OutputValue->bIsMaterialAttributes)
					|| (!bExpectedTexture && ExpectedComponentCount != OutputValue->ComponentCount))
				{
					OutError = FString::Printf(TEXT("%s '%s' output '%s' does not match its declared type '%s'."), BlockKind, *FunctionDefinition.Name, *OutputDefinition.Name, *OutputDefinition.Type);
					return false;
				}

				auto* OutputExpression = Cast<UMaterialExpressionFunctionOutput>(
					UMaterialEditingLibrary::CreateMaterialExpressionInFunction(MaterialFunction, UMaterialExpressionFunctionOutput::StaticClass(), 900, OutputPositionY));
				if (!OutputExpression)
				{
					OutError = FString::Printf(TEXT("%s '%s' failed to create output '%s'."), BlockKind, *FunctionDefinition.Name, *OutputDefinition.Name);
					return false;
				}

				OutputExpression->OutputName = FName(*OutputDefinition.Name);
				OutputExpression->Description = OutputDefinition.Metadata.Description;
				OutputExpression->SortPriority = OutputDefinition.Metadata.bHasSortPriority
					? OutputDefinition.Metadata.SortPriority
					: OutputIndex;
				RestoreOrGenerateFunctionOutputId(OutputExpression, ExistingOutputIdsByName);
				const Private::FCodeValue RoutedOutputValue = Private::CreateOutputRerouteValue(
					nullptr,
					MaterialFunction,
					*OutputValue,
					OutputDefinition.Name,
					OutputIndex);
				Private::ConnectCodeValueToInput(OutputExpression->A, RoutedOutputValue);
				GeneratedExpressionsByVariable.Add(OutputDefinition.Name, OutputExpression);
				OutputPositionY += 180;
			}

			if (bTransient)
			{
				FunctionSlowTask.EnterProgressFrame(1.0f);
			}
			else
			{
				FunctionSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Laying out '%s'..."), *FunctionDefinition.Name)));
				Private::LayoutGeneratedExpressions(
					nullptr,
					MaterialFunction,
					&FunctionDefinition.Layout,
					GeneratedExpressionsByVariable.IsEmpty() ? nullptr : &GeneratedExpressionsByVariable,
					RegionByVariable.IsEmpty() ? nullptr : &RegionByVariable);
			}
			FunctionSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Updating '%s'..."), *FunctionDefinition.Name)));
			UMaterialEditingLibrary::UpdateMaterialFunction(MaterialFunction, nullptr);
			MaterialFunction->PostEditChange();

			if (bTransient)
			{
				FunctionSlowTask.EnterProgressFrame(1.0f);
				// Modify()/PostEditChange dirtied the in-memory package; clear it so no save-all or
				// exit prompt can silently persist a virtual material function.
				MaterialFunction->GetPackage()->SetDirtyFlag(false);
			}
			else
			{
				MaterialFunction->MarkPackageDirty();
				Private::ApplySourceMetadata(MaterialFunction, SourceFilePath, SourceHash);

				FunctionSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Saving '%s'..."), *FunctionDefinition.Name)));
				FString SaveError;
				if (!Private::SaveAssetPackage(MaterialFunction, SaveError))
				{
					OutError = SaveError;
					return false;
				}
			}

			OutGeneratedAssetPath = MaterialFunction->GetPathName();
			return true;
		}
	}

	bool FMaterialGenerator::GenerateAssetsFromFile(const FString& InSourceFilePath, FString& OutMessage, const bool bForce, const bool bTransient)
	{
		const FString SourceFilePath = UE::DreamShader::NormalizeSourceFilePath(InSourceFilePath);
		FScopedSlowTask SourceSlowTask(
			6.0f,
			FText::FromString(FString::Printf(TEXT("Compiling DreamShader source '%s'..."), *FPaths::GetCleanFilename(SourceFilePath))));
		if (!IsRunningCommandlet())
		{
			SourceSlowTask.MakeDialogDelayed(0.35f);
		}

		SourceSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Reading DreamShader source '%s'..."), *FPaths::GetCleanFilename(SourceFilePath))));
		if (UE::DreamShader::IsDreamShaderHeaderFile(SourceFilePath))
		{
			OutMessage = FString::Printf(TEXT("DreamShader header '%s' does not generate assets directly. Recompile dependent .dsm or .dsf files instead."), *SourceFilePath);
			return false;
		}

		FString SourceText;
		FString PreparedSourceError;
		if (!LoadPreparedDreamShaderSource(SourceFilePath, SourceText, PreparedSourceError))
		{
			OutMessage = PreparedSourceError;
			return false;
		}

		FTextShaderDefinition Definition;
		FString ParseError;
		SourceSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Parsing DreamShader source '%s'..."), *FPaths::GetCleanFilename(SourceFilePath))));
		if (!FTextShaderParser::Parse(SourceText, Definition, ParseError))
		{
			OutMessage = FormatParseErrorWithSourceLocation(SourceFilePath, SourceText, ParseError);
			return false;
		}

		const FString SourceHash = Private::BuildSourceHash(SourceText);

		if (UE::DreamShader::IsDreamShaderFunctionFile(SourceFilePath) && !Definition.Name.IsEmpty())
		{
			OutMessage = FString::Printf(TEXT("%s: .dsf files cannot define top-level Shader blocks."), *SourceFilePath);
			return false;
		}

		bool bGeneratedHelperInclude = false;
		SourceSlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("Preparing DreamShader generated assets...")));
		if (!Definition.Functions.IsEmpty())
		{
			FString IncludeWriteError;
			if (!Private::WriteGeneratedInclude(SourceFilePath, Definition, IncludeWriteError))
			{
				OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *IncludeWriteError);
				return false;
			}

			bGeneratedHelperInclude = true;
		}

		TArray<FString> GeneratedAssetMessages;
		SourceSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Generating %d DreamShader function asset%s..."),
			Definition.MaterialFunctions.Num(),
			Definition.MaterialFunctions.Num() == 1 ? TEXT("") : TEXT("s"))));
		for (const FTextShaderMaterialFunctionDefinition& FunctionDefinition : Definition.MaterialFunctions)
		{
			FString GeneratedAssetPath;
			FString FunctionError;
			if (!GenerateMaterialFunctionAsset(SourceFilePath, SourceText, SourceHash, Definition, FunctionDefinition, bForce, bTransient, GeneratedAssetPath, FunctionError))
			{
				OutMessage = FormatGenerateError(SourceFilePath, FunctionError);
				return false;
			}

			GeneratedAssetMessages.Add(FString::Printf(
				TEXT("Generated %s %s from %s."),
				GetMaterialFunctionBlockKindText(FunctionDefinition.Kind),
				*GeneratedAssetPath,
				*SourceFilePath));
		}

		if (!Definition.Name.IsEmpty())
		{
			FString MaterialMessage;
			SourceSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Generating DreamShader material '%s'..."), *Definition.Name)));
			if (!GenerateMaterialFromFile(SourceFilePath, MaterialMessage, bForce, bTransient))
			{
				OutMessage = MaterialMessage;
				return false;
			}

			GeneratedAssetMessages.Add(MaterialMessage);
		}

		SourceSlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("Finishing DreamShader compile...")));
		if (GeneratedAssetMessages.IsEmpty())
		{
			if (bGeneratedHelperInclude)
			{
				OutMessage = FString::Printf(
					TEXT("Generated DreamShader helper include '%s' from %s."),
					*Private::BuildGeneratedIncludeVirtualPath(SourceFilePath),
					*SourceFilePath);
				return true;
			}

			if (!Definition.VirtualFunctions.IsEmpty())
			{
				OutMessage = FString::Printf(TEXT("DreamShader file '%s' contains VirtualFunction declarations only; no assets were generated."), *SourceFilePath);
				return true;
			}

			if (!Definition.GraphFunctions.IsEmpty())
			{
				OutMessage = FString::Printf(TEXT("DreamShader file '%s' contains GraphFunction declarations only; no assets were generated."), *SourceFilePath);
				return true;
			}

			OutMessage = FString::Printf(TEXT("DreamShader file '%s' did not contain any material, ShaderFunction, ShaderLayer, or ShaderLayerBlend assets to generate."), *SourceFilePath);
			return false;
		}

		OutMessage = FString::Join(GeneratedAssetMessages, TEXT("\n"));
		if (!Definition.Warnings.IsEmpty())
		{
			OutMessage += TEXT("\nWarnings:\n");
			OutMessage += FString::Join(Definition.Warnings, TEXT("\n"));
		}
		return true;
	}

	namespace Private
	{
		static const TCHAR* GInstanceHostPackageName = TEXT("/DreamShader/Host/M_DreamShaderHost");

		static FString GetHlslTypeForCustomOutput(const ECustomMaterialOutputType OutputType)
		{
			switch (OutputType)
			{
			case CMOT_Float1: return TEXT("float");
			case CMOT_Float2: return TEXT("float2");
			case CMOT_Float3: return TEXT("float3");
			default:          return TEXT("float4");
			}
		}

		struct FInstanceBackendOutput
		{
			FDreamShaderInstanceOutput Output;
			FString BindingSource;
		};

		struct FInstanceBackendModel
		{
			TArray<FDreamShaderInstanceParameter> Parameters;
			FString ConstDeclarations;
			TArray<FInstanceBackendOutput> Outputs;
			FString LoweredCode;
			int32 UsedTexCoordCount = 0;
			bool bUsesVertexColor = false;
		};

		// Rewrite the UE.* builtins the instance backend supports into their DS_* HLSL equivalents
		// (see Shaders/DreamShaderBuiltins.ush), recording which translator side effects the resource
		// must trigger. Unsupported UE.*/Substrate.* calls are left in place for the residual check.
		static bool LowerInstanceBuiltins(
			const FString& InCode,
			FString& OutCode,
			int32& InOutUsedTexCoordCount,
			bool& bInOutUsesVertexColor,
			FString& OutError)
		{
			OutCode.Reset(InCode.Len());
			int32 Index = 0;
			while (Index < InCode.Len())
			{
				const int32 CallIndex = InCode.Find(TEXT("UE."), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
				if (CallIndex == INDEX_NONE)
				{
					OutCode += InCode.Mid(Index);
					break;
				}

				// Token boundary: "UE" must not be the tail of a longer identifier (e.g. VALUE.x).
				if (CallIndex > 0 && (FChar::IsAlnum(InCode[CallIndex - 1]) || InCode[CallIndex - 1] == TCHAR('_')))
				{
					OutCode += InCode.Mid(Index, CallIndex + 3 - Index);
					Index = CallIndex + 3;
					continue;
				}

				// Read the member name and the balanced argument list.
				int32 Cursor = CallIndex + 3;
				FString MemberName;
				while (Cursor < InCode.Len() && (FChar::IsAlnum(InCode[Cursor]) || InCode[Cursor] == TCHAR('_')))
				{
					MemberName.AppendChar(InCode[Cursor]);
					++Cursor;
				}
				while (Cursor < InCode.Len() && FChar::IsWhitespace(InCode[Cursor]))
				{
					++Cursor;
				}
				if (MemberName.IsEmpty() || Cursor >= InCode.Len() || InCode[Cursor] != TCHAR('('))
				{
					OutCode += InCode.Mid(Index, Cursor - Index);
					Index = Cursor;
					continue;
				}

				int32 Depth = 0;
				int32 CloseIndex = INDEX_NONE;
				for (int32 Scan = Cursor; Scan < InCode.Len(); ++Scan)
				{
					const TCHAR Char = InCode[Scan];
					if (Char == TCHAR('('))
					{
						++Depth;
					}
					else if (Char == TCHAR(')'))
					{
						--Depth;
						if (Depth == 0)
						{
							CloseIndex = Scan;
							break;
						}
					}
				}
				if (CloseIndex == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("Unbalanced parentheses in UE.%s(...) call."), *MemberName);
					return false;
				}

				FString Arguments = InCode.Mid(Cursor + 1, CloseIndex - Cursor - 1).TrimStartAndEnd();
				const auto StripNamedArgument = [&Arguments](const TCHAR* ArgumentName)
				{
					const int32 NameLength = FCString::Strlen(ArgumentName);
					if (Arguments.StartsWith(ArgumentName, ESearchCase::IgnoreCase)
						&& (Arguments.Len() == NameLength || !FChar::IsAlnum(Arguments[NameLength])))
					{
						const int32 EqualsIndex = Arguments.Find(TEXT("="));
						if (EqualsIndex != INDEX_NONE)
						{
							Arguments = Arguments.Mid(EqualsIndex + 1).TrimStartAndEnd();
						}
					}
				};

				FString Replacement;
				if (MemberName.Equals(TEXT("Time"), ESearchCase::IgnoreCase))
				{
					if (Arguments.IsEmpty())
					{
						Replacement = TEXT("DS_TIME");
					}
					else
					{
						StripNamedArgument(TEXT("Period"));
						Replacement = FString::Printf(TEXT("DS_PERIODIC_TIME(%s)"), *Arguments);
					}
				}
				else if (MemberName.Equals(TEXT("TexCoord"), ESearchCase::IgnoreCase))
				{
					StripNamedArgument(TEXT("Index"));
					int32 CoordinateIndex = 0;
					if (!Arguments.IsEmpty())
					{
						if (!Arguments.IsNumeric())
						{
							OutError = FString::Printf(TEXT("Backend=\"Instance\" requires a literal integer index in UE.TexCoord(...); got '%s'."), *Arguments);
							return false;
						}
						CoordinateIndex = FCString::Atoi(*Arguments);
					}
					InOutUsedTexCoordCount = FMath::Max(InOutUsedTexCoordCount, CoordinateIndex + 1);
					Replacement = FString::Printf(TEXT("DS_TexCoord(Parameters, %d)"), CoordinateIndex);
				}
				else if (MemberName.Equals(TEXT("VertexColor"), ESearchCase::IgnoreCase))
				{
					bInOutUsesVertexColor = true;
					Replacement = TEXT("DS_VertexColor(Parameters)");
				}
				else
				{
					// Not a supported builtin — copy through; the residual UE.* check reports it.
					OutCode += InCode.Mid(Index, CloseIndex + 1 - Index);
					Index = CloseIndex + 1;
					continue;
				}

				OutCode += InCode.Mid(Index, CallIndex - Index);
				OutCode += Replacement;
				Index = CloseIndex + 1;
			}

			return true;
		}

		// Decide whether the file should use the instance backend: an explicit
		// Settings = { Backend = "..." } wins; otherwise the project's DefaultBackend applies
		// (with automatic Graph fallback for files the instance backend can't express).
		static bool ResolveInstanceBackendRequested(const FTextShaderDefinition& Definition, bool& bOutInstanceBackend, bool& bOutExplicit, FString& OutError)
		{
			bOutInstanceBackend = false;
			bOutExplicit = false;
			FString Value;
			if (!Definition.TryGetSetting(TEXT("Backend"), Value))
			{
				const UDreamShaderSettings* Settings = GetDefault<UDreamShaderSettings>();
				bOutInstanceBackend = Settings && Settings->DefaultBackend == EDreamShaderDefaultBackend::Instance;
				return true;
			}

			bOutExplicit = true;
			const FString Trimmed = Value.TrimStartAndEnd().TrimQuotes();
			if (Trimmed.Equals(TEXT("Instance"), ESearchCase::IgnoreCase))
			{
				bOutInstanceBackend = true;
				return true;
			}
			if (Trimmed.Equals(TEXT("Graph"), ESearchCase::IgnoreCase) || Trimmed.IsEmpty())
			{
				return true;
			}

			OutError = FString::Printf(TEXT("Unsupported Backend '%s'. Supported values: Graph, Instance."), *Value);
			return false;
		}

		// Gather DSL properties/outputs into the instance model. The instance backend covers the
		// pure-HLSL subset of the DSL: scalar/vector parameters and consts, HLSL Graph code, and
		// direct Base.<Property> bindings. Everything translator-level is rejected with a clear error.
		static bool BuildInstanceBackendModel(
			const FTextShaderDefinition& Definition,
			FInstanceBackendModel& OutModel,
			FString& OutError)
		{
			if (!Definition.GraphFunctions.IsEmpty() || !Definition.MaterialFunctions.IsEmpty() || !Definition.VirtualFunctions.IsEmpty())
			{
				OutError = TEXT("Backend=\"Instance\" supports plain HLSL Functions only; GraphFunction / ShaderFunction / VirtualFunction blocks need the Graph backend.");
				return false;
			}

			// Lower supported UE.* builtins to their DS_* equivalents first; whatever survives is a
			// translator-level graph node the raw-HLSL backend genuinely cannot express.
			if (!LowerInstanceBuiltins(Definition.Code, OutModel.LoweredCode, OutModel.UsedTexCoordCount, OutModel.bUsesVertexColor, OutError))
			{
				return false;
			}
			if (OutModel.LoweredCode.Contains(TEXT("UE.")) || OutModel.LoweredCode.Contains(TEXT("Substrate.")))
			{
				OutError = TEXT("Backend=\"Instance\" Graph code must be pure HLSL; only the UE.Time/UE.TexCoord/UE.VertexColor builtins are lowered — other UE.*/Substrate.* graph nodes need the Graph backend.");
				return false;
			}

			FString DomainValue;
			if (Definition.TryGetSetting(TEXT("Domain"), DomainValue) || Definition.TryGetSetting(TEXT("MaterialDomain"), DomainValue))
			{
				if (!DomainValue.TrimStartAndEnd().TrimQuotes().Equals(TEXT("Surface"), ESearchCase::IgnoreCase))
				{
					OutError = FString::Printf(TEXT("Backend=\"Instance\" only supports Domain=\"Surface\" (the shared host material's domain); got '%s'."), *DomainValue);
					return false;
				}
			}

			for (const FTextShaderPropertyDefinition& Property : Definition.Properties)
			{
				if (Property.Source != ETextShaderPropertySource::Parameter)
				{
					OutError = FString::Printf(TEXT("Backend=\"Instance\" does not support UE builtin property '%s'."), *Property.Name);
					return false;
				}

				switch (Property.Type)
				{
				case ETextShaderPropertyType::Scalar:
					if (Property.bConst)
					{
						OutModel.ConstDeclarations += FString::Printf(TEXT("static const float %s = %f;\n"), *Property.Name, Property.ScalarDefaultValue);
					}
					else
					{
						FDreamShaderInstanceParameter Parameter;
						Parameter.Name = FName(*Property.Name);
						Parameter.Type = EDreamShaderInstanceParameterType::Scalar;
						Parameter.ScalarDefault = static_cast<float>(Property.ScalarDefaultValue);
						OutModel.Parameters.Add(MoveTemp(Parameter));
					}
					break;

				case ETextShaderPropertyType::Vector:
					if (Property.bConst)
					{
						const FLinearColor& Value = Property.VectorDefaultValue;
						OutModel.ConstDeclarations += FString::Printf(
							TEXT("static const float4 %s = float4(%f, %f, %f, %f);\n"),
							*Property.Name, Value.R, Value.G, Value.B, Value.A);
					}
					else
					{
						FDreamShaderInstanceParameter Parameter;
						Parameter.Name = FName(*Property.Name);
						Parameter.Type = EDreamShaderInstanceParameterType::Vector;
						Parameter.VectorDefault = Property.VectorDefaultValue;
						OutModel.Parameters.Add(MoveTemp(Parameter));
					}
					break;

				default:
					OutError = FString::Printf(TEXT("Backend=\"Instance\" does not support texture property '%s' yet."), *Property.Name);
					return false;
				}
			}

			for (const FTextShaderOutputBinding& Binding : Definition.Outputs)
			{
				if (Binding.TargetKind != FTextShaderOutputBinding::ETargetKind::MaterialProperty)
				{
					OutError = TEXT("Backend=\"Instance\" only supports direct Base.<Property> output bindings.");
					return false;
				}

				FResolvedMaterialProperty Resolved;
				if (!ResolveMaterialProperty(Binding.MaterialProperty, Resolved))
				{
					OutError = FString::Printf(TEXT("Unknown material output '%s'."), *Binding.MaterialProperty);
					return false;
				}
				if (Resolved.bIsSubstrateMaterial || Resolved.OutputType == CMOT_MaterialAttributes)
				{
					OutError = FString::Printf(TEXT("Backend=\"Instance\" does not support the '%s' output (Substrate/MaterialAttributes are translator-level)."), *Binding.MaterialProperty);
					return false;
				}
				if (FMaterialAttributeDefinitionMap::GetShaderFrequency(Resolved.Property) != SF_Pixel)
				{
					// The eval functions receive FMaterialPixelParameters; vertex-frequency outputs
					// (WorldPositionOffset) would be emitted into an FMaterialVertexParameters custom.
					OutError = FString::Printf(TEXT("Backend=\"Instance\" only supports pixel-frequency outputs; '%s' is vertex-frequency."), *Binding.MaterialProperty);
					return false;
				}

				FInstanceBackendOutput Output;
				Output.Output.Property = Resolved.Property;
				Output.Output.EvalFunctionName = FString::Printf(TEXT("DreamShaderEval_%s"), *Binding.MaterialProperty);
				Output.Output.OutputType = Resolved.OutputType;
				if (!LowerInstanceBuiltins(Binding.SourceText, Output.BindingSource, OutModel.UsedTexCoordCount, OutModel.bUsesVertexColor, OutError))
				{
					return false;
				}
				OutModel.Outputs.Add(MoveTemp(Output));
			}

			if (OutModel.Outputs.IsEmpty())
			{
				OutError = TEXT("Backend=\"Instance\" requires at least one Base.<Property> output binding.");
				return false;
			}

			return true;
		}

		// Emit the instance's .ush: one eval function per bound material property, all sharing the
		// DSL parameters as arguments, plus the const declarations. Writes into the generated-shader
		// directory that DreamShaderModule maps as a virtual shader source root.
		static bool WriteInstanceGeneratedInclude(
			const FString& SourceFilePath,
			const FTextShaderDefinition& Definition,
			const FInstanceBackendModel& Model,
			FString& OutVirtualPath,
			FString& OutError)
		{
			const FString NormalizedCode = NormalizeShaderLanguageText(Model.LoweredCode);

			// Every eval function receives the translator's pixel parameters first (the injected
			// Custom body forwards its own 'Parameters'), then the DSL parameters.
			FString ParameterList = TEXT("FMaterialPixelParameters Parameters");
			for (const FDreamShaderInstanceParameter& Parameter : Model.Parameters)
			{
				ParameterList += FString::Printf(
					TEXT(", %s %s"),
					Parameter.Type == EDreamShaderInstanceParameterType::Scalar ? TEXT("float") : TEXT("float4"),
					*Parameter.Name.ToString());
			}

			FString Declarations;
			for (const FTextShaderVariableDeclaration& Declaration : Definition.OutputDeclarations)
			{
				const FString HlslType = NormalizeShaderTypeToken(Declaration.Type);
				const FString DefaultValue = Declaration.bHasDefaultValue
					? NormalizeShaderLanguageText(Declaration.DefaultValueText)
					: FString::Printf(TEXT("(%s)0"), *HlslType);
				Declarations += FString::Printf(TEXT("	%s %s = %s;\n"), *HlslType, *Declaration.Name, *DefaultValue);
			}

			const uint32 PathHash = GetTypeHash(UE::DreamShader::NormalizeSourceFilePath(SourceFilePath));
			const FString FileName = FString::Printf(TEXT("DSI_%s_%08x.ush"), *FPaths::GetBaseFilename(SourceFilePath), PathHash);
			OutVirtualPath = UE::DreamShader::GetGeneratedShaderVirtualDirectory() / FileName;

			// Imported/declared HLSL functions live in the (already written) functions include; alias
			// their DSL names to the collision-safe generated symbols so Graph code can call them
			// naturally. The defines are scoped inside this file's guard and undefined at the end so
			// they cannot leak into the rest of the material translation unit.
			FString FunctionIncludes = TEXT("#include \"/Plugin/DreamShader/DreamShaderBuiltins.ush\"\n");
			FString FunctionAliases;
			FString FunctionAliasUndefs;
			if (!Definition.Functions.IsEmpty())
			{
				FunctionIncludes += FString::Printf(TEXT("#include \"%s\"\n"), *BuildGeneratedIncludeVirtualPath(SourceFilePath));

				const auto IsPlainIdentifier = [](const FString& Name)
				{
					if (Name.IsEmpty() || (!FChar::IsAlpha(Name[0]) && Name[0] != TCHAR('_')))
					{
						return false;
					}
					for (const TCHAR Char : Name)
					{
						if (!FChar::IsAlnum(Char) && Char != TCHAR('_'))
						{
							return false;
						}
					}
					return true;
				};

				for (const FTextShaderFunctionDefinition& Function : Definition.Functions)
				{
					const FString Symbol = BuildGeneratedFunctionSymbolName(Function);
					if (IsPlainIdentifier(Function.Name) && Function.Name != Symbol)
					{
						FunctionAliases += FString::Printf(TEXT("#define %s %s\n"), *Function.Name, *Symbol);
						FunctionAliasUndefs += FString::Printf(TEXT("#undef %s\n"), *Function.Name);
					}
					else
					{
						// Namespaced names can't be macro aliases; call the symbol directly.
						FunctionAliases += FString::Printf(TEXT("// %s -> call as %s\n"), *Function.Name, *Symbol);
					}
				}
			}

			FString Content;
			Content += FString::Printf(TEXT("// Generated by DreamShader (instance backend) from %s -- do not edit.\n"), *FPaths::GetCleanFilename(SourceFilePath));
			const FString GuardMacro = FString::Printf(TEXT("DREAMSHADER_INSTANCE_%08X"), PathHash);
			Content += FString::Printf(TEXT("#ifndef %s\n#define %s\n\n"), *GuardMacro, *GuardMacro);
			Content += FunctionIncludes;
			Content += FunctionAliases;
			if (!FunctionIncludes.IsEmpty() || !FunctionAliases.IsEmpty())
			{
				Content += TEXT("\n");
			}
			if (!Model.ConstDeclarations.IsEmpty())
			{
				Content += Model.ConstDeclarations;
				Content += TEXT("\n");
			}

			for (const FInstanceBackendOutput& Output : Model.Outputs)
			{
				Content += FString::Printf(
					TEXT("%s %s(%s)\n{\n%s%s\n	return (%s);\n}\n\n"),
					*GetHlslTypeForCustomOutput(Output.Output.OutputType),
					*Output.Output.EvalFunctionName,
					*ParameterList,
					*Declarations,
					*NormalizedCode,
					*NormalizeShaderLanguageText(Output.BindingSource));
			}

			Content += FunctionAliasUndefs;
			Content += FString::Printf(TEXT("#endif // %s\n"), *GuardMacro);

			const FString DiskPath = UE::DreamShader::GetGeneratedShaderDirectory() / FileName;
			if (!FFileHelper::SaveStringToFile(Content, *DiskPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = FString::Printf(TEXT("Failed to write generated instance shader '%s'."), *DiskPath);
				return false;
			}

			return true;
		}

		// The single committed parent material every instance material derives from. Created on first
		// use and saved into the plugin's Content mount; its graph stays empty because the instance
		// resource overrides property compilation for every DSL-bound output.
		static UMaterial* EnsureInstanceHostMaterial(FString& OutError)
		{
			const FString HostObjectPath = FString::Printf(TEXT("%s.M_DreamShaderHost"), GInstanceHostPackageName);
			if (UMaterial* Existing = LoadObject<UMaterial>(nullptr, *HostObjectPath))
			{
				return Existing;
			}

			if (!FPackageName::MountPointExists(TEXT("/DreamShader/")))
			{
				const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamShader"));
				if (!Plugin)
				{
					OutError = TEXT("DreamShader plugin descriptor not found; cannot mount plugin content for the instance host material.");
					return nullptr;
				}

				const FString ContentDirectory = Plugin->GetContentDir();
				IFileManager::Get().MakeDirectory(*ContentDirectory, true);
				FPackageName::RegisterMountPoint(TEXT("/DreamShader/"), ContentDirectory);
			}

			UPackage* HostPackage = CreatePackage(GInstanceHostPackageName);
			if (!HostPackage)
			{
				OutError = FString::Printf(TEXT("Failed to create package '%s'."), GInstanceHostPackageName);
				return nullptr;
			}

			UMaterial* Host = NewObject<UMaterial>(HostPackage, TEXT("M_DreamShaderHost"), RF_Public | RF_Standalone);
			if (!Host)
			{
				OutError = TEXT("Failed to create the DreamShader instance host material.");
				return nullptr;
			}

			Host->PostEditChange();
			FAssetRegistryModule::AssetCreated(Host);

			// SaveAssetPackage only saves dirty packages; a freshly NewObject'd asset isn't dirty yet.
			Host->MarkPackageDirty();

			FString SaveError;
			if (!SaveAssetPackage(Host, SaveError))
			{
				// The host must persist: saved maps reference instances whose parent chain resolves
				// through this asset on every future session and at cook.
				OutError = FString::Printf(TEXT("Failed to save the instance host material: %s"), *SaveError);
				return nullptr;
			}

			return Host;
		}

		static bool GenerateInstanceMaterialFromDefinition(
			const FString& SourceFilePath,
			const FString& SourceHash,
			const FTextShaderDefinition& Definition,
			const FInstanceBackendModel& Model,
			FString& OutMessage,
			const bool bForce,
			const bool bTransient)
		{
			FString IncludeVirtualPath;
			FString IncludeError;
			if (!WriteInstanceGeneratedInclude(SourceFilePath, Definition, Model, IncludeVirtualPath, IncludeError))
			{
				OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *IncludeError);
				return false;
			}

			FString HostError;
			UMaterial* HostMaterial = EnsureInstanceHostMaterial(HostError);
			if (!HostMaterial)
			{
				OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *HostError);
				return false;
			}

			UDreamShaderMaterialInstance* Instance = nullptr;
			FString InstanceError;
			if (!CreateOrReuseInstanceMaterial(Definition, Instance, InstanceError, bTransient) || !Instance)
			{
				OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *InstanceError);
				return false;
			}

			if (!bForce && IsGeneratedAssetSourceCurrent(Instance, SourceFilePath, SourceHash))
			{
				OutMessage = FString::Printf(TEXT("Skipped %s from %s; source hash is unchanged."), *Instance->GetPathName(), *SourceFilePath);
				return true;
			}

			// SetParentEditorOnly with deferred recache: the single shader recache happens in
			// UpdateStaticPermutation below, after everything the translation consumes is in place.
			Instance->SetParentEditorOnly(HostMaterial, /*RecacheShader*/ false);
			Instance->ClearParameterValuesEditorOnly();
			Instance->SourceFilePath = SourceFilePath;
			Instance->SourceHash = SourceHash;
			Instance->GeneratedIncludeVirtualPath = IncludeVirtualPath;
			Instance->InstanceParameters = Model.Parameters;
			Instance->UsedTexCoordCount = Model.UsedTexCoordCount;
			Instance->bUsesVertexColorBuiltin = Model.bUsesVertexColor;
			Instance->InstanceOutputs.Reset(Model.Outputs.Num());
			for (const FInstanceBackendOutput& Output : Model.Outputs)
			{
				Instance->InstanceOutputs.Add(Output.Output);
			}

#if WITH_EDITORONLY_DATA
			Instance->EvalExpressions.Reset(Model.Outputs.Num());
			// The Custom body forwards its own 'Parameters' (in scope inside CustomExpressionN)
			// followed by the DSL parameters by name.
			FString ArgumentList = TEXT("Parameters");
			for (const FDreamShaderInstanceParameter& Parameter : Model.Parameters)
			{
				ArgumentList += TEXT(", ");
				ArgumentList += Parameter.Name.ToString();
			}
			for (const FInstanceBackendOutput& Output : Model.Outputs)
			{
				UMaterialExpressionCustom* EvalExpression = NewObject<UMaterialExpressionCustom>(Instance);
				EvalExpression->Inputs.Reset();
				for (const FDreamShaderInstanceParameter& Parameter : Model.Parameters)
				{
					FCustomInput Input;
					Input.InputName = Parameter.Name;
					EvalExpression->Inputs.Add(MoveTemp(Input));
				}
				// Dummy side-effect inputs, index-aligned with the chunks the resource compiles
				// (translator checks Inputs.Num() == CompiledInputs.Num()); their values are unused.
				for (int32 CoordinateIndex = 0; CoordinateIndex < Model.UsedTexCoordCount; ++CoordinateIndex)
				{
					FCustomInput Input;
					Input.InputName = FName(*FString::Printf(TEXT("DreamShaderUnusedTexCoord%d"), CoordinateIndex));
					EvalExpression->Inputs.Add(MoveTemp(Input));
				}
				if (Model.bUsesVertexColor)
				{
					FCustomInput Input;
					Input.InputName = TEXT("DreamShaderUnusedVertexColor");
					EvalExpression->Inputs.Add(MoveTemp(Input));
				}
				EvalExpression->OutputType = Output.Output.OutputType;
				EvalExpression->Code = FString::Printf(TEXT("return %s(%s);"), *Output.Output.EvalFunctionName, *ArgumentList);
				EvalExpression->IncludeFilePaths = { IncludeVirtualPath };
				EvalExpression->Description = FString::Printf(TEXT("DreamShader %s"), *Output.Output.EvalFunctionName);
				Instance->EvalExpressions.Add(EvalExpression);
			}
#endif

			for (const FDreamShaderInstanceParameter& Parameter : Model.Parameters)
			{
				if (Parameter.Type == EDreamShaderInstanceParameterType::Scalar)
				{
					Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Parameter.Name), Parameter.ScalarDefault);
				}
				else
				{
					Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(Parameter.Name), Parameter.VectorDefault);
				}
			}

			FString SettingValue;
			if (Definition.TryGetSetting(TEXT("BlendMode"), SettingValue) || Definition.TryGetSetting(TEXT("RenderType"), SettingValue))
			{
				EBlendMode BlendMode = BLEND_Opaque;
				if (TryResolveBlendModeSetting(SettingValue, BlendMode))
				{
					Instance->BasePropertyOverrides.bOverride_BlendMode = true;
					Instance->BasePropertyOverrides.BlendMode = BlendMode;
				}
			}
			if (Definition.TryGetSetting(TEXT("ShadingModel"), SettingValue))
			{
				EMaterialShadingModel ShadingModel = MSM_DefaultLit;
				if (TryResolveShadingModelSetting(SettingValue, ShadingModel))
				{
					Instance->BasePropertyOverrides.bOverride_ShadingModel = true;
					Instance->BasePropertyOverrides.ShadingModel = ShadingModel;
				}
			}

			Instance->UpdateStaticPermutation();
			Instance->PostEditChange();
			ApplySourceMetadata(Instance, SourceFilePath, SourceHash);

			if (bTransient)
			{
				// The editor-only setters and PostEditChange dirtied the in-memory package; clear it
				// so no save-all or exit prompt can silently persist a virtual instance material.
				Instance->GetPackage()->SetDirtyFlag(false);
			}
			else
			{
				// SaveAssetPackage only saves dirty packages; mark explicitly so a freshly created
				// (or hash-skipped-then-forced) instance actually reaches disk.
				Instance->MarkPackageDirty();

				FString SaveError;
				if (!SaveAssetPackage(Instance, SaveError))
				{
					OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *SaveError);
					return false;
				}
			}

			OutMessage = FString::Printf(
				TEXT("Generated DreamShader instance material %s from %s (%d output%s, %d parameter%s)."),
				*Instance->GetPathName(), *SourceFilePath,
				Model.Outputs.Num(), Model.Outputs.Num() == 1 ? TEXT("") : TEXT("s"),
				Model.Parameters.Num(), Model.Parameters.Num() == 1 ? TEXT("") : TEXT("s"));
			return true;
		}
	}

	bool FMaterialGenerator::GenerateMaterialFromFile(const FString& InSourceFilePath, FString& OutMessage, const bool bForce, const bool bTransient)
	{
		const FString SourceFilePath = UE::DreamShader::NormalizeSourceFilePath(InSourceFilePath);
		FScopedSlowTask MaterialSlowTask(
			11.0f,
			FText::FromString(FString::Printf(TEXT("Generating DreamShader material from '%s'..."), *FPaths::GetCleanFilename(SourceFilePath))));
		if (!IsRunningCommandlet())
		{
			MaterialSlowTask.MakeDialogDelayed(0.25f);
		}

		MaterialSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Reading material source '%s'..."), *FPaths::GetCleanFilename(SourceFilePath))));
		if (UE::DreamShader::IsDreamShaderHeaderFile(SourceFilePath) || UE::DreamShader::IsDreamShaderFunctionFile(SourceFilePath))
		{
			OutMessage = FString::Printf(TEXT("DreamShader source '%s' cannot generate a material asset directly."), *SourceFilePath);
			return false;
		}

		FString SourceText;
		FString PreparedSourceError;
		if (!LoadPreparedDreamShaderSource(SourceFilePath, SourceText, PreparedSourceError))
		{
			OutMessage = PreparedSourceError;
			return false;
		}

		FTextShaderDefinition Definition;
		FString ParseError;
		MaterialSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Parsing material source '%s'..."), *FPaths::GetCleanFilename(SourceFilePath))));
		if (!FTextShaderParser::Parse(SourceText, Definition, ParseError))
		{
			OutMessage = FormatParseErrorWithSourceLocation(SourceFilePath, SourceText, ParseError);
			return false;
		}

		const FString SourceHash = Private::BuildSourceHash(SourceText);

		if (Definition.Name.IsEmpty())
		{
			OutMessage = FString::Printf(TEXT("%s: This file does not define a top-level Shader block."), *SourceFilePath);
			return false;
		}

		if (Definition.Outputs.IsEmpty())
		{
			OutMessage = FString::Printf(TEXT("%s: Outputs block is required."), *SourceFilePath);
			return false;
		}

		TArray<Private::FResolvedNamedOutput> NamedOutputs;
		bool bUsesReturn = false;
		ECustomMaterialOutputType ReturnOutputType = CMOT_Float1;
		bool bReturnIsSubstrateMaterial = false;
		FString ValidationError;
		if (!Private::ValidateSettings(Definition, ValidationError)
			|| !Private::ValidateOutputs(Definition, NamedOutputs, bUsesReturn, ReturnOutputType, bReturnIsSubstrateMaterial, ValidationError))
		{
			OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *ValidationError);
			return false;
		}

		bool bUsesFrontMaterial = false;
		bool bUsesMaterialAttributesOutput = false;
		for (const FTextShaderOutputBinding& Binding : Definition.Outputs)
		{
			if (Binding.TargetKind != FTextShaderOutputBinding::ETargetKind::MaterialProperty)
			{
				continue;
			}

			Private::FResolvedMaterialProperty ResolvedProperty;
			if (!Private::ResolveMaterialProperty(Binding.MaterialProperty, ResolvedProperty))
			{
				continue;
			}

			bUsesFrontMaterial |= ResolvedProperty.bIsSubstrateMaterial;
			bUsesMaterialAttributesOutput |= ResolvedProperty.OutputType == CMOT_MaterialAttributes;
		}
		if (bUsesFrontMaterial && bUsesMaterialAttributesOutput)
		{
			OutMessage = FString::Printf(TEXT("%s: Base.FrontMaterial and Base.MaterialAttributes cannot be used by the same Shader."), *SourceFilePath);
			return false;
		}

		// Both backends consume the imported-functions include, so it is written before the
		// backend split.
		if (!Definition.Functions.IsEmpty())
		{
			FString IncludeWriteError;
			if (!Private::WriteGeneratedInclude(SourceFilePath, Definition, IncludeWriteError))
			{
				OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *IncludeWriteError);
				return false;
			}
		}

		bool bInstanceBackend = false;
		bool bExplicitBackend = false;
		FString BackendError;
		if (!Private::ResolveInstanceBackendRequested(Definition, bInstanceBackend, bExplicitBackend, BackendError))
		{
			OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *BackendError);
			return false;
		}
		if (bInstanceBackend)
		{
			Private::FInstanceBackendModel InstanceModel;
			FString CapabilityError;
			bool bCapable = !bUsesReturn;
			if (!bCapable)
			{
				CapabilityError = TEXT("Backend=\"Instance\" requires named Outputs with Base.<Property> bindings (return-style Outputs are not supported).");
			}
			else
			{
				bCapable = Private::BuildInstanceBackendModel(Definition, InstanceModel, CapabilityError);
			}

			if (bCapable)
			{
				MaterialSlowTask.EnterProgressFrame(8.0f, FText::FromString(FString::Printf(TEXT("Generating instance material for '%s'..."), *Definition.Name)));
				return Private::GenerateInstanceMaterialFromDefinition(
					SourceFilePath, SourceHash, Definition, InstanceModel,
					OutMessage, bForce, bTransient);
			}

			if (bExplicitBackend)
			{
				OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *CapabilityError);
				return false;
			}

			// DefaultBackend=Instance is a preference, not a mandate: files that need graph-only
			// features keep working through the Graph backend, with a note explaining why.
			UE_LOG(LogDreamShader, Display,
				TEXT("DreamShader: '%s' uses the Graph backend (Instance default not applicable: %s)"),
				*SourceFilePath, *CapabilityError);
		}

		UMaterial* Material = nullptr;
		FString MaterialError;
		MaterialSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Preparing material asset '%s'..."), *Definition.Name)));
		if (!Private::CreateOrReuseMaterial(Definition, Material, MaterialError, bTransient) || !Material)
		{
			OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *MaterialError);
			return false;
		}

		if (!bForce && Private::IsGeneratedAssetSourceCurrent(Material, SourceFilePath, SourceHash))
		{
			OutMessage = FString::Printf(TEXT("Skipped %s from %s; source hash is unchanged."), *Material->GetPathName(), *SourceFilePath);
			return true;
		}

		Material->Modify();
		MaterialSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Clearing old material graph '%s'..."), *Material->GetName())));
		Private::ClearMaterialExpressions(Material);
		Private::ResetMaterialToDefaults(Material);

		FString SettingsError;
		if (!Private::ApplySettings(Material, Definition, SettingsError))
		{
			OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *SettingsError);
			return false;
		}
		if (bUsesFrontMaterial)
		{
			FString ShadingModelValue;
			if (Definition.TryGetSetting(TEXT("ShadingModel"), ShadingModelValue)
				&& !ShadingModelValue.TrimStartAndEnd().Equals(TEXT("Substrate"), ESearchCase::IgnoreCase)
				&& !ShadingModelValue.TrimStartAndEnd().Equals(TEXT("Strata"), ESearchCase::IgnoreCase))
			{
				OutMessage = FString::Printf(TEXT("%s: Base.FrontMaterial requires ShadingModel=\"Substrate\" or no explicit ShadingModel setting."), *SourceFilePath);
				return false;
			}

			Material->SetShadingModel(MSM_Strata);
		}

		TMap<FString, UMaterialExpression*> GeneratedOutputTargetExpressions;
		TSet<FString> BoundOutputTargetPins;
		TMap<FString, Private::FCodeValue> GeneratedCodeValues;
		TMap<FString, UMaterialExpression*> GeneratedExpressionsByVariable;
		TMap<FString, FString> RegionByVariable;
		int32 OutputTargetPositionY = 200;
		TSet<FString> SeenPropertyNames;
		for (const FTextShaderPropertyDefinition& Property : Definition.Properties)
		{
			bool bNameConflict = false;
			for (const FString& ExistingPropertyName : SeenPropertyNames)
			{
				if (ExistingPropertyName.Equals(Property.Name, ESearchCase::IgnoreCase))
				{
					bNameConflict = true;
					break;
				}
			}

			if (bNameConflict)
			{
				OutMessage = FString::Printf(
					TEXT("%s: Property '%s' is declared more than once. Property names must be unique."),
					*SourceFilePath,
					*Property.Name);
				return false;
			}

			SeenPropertyNames.Add(Property.Name);
		}

		int32 MaterialAttributesSeedPositionY = OutputTargetPositionY;
		for (const FTextShaderVariableDeclaration& OutputDeclaration : Definition.OutputDeclarations)
		{
			if (!OutputDeclaration.bHasDefaultValue && Private::IsMaterialAttributesType(OutputDeclaration.Type))
			{
				FString SeedError;
				if (!SeedMaterialAttributesGraphValue(
					Material,
					nullptr,
					OutputDeclaration.Name,
					GeneratedCodeValues,
					MaterialAttributesSeedPositionY,
					SeedError))
				{
					OutMessage = FString::Printf(TEXT("%s: Output '%s': %s"), *SourceFilePath, *OutputDeclaration.Name, *SeedError);
					return false;
				}
			}
		}
		OutputTargetPositionY = FMath::Max(OutputTargetPositionY, MaterialAttributesSeedPositionY);

		bool bHasInitializedOutput = false;
		for (const FTextShaderVariableDeclaration& OutputDeclaration : Definition.OutputDeclarations)
		{
			if (OutputDeclaration.bHasDefaultValue)
			{
				bHasInitializedOutput = true;
				break;
			}
		}

		if (!Definition.Code.IsEmpty() || bHasInitializedOutput)
		{
			if (bUsesReturn)
			{
				OutMessage = FString::Printf(TEXT("%s: Graph blocks do not support binding Outputs to the reserved name 'return'."), *SourceFilePath);
				return false;
			}

			TArray<Private::FCodeStatement> CodeStatements;
			FString CodeParseError;
			MaterialSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Parsing Graph block for '%s'..."), *Definition.Name)));
			if (!AppendInitializedOutputStatements(Definition.OutputDeclarations, CodeStatements, CodeParseError))
			{
				OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *CodeParseError);
				return false;
			}

			FString CodeSourceFilePath;
			int32 CodeStartLine = 1;
			int32 CodeStartColumn = 1;
			ResolveCodeBlockLocation(SourceFilePath, SourceText, Definition.CodeStartIndex, CodeSourceFilePath, CodeStartLine, CodeStartColumn);
			TArray<Private::FCodeStatement> GraphStatements;
			int32 CodeParseErrorLine = 0;
			int32 CodeParseErrorColumn = 0;
			if (!Definition.Code.IsEmpty() && !Private::ParseCodeStatements(Definition.Code, GraphStatements, CodeParseError, &CodeParseErrorLine, &CodeParseErrorColumn))
			{
				OutMessage = FormatCodeBlockError(
					SourceFilePath,
					CodeSourceFilePath,
					CodeStartLine,
					CodeStartColumn,
					CodeParseError,
					CodeParseErrorLine,
					CodeParseErrorColumn);
				return false;
			}
			ApplyStatementRegionsRecursive(GraphStatements, Definition.GraphRegions);
			CodeStatements.Append(GraphStatements);

			Private::FCodeGraphBuilder CodeGraphBuilder(
				Material,
				nullptr,
				Definition,
				SourceFilePath,
				Private::BuildGeneratedIncludeVirtualPath(SourceFilePath),
				nullptr,
				CodeSourceFilePath,
				CodeStartLine,
				CodeStartColumn);
			FString CodeBuildError;
			MaterialSlowTask.EnterProgressFrame(2.0f, FText::FromString(FString::Printf(TEXT("Creating Graph nodes for '%s'..."), *Definition.Name)));
			if (!CodeGraphBuilder.Build(CodeStatements, GeneratedCodeValues, CodeBuildError))
			{
				OutMessage = FormatGenerateError(SourceFilePath, CodeBuildError);
				return false;
			}
			GeneratedExpressionsByVariable = CodeGraphBuilder.GetGeneratedExpressionsByVariable();
			RegionByVariable = CodeGraphBuilder.GetRegionByVariable();

			MaterialSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Connecting material outputs for '%s'..."), *Definition.Name)));
			for (const FTextShaderOutputBinding& Binding : Definition.Outputs)
			{
				Private::FCodeValue OutputValue;
				FString OutputExpressionError;
				if (!CodeGraphBuilder.EvaluateOutputExpression(Binding.SourceText, OutputValue, OutputExpressionError)
					|| !OutputValue.Expression)
				{
					OutMessage = FString::Printf(
						TEXT("%s: %s"),
						*SourceFilePath,
						*OutputExpressionError);
					return false;
				}

				int32 DeclaredComponents = 0;
				bool bDeclaredTexture = false;
				ETextShaderTextureType DeclaredTextureType = ETextShaderTextureType::Texture2D;
				bool bDeclaredSubstrate = false;
				if (Private::TryResolveOutputVariableComponentCount(Definition, Binding.SourceText, DeclaredComponents, bDeclaredTexture, DeclaredTextureType, bDeclaredSubstrate))
				{
					const bool bDeclaredMaterialAttributes = DeclaredComponents == 0 && !bDeclaredTexture && !bDeclaredSubstrate;
					if (bDeclaredTexture
						|| OutputValue.bIsTextureObject
						|| bDeclaredSubstrate != OutputValue.bIsSubstrateMaterial
						|| bDeclaredMaterialAttributes != OutputValue.bIsMaterialAttributes
						|| DeclaredComponents != OutputValue.ComponentCount)
					{
						OutMessage = FString::Printf(
							TEXT("%s: Graph output '%s' does not match its declared type."),
							*SourceFilePath,
							*Binding.SourceText);
						return false;
					}
				}

				if (Binding.TargetKind == FTextShaderOutputBinding::ETargetKind::MaterialProperty)
				{
					Private::FResolvedMaterialProperty ResolvedProperty;
					verify(Private::ResolveMaterialProperty(Binding.MaterialProperty, ResolvedProperty));
					if (ResolvedProperty.bIsSubstrateMaterial)
					{
						if (!OutputValue.bIsSubstrateMaterial)
						{
							OutMessage = FString::Printf(
								TEXT("%s: Material output '%s' expects a Substrate value."),
								*SourceFilePath,
								*Binding.MaterialProperty);
							return false;
						}
					}
					else if (ResolvedProperty.OutputType == CMOT_MaterialAttributes)
					{
						if (!OutputValue.bIsMaterialAttributes)
						{
							OutMessage = FString::Printf(
								TEXT("%s: Material output '%s' expects a MaterialAttributes value."),
								*SourceFilePath,
								*Binding.MaterialProperty);
							return false;
						}
						Material->bUseMaterialAttributes = true;
					}
					else if (OutputValue.bIsSubstrateMaterial)
					{
						OutMessage = FString::Printf(
							TEXT("%s: Material output '%s' expects a numeric value, but got Substrate."),
							*SourceFilePath,
							*Binding.MaterialProperty);
						return false;
					}

					FExpressionInput* MaterialInput = Material->GetExpressionInputForProperty(ResolvedProperty.Property);
					if (!MaterialInput)
					{
						OutMessage = FString::Printf(
							TEXT("%s: Failed to find material property '%s' while connecting Graph output '%s'."),
							*SourceFilePath,
							*Binding.MaterialProperty,
							*Binding.SourceText);
						return false;
					}

					const Private::FCodeValue RoutedOutputValue = Private::CreateOutputRerouteValue(
						Material,
						nullptr,
						OutputValue,
						Binding.MaterialProperty,
						static_cast<int32>(ResolvedProperty.Property));
					Private::ConnectCodeValueToInput(*MaterialInput, RoutedOutputValue);
				}
				else
				{
					UMaterialExpression* TargetExpression = nullptr;
					FString TargetError;
					if (!CreateOrReuseOutputTargetExpression(
						Material,
						Binding,
						GeneratedOutputTargetExpressions,
						OutputTargetPositionY,
						TargetExpression,
						TargetError)
						|| !ConnectExpressionSourceToTargetPin(
							OutputValue.Expression,
							OutputValue.OutputIndex,
							Binding.SourceText,
							Binding,
							TargetExpression,
							BoundOutputTargetPins,
							TargetError))
					{
						OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *TargetError);
						return false;
					}
				}
			}
		}
		else
		{
			if (bReturnIsSubstrateMaterial)
			{
				OutMessage = FString::Printf(
					TEXT("%s: Base.FrontMaterial expects a Substrate value and cannot be driven by a material Custom node. Use a Graph block and Substrate.* nodes."),
					*SourceFilePath);
				return false;
			}
			for (const Private::FResolvedNamedOutput& OutputDefinition : NamedOutputs)
			{
				if (OutputDefinition.bIsSubstrateMaterial)
				{
					OutMessage = FString::Printf(
						TEXT("%s: Output '%s' is declared as Substrate and cannot be generated by a material Custom node. Use a Graph block and Substrate.* nodes."),
						*SourceFilePath,
						*OutputDefinition.Name);
					return false;
				}
			}

			MaterialSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Creating Custom node for '%s'..."), *Definition.Name)));
			auto* CustomExpression = Cast<UMaterialExpressionCustom>(
				UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), 120, 0));
			if (!CustomExpression)
			{
				OutMessage = FString::Printf(TEXT("%s: Failed to create the material Custom node."), *SourceFilePath);
				return false;
			}

			CustomExpression->Description = Definition.Name;
			CustomExpression->OutputType = bUsesReturn ? ReturnOutputType : CMOT_Float1;
#if DREAMSHADER_UE_VERSION_AT_LEAST(5, 4)
			CustomExpression->ShowCode = false;
#endif
			CustomExpression->Inputs.Reset();
			CustomExpression->AdditionalOutputs.Reset();
			CustomExpression->IncludeFilePaths.Reset();

			FString PreparedCustomCode;
			bool bUsesGeneratedInclude = false;
			if (!Private::PrepareCustomNodeCode(
				Definition,
				Definition.HLSL,
				TArray<FString>(),
				Definition.Name,
				PreparedCustomCode,
				bUsesGeneratedInclude,
				OutMessage))
			{
				OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *OutMessage);
				return false;
			}
			CustomExpression->Code = Private::EnsureTopLevelReturn(PreparedCustomCode);

			if (bUsesGeneratedInclude)
			{
				CustomExpression->IncludeFilePaths.Add(Private::BuildGeneratedIncludeVirtualPath(SourceFilePath));
			}

			TMap<FString, UMaterialExpression*> GeneratedPropertyExpressions;
			TSet<FString> CreatingPropertyNames;
			int32 ParameterPositionY = -300;
			for (const FTextShaderPropertyDefinition& Property : Definition.Properties)
			{
				if (!ContainsIdentifierReference(PreparedCustomCode, Property.Name))
				{
					continue;
				}

				FString PropertyExpressionError;
				UMaterialExpression* PropertyExpression = nullptr;
				if (!CreateReferencedPropertyExpression(
					Material,
					nullptr,
					Definition.Properties,
					Property,
					GeneratedPropertyExpressions,
					CreatingPropertyNames,
					ParameterPositionY,
					PropertyExpression,
					PropertyExpressionError))
				{
					OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *PropertyExpressionError);
					return false;
				}

				FCustomInput Input;
				Input.InputName = FName(*Property.Name);
				CustomExpression->Inputs.Add(Input);
				CustomExpression->Inputs.Last().Input.Connect(GetPreferredOutputIndexForProperty(Property, PropertyExpression), PropertyExpression);
			}

			for (const Private::FResolvedNamedOutput& OutputDefinition : NamedOutputs)
			{
				FCustomOutput Output;
				Output.OutputName = FName(*OutputDefinition.Name);
				Output.OutputType = OutputDefinition.OutputType;
				CustomExpression->AdditionalOutputs.Add(Output);
			}

			Private::RebuildDreamShaderCustomOutputs(CustomExpression);

			for (const FTextShaderOutputBinding& Binding : Definition.Outputs)
			{
				int32 SourceOutputIndex = 0;
				if (!Binding.SourceText.Equals(TEXT("return"), ESearchCase::IgnoreCase)
					&& !TryResolveExpressionOutputIndexByName(CustomExpression, Binding.SourceText, SourceOutputIndex))
				{
					OutMessage = FString::Printf(
						TEXT("%s: Failed to resolve Custom output '%s'."),
						*SourceFilePath,
						*Binding.SourceText);
					return false;
				}

				if (Binding.TargetKind == FTextShaderOutputBinding::ETargetKind::MaterialProperty)
				{
					Private::FResolvedMaterialProperty ResolvedProperty;
					verify(Private::ResolveMaterialProperty(Binding.MaterialProperty, ResolvedProperty));
					if (ResolvedProperty.bIsSubstrateMaterial)
					{
						OutMessage = FString::Printf(
							TEXT("%s: Material output '%s' expects a Substrate value and cannot be driven by a material Custom node. Use a Graph block and Substrate.* nodes."),
							*SourceFilePath,
							*Binding.MaterialProperty);
						return false;
					}
					if (ResolvedProperty.OutputType == CMOT_MaterialAttributes)
					{
						Material->bUseMaterialAttributes = true;
					}

					FExpressionInput* MaterialInput = Material->GetExpressionInputForProperty(ResolvedProperty.Property);
					if (!MaterialInput)
					{
						OutMessage = FString::Printf(
							TEXT("%s: Failed to find material property '%s' while connecting '%s'."),
							*SourceFilePath,
							*Binding.MaterialProperty,
							*Binding.SourceText);
						return false;
					}

					Private::FCodeValue OutputValue;
					OutputValue.Expression = CustomExpression;
					OutputValue.OutputIndex = SourceOutputIndex;
					const Private::FCodeValue RoutedOutputValue = Private::CreateOutputRerouteValue(
						Material,
						nullptr,
						OutputValue,
						Binding.MaterialProperty,
						static_cast<int32>(ResolvedProperty.Property));
					Private::ConnectCodeValueToInput(*MaterialInput, RoutedOutputValue);
				}
				else
				{
					UMaterialExpression* TargetExpression = nullptr;
					FString TargetError;
					if (!CreateOrReuseOutputTargetExpression(
						Material,
						Binding,
						GeneratedOutputTargetExpressions,
						OutputTargetPositionY,
						TargetExpression,
						TargetError)
						|| !ConnectExpressionSourceToTargetPin(
							CustomExpression,
							SourceOutputIndex,
							Binding.SourceText,
							Binding,
							TargetExpression,
							BoundOutputTargetPins,
							TargetError))
					{
						OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *TargetError);
						return false;
					}
				}
			}
		}

		if (bTransient)
		{
			MaterialSlowTask.EnterProgressFrame(1.0f);
		}
		else
		{
			MaterialSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Laying out material graph '%s'..."), *Material->GetName())));
			Private::LayoutGeneratedExpressions(
				Material,
				nullptr,
				&Definition.Layout,
				GeneratedExpressionsByVariable.IsEmpty() ? nullptr : &GeneratedExpressionsByVariable,
				RegionByVariable.IsEmpty() ? nullptr : &RegionByVariable);
		}
		MaterialSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Compiling material '%s'..."), *Material->GetName())));
		UMaterialEditingLibrary::RecompileMaterial(Material);
		Material->PostEditChange();

		if (bTransient)
		{
			MaterialSlowTask.EnterProgressFrame(1.0f);
			// Modify()/PostEditChange dirtied the in-memory package; clear it so no save-all or
			// exit prompt can silently persist a virtual material and fork the source of truth.
			Material->GetPackage()->SetDirtyFlag(false);
		}
		else
		{
			Material->MarkPackageDirty();
			Private::ApplySourceMetadata(Material, SourceFilePath, SourceHash);

			MaterialSlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("Saving material '%s'..."), *Material->GetName())));
			FString SaveError;
			if (!Private::SaveAssetPackage(Material, SaveError))
			{
				OutMessage = FString::Printf(TEXT("%s: %s"), *SourceFilePath, *SaveError);
				return false;
			}
		}

		OutMessage = FString::Printf(TEXT("Generated %s from %s.%s"), *Material->GetPathName(), *SourceFilePath, bTransient ? TEXT(" (virtual)") : TEXT(""));
		return true;
	}
}
