#include "DreamShaderMaterialGeneratorCodeShared.h"

namespace UE::DreamShader::Editor::Private
{
	FCodeGraphBuilder::FCodeGraphBuilder(
		UMaterial* InMaterial,
		UMaterialFunction* InMaterialFunction,
		const FTextShaderDefinition& InDefinition,
		const FString& InSourceFilePath,
		const FString& InIncludeVirtualPath,
		const TArray<FTextShaderPropertyDefinition>* InLocalProperties)
		: Material(InMaterial)
		, MaterialFunction(InMaterialFunction)
		, Definition(InDefinition)
		, LocalProperties(InLocalProperties)
		, SourceFilePath(InSourceFilePath)
		, IncludeVirtualPath(InIncludeVirtualPath)
	{
	}

	bool FCodeGraphBuilder::Build(
		const TArray<FCodeStatement>& Statements,
		TMap<FString, FCodeValue>& InOutValues,
		FString& OutError)
	{
		Values = &InOutValues;

		for (const FCodeStatement& Statement : Statements)
		{
			if (!ExecuteStatement(Statement, OutError))
			{
				return false;
			}
		}

		return true;
	}

	bool FCodeGraphBuilder::ExecuteStatement(const FCodeStatement& Statement, FString& OutError)
	{
		if (Statement.bIsIfStatement)
		{
			return ExecuteIfStatement(Statement, OutError);
		}

		if (!Statement.Expression && !Statement.bIsDeclaration && !Statement.bUsesBraceInitializer)
		{
			OutError = TEXT("Encountered an invalid empty Graph statement.");
			return false;
		}

		if (Statement.bIsExpressionStatement)
		{
			return ExecuteExpressionStatement(Statement.Expression, OutError);
		}

		if (Statement.TargetName.IsEmpty())
		{
			OutError = TEXT("Encountered a Graph assignment without a target variable.");
			return false;
		}

		if (!Statement.bIsDeclaration)
		{
			FString MemberBaseName;
			FString MemberName;
			if (TrySplitMemberTarget(Statement.TargetName, MemberBaseName, MemberName))
			{
				FCodeValue EvaluatedMemberValue;
				if (Statement.bUsesBraceInitializer)
				{
					FString TargetTypeName;
					if (!ResolveTargetTypeForAssignment(Statement, TargetTypeName, OutError)
						|| !EvaluateBraceInitializer(TargetTypeName, Statement.InitializerText, EvaluatedMemberValue, OutError))
					{
						OutError = FString::Printf(TEXT("Failed to evaluate Graph assignment for '%s'. %s"), *Statement.TargetName, *OutError);
						return false;
					}
				}
				else if (Statement.Expression)
				{
					if (!EvaluateExpression(Statement.Expression, EvaluatedMemberValue, OutError))
					{
						OutError = FString::Printf(TEXT("Failed to evaluate Graph assignment for '%s'. %s"), *Statement.TargetName, *OutError);
						return false;
					}
				}
				else
				{
					OutError = FString::Printf(TEXT("MaterialAttributes member assignment '%s' requires a value."), *Statement.TargetName);
					return false;
				}

				if (!AssignMaterialAttributesMember(Statement.TargetName, EvaluatedMemberValue, OutError))
				{
					OutError = FString::Printf(TEXT("Failed to assign Graph member '%s'. %s"), *Statement.TargetName, *OutError);
					return false;
				}

				return true;
			}
		}

		if (Statement.bIsDeclaration && FindValue(Statement.TargetName))
		{
			OutError = FString::Printf(TEXT("Graph variable '%s' is declared more than once."), *Statement.TargetName);
			return false;
		}

		FCodeValue EvaluatedValue;
		if (Statement.bUsesBraceInitializer)
		{
			FString TargetTypeName;
			if (!ResolveTargetTypeForAssignment(Statement, TargetTypeName, OutError)
				|| !EvaluateBraceInitializer(TargetTypeName, Statement.InitializerText, EvaluatedValue, OutError))
			{
				OutError = FString::Printf(TEXT("Failed to evaluate Graph assignment for '%s'. %s"), *Statement.TargetName, *OutError);
				return false;
			}
		}
		else if (Statement.Expression)
		{
			if (!EvaluateExpression(Statement.Expression, EvaluatedValue, OutError))
			{
				OutError = FString::Printf(TEXT("Failed to evaluate Graph assignment for '%s'. %s"), *Statement.TargetName, *OutError);
				return false;
			}
		}
		else if (Statement.bIsDeclaration)
		{
			if (!CreateDefaultValue(Statement.DeclaredType, EvaluatedValue, OutError))
			{
				OutError = FString::Printf(TEXT("Failed to declare Graph variable '%s'. %s"), *Statement.TargetName, *OutError);
				return false;
			}
		}

		if (Statement.bIsDeclaration)
		{
			int32 ExpectedComponentCount = 1;
			bool bExpectedTexture = false;
			if (!TryResolveCodeDeclaredType(Statement.DeclaredType, ExpectedComponentCount, bExpectedTexture))
			{
				OutError = FString::Printf(TEXT("Unsupported Graph variable type '%s' for '%s'."), *Statement.DeclaredType, *Statement.TargetName);
				return false;
			}

			FCodeValue CoercedValue;
			if (!CoerceValueToType(EvaluatedValue, ExpectedComponentCount, bExpectedTexture, CoercedValue, OutError))
			{
				OutError = FString::Printf(
					TEXT("Graph variable '%s' is declared as '%s' but assigned an incompatible value. %s"),
					*Statement.TargetName,
					*Statement.DeclaredType,
					*OutError);
				return false;
			}

			EvaluatedValue = CoercedValue;
		}
		else if (const FCodeValue* ExistingValue = FindValue(Statement.TargetName))
		{
			FCodeValue CoercedValue;
			if (!CoerceValueToType(EvaluatedValue, ExistingValue->ComponentCount, ExistingValue->bIsTextureObject, CoercedValue, OutError))
			{
				OutError = FString::Printf(
					TEXT("Graph variable '%s' was previously assigned an incompatible value. %s"),
					*Statement.TargetName,
					*OutError);
				return false;
			}

			EvaluatedValue = CoercedValue;
		}

		(*Values).Add(Statement.TargetName, EvaluatedValue);
		return true;
	}

	static bool AreCodeValuesEquivalent(const FCodeValue& Left, const FCodeValue& Right)
	{
		return Left.Expression == Right.Expression
			&& Left.OutputIndex == Right.OutputIndex
			&& Left.ComponentCount == Right.ComponentCount
			&& Left.bIsTextureObject == Right.bIsTextureObject
			&& Left.bIsMaterialAttributes == Right.bIsMaterialAttributes
			&& Left.bIsSubstrate == Right.bIsSubstrate;
	}

	static void CollectChangedValueNames(
		const TMap<FString, FCodeValue>& BaseValues,
		const TMap<FString, FCodeValue>& BranchValues,
		TSet<FString>& OutNames)
	{
		for (const TPair<FString, FCodeValue>& Pair : BranchValues)
		{
			const FCodeValue* BaseValue = BaseValues.Find(Pair.Key);
			if (!BaseValue || !AreCodeValuesEquivalent(*BaseValue, Pair.Value))
			{
				OutNames.Add(Pair.Key);
			}
		}
	}

	bool FCodeGraphBuilder::ExecuteIfStatement(const FCodeStatement& Statement, FString& OutError)
	{
		if (!Values)
		{
			OutError = TEXT("Graph builder is not initialized.");
			return false;
		}

		TMap<FString, FCodeValue>* OuterValues = Values;
		const TMap<FString, FCodeValue> BaseValues = *OuterValues;

		TMap<FString, FCodeValue> ThenValues = BaseValues;
		Values = &ThenValues;
		for (const FCodeStatement& ThenStatement : Statement.ThenStatements)
		{
			if (!ExecuteStatement(ThenStatement, OutError))
			{
				Values = OuterValues;
				OutError = FString::Printf(TEXT("In Graph if body: %s"), *OutError);
				return false;
			}
		}

		TMap<FString, FCodeValue> ElseValues = BaseValues;
		Values = &ElseValues;
		for (const FCodeStatement& ElseStatement : Statement.ElseStatements)
		{
			if (!ExecuteStatement(ElseStatement, OutError))
			{
				Values = OuterValues;
				OutError = FString::Printf(TEXT("In Graph else body: %s"), *OutError);
				return false;
			}
		}

		Values = OuterValues;

		TSet<FString> ChangedNames;
		CollectChangedValueNames(BaseValues, ThenValues, ChangedNames);
		CollectChangedValueNames(BaseValues, ElseValues, ChangedNames);

		for (const FString& Name : ChangedNames)
		{
			const FCodeValue* ThenValue = ThenValues.Find(Name);
			const FCodeValue* ElseValue = ElseValues.Find(Name);
			if (!ThenValue || !ElseValue)
			{
				OutError = FString::Printf(TEXT("Graph if statement could not resolve both branch values for '%s'."), *Name);
				return false;
			}

			int32 ExpectedComponentCount = ThenValue->ComponentCount;
			bool bExpectedTexture = ThenValue->bIsTextureObject;
			if (const FCodeValue* BaseValue = BaseValues.Find(Name))
			{
				ExpectedComponentCount = BaseValue->ComponentCount;
				bExpectedTexture = BaseValue->bIsTextureObject;
			}
			else
			{
				int32 OutputComponentCount = 0;
				bool bOutputIsTexture = false;
				if (TryResolveOutputVariableComponentCount(Definition, Name, OutputComponentCount, bOutputIsTexture))
				{
					ExpectedComponentCount = OutputComponentCount;
					bExpectedTexture = bOutputIsTexture;
				}
			}

			if (bExpectedTexture)
			{
				OutError = FString::Printf(TEXT("Graph if statement cannot select Texture2D value '%s'."), *Name);
				return false;
			}
			if (IsSubstrateComponentType(ExpectedComponentCount, bExpectedTexture))
			{
				OutError = FString::Printf(TEXT("Graph if statement cannot select Substrate value '%s'. Use UE.SubstrateSelect for Substrate graphs."), *Name);
				return false;
			}

			FCodeValue CoercedThenValue;
			FCodeValue CoercedElseValue;
			if (!CoerceValueToType(*ThenValue, ExpectedComponentCount, bExpectedTexture, CoercedThenValue, OutError)
				|| !CoerceValueToType(*ElseValue, ExpectedComponentCount, bExpectedTexture, CoercedElseValue, OutError))
			{
				OutError = FString::Printf(TEXT("Graph if branches assign incompatible values to '%s'. %s"), *Name, *OutError);
				return false;
			}

			FCodeValue ConditionalValue;
			if (!CreateConditionalValue(Statement.Condition, CoercedThenValue, CoercedElseValue, ConditionalValue, OutError))
			{
				OutError = FString::Printf(TEXT("Graph if statement failed to merge '%s'. %s"), *Name, *OutError);
				return false;
			}

			(*Values).Add(Name, ConditionalValue);
		}

		return true;
	}

	bool FCodeGraphBuilder::CreateConditionalValue(
		const FCodeCondition& Condition,
		const FCodeValue& TrueValue,
		const FCodeValue& FalseValue,
		FCodeValue& OutValue,
		FString& OutError)
	{
		if (TrueValue.bIsTextureObject || FalseValue.bIsTextureObject)
		{
			OutError = TEXT("Texture2D values cannot be selected by Graph if statements.");
			return false;
		}
		if (TrueValue.bIsSubstrate || FalseValue.bIsSubstrate)
		{
			OutError = TEXT("Substrate values cannot be selected by Graph if statements. Use UE.SubstrateSelect for Substrate graphs.");
			return false;
		}
		if (TrueValue.bIsMaterialAttributes != FalseValue.bIsMaterialAttributes)
		{
			OutError = TEXT("Graph if branches cannot mix MaterialAttributes and numeric values.");
			return false;
		}

		FCodeValue LeftValue;
		if (!EvaluateExpression(Condition.Left, LeftValue, OutError))
		{
			OutError = FString::Printf(TEXT("Failed to evaluate Graph if condition. %s"), *OutError);
			return false;
		}

		if (LeftValue.bIsTextureObject || LeftValue.bIsMaterialAttributes || LeftValue.bIsSubstrate || LeftValue.ComponentCount != 1)
		{
			OutError = TEXT("Graph if condition left side must evaluate to a scalar value.");
			return false;
		}

		FCodeValue RightValue;
		if (Condition.Operator == TEXT("truthy"))
		{
			RightValue.Expression = CreateScalarLiteralNode(0.0, ConsumeNodeY());
			if (!RightValue.Expression)
			{
				OutError = TEXT("Failed to create a zero literal for Graph if condition.");
				return false;
			}
			RightValue.ComponentCount = 1;
		}
		else
		{
			if (!EvaluateExpression(Condition.Right, RightValue, OutError))
			{
				OutError = FString::Printf(TEXT("Failed to evaluate Graph if condition. %s"), *OutError);
				return false;
			}
		}

		if (RightValue.bIsTextureObject || RightValue.bIsMaterialAttributes || RightValue.bIsSubstrate || RightValue.ComponentCount != 1)
		{
			OutError = TEXT("Graph if condition right side must evaluate to a scalar value.");
			return false;
		}

		auto* IfExpression = Cast<UMaterialExpressionIf>(
			CreateExpression(UMaterialExpressionIf::StaticClass(), 520, ConsumeNodeY()));
		if (!IfExpression)
		{
			OutError = TEXT("Failed to create a Material If node.");
			return false;
		}

		ConnectCodeValueToInput(IfExpression->A, LeftValue);
		ConnectCodeValueToInput(IfExpression->B, RightValue);

		const auto ConnectBranches = [&](const FCodeValue& GreaterValue, const FCodeValue& EqualValue, const FCodeValue& LessValue)
		{
			ConnectCodeValueToInput(IfExpression->AGreaterThanB, GreaterValue);
			ConnectCodeValueToInput(IfExpression->AEqualsB, EqualValue);
			ConnectCodeValueToInput(IfExpression->ALessThanB, LessValue);
		};

		if (Condition.Operator == TEXT("truthy") || Condition.Operator == TEXT(">"))
		{
			ConnectBranches(TrueValue, FalseValue, FalseValue);
		}
		else if (Condition.Operator == TEXT("<"))
		{
			ConnectBranches(FalseValue, FalseValue, TrueValue);
		}
		else if (Condition.Operator == TEXT(">="))
		{
			ConnectBranches(TrueValue, TrueValue, FalseValue);
		}
		else if (Condition.Operator == TEXT("<="))
		{
			ConnectBranches(FalseValue, TrueValue, TrueValue);
		}
		else if (Condition.Operator == TEXT("=="))
		{
			ConnectBranches(FalseValue, TrueValue, FalseValue);
		}
		else if (Condition.Operator == TEXT("!="))
		{
			ConnectBranches(TrueValue, FalseValue, TrueValue);
		}
		else
		{
			OutError = FString::Printf(TEXT("Unsupported Graph if comparison operator '%s'."), *Condition.Operator);
			return false;
		}

		OutValue.Expression = IfExpression;
		OutValue.OutputIndex = 0;
		OutValue.ComponentCount = TrueValue.ComponentCount;
		OutValue.bIsTextureObject = false;
		OutValue.bIsMaterialAttributes = TrueValue.bIsMaterialAttributes;
		OutValue.bIsSubstrate = false;
		return true;
	}

	bool FCodeGraphBuilder::EvaluateOutputExpression(const FString& ExpressionText, FCodeValue& OutValue, FString& OutError)
	{
		TSharedPtr<FCodeExpression> ParsedExpression;
		if (!ParseCodeExpression(ExpressionText, ParsedExpression, OutError))
		{
			OutError = FString::Printf(TEXT("In output expression '%s': %s"), *ExpressionText, *OutError);
			return false;
		}

		if (!EvaluateExpression(ParsedExpression, OutValue, OutError))
		{
			OutError = FString::Printf(TEXT("In output expression '%s': %s"), *ExpressionText, *OutError);
			return false;
		}

		return true;
	}

	FCodeValue* FCodeGraphBuilder::FindValue(const FString& Name) const
	{
		if (!Values)
		{
			return nullptr;
		}

		if (FCodeValue* ExactMatch = Values->Find(Name))
		{
			return ExactMatch;
		}

		for (TPair<FString, FCodeValue>& Pair : *Values)
		{
			if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase))
			{
				return &Pair.Value;
			}
		}

		return nullptr;
	}

	int32 FCodeGraphBuilder::ConsumeNodeY()
	{
		const int32 Result = NextNodeY;
		NextNodeY += 180;
		return Result;
	}

	UMaterialExpression* FCodeGraphBuilder::CreateExpression(
		const TSubclassOf<UMaterialExpression> ExpressionClass,
		const int32 PositionX,
		const int32 PositionY) const
	{
		return UMaterialEditingLibrary::CreateMaterialExpressionEx(
			Material,
			MaterialFunction,
			ExpressionClass,
			nullptr,
			PositionX,
			PositionY);
	}

	UMaterialExpression* FCodeGraphBuilder::CreateScalarLiteralNode(const double Value, const int32 PositionY) const
	{
		auto* Expression = Cast<UMaterialExpressionConstant>(
			CreateExpression(UMaterialExpressionConstant::StaticClass(), -1120, PositionY));
		if (Expression)
		{
			Expression->R = static_cast<float>(Value);
		}
		return Expression;
	}

	bool FCodeGraphBuilder::CreateMaterialAttributesValue(FCodeValue& OutValue, FString& OutError)
	{
		auto* Expression = Cast<UMaterialExpressionMakeMaterialAttributes>(
			CreateExpression(UMaterialExpressionMakeMaterialAttributes::StaticClass(), 200, ConsumeNodeY()));
		if (!Expression)
		{
			OutError = TEXT("Failed to create a MakeMaterialAttributes node.");
			return false;
		}

		OutValue = FCodeValue{};
		OutValue.Expression = Expression;
		OutValue.OutputIndex = 0;
		OutValue.ComponentCount = 0;
		OutValue.bIsTextureObject = false;
		OutValue.bIsMaterialAttributes = true;
		return true;
	}

	bool FCodeGraphBuilder::CreateDefaultValue(const FString& DeclaredType, FCodeValue& OutValue, FString& OutError)
	{
		int32 ComponentCount = 1;
		bool bIsTexture = false;
		if (!TryResolveCodeDeclaredType(DeclaredType, ComponentCount, bIsTexture))
		{
			OutError = FString::Printf(TEXT("Unsupported Graph variable type '%s'."), *DeclaredType);
			return false;
		}

		if (IsMaterialAttributesComponentType(ComponentCount, bIsTexture))
		{
			return CreateMaterialAttributesValue(OutValue, OutError);
		}

		if (IsSubstrateComponentType(ComponentCount, bIsTexture))
		{
			OutError = FString::Printf(TEXT("Graph variable type '%s' requires an explicit initializer."), *DeclaredType);
			return false;
		}

		if (bIsTexture)
		{
			OutError = FString::Printf(TEXT("Graph variable type '%s' requires an explicit initializer."), *DeclaredType);
			return false;
		}

		FCodeValue ZeroScalar;
		ZeroScalar.Expression = CreateScalarLiteralNode(0.0, ConsumeNodeY());
		if (!ZeroScalar.Expression)
		{
			OutError = TEXT("Failed to create a default literal node.");
			return false;
		}
		ZeroScalar.ComponentCount = 1;

		if (ComponentCount == 1)
		{
			OutValue = ZeroScalar;
			return true;
		}

		TArray<FCodeValue> Parts;
		for (int32 Index = 0; Index < ComponentCount; ++Index)
		{
			Parts.Add(ZeroScalar);
		}

		if (!AppendValues(Parts, OutValue, OutError))
		{
			return false;
		}

		OutValue.ComponentCount = ComponentCount;
		return true;
	}

	bool FCodeGraphBuilder::CoerceValueToType(
		const FCodeValue& InValue,
		const int32 ExpectedComponentCount,
		const bool bExpectedTexture,
		FCodeValue& OutValue,
		FString& OutError)
	{
		if (IsMaterialAttributesComponentType(ExpectedComponentCount, bExpectedTexture))
		{
			if (!InValue.bIsMaterialAttributes)
			{
				OutError = TEXT("Expected a MaterialAttributes value.");
				return false;
			}

			OutValue = InValue;
			return true;
		}

		if (IsSubstrateComponentType(ExpectedComponentCount, bExpectedTexture))
		{
			if (!InValue.bIsSubstrate)
			{
				OutError = TEXT("Expected a Substrate value.");
				return false;
			}

			OutValue = InValue;
			return true;
		}

		if (bExpectedTexture)
		{
			if (!InValue.bIsTextureObject)
			{
				OutError = TEXT("Expected a texture object value.");
				return false;
			}

			OutValue = InValue;
			return true;
		}

		if (InValue.bIsMaterialAttributes)
		{
			OutError = TEXT("MaterialAttributes values cannot be assigned to numeric outputs.");
			return false;
		}

		if (InValue.bIsSubstrate)
		{
			OutError = TEXT("Substrate values cannot be assigned to numeric outputs.");
			return false;
		}

		if (InValue.bIsTextureObject)
		{
			OutError = TEXT("Texture objects cannot be assigned to numeric outputs.");
			return false;
		}

		if (InValue.ComponentCount == ExpectedComponentCount)
		{
			OutValue = InValue;
			return true;
		}

		if (ExpectedComponentCount > 0 && InValue.ComponentCount > ExpectedComponentCount)
		{
			static const TCHAR* LeadingSwizzles[] = { TEXT(""), TEXT("r"), TEXT("rg"), TEXT("rgb"), TEXT("rgba") };
			check(ExpectedComponentCount < UE_ARRAY_COUNT(LeadingSwizzles));
			return CreateSwizzleExpression(InValue, LeadingSwizzles[ExpectedComponentCount], OutValue, OutError);
		}

		if (ExpectedComponentCount > 1 && InValue.ComponentCount == 1)
		{
			TArray<FCodeValue> ReplicatedParts;
			ReplicatedParts.Reserve(ExpectedComponentCount);
			for (int32 Index = 0; Index < ExpectedComponentCount; ++Index)
			{
				ReplicatedParts.Add(InValue);
			}

			if (!AppendValues(ReplicatedParts, OutValue, OutError))
			{
				return false;
			}

			OutValue.ComponentCount = ExpectedComponentCount;
			return true;
		}

		OutError = FString::Printf(
			TEXT("Expected %d component(s) but got %d."),
			ExpectedComponentCount,
			InValue.ComponentCount);
		return false;
	}

	bool FCodeGraphBuilder::EvaluateBraceInitializer(
		const FString& ConstructorType,
		const FString& InitializerText,
		FCodeValue& OutValue,
		FString& OutError)
	{
		const FString Trimmed = InitializerText.TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("{")) || !Trimmed.EndsWith(TEXT("}")))
		{
			OutError = FString::Printf(TEXT("Initializer '%s' is not a valid brace initializer."), *InitializerText);
			return false;
		}

		const FString InnerText = Trimmed.Mid(1, Trimmed.Len() - 2).TrimStartAndEnd();
		if (InnerText.IsEmpty())
		{
			return CreateDefaultValue(ConstructorType, OutValue, OutError);
		}

		const FString ConstructorExpression = FString::Printf(TEXT("%s(%s)"), *ConstructorType, *InnerText);
		TSharedPtr<FCodeExpression> ParsedExpression;
		if (!ParseCodeExpression(ConstructorExpression, ParsedExpression, OutError))
		{
			OutError = FString::Printf(TEXT("Invalid brace initializer for type '%s'. %s"), *ConstructorType, *OutError);
			return false;
		}

		return EvaluateExpression(ParsedExpression, OutValue, OutError);
	}

	bool FCodeGraphBuilder::ResolveTargetTypeForAssignment(
		const FCodeStatement& Statement,
		FString& OutTypeName,
		FString& OutError) const
	{
		if (Statement.bIsDeclaration)
		{
			OutTypeName = Statement.DeclaredType;
			return true;
		}

		FString BaseName;
		FString MemberName;
		if (TrySplitMemberTarget(Statement.TargetName, BaseName, MemberName))
		{
			int32 MemberComponentCount = 0;
			return ResolveMaterialAttributesMemberType(MemberName, MemberComponentCount, OutTypeName, OutError);
		}

		if (const FCodeValue* ExistingValue = FindValue(Statement.TargetName))
		{
		if (ExistingValue->bIsTextureObject)
		{
			OutError = FString::Printf(TEXT("Brace initializer assignment is not supported for texture variable '%s'."), *Statement.TargetName);
			return false;
		}

			if (ResolveTypeNameForComponentCount(ExistingValue->ComponentCount, OutTypeName))
			{
				return true;
			}
		}

		int32 OutputComponentCount = 1;
		bool bOutputIsTexture = false;
		if (TryResolveOutputVariableComponentCount(Definition, Statement.TargetName, OutputComponentCount, bOutputIsTexture))
		{
			if (bOutputIsTexture)
			{
				OutError = FString::Printf(TEXT("Brace initializer assignment is not supported for texture output '%s'."), *Statement.TargetName);
				return false;
			}

			if (ResolveTypeNameForComponentCount(OutputComponentCount, OutTypeName))
			{
				return true;
			}
		}

		OutError = FString::Printf(TEXT("Brace initializer assignment for '%s' requires a declared scalar or vector target type."), *Statement.TargetName);
		return false;
	}

	bool FCodeGraphBuilder::ResolveMaterialAttributesMemberType(
		const FString& MemberName,
		int32& OutComponentCount,
		FString& OutTypeName,
		FString& OutError) const
	{
		FResolvedMaterialProperty ResolvedProperty;
		if (!ResolveMaterialProperty(MemberName, ResolvedProperty)
			|| ResolvedProperty.Property == MP_MaterialAttributes)
		{
			OutError = FString::Printf(TEXT("Unsupported MaterialAttributes member '%s'."), *MemberName);
			return false;
		}

		if (!TryGetComponentCountForOutputType(ResolvedProperty.OutputType, OutComponentCount)
			|| OutComponentCount <= 0
			|| !ResolveTypeNameForComponentCount(OutComponentCount, OutTypeName))
		{
			OutError = FString::Printf(TEXT("MaterialAttributes member '%s' does not have a numeric scalar/vector type."), *MemberName);
			return false;
		}

		return true;
	}

	bool FCodeGraphBuilder::AssignMaterialAttributesMember(const FString& TargetName, const FCodeValue& InValue, FString& OutError)
	{
		FString BaseName;
		FString MemberName;
		if (!TrySplitMemberTarget(TargetName, BaseName, MemberName))
		{
			OutError = FString::Printf(TEXT("Invalid MaterialAttributes member assignment target '%s'."), *TargetName);
			return false;
		}

		FCodeValue* BaseValue = FindValue(BaseName);
		if (!BaseValue)
		{
			OutError = FString::Printf(TEXT("Unknown MaterialAttributes variable '%s'."), *BaseName);
			return false;
		}
		if (!BaseValue->bIsMaterialAttributes)
		{
			OutError = FString::Printf(TEXT("Graph variable '%s' is not a MaterialAttributes value."), *BaseName);
			return false;
		}

		FResolvedMaterialProperty ResolvedProperty;
		if (!ResolveMaterialProperty(MemberName, ResolvedProperty)
			|| ResolvedProperty.Property == MP_MaterialAttributes)
		{
			OutError = FString::Printf(TEXT("Unsupported MaterialAttributes member '%s'."), *MemberName);
			return false;
		}

		int32 ExpectedComponentCount = 0;
		if (!TryGetComponentCountForOutputType(ResolvedProperty.OutputType, ExpectedComponentCount) || ExpectedComponentCount <= 0)
		{
			OutError = FString::Printf(TEXT("MaterialAttributes member '%s' cannot be assigned from Graph code."), *MemberName);
			return false;
		}

		FCodeValue CoercedValue;
		if (!CoerceValueToType(InValue, ExpectedComponentCount, false, CoercedValue, OutError))
		{
			OutError = FString::Printf(
				TEXT("MaterialAttributes member '%s' expects %d component(s). %s"),
				*MemberName,
				ExpectedComponentCount,
				*OutError);
			return false;
		}

		UMaterialExpressionSetMaterialAttributes* SetAttributes = Cast<UMaterialExpressionSetMaterialAttributes>(
			CreateExpression(UMaterialExpressionSetMaterialAttributes::StaticClass(), 320, ConsumeNodeY()));
		if (!SetAttributes)
		{
			OutError = TEXT("Failed to create a SetMaterialAttributes node.");
			return false;
		}

		if (!SetAttributes->ConnectInputAttribute(MP_MaterialAttributes, BaseValue->Expression, BaseValue->OutputIndex))
		{
			OutError = FString::Printf(TEXT("Failed to connect '%s' as the SetMaterialAttributes base value."), *BaseName);
			return false;
		}

		if (!SetAttributes->ConnectInputAttribute(ResolvedProperty.Property, CoercedValue.Expression, CoercedValue.OutputIndex))
		{
			OutError = FString::Printf(TEXT("Failed to connect MaterialAttributes member '%s'."), *MemberName);
			return false;
		}

		BaseValue->Expression = SetAttributes;
		BaseValue->OutputIndex = 0;
		BaseValue->ComponentCount = 0;
		BaseValue->bIsTextureObject = false;
		BaseValue->bIsMaterialAttributes = true;

		return true;
	}

	bool FCodeGraphBuilder::TryFlattenQualifiedName(const TSharedPtr<FCodeExpression>& Expression, FString& OutName)
	{
		if (!Expression)
		{
			return false;
		}

		if (Expression->Kind == ECodeExpressionKind::Name)
		{
			OutName = Expression->Text;
			return true;
		}

		if (Expression->Kind == ECodeExpressionKind::MemberAccess)
		{
			FString LeftName;
			if (!TryFlattenQualifiedName(Expression->Left, LeftName))
			{
				return false;
			}

			OutName = Expression->Text.StartsWith(TEXT("::"))
				? LeftName + Expression->Text
				: LeftName + TEXT(".") + Expression->Text;
			return true;
		}

		return false;
	}

	bool FCodeGraphBuilder::TryExtractTextLiteral(const TSharedPtr<FCodeExpression>& Expression, FString& OutText) const
	{
		if (!Expression)
		{
			return false;
		}

		if (Expression->Kind == ECodeExpressionKind::StringLiteral)
		{
			OutText = Expression->Text;
			return true;
		}

		if (TryFlattenQualifiedName(Expression, OutText))
		{
			return true;
		}

		return false;
	}

	bool FCodeGraphBuilder::TryExtractLiteralText(const TSharedPtr<FCodeExpression>& Expression, FString& OutText) const
	{
		if (!Expression)
		{
			return false;
		}

		if (Expression->Kind == ECodeExpressionKind::NumberLiteral)
		{
			OutText = Expression->Text;
			return true;
		}

		if (Expression->Kind == ECodeExpressionKind::Unary
			&& (Expression->Text == TEXT("+") || Expression->Text == TEXT("-")))
		{
			FString InnerText;
			if (!TryExtractLiteralText(Expression->Left, InnerText))
			{
				return false;
			}

			OutText = Expression->Text + InnerText;
			return true;
		}

		return TryExtractTextLiteral(Expression, OutText);
	}

	bool FCodeGraphBuilder::TryExtractAssetReferenceText(const TSharedPtr<FCodeExpression>& Expression, FString& OutText) const
	{
		if (!Expression)
		{
			return false;
		}

		if (TryExtractLiteralText(Expression, OutText))
		{
			return true;
		}

		if (Expression->Kind != ECodeExpressionKind::Call)
		{
			return false;
		}

		FString CalleeName;
		if (!TryFlattenQualifiedName(Expression->Left, CalleeName)
			|| !CalleeName.Equals(TEXT("Path"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		TArray<FString> Parts;
		for (const FCodeCallArgument& Argument : Expression->Arguments)
		{
			if (Argument.bIsNamed)
			{
				return false;
			}

			if (Argument.Expression && Argument.Expression->Kind == ECodeExpressionKind::StringLiteral)
			{
				FString Escaped = Argument.Expression->Text;
				Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
				Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
				Parts.Add(FString::Printf(TEXT("\"%s\""), *Escaped));
				continue;
			}

			FString LiteralText;
			if (!TryExtractLiteralText(Argument.Expression, LiteralText))
			{
				return false;
			}
			Parts.Add(LiteralText);
		}

		OutText = FString::Printf(TEXT("Path(%s)"), *FString::Join(Parts, TEXT(", ")));
		return true;
	}

	bool FCodeGraphBuilder::TryExtractScalarLiteral(const TSharedPtr<FCodeExpression>& Expression, double& OutValue) const
	{
		if (!Expression)
		{
			return false;
		}

		if (Expression->Kind == ECodeExpressionKind::NumberLiteral)
		{
			return ParseScalarLiteral(Expression->Text, OutValue);
		}

		if (Expression->Kind == ECodeExpressionKind::Unary
			&& (Expression->Text == TEXT("+") || Expression->Text == TEXT("-")))
		{
			double InnerValue = 0.0;
			if (!TryExtractScalarLiteral(Expression->Left, InnerValue))
			{
				return false;
			}

			OutValue = (Expression->Text == TEXT("-")) ? -InnerValue : InnerValue;
			return true;
		}

		FString TextValue;
		return TryExtractTextLiteral(Expression, TextValue) && ParseScalarLiteral(TextValue, OutValue);
	}

	bool FCodeGraphBuilder::TryExtractIntegerLiteral(const TSharedPtr<FCodeExpression>& Expression, int32& OutValue) const
	{
		if (!Expression)
		{
			return false;
		}

		if (Expression->Kind == ECodeExpressionKind::NumberLiteral)
		{
			return ParseIntegerLiteral(Expression->Text, OutValue);
		}

		if (Expression->Kind == ECodeExpressionKind::Unary
			&& (Expression->Text == TEXT("+") || Expression->Text == TEXT("-")))
		{
			int32 InnerValue = 0;
			if (!TryExtractIntegerLiteral(Expression->Left, InnerValue))
			{
				return false;
			}

			OutValue = (Expression->Text == TEXT("-")) ? -InnerValue : InnerValue;
			return true;
		}

		FString TextValue;
		return TryExtractTextLiteral(Expression, TextValue) && ParseIntegerLiteral(TextValue, OutValue);
	}

	bool FCodeGraphBuilder::TryExtractBooleanLiteral(const TSharedPtr<FCodeExpression>& Expression, bool& OutValue) const
	{
		if (!Expression)
		{
			return false;
		}

		FString TextValue;
		return TryExtractTextLiteral(Expression, TextValue) && ParseBooleanLiteral(TextValue, OutValue);
	}

	bool FCodeGraphBuilder::IsDefaultArgument(const TSharedPtr<FCodeExpression>& Expression)
	{
		FString Name;
		return TryFlattenQualifiedName(Expression, Name) && Name.Equals(TEXT("default"), ESearchCase::IgnoreCase);
	}

	const FCodeCallArgument* FCodeGraphBuilder::FindNamedArgument(const TArray<FCodeCallArgument>& Arguments, const TCHAR* Name) const
	{
		const FString Normalized = UE::DreamShader::NormalizeSettingKey(Name);
		for (const FCodeCallArgument& Argument : Arguments)
		{
			if (Argument.bIsNamed && UE::DreamShader::NormalizeSettingKey(Argument.Name) == Normalized)
			{
				return &Argument;
			}
		}

		return nullptr;
	}

	const FCodeCallArgument* FCodeGraphBuilder::FindPositionalArgument(const TArray<FCodeCallArgument>& Arguments, const int32 PositionIndex) const
	{
		int32 CurrentIndex = 0;
		for (const FCodeCallArgument& Argument : Arguments)
		{
			if (!Argument.bIsNamed)
			{
				if (CurrentIndex == PositionIndex)
				{
					return &Argument;
				}
				++CurrentIndex;
			}
		}

		return nullptr;
	}

	bool FCodeGraphBuilder::ExecuteExpressionStatement(const TSharedPtr<FCodeExpression>& Expression, FString& OutError)
	{
		if (!Expression || Expression->Kind != ECodeExpressionKind::Call)
		{
			OutError = TEXT("Graph expression statements currently support only Function calls with explicit out arguments.");
			return false;
		}

		FString CalleeName;
		if (!TryFlattenQualifiedName(Expression->Left, CalleeName))
		{
			OutError = TEXT("Graph expression statements must call a named Function.");
			return false;
		}

		const FTextShaderFunctionDefinition* Function = FindFunctionDefinition(CalleeName);
		const FTextShaderFunctionDefinition* GraphFunction = FindGraphFunctionDefinition(CalleeName);
		if (!Function && !GraphFunction)
		{
			OutError = FString::Printf(
				TEXT("Graph expression statement '%s' is unsupported. Only DreamShader Function or GraphFunction calls may use statement syntax."),
				*CalleeName);
			return false;
		}
		if (Function && GraphFunction)
		{
			OutError = FString::Printf(TEXT("Graph expression statement '%s' is ambiguous because both Function and GraphFunction definitions exist."), *CalleeName);
			return false;
		}

		return Function
			? ExecuteCustomFunctionCall(*Function, Expression->Arguments, OutError)
			: ExecuteGraphFunctionCall(*GraphFunction, Expression->Arguments, OutError);
	}

	bool FCodeGraphBuilder::EvaluateExpression(const TSharedPtr<FCodeExpression>& Expression, FCodeValue& OutValue, FString& OutError)
	{
		if (!Expression)
		{
			OutError = TEXT("Empty Graph expression.");
			return false;
		}

		switch (Expression->Kind)
		{
		case ECodeExpressionKind::Name:
		{
			if (FCodeValue* ExistingValue = FindValue(Expression->Text))
			{
				OutValue = *ExistingValue;
				return true;
			}

			OutError = FString::Printf(TEXT("Unknown Graph identifier '%s'."), *Expression->Text);
			return false;
		}

		case ECodeExpressionKind::NumberLiteral:
		{
			double ParsedValue = 0.0;
			if (!ParseScalarLiteral(Expression->Text, ParsedValue))
			{
				OutError = FString::Printf(TEXT("Invalid numeric literal '%s'."), *Expression->Text);
				return false;
			}

			OutValue.Expression = CreateScalarLiteralNode(ParsedValue, ConsumeNodeY());
			OutValue.ComponentCount = 1;
			return OutValue.Expression != nullptr;
		}

		case ECodeExpressionKind::StringLiteral:
			OutError = TEXT("String literals can only be used in named UE builtin arguments.");
			return false;

		case ECodeExpressionKind::Unary:
			return EvaluateUnary(Expression, OutValue, OutError);

		case ECodeExpressionKind::Binary:
			return EvaluateBinary(Expression, OutValue, OutError);

		case ECodeExpressionKind::MemberAccess:
			return EvaluateMemberAccess(Expression, OutValue, OutError);

		case ECodeExpressionKind::Call:
			return EvaluateCall(Expression, OutValue, OutError);

		default:
			OutError = TEXT("Unsupported Graph expression kind.");
			return false;
		}
	}

	bool FCodeGraphBuilder::EvaluateUnary(const TSharedPtr<FCodeExpression>& Expression, FCodeValue& OutValue, FString& OutError)
	{
		FCodeValue Operand;
		if (!EvaluateExpression(Expression->Left, Operand, OutError))
		{
			return false;
		}

		if (Expression->Text == TEXT("+"))
		{
			OutValue = Operand;
			return true;
		}

		if (Expression->Text == TEXT("-"))
		{
			FCodeValue MinusOne;
			MinusOne.Expression = CreateScalarLiteralNode(-1.0, ConsumeNodeY());
			MinusOne.ComponentCount = 1;
			return CreateBinaryOperatorNode(TEXT("*"), Operand, MinusOne, OutValue, OutError);
		}

		OutError = FString::Printf(TEXT("Unsupported unary operator '%s'."), *Expression->Text);
		return false;
	}

	bool FCodeGraphBuilder::EvaluateBinary(const TSharedPtr<FCodeExpression>& Expression, FCodeValue& OutValue, FString& OutError)
	{
		FCodeValue LeftValue;
		FCodeValue RightValue;
		if (!EvaluateExpression(Expression->Left, LeftValue, OutError)
			|| !EvaluateExpression(Expression->Right, RightValue, OutError))
		{
			return false;
		}

		return CreateBinaryOperatorNode(Expression->Text, LeftValue, RightValue, OutValue, OutError);
	}

	bool FCodeGraphBuilder::CreateBinaryOperatorNode(
		const FString& Operator,
		const FCodeValue& LeftValue,
		const FCodeValue& RightValue,
		FCodeValue& OutValue,
		FString& OutError)
	{
		if (LeftValue.bIsTextureObject || RightValue.bIsTextureObject)
		{
			OutError = TEXT("Arithmetic operators cannot be applied to Texture2D values.");
			return false;
		}
		if (LeftValue.bIsSubstrate || RightValue.bIsSubstrate)
		{
			OutError = TEXT("Arithmetic operators cannot be applied to Substrate values.");
			return false;
		}
		if (LeftValue.bIsMaterialAttributes || RightValue.bIsMaterialAttributes)
		{
			OutError = TEXT("Arithmetic operators cannot be applied to MaterialAttributes values.");
			return false;
		}

		UMaterialExpression* Expression = nullptr;
		const int32 PositionY = ConsumeNodeY();

		if (Operator == TEXT("+"))
		{
			auto* AddExpression = Cast<UMaterialExpressionAdd>(
				CreateExpression(UMaterialExpressionAdd::StaticClass(), 200, PositionY));
			if (AddExpression)
			{
				ConnectCodeValueToInput(AddExpression->A, LeftValue);
				ConnectCodeValueToInput(AddExpression->B, RightValue);
				Expression = AddExpression;
			}
		}
		else if (Operator == TEXT("-"))
		{
			auto* SubtractExpression = Cast<UMaterialExpressionSubtract>(
				CreateExpression(UMaterialExpressionSubtract::StaticClass(), 200, PositionY));
			if (SubtractExpression)
			{
				ConnectCodeValueToInput(SubtractExpression->A, LeftValue);
				ConnectCodeValueToInput(SubtractExpression->B, RightValue);
				Expression = SubtractExpression;
			}
		}
		else if (Operator == TEXT("*"))
		{
			auto* MultiplyExpression = Cast<UMaterialExpressionMultiply>(
				CreateExpression(UMaterialExpressionMultiply::StaticClass(), 200, PositionY));
			if (MultiplyExpression)
			{
				ConnectCodeValueToInput(MultiplyExpression->A, LeftValue);
				ConnectCodeValueToInput(MultiplyExpression->B, RightValue);
				Expression = MultiplyExpression;
			}
		}
		else if (Operator == TEXT("/"))
		{
			auto* DivideExpression = Cast<UMaterialExpressionDivide>(
				CreateExpression(UMaterialExpressionDivide::StaticClass(), 200, PositionY));
			if (DivideExpression)
			{
				ConnectCodeValueToInput(DivideExpression->A, LeftValue);
				ConnectCodeValueToInput(DivideExpression->B, RightValue);
				Expression = DivideExpression;
			}
		}

		if (!Expression)
		{
			OutError = FString::Printf(TEXT("Unsupported or failed binary operator '%s'."), *Operator);
			return false;
		}

		OutValue.Expression = Expression;
		OutValue.ComponentCount = FMath::Max(LeftValue.ComponentCount, RightValue.ComponentCount);
		return true;
	}

	bool FCodeGraphBuilder::EvaluateMemberAccess(const TSharedPtr<FCodeExpression>& Expression, FCodeValue& OutValue, FString& OutError)
	{
		FCodeValue BaseValue;
		if (!EvaluateExpression(Expression->Left, BaseValue, OutError))
		{
			return false;
		}

		if (BaseValue.bIsMaterialAttributes)
		{
			FResolvedMaterialProperty ResolvedProperty;
			if (!ResolveMaterialProperty(Expression->Text, ResolvedProperty)
				|| ResolvedProperty.Property == MP_MaterialAttributes)
			{
				OutError = FString::Printf(TEXT("Unsupported MaterialAttributes member '%s'."), *Expression->Text);
				return false;
			}

			int32 OutputComponents = 0;
			if (!TryGetComponentCountForOutputType(ResolvedProperty.OutputType, OutputComponents) || OutputComponents <= 0)
			{
				OutError = FString::Printf(TEXT("MaterialAttributes member '%s' cannot be read as a numeric value."), *Expression->Text);
				return false;
			}

			auto* BreakAttributes = Cast<UMaterialExpressionBreakMaterialAttributes>(
				CreateExpression(UMaterialExpressionBreakMaterialAttributes::StaticClass(), 420, ConsumeNodeY()));
			if (!BreakAttributes)
			{
				OutError = TEXT("Failed to create a BreakMaterialAttributes node.");
				return false;
			}

			ConnectCodeValueToInput(BreakAttributes->MaterialAttributes, BaseValue);
			int32 OutputIndex = INDEX_NONE;
			if (!TryResolveMaterialAttributesBreakOutputIndex(ResolvedProperty.Property, OutputIndex)
				|| !BreakAttributes->Outputs.IsValidIndex(OutputIndex))
			{
				OutError = FString::Printf(TEXT("BreakMaterialAttributes does not expose member '%s'."), *Expression->Text);
				return false;
			}

			OutValue.Expression = BreakAttributes;
			OutValue.OutputIndex = OutputIndex;
			OutValue.ComponentCount = OutputComponents;
			OutValue.bIsTextureObject = false;
			OutValue.bIsMaterialAttributes = false;
			return true;
		}

		if (BaseValue.bIsTextureObject)
		{
			OutError = TEXT("Texture2D values do not support swizzle/member access in Code.");
			return false;
		}
		if (BaseValue.bIsSubstrate)
		{
			OutError = TEXT("Substrate values do not support swizzle/member access in Code.");
			return false;
		}

		return CreateSwizzleExpression(BaseValue, Expression->Text, OutValue, OutError);
	}

	bool FCodeGraphBuilder::CreateSingleChannelMask(
		const FCodeValue& BaseValue,
		const int32 ChannelIndex,
		FCodeValue& OutValue,
		FString& OutError)
	{
		static const TCHAR* ChannelNames[] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
		if (ChannelIndex >= 0 && ChannelIndex < UE_ARRAY_COUNT(ChannelNames))
		{
			int32 DirectOutputIndex = INDEX_NONE;
			if (TryResolveExpressionOutputIndex(BaseValue.Expression, ChannelNames[ChannelIndex], DirectOutputIndex))
			{
				OutValue.Expression = BaseValue.Expression;
				OutValue.OutputIndex = DirectOutputIndex;
				OutValue.ComponentCount = 1;
				OutValue.bIsTextureObject = false;
				OutValue.bIsMaterialAttributes = false;
				return true;
			}
		}

		auto* MaskExpression = Cast<UMaterialExpressionComponentMask>(
			CreateExpression(UMaterialExpressionComponentMask::StaticClass(), 400, ConsumeNodeY()));
		if (!MaskExpression)
		{
			OutError = TEXT("Failed to create a ComponentMask node.");
			return false;
		}

		ConnectCodeValueToInput(MaskExpression->Input, BaseValue);
		MaskExpression->R = ChannelIndex == 0;
		MaskExpression->G = ChannelIndex == 1;
		MaskExpression->B = ChannelIndex == 2;
		MaskExpression->A = ChannelIndex == 3;

		OutValue.Expression = MaskExpression;
		OutValue.ComponentCount = 1;
		return true;
	}

	bool FCodeGraphBuilder::AppendValues(const TArray<FCodeValue>& Parts, FCodeValue& OutValue, FString& OutError)
	{
		if (Parts.IsEmpty())
		{
			OutError = TEXT("Cannot build an empty vector.");
			return false;
		}
		for (const FCodeValue& Part : Parts)
		{
			if (Part.bIsTextureObject || Part.bIsMaterialAttributes || Part.bIsSubstrate)
			{
				OutError = TEXT("AppendVector inputs must be numeric scalar/vector values.");
				return false;
			}
		}

		FCodeValue Current = Parts[0];
		for (int32 Index = 1; Index < Parts.Num(); ++Index)
		{
			auto* AppendExpression = Cast<UMaterialExpressionAppendVector>(
				CreateExpression(UMaterialExpressionAppendVector::StaticClass(), 420, ConsumeNodeY()));
			if (!AppendExpression)
			{
				OutError = TEXT("Failed to create an AppendVector node.");
				return false;
			}

			ConnectCodeValueToInput(AppendExpression->A, Current);
			ConnectCodeValueToInput(AppendExpression->B, Parts[Index]);

			Current.Expression = AppendExpression;
			Current.ComponentCount += Parts[Index].ComponentCount;
		}

		OutValue = Current;
		return true;
	}

	bool FCodeGraphBuilder::CreateSwizzleExpression(
		const FCodeValue& BaseValue,
		const FString& Swizzle,
		FCodeValue& OutValue,
		FString& OutError)
	{
		if (Swizzle.IsEmpty() || Swizzle.Len() > 4)
		{
			OutError = FString::Printf(TEXT("Unsupported swizzle '%s'."), *Swizzle);
			return false;
		}

		if (BaseValue.Expression && (Swizzle.Equals(TEXT("rgb"), ESearchCase::IgnoreCase) || Swizzle.Equals(TEXT("rgba"), ESearchCase::IgnoreCase)))
		{
			int32 DirectOutputIndex = INDEX_NONE;
			if (TryResolveExpressionOutputIndex(BaseValue.Expression, Swizzle.ToUpper(), DirectOutputIndex))
			{
				OutValue.Expression = BaseValue.Expression;
				OutValue.OutputIndex = DirectOutputIndex;
				OutValue.ComponentCount = Swizzle.Len();
				OutValue.bIsTextureObject = false;
				OutValue.bIsMaterialAttributes = false;
				return true;
			}
		}

		TArray<FCodeValue> Channels;
		for (int32 Index = 0; Index < Swizzle.Len(); ++Index)
		{
			const TCHAR ChannelChar = FChar::ToLower(Swizzle[Index]);
			int32 ChannelIndex = INDEX_NONE;
			switch (ChannelChar)
			{
			case TCHAR('x'):
			case TCHAR('r'):
				ChannelIndex = 0;
				break;
			case TCHAR('y'):
			case TCHAR('g'):
				ChannelIndex = 1;
				break;
			case TCHAR('z'):
			case TCHAR('b'):
				ChannelIndex = 2;
				break;
			case TCHAR('w'):
			case TCHAR('a'):
				ChannelIndex = 3;
				break;
			default:
				break;
			}

			if (ChannelIndex == INDEX_NONE || ChannelIndex >= BaseValue.ComponentCount)
			{
				OutError = FString::Printf(TEXT("Swizzle '%s' is invalid for a value with %d components."), *Swizzle, BaseValue.ComponentCount);
				return false;
			}

			FCodeValue ChannelValue;
			if (!CreateSingleChannelMask(BaseValue, ChannelIndex, ChannelValue, OutError))
			{
				return false;
			}
			Channels.Add(ChannelValue);
		}

		if (Channels.Num() == 1)
		{
			OutValue = Channels[0];
			return true;
		}

		if (!AppendValues(Channels, OutValue, OutError))
		{
			return false;
		}

		OutValue.ComponentCount = Channels.Num();
		return true;
	}

	bool FCodeGraphBuilder::EvaluateCall(const TSharedPtr<FCodeExpression>& Expression, FCodeValue& OutValue, FString& OutError)
	{
		FString CalleeName;
		if (!TryFlattenQualifiedName(Expression->Left, CalleeName))
		{
			OutError = TEXT("Graph calls must target a named function.");
			return false;
		}

		if (IsVectorConstructorName(CalleeName))
		{
			return EvaluateVectorConstructor(CalleeName, Expression->Arguments, OutValue, OutError);
		}

		if (CalleeName.StartsWith(TEXT("UE."), ESearchCase::IgnoreCase))
		{
			return EvaluateUEBuiltinCall(CalleeName, Expression->Arguments, OutValue, OutError);
		}

		if (const FTextShaderPropertyDefinition* Property = FindPropertyDefinition(CalleeName))
		{
			if (Property->ParameterNodeType.Equals(TEXT("StaticSwitchParameter"), ESearchCase::IgnoreCase))
			{
				return EvaluateStaticSwitchParameterCall(*Property, Expression->Arguments, OutValue, OutError);
			}
		}

		const FTextShaderFunctionDefinition* CustomFunction = FindFunctionDefinition(CalleeName);
		const FTextShaderFunctionDefinition* GraphFunction = FindGraphFunctionDefinition(CalleeName);
		const FTextShaderMaterialFunctionDefinition* MaterialFunctionDefinition = FindMaterialFunctionDefinition(CalleeName);
		const FTextShaderVirtualFunctionDefinition* VirtualFunctionDefinition = FindVirtualFunctionDefinition(CalleeName);

		TArray<FString> MatchedKinds;
		if (CustomFunction)
		{
			MatchedKinds.Add(TEXT("Function"));
		}
		if (GraphFunction)
		{
			MatchedKinds.Add(TEXT("GraphFunction"));
		}
		if (MaterialFunctionDefinition)
		{
			MatchedKinds.Add(TEXT("ShaderFunction"));
		}
		if (VirtualFunctionDefinition)
		{
			MatchedKinds.Add(TEXT("VirtualFunction"));
		}
		if (MatchedKinds.Num() > 1)
		{
			OutError = FString::Printf(
				TEXT("Graph call '%s' is ambiguous because multiple definitions use that name: %s."),
				*CalleeName,
				*FString::Join(MatchedKinds, TEXT(", ")));
			return false;
		}

		if (MaterialFunctionDefinition)
		{
			return EvaluateMaterialFunctionCall(*MaterialFunctionDefinition, Expression->Arguments, OutValue, OutError);
		}
		if (VirtualFunctionDefinition)
		{
			return EvaluateVirtualFunctionCall(*VirtualFunctionDefinition, Expression->Arguments, OutValue, OutError);
		}
		if (GraphFunction)
		{
			return EvaluateGraphFunctionCall(*GraphFunction, Expression->Arguments, OutValue, OutError);
		}

		return EvaluateCustomFunctionCall(CalleeName, Expression->Arguments, OutValue, OutError);
	}

	bool FCodeGraphBuilder::IsVectorConstructorName(const FString& InName)
	{
		return InName.Equals(TEXT("float"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("float1"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("float2"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("float3"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("float4"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("vec2"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("vec3"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("vec4"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("half"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("half1"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("half2"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("half3"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("half4"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("int"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("int2"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("int3"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("int4"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("ivec2"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("ivec3"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("ivec4"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("uint"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("uint2"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("uint3"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("uint4"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("uvec2"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("uvec3"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("uvec4"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("bool"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("bool2"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("bool3"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("bool4"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("bvec2"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("bvec3"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("bvec4"), ESearchCase::IgnoreCase);
	}

	int32 FCodeGraphBuilder::GetConstructorComponentCount(const FString& InName)
	{
		if (InName.EndsWith(TEXT("2")))
		{
			return 2;
		}
		if (InName.EndsWith(TEXT("3")))
		{
			return 3;
		}
		if (InName.EndsWith(TEXT("4")))
		{
			return 4;
		}
		return 1;
	}

	bool FCodeGraphBuilder::EvaluateVectorConstructor(
		const FString& ConstructorName,
		const TArray<FCodeCallArgument>& Arguments,
		FCodeValue& OutValue,
		FString& OutError)
	{
		const int32 ExpectedComponents = GetConstructorComponentCount(ConstructorName);
		TArray<FCodeValue> Parts;

		for (const FCodeCallArgument& Argument : Arguments)
		{
			if (Argument.bIsNamed)
			{
				OutError = FString::Printf(TEXT("Constructor '%s' does not accept named arguments."), *ConstructorName);
				return false;
			}

			FCodeValue EvaluatedArgument;
			if (!EvaluateExpression(Argument.Expression, EvaluatedArgument, OutError))
			{
				return false;
			}

			if (EvaluatedArgument.bIsTextureObject)
			{
				OutError = FString::Printf(TEXT("Constructor '%s' cannot use Texture2D arguments."), *ConstructorName);
				return false;
			}
			if (EvaluatedArgument.bIsSubstrate)
			{
				OutError = FString::Printf(TEXT("Constructor '%s' cannot use Substrate arguments."), *ConstructorName);
				return false;
			}
			if (EvaluatedArgument.bIsMaterialAttributes)
			{
				OutError = FString::Printf(TEXT("Constructor '%s' cannot use MaterialAttributes arguments."), *ConstructorName);
				return false;
			}

			Parts.Add(EvaluatedArgument);
		}

		if (ExpectedComponents == 1)
		{
			if (Parts.Num() != 1 || Parts[0].ComponentCount != 1)
			{
				OutError = FString::Printf(TEXT("Constructor '%s' expects a single scalar input."), *ConstructorName);
				return false;
			}

			OutValue = Parts[0];
			return true;
		}

		if (Parts.Num() == 1 && Parts[0].ComponentCount == 1)
		{
			TArray<FCodeValue> ReplicatedParts;
			for (int32 Index = 0; Index < ExpectedComponents; ++Index)
			{
				ReplicatedParts.Add(Parts[0]);
			}
			if (!AppendValues(ReplicatedParts, OutValue, OutError))
			{
				return false;
			}
			OutValue.ComponentCount = ExpectedComponents;
			return true;
		}

		int32 TotalComponents = 0;
		for (const FCodeValue& Part : Parts)
		{
			TotalComponents += Part.ComponentCount;
		}

		if (TotalComponents != ExpectedComponents)
		{
			OutError = FString::Printf(TEXT("Constructor '%s' expects %d total components but got %d."), *ConstructorName, ExpectedComponents, TotalComponents);
			return false;
		}

		if (!AppendValues(Parts, OutValue, OutError))
		{
			return false;
		}

		OutValue.ComponentCount = ExpectedComponents;
		return true;
	}

	const FTextShaderPropertyDefinition* FCodeGraphBuilder::FindPropertyDefinition(const FString& PropertyName) const
	{
		if (LocalProperties)
		{
			for (const FTextShaderPropertyDefinition& Property : *LocalProperties)
			{
				if (Property.Name.Equals(PropertyName, ESearchCase::IgnoreCase))
				{
					return &Property;
				}
			}
		}

		for (const FTextShaderPropertyDefinition& Property : Definition.Properties)
		{
			if (Property.Name.Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				return &Property;
			}
		}

		return nullptr;
	}

	bool FCodeGraphBuilder::EvaluateStaticSwitchParameterCall(
		const FTextShaderPropertyDefinition& Property,
		const TArray<FCodeCallArgument>& Arguments,
		FCodeValue& OutValue,
		FString& OutError)
	{
		const FCodeCallArgument* TrueArgument = FindNamedArgument(Arguments, TEXT("True"));
		if (!TrueArgument)
		{
			TrueArgument = FindNamedArgument(Arguments, TEXT("A"));
		}
		if (!TrueArgument)
		{
			TrueArgument = FindPositionalArgument(Arguments, 0);
		}

		const FCodeCallArgument* FalseArgument = FindNamedArgument(Arguments, TEXT("False"));
		if (!FalseArgument)
		{
			FalseArgument = FindNamedArgument(Arguments, TEXT("B"));
		}
		if (!FalseArgument)
		{
			FalseArgument = FindPositionalArgument(Arguments, 1);
		}

		if (!TrueArgument || !FalseArgument)
		{
			OutError = FString::Printf(TEXT("StaticSwitchParameter '%s' requires True=... and False=... inputs."), *Property.Name);
			return false;
		}

		FCodeValue TrueValue;
		if (!EvaluateExpression(TrueArgument->Expression, TrueValue, OutError))
		{
			OutError = FString::Printf(TEXT("StaticSwitchParameter '%s' True input: %s"), *Property.Name, *OutError);
			return false;
		}

		FCodeValue FalseValue;
		if (!EvaluateExpression(FalseArgument->Expression, FalseValue, OutError))
		{
			OutError = FString::Printf(TEXT("StaticSwitchParameter '%s' False input: %s"), *Property.Name, *OutError);
			return false;
		}

		if (TrueValue.bIsTextureObject || FalseValue.bIsTextureObject)
		{
			OutError = FString::Printf(TEXT("StaticSwitchParameter '%s' cannot switch Texture object values."), *Property.Name);
			return false;
		}
		if (TrueValue.bIsSubstrate != FalseValue.bIsSubstrate)
		{
			OutError = FString::Printf(TEXT("StaticSwitchParameter '%s' cannot mix Substrate and non-Substrate branches."), *Property.Name);
			return false;
		}
		if (TrueValue.bIsMaterialAttributes != FalseValue.bIsMaterialAttributes)
		{
			OutError = FString::Printf(TEXT("StaticSwitchParameter '%s' cannot mix MaterialAttributes and numeric branches."), *Property.Name);
			return false;
		}
		if (TrueValue.ComponentCount != FalseValue.ComponentCount)
		{
			OutError = FString::Printf(
				TEXT("StaticSwitchParameter '%s' branches must have the same component count, got %d and %d."),
				*Property.Name,
				TrueValue.ComponentCount,
				FalseValue.ComponentCount);
			return false;
		}

		UMaterialExpressionStaticSwitchParameter* SwitchExpression = nullptr;
		if (FCodeValue* ExistingValue = FindValue(Property.Name))
		{
			SwitchExpression = Cast<UMaterialExpressionStaticSwitchParameter>(ExistingValue->Expression);
		}

		if (!SwitchExpression)
		{
			SwitchExpression = Cast<UMaterialExpressionStaticSwitchParameter>(
				CreateExpression(UMaterialExpressionStaticSwitchParameter::StaticClass(), 600, ConsumeNodeY()));
		}

		if (!SwitchExpression)
		{
			OutError = FString::Printf(TEXT("Failed to create StaticSwitchParameter node '%s'."), *Property.Name);
			return false;
		}

		SwitchExpression->ParameterName = FName(*Property.Name);
		SwitchExpression->DefaultValue = Property.ScalarDefaultValue != 0.0 ? 1U : 0U;
		if (!SwitchExpression->ExpressionGUID.IsValid())
		{
			SwitchExpression->ExpressionGUID = FGuid::NewGuid();
		}
		if (!ApplyExpressionMetadata(SwitchExpression, Property.Metadata, OutError))
		{
			OutError = FString::Printf(TEXT("StaticSwitchParameter '%s': %s"), *Property.Name, *OutError);
			return false;
		}

		ConnectCodeValueToInput(SwitchExpression->A, TrueValue);
		ConnectCodeValueToInput(SwitchExpression->B, FalseValue);

		OutValue.Expression = SwitchExpression;
		OutValue.OutputIndex = 0;
		OutValue.ComponentCount = TrueValue.ComponentCount;
		OutValue.bIsTextureObject = false;
		OutValue.bIsMaterialAttributes = TrueValue.bIsMaterialAttributes;
		OutValue.bIsSubstrate = TrueValue.bIsSubstrate;
		return true;
	}
}
