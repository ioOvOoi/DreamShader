#include "DreamShaderMaterialGeneratorCodeShared.h"

namespace UE::DreamShader::Editor::Private
{
	namespace
	{
		bool ApplyFunctionCallOutputType(
			UMaterialExpressionMaterialFunctionCall* FunctionCall,
			const int32 FunctionOutputIndex,
			int32& InOutComponentCount,
			bool& bInOutIsTextureObject,
			bool& bInOutIsSubstrateMaterial)
		{
			if (!FunctionCall || !FunctionCall->FunctionOutputs.IsValidIndex(FunctionOutputIndex))
			{
				return false;
			}

			// MaterialFunctionCall::GetOutputValueType can report scalar for function outputs
			// even when the referenced FunctionOutput is a vector. Keep the source declaration
			// authoritative so vector outputs are not expanded through invalid AppendVector nodes.
			if (InOutComponentCount > 0 || bInOutIsTextureObject || bInOutIsSubstrateMaterial || IsMaterialAttributesComponentType(InOutComponentCount, bInOutIsTextureObject, bInOutIsSubstrateMaterial))
			{
				return true;
			}

			int32 ResolvedComponentCount = 0;
			bool bResolvedIsTextureObject = false;
			const EMaterialValueType OutputValueType = FunctionCall->GetOutputValueType(FunctionOutputIndex);
			if (IsSubstrateMaterialValueType(OutputValueType))
			{
				InOutComponentCount = 0;
				bInOutIsTextureObject = false;
				bInOutIsSubstrateMaterial = true;
				return true;
			}
			if (TryResolveMaterialValueType(OutputValueType, ResolvedComponentCount, bResolvedIsTextureObject))
			{
				InOutComponentCount = ResolvedComponentCount;
				bInOutIsTextureObject = bResolvedIsTextureObject;
				bInOutIsSubstrateMaterial = false;
				return true;
			}

			const FFunctionExpressionOutput& FunctionOutput = FunctionCall->FunctionOutputs[FunctionOutputIndex];
			if (FunctionOutput.Output.Mask)
			{
				const int32 MaskComponentCount =
					(FunctionOutput.Output.MaskR ? 1 : 0)
					+ (FunctionOutput.Output.MaskG ? 1 : 0)
					+ (FunctionOutput.Output.MaskB ? 1 : 0)
					+ (FunctionOutput.Output.MaskA ? 1 : 0);
				if (MaskComponentCount > 0)
				{
					InOutComponentCount = MaskComponentCount;
					bInOutIsTextureObject = false;
					bInOutIsSubstrateMaterial = false;
					return true;
				}
			}

			return false;
		}

		FString BuildFunctionSourceArgumentList(const FTextShaderFunctionDefinition& Function, const TArray<FString>& ResultVariableNames)
		{
			TArray<FString> Parameters;
			for (const FTextShaderFunctionParameter& Input : Function.Inputs)
			{
				Parameters.Add(Input.Name);
			}
			for (const FString& ResultVariableName : ResultVariableNames)
			{
				Parameters.Add(ResultVariableName);
			}
			return FString::Join(Parameters, TEXT(", "));
		}

		bool IsSubstrateTypeUnsupportedForEngine(const FString& TypeName)
		{
			return IsSubstrateMaterialType(TypeName) && !IsSubstrateMaterialTypeSupported();
		}
	}

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
				ShortName.RightChopInline(SlashIndex + 1, EAllowShrinking::No);
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
				ShortName.RightChopInline(SlashIndex + 1, EAllowShrinking::No);
			}

			if (ShortName.Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return &Function;
			}
		}

		return nullptr;
	}

	bool FCodeGraphBuilder::EvaluateCustomFunctionCall(
		const FString& FunctionName,
		const TArray<FCodeCallArgument>& Arguments,
		FCodeValue& OutValue,
		FString& OutError)
	{
		const FTextShaderFunctionDefinition* Function = FindFunctionDefinition(FunctionName);
		if (!Function)
		{
			OutError = FString::Printf(TEXT("Unknown Graph function '%s'."), *FunctionName);
			return false;
		}

		if (Function->Results.Num() != 1)
		{
			OutError = FString::Printf(
				TEXT("DreamShader Function '%s' has %d outputs and must be called with explicit out variables, for example %s(..., ResultA, ResultB)."),
				*FunctionName,
				Function->Results.Num(),
				*FunctionName);
			return false;
		}

		if (Arguments.Num() != Function->Inputs.Num())
		{
			OutError = FString::Printf(
				TEXT("DreamShader Function '%s' returns one value and expects %d input argument(s) when used as a value expression, but got %d."),
				*FunctionName,
				Function->Inputs.Num(),
				Arguments.Num());
			return false;
		}

		for (const FCodeCallArgument& Argument : Arguments)
		{
			if (Argument.bIsNamed)
			{
				OutError = FString::Printf(TEXT("DreamShader Function '%s' currently uses positional arguments only."), *FunctionName);
				return false;
			}
		}

		ECustomMaterialOutputType ResultOutputType = CMOT_Float1;
		if (IsSubstrateMaterialType(Function->Results[0].Type))
		{
			OutError = IsSubstrateMaterialTypeSupported()
				? FString::Printf(TEXT("DreamShader Function '%s' result '%s' uses Substrate, which is not supported by HLSL Custom node functions. Use GraphFunction or ShaderFunction instead."), *FunctionName, *Function->Results[0].Name)
				: FString::Printf(TEXT("DreamShader Function '%s' result '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *FunctionName, *Function->Results[0].Name);
			return false;
		}
		if (!TryResolveCustomOutputType(Function->Results[0].Type, ResultOutputType))
		{
			OutError = FString::Printf(TEXT("DreamShader Function '%s' has unsupported result type '%s'."), *FunctionName, *Function->Results[0].Type);
			return false;
		}

		int32 ResultComponents = 1;
		verify(TryGetComponentCountForOutputType(ResultOutputType, ResultComponents));

		TArray<FCodeValue> InputValues;
		InputValues.Reserve(Function->Inputs.Num());
		for (int32 InputIndex = 0; InputIndex < Function->Inputs.Num(); ++InputIndex)
		{
			const FTextShaderFunctionParameter& InputDefinition = Function->Inputs[InputIndex];
			FCodeValue InputValue;
			if (!EvaluateExpression(Arguments[InputIndex].Expression, InputValue, OutError))
			{
				OutError = FString::Printf(TEXT("DreamShader Function '%s' input '%s': %s"), *FunctionName, *InputDefinition.Name, *OutError);
				return false;
			}

			int32 ExpectedComponentCount = 1;
			bool bExpectedTexture = false;
			bool bExpectedSubstrate = false;
			ETextShaderTextureType ExpectedTextureType = ETextShaderTextureType::Texture2D;
			if (!TryResolveCodeDeclaredType(InputDefinition.Type, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate))
			{
				if (IsSubstrateTypeUnsupportedForEngine(InputDefinition.Type))
				{
					OutError = FString::Printf(TEXT("DreamShader Function '%s' input '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *FunctionName, *InputDefinition.Name);
					return false;
				}
				OutError = FString::Printf(TEXT("DreamShader Function '%s' input '%s' uses unsupported type '%s'."), *FunctionName, *InputDefinition.Name, *InputDefinition.Type);
				return false;
			}
			if (bExpectedSubstrate)
			{
				OutError = FString::Printf(TEXT("DreamShader Function '%s' input '%s' uses Substrate, which is not supported by HLSL Custom node functions. Use GraphFunction or ShaderFunction instead."), *FunctionName, *InputDefinition.Name);
				return false;
			}

			FCodeValue CoercedValue;
			if (!CoerceValueToType(InputValue, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate, CoercedValue, OutError))
			{
				OutError = FString::Printf(TEXT("DreamShader Function '%s' input '%s': %s"), *FunctionName, *InputDefinition.Name, *OutError);
				return false;
			}
			InputValues.Add(CoercedValue);
		}

		auto* CustomExpression = Cast<UMaterialExpressionCustom>(
			CreateExpression(UMaterialExpressionCustom::StaticClass(), 640, ConsumeNodeY()));
		if (!CustomExpression)
		{
			OutError = FString::Printf(TEXT("Failed to create a Custom node for DreamShader Function '%s'."), *FunctionName);
			return false;
		}

		CustomExpression->Description = Function->Name;
		CustomExpression->OutputType = ResultOutputType;
		CustomExpression->ShowCode = false;
		CustomExpression->Inputs.Reset();
		CustomExpression->AdditionalOutputs.Reset();
		CustomExpression->IncludeFilePaths.Reset();

		for (int32 InputIndex = 0; InputIndex < Function->Inputs.Num(); ++InputIndex)
		{
			FCustomInput Input;
			Input.InputName = FName(*Function->Inputs[InputIndex].Name);
			CustomExpression->Inputs.Add(Input);
			ConnectCodeValueToInput(CustomExpression->Inputs.Last().Input, InputValues[InputIndex]);
		}

		const FString CustomCode = FString::Printf(
			TEXT("return %s(%s);"),
			*BuildGeneratedFunctionSymbolName(*Function),
			*(Function->bSelfContained
				? BuildFunctionSourceArgumentList(*Function, TArray<FString>())
				: BuildFunctionArgumentList(*Function, TArray<FString>())));

		if (Function->bSelfContained)
		{
			TArray<FString> EmbeddedFunctionNames;
			EmbeddedFunctionNames.Add(Function->Name);

			FString PreparedCustomCode;
			bool bUsesGeneratedInclude = false;
			if (!PrepareCustomNodeCode(
				Definition,
				CustomCode,
				EmbeddedFunctionNames,
				Function->Name,
				PreparedCustomCode,
				bUsesGeneratedInclude,
				OutError))
			{
				return false;
			}

			CustomExpression->Code = PreparedCustomCode;
			if (bUsesGeneratedInclude)
			{
				CustomExpression->IncludeFilePaths.Add(IncludeVirtualPath);
			}
		}
		else
		{
			CustomExpression->Code = CustomCode;
			CustomExpression->IncludeFilePaths.Add(IncludeVirtualPath);
		}

		CustomExpression->RebuildOutputs();

		OutValue.Expression = CustomExpression;
		OutValue.OutputIndex = 0;
		OutValue.ComponentCount = ResultComponents;
		OutValue.bIsTextureObject = false;
		OutValue.bIsMaterialAttributes = IsMaterialAttributesComponentType(ResultComponents, false);
		return true;
	}

	bool FCodeGraphBuilder::EvaluateGraphFunctionCall(
		const FTextShaderFunctionDefinition& Function,
		const TArray<FCodeCallArgument>& Arguments,
		FCodeValue& OutValue,
		FString& OutError)
	{
		if (!Values)
		{
			OutError = TEXT("GraphFunction value call requires an active Graph build context.");
			return false;
		}

		if (Function.Results.Num() != 1)
		{
			OutError = FString::Printf(
				TEXT("DreamShader GraphFunction '%s' has %d outputs and must be called with explicit out variables, for example %s(..., ResultA, ResultB)."),
				*Function.Name,
				Function.Results.Num(),
				*Function.Name);
			return false;
		}

		if (Arguments.Num() != Function.Inputs.Num())
		{
			OutError = FString::Printf(
				TEXT("DreamShader GraphFunction '%s' returns one value and expects %d input argument(s) when used as a value expression, but got %d."),
				*Function.Name,
				Function.Inputs.Num(),
				Arguments.Num());
			return false;
		}

		for (const FCodeCallArgument& Argument : Arguments)
		{
			if (Argument.bIsNamed)
			{
				OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' currently uses positional arguments only."), *Function.Name);
				return false;
			}
		}

		FString TempResultName;
		for (int32 Attempt = 0; Attempt < 1024; ++Attempt)
		{
			TempResultName = FString::Printf(
				TEXT("__ds_%s_value%d"),
				*UE::DreamShader::SanitizeIdentifier(Function.Name),
				Values->Num() + Attempt);
			if (!FindValue(TempResultName))
			{
				break;
			}
		}

		TArray<FCodeCallArgument> ExpandedArguments = Arguments;
		FCodeCallArgument ResultArgument;
		ResultArgument.Expression = MakeShared<FCodeExpression>();
		ResultArgument.Expression->Kind = ECodeExpressionKind::Name;
		ResultArgument.Expression->Text = TempResultName;
		ExpandedArguments.Add(ResultArgument);

		if (!ExecuteGraphFunctionCall(Function, ExpandedArguments, OutError))
		{
			return false;
		}

		if (const FCodeValue* ResultValue = FindValue(TempResultName))
		{
			OutValue = *ResultValue;
			return true;
		}

		OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' did not produce a value result."), *Function.Name);
		return false;
	}

	bool FCodeGraphBuilder::ExecuteCustomFunctionCall(
		const FTextShaderFunctionDefinition& Function,
		const TArray<FCodeCallArgument>& Arguments,
		FString& OutError)
	{
		if (Function.Results.IsEmpty())
		{
			OutError = FString::Printf(TEXT("DreamShader Function '%s' must declare at least one out result."), *Function.Name);
			return false;
		}

		const int32 ExpectedArgumentCount = Function.Inputs.Num() + Function.Results.Num();
		if (Arguments.Num() != ExpectedArgumentCount)
		{
			OutError = FString::Printf(
				TEXT("DreamShader Function '%s' expects %d arguments (%d inputs, %d out targets) but got %d."),
				*Function.Name,
				ExpectedArgumentCount,
				Function.Inputs.Num(),
				Function.Results.Num(),
				Arguments.Num());
			return false;
		}

		for (const FCodeCallArgument& Argument : Arguments)
		{
			if (Argument.bIsNamed)
			{
				OutError = FString::Printf(TEXT("DreamShader Function '%s' currently uses positional arguments only."), *Function.Name);
				return false;
			}
		}

		ECustomMaterialOutputType PrimaryOutputType = CMOT_Float1;
		if (IsSubstrateMaterialType(Function.Results[0].Type))
		{
			OutError = IsSubstrateMaterialTypeSupported()
				? FString::Printf(TEXT("DreamShader Function '%s' result '%s' uses Substrate, which is not supported by HLSL Custom node functions. Use GraphFunction or ShaderFunction instead."), *Function.Name, *Function.Results[0].Name)
				: FString::Printf(TEXT("DreamShader Function '%s' result '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *Function.Name, *Function.Results[0].Name);
			return false;
		}
		if (!TryResolveCustomOutputType(Function.Results[0].Type, PrimaryOutputType))
		{
			OutError = FString::Printf(TEXT("DreamShader Function '%s' has unsupported result type '%s'."), *Function.Name, *Function.Results[0].Type);
			return false;
		}

		int32 PrimaryOutputComponents = 1;
		verify(TryGetComponentCountForOutputType(PrimaryOutputType, PrimaryOutputComponents));

		TArray<FCodeValue> InputValues;
		InputValues.Reserve(Function.Inputs.Num());
		for (int32 InputIndex = 0; InputIndex < Function.Inputs.Num(); ++InputIndex)
		{
			const FTextShaderFunctionParameter& InputDefinition = Function.Inputs[InputIndex];
			FCodeValue InputValue;
			if (!EvaluateExpression(Arguments[InputIndex].Expression, InputValue, OutError))
			{
				OutError = FString::Printf(TEXT("DreamShader Function '%s' input '%s': %s"), *Function.Name, *InputDefinition.Name, *OutError);
				return false;
			}

			int32 ExpectedComponentCount = 1;
			bool bExpectedTexture = false;
			bool bExpectedSubstrate = false;
			ETextShaderTextureType ExpectedTextureType = ETextShaderTextureType::Texture2D;
			if (!TryResolveCodeDeclaredType(InputDefinition.Type, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate))
			{
				if (IsSubstrateTypeUnsupportedForEngine(InputDefinition.Type))
				{
					OutError = FString::Printf(TEXT("DreamShader Function '%s' input '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *Function.Name, *InputDefinition.Name);
					return false;
				}
				OutError = FString::Printf(TEXT("DreamShader Function '%s' input '%s' uses unsupported type '%s'."), *Function.Name, *InputDefinition.Name, *InputDefinition.Type);
				return false;
			}
			if (bExpectedSubstrate)
			{
				OutError = FString::Printf(TEXT("DreamShader Function '%s' input '%s' uses Substrate, which is not supported by HLSL Custom node functions. Use GraphFunction or ShaderFunction instead."), *Function.Name, *InputDefinition.Name);
				return false;
			}

			FCodeValue CoercedValue;
			if (!CoerceValueToType(InputValue, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate, CoercedValue, OutError))
			{
				OutError = FString::Printf(TEXT("DreamShader Function '%s' input '%s': %s"), *Function.Name, *InputDefinition.Name, *OutError);
				return false;
			}
			InputValues.Add(CoercedValue);
		}

		TArray<FString> ResultTargetNames;
		ResultTargetNames.Reserve(Function.Results.Num());
		TSet<FString> SeenTargetNames;
		for (int32 ResultIndex = 0; ResultIndex < Function.Results.Num(); ++ResultIndex)
		{
			const FCodeCallArgument& Argument = Arguments[Function.Inputs.Num() + ResultIndex];
			if (!Argument.Expression || Argument.Expression->Kind != ECodeExpressionKind::Name)
			{
				OutError = FString::Printf(
					TEXT("DreamShader Function '%s' out argument %d must be a plain variable name."),
					*Function.Name,
					ResultIndex + 1);
				return false;
			}

			const FString TargetName = Argument.Expression->Text.TrimStartAndEnd();
			if (TargetName.IsEmpty())
			{
				OutError = FString::Printf(TEXT("DreamShader Function '%s' has an empty out target name."), *Function.Name);
				return false;
			}

			if (SeenTargetNames.Contains(TargetName))
			{
				OutError = FString::Printf(TEXT("DreamShader Function '%s' cannot write multiple out results into '%s' in the same call."), *Function.Name, *TargetName);
				return false;
			}

			SeenTargetNames.Add(TargetName);
			ResultTargetNames.Add(TargetName);
		}

		auto* CustomExpression = Cast<UMaterialExpressionCustom>(
			CreateExpression(UMaterialExpressionCustom::StaticClass(), 640, ConsumeNodeY()));
		if (!CustomExpression)
		{
			OutError = FString::Printf(TEXT("Failed to create a Custom node for DreamShader Function '%s'."), *Function.Name);
			return false;
		}

		CustomExpression->Description = Function.Name;
		CustomExpression->OutputType = PrimaryOutputType;
		CustomExpression->ShowCode = false;
		CustomExpression->Inputs.Reset();
		CustomExpression->AdditionalOutputs.Reset();
		CustomExpression->IncludeFilePaths.Reset();

		for (int32 InputIndex = 0; InputIndex < Function.Inputs.Num(); ++InputIndex)
		{
			FCustomInput Input;
			Input.InputName = FName(*Function.Inputs[InputIndex].Name);
			CustomExpression->Inputs.Add(Input);
			ConnectCodeValueToInput(CustomExpression->Inputs.Last().Input, InputValues[InputIndex]);
		}

		TArray<FString> ResultVariableNames;
		ResultVariableNames.Reserve(Function.Results.Num());

		FString CustomCode;
		for (int32 ResultIndex = 0; ResultIndex < Function.Results.Num(); ++ResultIndex)
		{
			const FString TempName = FString::Printf(
				TEXT("__ds_%s_out%d"),
				*UE::DreamShader::SanitizeIdentifier(Function.Name),
				ResultIndex);
			ResultVariableNames.Add(TempName);
			CustomCode += FString::Printf(
				TEXT("%s %s = (%s)0;\n"),
				*Function.Results[ResultIndex].Type,
				*TempName,
				*Function.Results[ResultIndex].Type);
		}

		TArray<FString> SecondaryResultVariables;
		for (int32 ResultIndex = 1; ResultIndex < ResultVariableNames.Num(); ++ResultIndex)
		{
			SecondaryResultVariables.Add(ResultVariableNames[ResultIndex]);
		}

		if (Function.bSelfContained)
		{
			CustomCode += FString::Printf(
				TEXT("%s(%s);\n"),
				*BuildGeneratedFunctionSymbolName(Function),
				*BuildFunctionSourceArgumentList(Function, ResultVariableNames));
		}
		else
		{
			CustomCode += FString::Printf(
				TEXT("%s = %s(%s);\n"),
				*ResultVariableNames[0],
				*BuildGeneratedFunctionSymbolName(Function),
				*BuildFunctionArgumentList(Function, SecondaryResultVariables));
		}

		for (int32 ResultIndex = 1; ResultIndex < ResultVariableNames.Num(); ++ResultIndex)
		{
			CustomCode += FString::Printf(
				TEXT("%s = %s;\n"),
				*ResultTargetNames[ResultIndex],
				*ResultVariableNames[ResultIndex]);
		}

		CustomCode += FString::Printf(TEXT("return %s;"), *ResultVariableNames[0]);

		if (Function.bSelfContained)
		{
			TArray<FString> EmbeddedFunctionNames;
			EmbeddedFunctionNames.Add(Function.Name);

			FString PreparedCustomCode;
			bool bUsesGeneratedInclude = false;
			if (!PrepareCustomNodeCode(
				Definition,
				CustomCode,
				EmbeddedFunctionNames,
				Function.Name,
				PreparedCustomCode,
				bUsesGeneratedInclude,
				OutError))
			{
				return false;
			}

			CustomExpression->Code = PreparedCustomCode;
			if (bUsesGeneratedInclude)
			{
				CustomExpression->IncludeFilePaths.Add(IncludeVirtualPath);
			}
		}
		else
		{
			CustomExpression->Code = CustomCode;
			CustomExpression->IncludeFilePaths.Add(IncludeVirtualPath);
		}

		for (int32 ResultIndex = 1; ResultIndex < Function.Results.Num(); ++ResultIndex)
		{
			ECustomMaterialOutputType AdditionalOutputType = CMOT_Float1;
			if (IsSubstrateMaterialType(Function.Results[ResultIndex].Type))
			{
				OutError = IsSubstrateMaterialTypeSupported()
					? FString::Printf(TEXT("DreamShader Function '%s' result '%s' uses Substrate, which is not supported by HLSL Custom node functions. Use GraphFunction or ShaderFunction instead."), *Function.Name, *Function.Results[ResultIndex].Name)
					: FString::Printf(TEXT("DreamShader Function '%s' result '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *Function.Name, *Function.Results[ResultIndex].Name);
				return false;
			}
			if (!TryResolveCustomOutputType(Function.Results[ResultIndex].Type, AdditionalOutputType))
			{
				OutError = FString::Printf(
					TEXT("DreamShader Function '%s' has unsupported result type '%s'."),
					*Function.Name,
					*Function.Results[ResultIndex].Type);
				return false;
			}

			FCustomOutput Output;
			Output.OutputName = FName(*ResultTargetNames[ResultIndex]);
			Output.OutputType = AdditionalOutputType;
			CustomExpression->AdditionalOutputs.Add(Output);
		}

		CustomExpression->RebuildOutputs();

		for (int32 ResultIndex = 0; ResultIndex < Function.Results.Num(); ++ResultIndex)
		{
			ECustomMaterialOutputType ResultOutputType = CMOT_Float1;
			if (IsSubstrateMaterialType(Function.Results[ResultIndex].Type))
			{
				OutError = IsSubstrateMaterialTypeSupported()
					? FString::Printf(TEXT("DreamShader Function '%s' result '%s' uses Substrate, which is not supported by HLSL Custom node functions. Use GraphFunction or ShaderFunction instead."), *Function.Name, *Function.Results[ResultIndex].Name)
					: FString::Printf(TEXT("DreamShader Function '%s' result '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *Function.Name, *Function.Results[ResultIndex].Name);
				return false;
			}
			if (!TryResolveCustomOutputType(Function.Results[ResultIndex].Type, ResultOutputType))
			{
				OutError = FString::Printf(
					TEXT("DreamShader Function '%s' has unsupported result type '%s'."),
					*Function.Name,
					*Function.Results[ResultIndex].Type);
				return false;
			}

			int32 ResultComponents = 1;
			verify(TryGetComponentCountForOutputType(ResultOutputType, ResultComponents));

			FCodeValue ResultValue;
			ResultValue.Expression = CustomExpression;
			ResultValue.OutputIndex = ResultIndex;
			ResultValue.ComponentCount = ResultComponents;
			ResultValue.bIsTextureObject = false;
			ResultValue.bIsMaterialAttributes = IsMaterialAttributesComponentType(ResultComponents, false);
			(*Values).Add(ResultTargetNames[ResultIndex], ResultValue);
		}

		return true;
	}

	bool FCodeGraphBuilder::ExecuteGraphFunctionCall(
		const FTextShaderFunctionDefinition& Function,
		const TArray<FCodeCallArgument>& Arguments,
		FString& OutError)
	{
		if (!Values)
		{
			OutError = TEXT("GraphFunction call requires an active Graph build context.");
			return false;
		}

		if (Function.Results.IsEmpty())
		{
			OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' must declare at least one out result."), *Function.Name);
			return false;
		}

		static TArray<FString> ActiveGraphFunctionStack;
		if (ActiveGraphFunctionStack.ContainsByPredicate([&Function](const FString& ActiveName)
			{
				return ActiveName.Equals(Function.Name, ESearchCase::IgnoreCase);
			}))
		{
			TArray<FString> Cycle = ActiveGraphFunctionStack;
			Cycle.Add(Function.Name);
			OutError = FString::Printf(TEXT("GraphFunction cycle detected: %s."), *FString::Join(Cycle, TEXT(" -> ")));
			return false;
		}

		struct FActiveGraphFunctionGuard
		{
			TArray<FString>& Stack;
			explicit FActiveGraphFunctionGuard(TArray<FString>& InStack, const FString& Name)
				: Stack(InStack)
			{
				Stack.Add(Name);
			}
			~FActiveGraphFunctionGuard()
			{
				Stack.Pop(EAllowShrinking::No);
			}
		};
		FActiveGraphFunctionGuard ActiveGuard(ActiveGraphFunctionStack, Function.Name);

		const int32 ExpectedArgumentCount = Function.Inputs.Num() + Function.Results.Num();
		if (Arguments.Num() != ExpectedArgumentCount)
		{
			OutError = FString::Printf(
				TEXT("DreamShader GraphFunction '%s' expects %d arguments (%d inputs, %d out targets) but got %d."),
				*Function.Name,
				ExpectedArgumentCount,
				Function.Inputs.Num(),
				Function.Results.Num(),
				Arguments.Num());
			return false;
		}

		for (const FCodeCallArgument& Argument : Arguments)
		{
			if (Argument.bIsNamed)
			{
				OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' currently uses positional arguments only."), *Function.Name);
				return false;
			}
		}

		if (Function.HLSL.IsEmpty())
		{
			TArray<FCodeValue> InputValues;
			InputValues.Reserve(Function.Inputs.Num());
			TMap<FString, FCodeValue> LocalValues = *Values;
			for (int32 InputIndex = 0; InputIndex < Function.Inputs.Num(); ++InputIndex)
			{
				const FTextShaderFunctionParameter& InputDefinition = Function.Inputs[InputIndex];
				FCodeValue InputValue;
				if (!EvaluateExpression(Arguments[InputIndex].Expression, InputValue, OutError))
				{
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' input '%s': %s"), *Function.Name, *InputDefinition.Name, *OutError);
					return false;
				}

				int32 ExpectedComponentCount = 1;
				bool bExpectedTexture = false;
				bool bExpectedSubstrate = false;
				ETextShaderTextureType ExpectedTextureType = ETextShaderTextureType::Texture2D;
				if (!TryResolveCodeDeclaredType(InputDefinition.Type, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate))
				{
					if (IsSubstrateTypeUnsupportedForEngine(InputDefinition.Type))
					{
						OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' input '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *Function.Name, *InputDefinition.Name);
						return false;
					}
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' input '%s' uses unsupported type '%s'."), *Function.Name, *InputDefinition.Name, *InputDefinition.Type);
					return false;
				}

				FCodeValue CoercedValue;
				if (!CoerceValueToType(InputValue, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate, CoercedValue, OutError))
				{
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' input '%s': %s"), *Function.Name, *InputDefinition.Name, *OutError);
					return false;
				}

				LocalValues.Add(InputDefinition.Name, CoercedValue);
				InputValues.Add(CoercedValue);
			}

			TArray<FString> ResultTargetNames;
			ResultTargetNames.Reserve(Function.Results.Num());
			TSet<FString> SeenTargetNames;
			for (int32 ResultIndex = 0; ResultIndex < Function.Results.Num(); ++ResultIndex)
			{
				const FCodeCallArgument& Argument = Arguments[Function.Inputs.Num() + ResultIndex];
				if (!Argument.Expression || Argument.Expression->Kind != ECodeExpressionKind::Name)
				{
					OutError = FString::Printf(
						TEXT("DreamShader GraphFunction '%s' out argument %d must be a plain variable name."),
						*Function.Name,
						ResultIndex + 1);
					return false;
				}

				const FString TargetName = Argument.Expression->Text.TrimStartAndEnd();
				if (TargetName.IsEmpty())
				{
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' has an empty out target name."), *Function.Name);
					return false;
				}

				if (SeenTargetNames.Contains(TargetName))
				{
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' cannot write multiple out results into '%s' in the same call."), *Function.Name, *TargetName);
					return false;
				}

				SeenTargetNames.Add(TargetName);
				ResultTargetNames.Add(TargetName);
			}

			TMap<FString, FCodeValue>* PreviousValues = Values;
			struct FScopedGraphFunctionValues
			{
				TMap<FString, FCodeValue>*& ValuesRef;
				TMap<FString, FCodeValue>* Previous;
				FScopedGraphFunctionValues(TMap<FString, FCodeValue>*& InValuesRef, TMap<FString, FCodeValue>* NewValues)
					: ValuesRef(InValuesRef)
					, Previous(InValuesRef)
				{
					ValuesRef = NewValues;
				}
				~FScopedGraphFunctionValues()
				{
					ValuesRef = Previous;
				}
			};

			TArray<FCodeStatement> Statements;
			FString ParseError;
			if (!ParseCodeStatements(Function.HLSL, Statements, ParseError))
			{
				OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' Graph body is invalid: %s"), *Function.Name, *ParseError);
				return false;
			}

			{
				FScopedGraphFunctionValues ScopedValues(Values, &LocalValues);
				for (const FCodeStatement& Statement : Statements)
				{
					if (!ExecuteStatement(Statement, OutError))
					{
						OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s': %s"), *Function.Name, *FormatStatementError(Statement, OutError));
						return false;
					}
				}
			}

			for (int32 ResultIndex = 0; ResultIndex < Function.Results.Num(); ++ResultIndex)
			{
				const FTextShaderFunctionParameter& ResultDefinition = Function.Results[ResultIndex];
				const FCodeValue* ResultValue = LocalValues.Find(ResultDefinition.Name);
				if (!ResultValue || !ResultValue->Expression)
				{
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' result '%s' was never assigned."), *Function.Name, *ResultDefinition.Name);
					return false;
				}

				int32 ExpectedComponentCount = 1;
				bool bExpectedTexture = false;
				bool bExpectedSubstrate = false;
				ETextShaderTextureType ExpectedTextureType = ETextShaderTextureType::Texture2D;
				if (!TryResolveCodeDeclaredType(ResultDefinition.Type, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate))
				{
					if (IsSubstrateTypeUnsupportedForEngine(ResultDefinition.Type))
					{
						OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' result '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *Function.Name, *ResultDefinition.Name);
						return false;
					}
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' result '%s' uses unsupported type '%s'."), *Function.Name, *ResultDefinition.Name, *ResultDefinition.Type);
					return false;
				}

				FCodeValue CoercedResult;
				if (!CoerceValueToType(*ResultValue, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate, CoercedResult, OutError))
				{
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' result '%s': %s"), *Function.Name, *ResultDefinition.Name, *OutError);
					return false;
				}

				PreviousValues->Add(ResultTargetNames[ResultIndex], CoercedResult);
			}

			return true;
		}

		ECustomMaterialOutputType PrimaryOutputType = CMOT_Float1;
		if (IsSubstrateMaterialType(Function.Results[0].Type))
		{
			OutError = IsSubstrateMaterialTypeSupported()
				? FString::Printf(TEXT("DreamShader GraphFunction '%s' result '%s' uses Substrate, which is only supported by GraphFunction Graph blocks."), *Function.Name, *Function.Results[0].Name)
				: FString::Printf(TEXT("DreamShader GraphFunction '%s' result '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *Function.Name, *Function.Results[0].Name);
			return false;
		}
		if (!TryResolveCustomOutputType(Function.Results[0].Type, PrimaryOutputType))
		{
			OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' has unsupported result type '%s'."), *Function.Name, *Function.Results[0].Type);
			return false;
		}

		int32 PrimaryOutputComponents = 1;
		verify(TryGetComponentCountForOutputType(PrimaryOutputType, PrimaryOutputComponents));

		TArray<FCodeValue> InputValues;
		InputValues.Reserve(Function.Inputs.Num());
		TMap<FString, FCodeValue> LocalValues = *Values;
		for (int32 InputIndex = 0; InputIndex < Function.Inputs.Num(); ++InputIndex)
		{
			const FTextShaderFunctionParameter& InputDefinition = Function.Inputs[InputIndex];
			FCodeValue InputValue;
			if (!EvaluateExpression(Arguments[InputIndex].Expression, InputValue, OutError))
			{
				OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' input '%s': %s"), *Function.Name, *InputDefinition.Name, *OutError);
				return false;
			}

			int32 ExpectedComponentCount = 1;
			bool bExpectedTexture = false;
			bool bExpectedSubstrate = false;
			ETextShaderTextureType ExpectedTextureType = ETextShaderTextureType::Texture2D;
			if (!TryResolveCodeDeclaredType(InputDefinition.Type, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate))
			{
				if (IsSubstrateTypeUnsupportedForEngine(InputDefinition.Type))
				{
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' input '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *Function.Name, *InputDefinition.Name);
					return false;
				}
				OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' input '%s' uses unsupported type '%s'."), *Function.Name, *InputDefinition.Name, *InputDefinition.Type);
				return false;
			}
			if (bExpectedSubstrate)
			{
				OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' input '%s' uses Substrate, which is only supported by GraphFunction Graph blocks."), *Function.Name, *InputDefinition.Name);
				return false;
			}

			FCodeValue CoercedValue;
			if (!CoerceValueToType(InputValue, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate, CoercedValue, OutError))
			{
				OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' input '%s': %s"), *Function.Name, *InputDefinition.Name, *OutError);
				return false;
			}

			LocalValues.Add(InputDefinition.Name, CoercedValue);
			InputValues.Add(CoercedValue);
		}

		TArray<FString> ResultTargetNames;
		ResultTargetNames.Reserve(Function.Results.Num());
		TSet<FString> SeenTargetNames;
		for (int32 ResultIndex = 0; ResultIndex < Function.Results.Num(); ++ResultIndex)
		{
			const FCodeCallArgument& Argument = Arguments[Function.Inputs.Num() + ResultIndex];
			if (!Argument.Expression || Argument.Expression->Kind != ECodeExpressionKind::Name)
			{
				OutError = FString::Printf(
					TEXT("DreamShader GraphFunction '%s' out argument %d must be a plain variable name."),
					*Function.Name,
					ResultIndex + 1);
				return false;
			}

			const FString TargetName = Argument.Expression->Text.TrimStartAndEnd();
			if (TargetName.IsEmpty())
			{
				OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' has an empty out target name."), *Function.Name);
				return false;
			}

			if (SeenTargetNames.Contains(TargetName))
			{
				OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' cannot write multiple out results into '%s' in the same call."), *Function.Name, *TargetName);
				return false;
			}

			SeenTargetNames.Add(TargetName);
			ResultTargetNames.Add(TargetName);
		}

		auto* CustomExpression = Cast<UMaterialExpressionCustom>(
			CreateExpression(UMaterialExpressionCustom::StaticClass(), 640, ConsumeNodeY()));
		if (!CustomExpression)
		{
			OutError = FString::Printf(TEXT("Failed to create a Custom node for DreamShader GraphFunction '%s'."), *Function.Name);
			return false;
		}

		CustomExpression->Description = Function.Name;
		CustomExpression->OutputType = PrimaryOutputType;
		CustomExpression->ShowCode = false;
		CustomExpression->Inputs.Reset();
		CustomExpression->AdditionalOutputs.Reset();
		CustomExpression->IncludeFilePaths.Reset();

		for (int32 InputIndex = 0; InputIndex < Function.Inputs.Num(); ++InputIndex)
		{
			FCustomInput Input;
			Input.InputName = FName(*Function.Inputs[InputIndex].Name);
			CustomExpression->Inputs.Add(Input);
			ConnectCodeValueToInput(CustomExpression->Inputs.Last().Input, InputValues[InputIndex]);
		}

		TSet<FString> CustomInputNames;
		for (const FTextShaderFunctionParameter& InputDefinition : Function.Inputs)
		{
			CustomInputNames.Add(UE::DreamShader::NormalizeSettingKey(InputDefinition.Name));
		}

		auto AddCustomInputValue = [&CustomExpression, &CustomInputNames](const FString& BaseName, const FCodeValue& Value) -> FString
		{
			FString InputName = UE::DreamShader::SanitizeIdentifier(BaseName);
			if (InputName.IsEmpty())
			{
				InputName = TEXT("__ds_input");
			}

			const FString OriginalInputName = InputName;
			for (int32 Attempt = 0; CustomInputNames.Contains(UE::DreamShader::NormalizeSettingKey(InputName)); ++Attempt)
			{
				InputName = FString::Printf(TEXT("%s_%d"), *OriginalInputName, Attempt + 1);
			}

			FCustomInput Input;
			Input.InputName = FName(*InputName);
			CustomExpression->Inputs.Add(Input);
			ConnectCodeValueToInput(CustomExpression->Inputs.Last().Input, Value);
			CustomInputNames.Add(UE::DreamShader::NormalizeSettingKey(InputName));
			return InputName;
		};

		TMap<FString, FCodeValue>* PreviousValues = Values;
		struct FScopedCodeValues
		{
			TMap<FString, FCodeValue>*& ValuesRef;
			TMap<FString, FCodeValue>* Previous;
			FScopedCodeValues(TMap<FString, FCodeValue>*& InValuesRef, TMap<FString, FCodeValue>* NewValues)
				: ValuesRef(InValuesRef)
				, Previous(InValuesRef)
			{
				ValuesRef = NewValues;
			}
			~FScopedCodeValues()
			{
				ValuesRef = Previous;
			}
		};

		auto RewriteUEInputs = [this, &Function, &AddCustomInputValue, &LocalValues, &OutError](const FString& SourceCode, FString& OutRewrittenCode) -> bool
		{
			OutRewrittenCode.Reset();
			OutRewrittenCode.Reserve(SourceCode.Len());

			FScopedCodeValues ScopedValues(Values, &LocalValues);
			bool bInString = false;
			bool bInChar = false;
			bool bInLineComment = false;
			bool bInBlockComment = false;
			int32 AutoInputIndex = 0;

			for (int32 Index = 0; Index < SourceCode.Len(); ++Index)
			{
				const TCHAR Char = SourceCode[Index];

				if (bInLineComment)
				{
					OutRewrittenCode.AppendChar(Char);
					if (Char == TCHAR('\n'))
					{
						bInLineComment = false;
					}
					continue;
				}

				if (bInBlockComment)
				{
					OutRewrittenCode.AppendChar(Char);
					if (Char == TCHAR('*') && SourceCode.IsValidIndex(Index + 1) && SourceCode[Index + 1] == TCHAR('/'))
					{
						OutRewrittenCode.AppendChar(SourceCode[++Index]);
						bInBlockComment = false;
					}
					continue;
				}

				if (bInString || bInChar)
				{
					OutRewrittenCode.AppendChar(Char);
					if (Char == TCHAR('\\') && SourceCode.IsValidIndex(Index + 1))
					{
						OutRewrittenCode.AppendChar(SourceCode[++Index]);
						continue;
					}
					if (bInString && Char == TCHAR('"'))
					{
						bInString = false;
					}
					else if (bInChar && Char == TCHAR('\''))
					{
						bInChar = false;
					}
					continue;
				}

				if (Char == TCHAR('/') && SourceCode.IsValidIndex(Index + 1))
				{
					if (SourceCode[Index + 1] == TCHAR('/'))
					{
						OutRewrittenCode.AppendChar(Char);
						OutRewrittenCode.AppendChar(SourceCode[++Index]);
						bInLineComment = true;
						continue;
					}
					if (SourceCode[Index + 1] == TCHAR('*'))
					{
						OutRewrittenCode.AppendChar(Char);
						OutRewrittenCode.AppendChar(SourceCode[++Index]);
						bInBlockComment = true;
						continue;
					}
				}

				if (Char == TCHAR('"'))
				{
					OutRewrittenCode.AppendChar(Char);
					bInString = true;
					continue;
				}

				if (Char == TCHAR('\''))
				{
					OutRewrittenCode.AppendChar(Char);
					bInChar = true;
					continue;
				}

				const bool bCanStartUECall =
					FChar::ToUpper(Char) == TCHAR('U')
					&& SourceCode.IsValidIndex(Index + 2)
					&& FChar::ToUpper(SourceCode[Index + 1]) == TCHAR('E')
					&& SourceCode[Index + 2] == TCHAR('.')
					&& IsIdentifierBoundary(SourceCode, Index - 1);
				if (!bCanStartUECall)
				{
					OutRewrittenCode.AppendChar(Char);
					continue;
				}

				int32 Cursor = Index + 3;
				if (!SourceCode.IsValidIndex(Cursor) || !(FChar::IsAlpha(SourceCode[Cursor]) || SourceCode[Cursor] == TCHAR('_')))
				{
					OutRewrittenCode.AppendChar(Char);
					continue;
				}

				++Cursor;
				while (SourceCode.IsValidIndex(Cursor) && (FChar::IsAlnum(SourceCode[Cursor]) || SourceCode[Cursor] == TCHAR('_')))
				{
					++Cursor;
				}

				SkipWhitespace(SourceCode, Cursor);
				if (!SourceCode.IsValidIndex(Cursor) || SourceCode[Cursor] != TCHAR('('))
				{
					OutRewrittenCode.AppendChar(Char);
					continue;
				}

				int32 CloseIndex = INDEX_NONE;
				if (!FindMatchingDelimiter(SourceCode, Cursor, TCHAR('('), TCHAR(')'), CloseIndex))
				{
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' contains an unterminated UE.* call."), *Function.Name);
					return false;
				}

				const FString CallText = SourceCode.Mid(Index, CloseIndex - Index + 1);
				TSharedPtr<FCodeExpression> Expression;
				if (!ParseCodeExpression(CallText, Expression, OutError))
				{
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' UE input '%s': %s"), *Function.Name, *CallText, *OutError);
					return false;
				}

				FCodeValue UEValue;
				if (!EvaluateExpression(Expression, UEValue, OutError))
				{
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' UE input '%s': %s"), *Function.Name, *CallText, *OutError);
					return false;
				}

				if (UEValue.bIsTextureObject || UEValue.bIsMaterialAttributes || UEValue.bIsSubstrateMaterial)
				{
					OutError = FString::Printf(TEXT("DreamShader GraphFunction '%s' UE input '%s' cannot be passed into a Custom node input."), *Function.Name, *CallText);
					return false;
				}

				const FString InputName = AddCustomInputValue(
					FString::Printf(TEXT("__ds_%s_UE%d"), *UE::DreamShader::SanitizeIdentifier(Function.Name), AutoInputIndex++),
					UEValue);
				OutRewrittenCode += InputName;
				Index = CloseIndex;
			}

			return true;
		};

		FString RewrittenHLSL;
		if (!RewriteUEInputs(Function.HLSL, RewrittenHLSL))
		{
			return false;
		}

		FString CustomCode;
		for (const FTextShaderFunctionParameter& ResultDefinition : Function.Results)
		{
			CustomCode += FString::Printf(
				TEXT("%s %s = (%s)0;\n"),
				*ResultDefinition.Type,
				*ResultDefinition.Name,
				*ResultDefinition.Type);
		}

		CustomCode += RewrittenHLSL;
		if (!CustomCode.EndsWith(TEXT("\n")))
		{
			CustomCode += TEXT("\n");
		}

		for (int32 ResultIndex = 1; ResultIndex < Function.Results.Num(); ++ResultIndex)
		{
			CustomCode += FString::Printf(
				TEXT("%s = %s;\n"),
				*ResultTargetNames[ResultIndex],
				*Function.Results[ResultIndex].Name);
		}

		CustomCode += FString::Printf(TEXT("return %s;"), *Function.Results[0].Name);

		FString PreparedCustomCode;
		bool bUsesGeneratedInclude = false;
		if (!PrepareCustomNodeCode(
			Definition,
			CustomCode,
			TArray<FString>(),
			Function.Name,
			PreparedCustomCode,
			bUsesGeneratedInclude,
			OutError))
		{
			return false;
		}

		CustomExpression->Code = PreparedCustomCode;
		if (bUsesGeneratedInclude)
		{
			CustomExpression->IncludeFilePaths.Add(IncludeVirtualPath);
		}

		for (int32 ResultIndex = 1; ResultIndex < Function.Results.Num(); ++ResultIndex)
		{
			ECustomMaterialOutputType AdditionalOutputType = CMOT_Float1;
			if (IsSubstrateMaterialType(Function.Results[ResultIndex].Type))
			{
				OutError = IsSubstrateMaterialTypeSupported()
					? FString::Printf(TEXT("DreamShader GraphFunction '%s' result '%s' uses Substrate, which is only supported by GraphFunction Graph blocks."), *Function.Name, *Function.Results[ResultIndex].Name)
					: FString::Printf(TEXT("DreamShader GraphFunction '%s' result '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *Function.Name, *Function.Results[ResultIndex].Name);
				return false;
			}
			if (!TryResolveCustomOutputType(Function.Results[ResultIndex].Type, AdditionalOutputType))
			{
				OutError = FString::Printf(
					TEXT("DreamShader GraphFunction '%s' has unsupported result type '%s'."),
					*Function.Name,
					*Function.Results[ResultIndex].Type);
				return false;
			}

			FCustomOutput Output;
			Output.OutputName = FName(*ResultTargetNames[ResultIndex]);
			Output.OutputType = AdditionalOutputType;
			CustomExpression->AdditionalOutputs.Add(Output);
		}

		CustomExpression->RebuildOutputs();

		for (int32 ResultIndex = 0; ResultIndex < Function.Results.Num(); ++ResultIndex)
		{
			ECustomMaterialOutputType ResultOutputType = CMOT_Float1;
			if (IsSubstrateMaterialType(Function.Results[ResultIndex].Type))
			{
				OutError = IsSubstrateMaterialTypeSupported()
					? FString::Printf(TEXT("DreamShader GraphFunction '%s' result '%s' uses Substrate, which is only supported by GraphFunction Graph blocks."), *Function.Name, *Function.Results[ResultIndex].Name)
					: FString::Printf(TEXT("DreamShader GraphFunction '%s' result '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *Function.Name, *Function.Results[ResultIndex].Name);
				return false;
			}
			if (!TryResolveCustomOutputType(Function.Results[ResultIndex].Type, ResultOutputType))
			{
				OutError = FString::Printf(
					TEXT("DreamShader GraphFunction '%s' has unsupported result type '%s'."),
					*Function.Name,
					*Function.Results[ResultIndex].Type);
				return false;
			}

			int32 ResultComponents = 1;
			verify(TryGetComponentCountForOutputType(ResultOutputType, ResultComponents));

			FCodeValue ResultValue;
			ResultValue.Expression = CustomExpression;
			ResultValue.OutputIndex = ResultIndex;
			ResultValue.ComponentCount = ResultComponents;
			ResultValue.bIsTextureObject = false;
			ResultValue.bIsMaterialAttributes = IsMaterialAttributesComponentType(ResultComponents, false);
			PreviousValues->Add(ResultTargetNames[ResultIndex], ResultValue);
		}

		return true;
	}

	bool FCodeGraphBuilder::EvaluateMaterialFunctionCall(
		const FTextShaderMaterialFunctionDefinition& Function,
		const TArray<FCodeCallArgument>& Arguments,
		FCodeValue& OutValue,
		FString& OutError)
	{
		FString PackageName;
		FString ObjectPath;
		FString AssetName;
		if (!ResolveDreamShaderAssetDestination(Function.Name, Function.Root, PackageName, ObjectPath, AssetName, OutError))
		{
			return false;
		}

		return EvaluateMaterialFunctionCallAsset(
			UE::DreamShader::LexToString(Function.Kind),
			Function.Name,
			ObjectPath,
			Function.Inputs,
			Function.Outputs,
			Arguments,
			OutValue,
			OutError);
	}

	bool FCodeGraphBuilder::EvaluateVirtualFunctionCall(
		const FTextShaderVirtualFunctionDefinition& Function,
		const TArray<FCodeCallArgument>& Arguments,
		FCodeValue& OutValue,
		FString& OutError)
	{
		FString ObjectPath;
		if (!TryResolveDreamShaderAssetReference(Function.Asset, ObjectPath, OutError))
		{
			OutError = FString::Printf(TEXT("VirtualFunction '%s' asset reference is invalid: %s"), *Function.Name, *OutError);
			return false;
		}

		return EvaluateMaterialFunctionCallAsset(
			TEXT("VirtualFunction"),
			Function.Name,
			ObjectPath,
			Function.Inputs,
			Function.Outputs,
			Arguments,
			OutValue,
			OutError);
	}

	bool FCodeGraphBuilder::ExecuteMaterialFunctionCall(
		const FTextShaderMaterialFunctionDefinition& Function,
		const TArray<FCodeCallArgument>& Arguments,
		FString& OutError)
	{
		FString PackageName;
		FString ObjectPath;
		FString AssetName;
		if (!ResolveDreamShaderAssetDestination(Function.Name, Function.Root, PackageName, ObjectPath, AssetName, OutError))
		{
			return false;
		}

		return ExecuteMaterialFunctionCallAsset(
			UE::DreamShader::LexToString(Function.Kind),
			Function.Name,
			ObjectPath,
			Function.Inputs,
			Function.Outputs,
			Arguments,
			OutError);
	}

	bool FCodeGraphBuilder::ExecuteVirtualFunctionCall(
		const FTextShaderVirtualFunctionDefinition& Function,
		const TArray<FCodeCallArgument>& Arguments,
		FString& OutError)
	{
		FString ObjectPath;
		if (!TryResolveDreamShaderAssetReference(Function.Asset, ObjectPath, OutError))
		{
			OutError = FString::Printf(TEXT("VirtualFunction '%s' asset reference is invalid: %s"), *Function.Name, *OutError);
			return false;
		}

		return ExecuteMaterialFunctionCallAsset(
			TEXT("VirtualFunction"),
			Function.Name,
			ObjectPath,
			Function.Inputs,
			Function.Outputs,
			Arguments,
			OutError);
	}

	bool FCodeGraphBuilder::CreateAndConnectMaterialFunctionCallAsset(
		const FString& CallKind,
		const FString& FunctionName,
		const FString& ObjectPath,
		const TArray<FTextShaderFunctionParameter>& Inputs,
		const TArray<FTextShaderFunctionParameter>& Outputs,
		const TArray<FCodeCallArgument>& InputArguments,
		UMaterialExpressionMaterialFunctionCall*& OutFunctionCall,
		FString& OutError)
	{
		OutFunctionCall = nullptr;
		if (Outputs.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s '%s' must declare at least one output."), *CallKind, *FunctionName);
			return false;
		}

		UMaterialFunction* MaterialFunctionAsset = LoadObject<UMaterialFunction>(nullptr, *ObjectPath);
		if (!MaterialFunctionAsset)
		{
			OutError = FString::Printf(TEXT("%s '%s' could not load MaterialFunction asset '%s'."), *CallKind, *FunctionName, *ObjectPath);
			return false;
		}

		auto* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(
			CreateExpression(UMaterialExpressionMaterialFunctionCall::StaticClass(), 640, ConsumeNodeY()));
		if (!FunctionCall)
		{
			OutError = FString::Printf(TEXT("Failed to create a MaterialFunctionCall node for '%s'."), *FunctionName);
			return false;
		}

		if (!FunctionCall->SetMaterialFunction(MaterialFunctionAsset))
		{
			OutError = FString::Printf(TEXT("Failed to assign material function '%s' to the generated call node."), *FunctionName);
			return false;
		}

		TArray<const FCodeCallArgument*> PositionalArguments;
		for (const FCodeCallArgument& Argument : InputArguments)
		{
			if (!Argument.bIsNamed)
			{
				PositionalArguments.Add(&Argument);
			}
		}

		int32 PositionalArgumentIndex = 0;
		for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
		{
			const FTextShaderFunctionParameter& InputDefinition = Inputs[InputIndex];
			const FCodeCallArgument* InputArgument = FindNamedArgument(InputArguments, *InputDefinition.Name);
			if (!InputArgument && PositionalArguments.IsValidIndex(PositionalArgumentIndex))
			{
				InputArgument = PositionalArguments[PositionalArgumentIndex++];
			}

			int32 FunctionInputIndex = INDEX_NONE;
			for (int32 CandidateIndex = 0; CandidateIndex < FunctionCall->FunctionInputs.Num(); ++CandidateIndex)
			{
				const FFunctionExpressionInput& CandidateInput = FunctionCall->FunctionInputs[CandidateIndex];
				const FName CandidateName = CandidateInput.ExpressionInput
					? CandidateInput.ExpressionInput->InputName
					: CandidateInput.Input.InputName;
				if (CandidateName.ToString().Equals(InputDefinition.Name, ESearchCase::IgnoreCase))
				{
					FunctionInputIndex = CandidateIndex;
					break;
				}
			}
			if (FunctionInputIndex == INDEX_NONE && FunctionCall->FunctionInputs.IsValidIndex(InputIndex))
			{
				FunctionInputIndex = InputIndex;
			}
			if (!FunctionCall->FunctionInputs.IsValidIndex(FunctionInputIndex))
			{
				OutError = FString::Printf(TEXT("%s '%s' input '%s' does not exist on MaterialFunction asset '%s'."), *CallKind, *FunctionName, *InputDefinition.Name, *ObjectPath);
				return false;
			}

			if (!InputArgument)
			{
				if (InputDefinition.bOptional)
				{
					continue;
				}

				OutError = FString::Printf(TEXT("%s '%s' is missing required input '%s'."), *CallKind, *FunctionName, *InputDefinition.Name);
				return false;
			}

			if (IsDefaultArgument(InputArgument->Expression))
			{
				if (InputDefinition.bOptional)
				{
					continue;
				}

				OutError = FString::Printf(TEXT("%s '%s' input '%s' is not optional and cannot use default."), *CallKind, *FunctionName, *InputDefinition.Name);
				return false;
			}

			FCodeValue InputValue;
			if (!EvaluateExpression(InputArgument->Expression, InputValue, OutError))
			{
				OutError = FString::Printf(TEXT("%s '%s' input '%s': %s"), *CallKind, *FunctionName, *InputDefinition.Name, *OutError);
				return false;
			}

			int32 ExpectedComponentCount = 1;
			bool bExpectedTexture = false;
			bool bExpectedSubstrate = false;
			ETextShaderTextureType ExpectedTextureType = ETextShaderTextureType::Texture2D;
			if (!TryResolveCodeDeclaredType(InputDefinition.Type, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate))
			{
				if (IsSubstrateTypeUnsupportedForEngine(InputDefinition.Type))
				{
					OutError = FString::Printf(TEXT("%s '%s' input '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *CallKind, *FunctionName, *InputDefinition.Name);
					return false;
				}
				OutError = FString::Printf(TEXT("%s '%s' input '%s' uses unsupported type '%s'."), *CallKind, *FunctionName, *InputDefinition.Name, *InputDefinition.Type);
				return false;
			}

			FCodeValue CoercedValue;
			if (!CoerceValueToType(InputValue, ExpectedComponentCount, bExpectedTexture, ExpectedTextureType, bExpectedSubstrate, CoercedValue, OutError))
			{
				OutError = FString::Printf(TEXT("%s '%s' input '%s': %s"), *CallKind, *FunctionName, *InputDefinition.Name, *OutError);
				return false;
			}

			ConnectCodeValueToInput(FunctionCall->FunctionInputs[FunctionInputIndex].Input, CoercedValue);
		}

		if (PositionalArgumentIndex < PositionalArguments.Num())
		{
			OutError = FString::Printf(
				TEXT("%s '%s' received %d positional input argument(s), but only %d input(s) are declared."),
				*CallKind,
				*FunctionName,
				PositionalArguments.Num(),
				Inputs.Num());
			return false;
		}

		for (const FCodeCallArgument& Argument : InputArguments)
		{
			if (!Argument.bIsNamed)
			{
				continue;
			}

			bool bMatchesInput = false;
			for (const FTextShaderFunctionParameter& Input : Inputs)
			{
				if (Input.Name.Equals(Argument.Name, ESearchCase::IgnoreCase))
				{
					bMatchesInput = true;
					break;
				}
			}

			if (!bMatchesInput)
			{
				OutError = FString::Printf(TEXT("%s '%s' does not have an input named '%s'."), *CallKind, *FunctionName, *Argument.Name);
				return false;
			}
		}

		OutFunctionCall = FunctionCall;
		return true;
	}

	bool FCodeGraphBuilder::ExecuteMaterialFunctionCallAsset(
		const FString& CallKind,
		const FString& FunctionName,
		const FString& ObjectPath,
		const TArray<FTextShaderFunctionParameter>& Inputs,
		const TArray<FTextShaderFunctionParameter>& Outputs,
		const TArray<FCodeCallArgument>& Arguments,
		FString& OutError)
	{
		if (!Values)
		{
			OutError = FString::Printf(TEXT("%s '%s' statement call requires an active Graph build context."), *CallKind, *FunctionName);
			return false;
		}

		if (Outputs.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s '%s' must declare at least one output."), *CallKind, *FunctionName);
			return false;
		}

		for (const FCodeCallArgument& Argument : Arguments)
		{
			if (Argument.bIsNamed)
			{
				OutError = FString::Printf(TEXT("%s '%s' statement calls currently use positional arguments only."), *CallKind, *FunctionName);
				return false;
			}
		}

		if (Arguments.Num() < Outputs.Num())
		{
			OutError = FString::Printf(
				TEXT("%s '%s' expects output target arguments after its inputs, but got %d total argument(s) for %d output(s)."),
				*CallKind,
				*FunctionName,
				Arguments.Num(),
				Outputs.Num());
			return false;
		}

		const int32 InputArgumentCount = Arguments.Num() - Outputs.Num();
		if (InputArgumentCount > Inputs.Num())
		{
			OutError = FString::Printf(
				TEXT("%s '%s' expects at most %d input argument(s) followed by %d output target(s), but got %d input argument(s)."),
				*CallKind,
				*FunctionName,
				Inputs.Num(),
				Outputs.Num(),
				InputArgumentCount);
			return false;
		}

		for (int32 InputIndex = InputArgumentCount; InputIndex < Inputs.Num(); ++InputIndex)
		{
			if (!Inputs[InputIndex].bOptional)
			{
				OutError = FString::Printf(
					TEXT("%s '%s' is missing required input '%s'."),
					*CallKind,
					*FunctionName,
					*Inputs[InputIndex].Name);
				return false;
			}
		}

		TArray<FCodeCallArgument> InputArguments;
		InputArguments.Reserve(InputArgumentCount);
		for (int32 InputIndex = 0; InputIndex < InputArgumentCount; ++InputIndex)
		{
			InputArguments.Add(Arguments[InputIndex]);
		}

		TArray<FString> OutputTargetNames;
		OutputTargetNames.Reserve(Outputs.Num());
		TSet<FString> SeenTargetNames;
		for (int32 OutputIndex = 0; OutputIndex < Outputs.Num(); ++OutputIndex)
		{
			const FCodeCallArgument& Argument = Arguments[InputArgumentCount + OutputIndex];
			if (!Argument.Expression || Argument.Expression->Kind != ECodeExpressionKind::Name)
			{
				OutError = FString::Printf(
					TEXT("%s '%s' output argument %d must be a plain variable name."),
					*CallKind,
					*FunctionName,
					OutputIndex + 1);
				return false;
			}

			const FString TargetName = Argument.Expression->Text.TrimStartAndEnd();
			if (TargetName.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s '%s' has an empty output target name."), *CallKind, *FunctionName);
				return false;
			}

			if (SeenTargetNames.Contains(TargetName))
			{
				OutError = FString::Printf(TEXT("%s '%s' cannot write multiple outputs into '%s' in the same call."), *CallKind, *FunctionName, *TargetName);
				return false;
			}

			SeenTargetNames.Add(TargetName);
			OutputTargetNames.Add(TargetName);
		}

		UMaterialExpressionMaterialFunctionCall* FunctionCall = nullptr;
		if (!CreateAndConnectMaterialFunctionCallAsset(
			CallKind,
			FunctionName,
			ObjectPath,
			Inputs,
			Outputs,
			InputArguments,
			FunctionCall,
			OutError))
		{
			return false;
		}

		for (int32 OutputIndex = 0; OutputIndex < Outputs.Num(); ++OutputIndex)
		{
			int32 OutputComponents = 0;
			bool bIsTextureObject = false;
			ETextShaderTextureType TextureType = ETextShaderTextureType::Texture2D;
			bool bIsSubstrateMaterial = false;
			if (!TryResolveCodeDeclaredType(Outputs[OutputIndex].Type, OutputComponents, bIsTextureObject, TextureType, bIsSubstrateMaterial))
			{
				if (IsSubstrateTypeUnsupportedForEngine(Outputs[OutputIndex].Type))
				{
					OutError = FString::Printf(TEXT("%s '%s' output '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *CallKind, *FunctionName, *Outputs[OutputIndex].Name);
					return false;
				}
				OutError = FString::Printf(TEXT("%s '%s' output '%s' uses unsupported type '%s'."), *CallKind, *FunctionName, *Outputs[OutputIndex].Name, *Outputs[OutputIndex].Type);
				return false;
			}

			int32 FunctionOutputIndex = INDEX_NONE;
			for (int32 CandidateIndex = 0; CandidateIndex < FunctionCall->FunctionOutputs.Num(); ++CandidateIndex)
			{
				const FFunctionExpressionOutput& CandidateOutput = FunctionCall->FunctionOutputs[CandidateIndex];
				const FName CandidateName = CandidateOutput.ExpressionOutput
					? CandidateOutput.ExpressionOutput->OutputName
					: CandidateOutput.Output.OutputName;
				if (CandidateName.ToString().Equals(Outputs[OutputIndex].Name, ESearchCase::IgnoreCase))
				{
					FunctionOutputIndex = CandidateIndex;
					break;
				}
			}
			if (FunctionOutputIndex == INDEX_NONE && FunctionCall->FunctionOutputs.IsValidIndex(OutputIndex))
			{
				FunctionOutputIndex = OutputIndex;
			}
			if (!FunctionCall->FunctionOutputs.IsValidIndex(FunctionOutputIndex))
			{
				OutError = FString::Printf(TEXT("%s '%s' output '%s' does not exist on MaterialFunction asset '%s'."), *CallKind, *FunctionName, *Outputs[OutputIndex].Name, *ObjectPath);
				return false;
			}
			ApplyFunctionCallOutputType(FunctionCall, FunctionOutputIndex, OutputComponents, bIsTextureObject, bIsSubstrateMaterial);

			FCodeValue OutputValue;
			OutputValue.Expression = FunctionCall;
			OutputValue.OutputIndex = FunctionOutputIndex;
			OutputValue.ComponentCount = OutputComponents;
			OutputValue.bIsTextureObject = bIsTextureObject;
			OutputValue.TextureType = TextureType;
			OutputValue.bIsMaterialAttributes = IsMaterialAttributesComponentType(OutputComponents, bIsTextureObject, bIsSubstrateMaterial);
			OutputValue.bIsSubstrateMaterial = bIsSubstrateMaterial;
			(*Values).Add(OutputTargetNames[OutputIndex], OutputValue);
		}

		return true;
	}

	bool FCodeGraphBuilder::EvaluateMaterialFunctionCallAsset(
		const FString& CallKind,
		const FString& FunctionName,
		const FString& ObjectPath,
		const TArray<FTextShaderFunctionParameter>& Inputs,
		const TArray<FTextShaderFunctionParameter>& Outputs,
		const TArray<FCodeCallArgument>& Arguments,
		FCodeValue& OutValue,
		FString& OutError)
	{
		auto TryInlineBreakOutFunction = [&]() -> bool
		{
			if (!CallKind.Equals(TEXT("VirtualFunction"), ESearchCase::IgnoreCase)
				|| !(FunctionName.Equals(TEXT("BreakOutFloat2Components"), ESearchCase::IgnoreCase)
					|| FunctionName.Equals(TEXT("BreakOutFloat3Components"), ESearchCase::IgnoreCase)
					|| FunctionName.Equals(TEXT("BreakOutFloat4Components"), ESearchCase::IgnoreCase)))
			{
				return false;
			}

			const FCodeCallArgument* InputArgument = FindPositionalArgument(Arguments, 0);
			if (!InputArgument && !Inputs.IsEmpty())
			{
				InputArgument = FindNamedArgument(Arguments, *Inputs[0].Name);
			}
			if (!InputArgument || IsDefaultArgument(InputArgument->Expression))
			{
				return false;
			}

			const FCodeCallArgument* OutputArgument = FindNamedArgument(Arguments, TEXT("Output"));
			const FCodeCallArgument* OutputIndexOnlyArgument = FindNamedArgument(Arguments, TEXT("OutputIndex"));
			if (OutputArgument && OutputIndexOnlyArgument)
			{
				return false;
			}

			bool bOutputIndexArgument = false;
			if (!OutputArgument)
			{
				OutputArgument = FindNamedArgument(Arguments, TEXT("OutputName"));
				if (OutputArgument && OutputIndexOnlyArgument)
				{
					return false;
				}
			}
			if (!OutputArgument)
			{
				OutputArgument = OutputIndexOnlyArgument;
				bOutputIndexArgument = OutputArgument != nullptr;
			}
			if (!OutputArgument)
			{
				return false;
			}

			int32 OutputChannelIndex = INDEX_NONE;
			if (bOutputIndexArgument)
			{
				if (!TryExtractIntegerLiteral(OutputArgument->Expression, OutputChannelIndex)
					|| !Outputs.IsValidIndex(OutputChannelIndex))
				{
					return false;
				}
			}
			else
			{
				FString OutputText;
				if (!TryExtractLiteralText(OutputArgument->Expression, OutputText))
				{
					return false;
				}
				OutputText.TrimStartAndEndInline();

				if (ParseIntegerLiteral(OutputText, OutputChannelIndex))
				{
					if (!Outputs.IsValidIndex(OutputChannelIndex))
					{
						return false;
					}
				}
				else
				{
					for (int32 CandidateIndex = 0; CandidateIndex < Outputs.Num(); ++CandidateIndex)
					{
						if (Outputs[CandidateIndex].Name.Equals(OutputText, ESearchCase::IgnoreCase))
						{
							OutputChannelIndex = CandidateIndex;
							break;
						}
					}
				}
			}

			static const TCHAR* Swizzles[] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
			if (OutputChannelIndex < 0 || OutputChannelIndex >= UE_ARRAY_COUNT(Swizzles))
			{
				return false;
			}

			FCodeValue InputValue;
			if (!EvaluateExpression(InputArgument->Expression, InputValue, OutError))
			{
				return true;
			}
			if (!CreateSwizzleExpression(InputValue, Swizzles[OutputChannelIndex], OutValue, OutError))
			{
				return true;
			}
			return true;
		};

		if (TryInlineBreakOutFunction())
		{
			return OutError.IsEmpty();
		}

		const FCodeCallArgument* OutputNameArgument = FindNamedArgument(Arguments, TEXT("Output"));
		if (!OutputNameArgument)
		{
			OutputNameArgument = FindNamedArgument(Arguments, TEXT("OutputName"));
		}
		const FCodeCallArgument* OutputIndexArgument = FindNamedArgument(Arguments, TEXT("OutputIndex"));
		if (OutputNameArgument && OutputIndexArgument)
		{
			OutError = FString::Printf(TEXT("%s '%s' cannot use OutputName/Output together with OutputIndex."), *CallKind, *FunctionName);
			return false;
		}

		TArray<FCodeCallArgument> InputArguments;
		for (const FCodeCallArgument& Argument : Arguments)
		{
			if (Argument.bIsNamed)
			{
				const FString NormalizedName = UE::DreamShader::NormalizeSettingKey(Argument.Name);
				if (NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("Output"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("OutputName"))
					|| NormalizedName == UE::DreamShader::NormalizeSettingKey(TEXT("OutputIndex")))
				{
					continue;
				}
			}

			InputArguments.Add(Argument);
		}

		FString FunctionCallReuseKey;
		FString OutputReuseKey;
		if (TryBuildReusableCallKey(CallKind, FunctionName, InputArguments, FunctionCallReuseKey))
		{
			FunctionCallReuseKey = FString::Printf(TEXT("%s|Asset=%s"), *FunctionCallReuseKey, *ObjectPath);
			if (OutputNameArgument)
			{
				FString OutputNameText;
				if (TryExtractLiteralText(OutputNameArgument->Expression, OutputNameText))
				{
					OutputReuseKey = FunctionCallReuseKey + FString::Printf(TEXT("|OutputName=%s"), *NormalizeCodeReuseLiteralText(OutputNameText));
				}
			}
			else if (OutputIndexArgument)
			{
				FString OutputIndexText;
				if (TryExtractLiteralText(OutputIndexArgument->Expression, OutputIndexText))
				{
					OutputReuseKey = FunctionCallReuseKey + FString::Printf(TEXT("|OutputIndex=%s"), *NormalizeCodeReuseLiteralText(OutputIndexText));
				}
			}
			else if (Outputs.Num() == 1)
			{
				OutputReuseKey = FunctionCallReuseKey + TEXT("|OutputIndex=0");
			}
			if (TryFindReusableExpressionValue(OutputReuseKey, OutValue))
			{
				return true;
			}
		}

		UMaterialExpressionMaterialFunctionCall* FunctionCall = nullptr;
		FCodeValue ReusableFunctionCallValue;
		if (TryFindReusableExpressionValue(FunctionCallReuseKey, ReusableFunctionCallValue))
		{
			FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(ReusableFunctionCallValue.Expression);
		}
		if (!FunctionCall
			&& !CreateAndConnectMaterialFunctionCallAsset(
				CallKind,
				FunctionName,
				ObjectPath,
				Inputs,
				Outputs,
				InputArguments,
				FunctionCall,
				OutError))
		{
			return false;
		}
		if (!FunctionCallReuseKey.IsEmpty() && FunctionCall)
		{
			FCodeValue FunctionCallValue;
			FunctionCallValue.Expression = FunctionCall;
			FunctionCallValue.OutputIndex = 0;
			FunctionCallValue.ComponentCount = 0;
			AddReusableExpressionValue(FunctionCallReuseKey, FunctionCallValue);
		}

		int32 OutputIndex = 0;
		if (OutputIndexArgument)
		{
			if (!TryExtractIntegerLiteral(OutputIndexArgument->Expression, OutputIndex)
				|| OutputIndex < 0
				|| !Outputs.IsValidIndex(OutputIndex))
			{
				OutError = FString::Printf(TEXT("%s '%s' OutputIndex is out of range."), *CallKind, *FunctionName);
				return false;
			}
		}
		else if (OutputNameArgument)
		{
			FString OutputName;
			if (!TryExtractLiteralText(OutputNameArgument->Expression, OutputName))
			{
				OutError = FString::Printf(TEXT("%s '%s' OutputName must be a literal value."), *CallKind, *FunctionName);
				return false;
			}

			OutputIndex = INDEX_NONE;
			for (int32 CandidateIndex = 0; CandidateIndex < Outputs.Num(); ++CandidateIndex)
			{
				if (Outputs[CandidateIndex].Name.Equals(OutputName, ESearchCase::IgnoreCase))
				{
					OutputIndex = CandidateIndex;
					break;
				}
			}

			if (OutputIndex == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("%s '%s' does not expose an output named '%s'."), *CallKind, *FunctionName, *OutputName);
				return false;
			}
		}
		else if (Outputs.Num() != 1)
		{
			OutError = FString::Printf(TEXT("%s '%s' exposes multiple outputs. Specify Output=\"Name\" or OutputIndex=N."), *CallKind, *FunctionName);
			return false;
		}

		int32 OutputComponents = 0;
		bool bIsTextureObject = false;
		ETextShaderTextureType TextureType = ETextShaderTextureType::Texture2D;
		bool bIsSubstrateMaterial = false;
		if (!TryResolveCodeDeclaredType(Outputs[OutputIndex].Type, OutputComponents, bIsTextureObject, TextureType, bIsSubstrateMaterial))
		{
			if (IsSubstrateTypeUnsupportedForEngine(Outputs[OutputIndex].Type))
			{
				OutError = FString::Printf(TEXT("%s '%s' output '%s' uses Substrate, which requires Unreal Engine 5.4 or newer."), *CallKind, *FunctionName, *Outputs[OutputIndex].Name);
				return false;
			}
			OutError = FString::Printf(TEXT("%s '%s' output '%s' uses unsupported type '%s'."), *CallKind, *FunctionName, *Outputs[OutputIndex].Name, *Outputs[OutputIndex].Type);
			return false;
		}

		int32 FunctionOutputIndex = INDEX_NONE;
		for (int32 CandidateIndex = 0; CandidateIndex < FunctionCall->FunctionOutputs.Num(); ++CandidateIndex)
		{
			const FFunctionExpressionOutput& CandidateOutput = FunctionCall->FunctionOutputs[CandidateIndex];
			const FName CandidateName = CandidateOutput.ExpressionOutput
				? CandidateOutput.ExpressionOutput->OutputName
				: CandidateOutput.Output.OutputName;
			if (CandidateName.ToString().Equals(Outputs[OutputIndex].Name, ESearchCase::IgnoreCase))
			{
				FunctionOutputIndex = CandidateIndex;
				break;
			}
		}
		if (FunctionOutputIndex == INDEX_NONE && FunctionCall->FunctionOutputs.IsValidIndex(OutputIndex))
		{
			FunctionOutputIndex = OutputIndex;
		}
		if (!FunctionCall->FunctionOutputs.IsValidIndex(FunctionOutputIndex))
		{
			OutError = FString::Printf(TEXT("%s '%s' output '%s' does not exist on MaterialFunction asset '%s'."), *CallKind, *FunctionName, *Outputs[OutputIndex].Name, *ObjectPath);
			return false;
		}
		ApplyFunctionCallOutputType(FunctionCall, FunctionOutputIndex, OutputComponents, bIsTextureObject, bIsSubstrateMaterial);

		OutValue.Expression = FunctionCall;
		OutValue.OutputIndex = FunctionOutputIndex;
		OutValue.ComponentCount = OutputComponents;
		OutValue.bIsTextureObject = bIsTextureObject;
		OutValue.TextureType = TextureType;
		OutValue.bIsMaterialAttributes = IsMaterialAttributesComponentType(OutputComponents, bIsTextureObject, bIsSubstrateMaterial);
		OutValue.bIsSubstrateMaterial = bIsSubstrateMaterial;
		AddReusableExpressionValue(OutputReuseKey, OutValue);
		return true;
	}

	FString FCodeGraphBuilder::BuildFunctionArgumentList(const FTextShaderFunctionDefinition& Function, const TArray<FString>& ResultVariableNames)
	{
		TArray<FString> Parameters;
		for (const FTextShaderFunctionParameter& Input : Function.Inputs)
		{
			Parameters.Add(Input.Name);
			if (IsTextureFunctionParameterType(Input.Type))
			{
				Parameters.Add(Input.Name + TEXT("Sampler"));
			}
		}
		for (int32 ResultIndex = 1; ResultIndex < Function.Results.Num(); ++ResultIndex)
		{
			if (ResultVariableNames.IsValidIndex(ResultIndex - 1))
			{
				Parameters.Add(ResultVariableNames[ResultIndex - 1]);
			}
		}
		return FString::Join(Parameters, TEXT(", "));
	}
}
