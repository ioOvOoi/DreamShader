// Copyright (c) 2026 TypeDreamMoon. All rights reserved.
//
// FCodeGraphBuilder name->definition lookups (Function / GraphFunction / MaterialFunction /
// VirtualFunction). Pure read-only scans over the FTextShaderDefinition arrays -- no graph state,
// no UMaterialExpression creation. Extracted byte-for-byte from
// DreamShaderMaterialGeneratorCodeCalls.cpp; the member declarations stay in the FCodeGraphBuilder
// class header, so every call site is unchanged.

#include "DreamShaderMaterialGeneratorCodeShared.h"

namespace UE::DreamShader::Editor::Private
{
	const FTextShaderFunctionDefinition* FCodeGraphBuilder::FindFunctionDefinition(const FString& FunctionName) const
	{
		for (const FTextShaderFunctionDefinition& Function : Definition.Functions)
		{
			if (Function.Name.Equals(FunctionName, ESearchCase::IgnoreCase)
				|| BuildGeneratedFunctionSymbolName(Function).Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return &Function;
			}
		}

		return nullptr;
	}

	const FTextShaderFunctionDefinition* FCodeGraphBuilder::FindGraphFunctionDefinition(const FString& FunctionName) const
	{
		for (const FTextShaderFunctionDefinition& Function : Definition.GraphFunctions)
		{
			if (Function.Name.Equals(FunctionName, ESearchCase::IgnoreCase)
				|| BuildGeneratedFunctionSymbolName(Function).Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return &Function;
			}
		}

		return nullptr;
	}

	const FTextShaderMaterialFunctionDefinition* FCodeGraphBuilder::FindMaterialFunctionDefinition(const FString& FunctionName) const
	{
		for (const FTextShaderMaterialFunctionDefinition& Function : Definition.MaterialFunctions)
		{
			if (Function.Name.Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return &Function;
			}

			FString ShortName = Function.Name;
			ShortName.ReplaceInline(TEXT("\\"), TEXT("/"));
			const int32 SlashIndex = ShortName.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (SlashIndex != INDEX_NONE)
			{
				ShortName.RightChopInline(SlashIndex + 1, DREAMSHADER_ALLOW_SHRINKING_NO);
			}

			if (ShortName.Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return &Function;
			}
		}

		return nullptr;
	}

	const FTextShaderVirtualFunctionDefinition* FCodeGraphBuilder::FindVirtualFunctionDefinition(const FString& FunctionName) const
	{
		for (const FTextShaderVirtualFunctionDefinition& Function : Definition.VirtualFunctions)
		{
			if (Function.Name.Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return &Function;
			}

			FString ShortName = Function.Name;
			ShortName.ReplaceInline(TEXT("\\"), TEXT("/"));
			const int32 SlashIndex = ShortName.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (SlashIndex != INDEX_NONE)
			{
				ShortName.RightChopInline(SlashIndex + 1, DREAMSHADER_ALLOW_SHRINKING_NO);
			}

			if (ShortName.Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return &Function;
			}
		}

		return nullptr;
	}
}
