// Copyright (c) 2026 TypeDreamMoon. All rights reserved.
//
// Pure DreamShaderLang literal parsers, extracted from DreamShaderMaterialGeneratorSupport.cpp as
// part of decoupling that oversized translation unit. No UObject or graph state — leaf utilities
// shared (in namespace UE::DreamShader::Editor::Private) via DreamShaderMaterialGeneratorPrivate.h.

#include "DreamShaderMaterialGeneratorPrivate.h"

namespace UE::DreamShader::Editor::Private
{
	bool ParseScalarLiteral(const FString& InText, double& OutValue)
	{
		const FString Candidate = InText.TrimStartAndEnd();
		return LexTryParseString(OutValue, *Candidate);
	}

	bool ParseBooleanLiteral(const FString& InText, bool& OutValue)
	{
		const FString Candidate = InText.TrimStartAndEnd();
		if (Candidate.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			OutValue = true;
			return true;
		}
		if (Candidate.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			OutValue = false;
			return true;
		}
		return false;
	}

	bool ParseIntegerLiteral(const FString& InText, int32& OutValue)
	{
		const FString Candidate = InText.TrimStartAndEnd();
		return LexTryParseString(OutValue, *Candidate);
	}

	bool ParseUnsignedInteger32Literal(const FString& InText, uint32& OutValue)
	{
		const FString Candidate = InText.TrimStartAndEnd();
		int64 Tmp = 0;
		if (!LexTryParseString(Tmp, *Candidate) || Tmp < 0 || Tmp > static_cast<int64>(MAX_uint32))
		{
			return false;
		}
		OutValue = static_cast<uint32>(Tmp);
		return true;
	}

	bool ParseVectorLiteral(const FString& InText, TArray<double>& OutValues)
	{
		OutValues.Reset();

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
		if (Parts.IsEmpty())
		{
			return false;
		}

		for (const FString& Part : Parts)
		{
			double ParsedValue = 0.0;
			if (!LexTryParseString(ParsedValue, *Part.TrimStartAndEnd()))
			{
				return false;
			}

			OutValues.Add(ParsedValue);
		}

		return true;
	}
}
