#include "DreamShaderMaterialGeneratorPrivate.h"

namespace UE::DreamShader::Editor::Private
{
	static FString RemoveComments(const FString& Input)
	{
		FString Output;
		Output.Reserve(Input.Len());

		bool bInString = false;
		bool bInLineComment = false;
		bool bInBlockComment = false;

		for (int32 Index = 0; Index < Input.Len(); ++Index)
		{
			const TCHAR Char = Input[Index];
			const TCHAR Next = Input.IsValidIndex(Index + 1) ? Input[Index + 1] : TCHAR('\0');

			if (bInLineComment)
			{
				if (Char == TCHAR('\n'))
				{
					bInLineComment = false;
					Output.AppendChar(Char);
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
				Output.AppendChar(Char);
				if (Char == TCHAR('\\') && Input.IsValidIndex(Index + 1))
				{
					Output.AppendChar(Input[++Index]);
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
				Output.AppendChar(Char);
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

			Output.AppendChar(Char);
		}

		return Output;
	}

	static TArray<FString> SplitTopLevelSegments(const FString& InText, const TCHAR Delimiter)
	{
		TArray<FString> Segments;
		FString Current;
		int32 ParenthesisDepth = 0;
		int32 BraceDepth = 0;
		int32 BracketDepth = 0;
		bool bInString = false;

		for (int32 Index = 0; Index < InText.Len(); ++Index)
		{
			const TCHAR Char = InText[Index];

			if (bInString)
			{
				Current.AppendChar(Char);
				if (Char == TCHAR('\\') && InText.IsValidIndex(Index + 1))
				{
					Current.AppendChar(InText[++Index]);
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

			if (Char == TCHAR('{'))
			{
				++BraceDepth;
				Current.AppendChar(Char);
				continue;
			}

			if (Char == TCHAR('}'))
			{
				BraceDepth = FMath::Max(0, BraceDepth - 1);
				Current.AppendChar(Char);
				continue;
			}

			if (Char == TCHAR('['))
			{
				++BracketDepth;
				Current.AppendChar(Char);
				continue;
			}

			if (Char == TCHAR(']'))
			{
				BracketDepth = FMath::Max(0, BracketDepth - 1);
				Current.AppendChar(Char);
				continue;
			}

			if (Char == Delimiter && ParenthesisDepth == 0 && BraceDepth == 0 && BracketDepth == 0)
			{
				Current.TrimStartAndEndInline();
				if (!Current.IsEmpty())
				{
					Segments.Add(Current);
				}
				Current.Reset();
				continue;
			}

			Current.AppendChar(Char);
		}

		Current.TrimStartAndEndInline();
		if (!Current.IsEmpty())
		{
			Segments.Add(Current);
		}

		return Segments;
	}

	static bool IsIdentifierBoundary(const FString& Text, const int32 Index)
	{
		if (!Text.IsValidIndex(Index))
		{
			return true;
		}

		const TCHAR Char = Text[Index];
		return !(FChar::IsAlnum(Char) || Char == TCHAR('_'));
	}

	static bool MatchesKeywordAt(const FString& Text, const int32 Index, const TCHAR* Keyword)
	{
		const int32 KeywordLength = FCString::Strlen(Keyword);
		return Text.IsValidIndex(Index)
			&& Index + KeywordLength <= Text.Len()
			&& Text.Mid(Index, KeywordLength).Equals(Keyword, ESearchCase::CaseSensitive)
			&& IsIdentifierBoundary(Text, Index - 1)
			&& IsIdentifierBoundary(Text, Index + KeywordLength);
	}

	static void SkipWhitespace(const FString& Text, int32& InOutIndex)
	{
		while (Text.IsValidIndex(InOutIndex) && FChar::IsWhitespace(Text[InOutIndex]))
		{
			++InOutIndex;
		}
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

		int32 Depth = 0;
		bool bInString = false;
		for (int32 Index = OpenIndex; Index < Text.Len(); ++Index)
		{
			const TCHAR Char = Text[Index];

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

			if (Char == OpenChar)
			{
				++Depth;
				continue;
			}

			if (Char == CloseChar)
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

	static bool FindIfStatementEnd(const FString& Text, const int32 IfIndex, int32& OutEndIndex, FString& OutError)
	{
		if (!MatchesKeywordAt(Text, IfIndex, TEXT("if")))
		{
			OutError = TEXT("Expected 'if'.");
			return false;
		}

		int32 Index = IfIndex + 2;
		SkipWhitespace(Text, Index);
		if (!Text.IsValidIndex(Index) || Text[Index] != TCHAR('('))
		{
			OutError = TEXT("Graph if statement is missing a condition block.");
			return false;
		}

		int32 ConditionCloseIndex = INDEX_NONE;
		if (!FindMatchingDelimiter(Text, Index, TCHAR('('), TCHAR(')'), ConditionCloseIndex))
		{
			OutError = TEXT("Graph if statement has an unterminated condition block.");
			return false;
		}

		Index = ConditionCloseIndex + 1;
		SkipWhitespace(Text, Index);
		if (!Text.IsValidIndex(Index) || Text[Index] != TCHAR('{'))
		{
			OutError = TEXT("Graph if statement is missing a '{ ... }' body.");
			return false;
		}

		int32 ThenCloseIndex = INDEX_NONE;
		if (!FindMatchingDelimiter(Text, Index, TCHAR('{'), TCHAR('}'), ThenCloseIndex))
		{
			OutError = TEXT("Graph if statement has an unterminated body block.");
			return false;
		}

		Index = ThenCloseIndex + 1;
		SkipWhitespace(Text, Index);
		if (MatchesKeywordAt(Text, Index, TEXT("else")))
		{
			Index += 4;
			SkipWhitespace(Text, Index);
			if (MatchesKeywordAt(Text, Index, TEXT("if")))
			{
				return FindIfStatementEnd(Text, Index, OutEndIndex, OutError);
			}

			if (!Text.IsValidIndex(Index) || Text[Index] != TCHAR('{'))
			{
				OutError = TEXT("Graph else statement is missing a '{ ... }' body.");
				return false;
			}

			int32 ElseCloseIndex = INDEX_NONE;
			if (!FindMatchingDelimiter(Text, Index, TCHAR('{'), TCHAR('}'), ElseCloseIndex))
			{
				OutError = TEXT("Graph else statement has an unterminated body block.");
				return false;
			}

			OutEndIndex = ElseCloseIndex + 1;
			return true;
		}

		OutEndIndex = Index;
		return true;
	}

	static bool SplitStatements(const FString& BlockContent, TArray<FString>& OutStatements, FString& OutError)
	{
		OutStatements.Reset();

		int32 Index = 0;
		while (Index < BlockContent.Len())
		{
			SkipWhitespace(BlockContent, Index);
			while (BlockContent.IsValidIndex(Index) && BlockContent[Index] == TCHAR(';'))
			{
				++Index;
				SkipWhitespace(BlockContent, Index);
			}

			if (Index >= BlockContent.Len())
			{
				break;
			}

			const int32 StatementStartIndex = Index;
			if (MatchesKeywordAt(BlockContent, Index, TEXT("if")))
			{
				int32 StatementEndIndex = INDEX_NONE;
				if (!FindIfStatementEnd(BlockContent, Index, StatementEndIndex, OutError))
				{
					return false;
				}

				FString Statement = BlockContent.Mid(StatementStartIndex, StatementEndIndex - StatementStartIndex).TrimStartAndEnd();
				if (!Statement.IsEmpty())
				{
					OutStatements.Add(Statement);
				}
				Index = StatementEndIndex;
				continue;
			}

			int32 ParenthesisDepth = 0;
			int32 BraceDepth = 0;
			int32 BracketDepth = 0;
			bool bInString = false;
			for (; Index < BlockContent.Len(); ++Index)
			{
				const TCHAR Char = BlockContent[Index];

				if (bInString)
				{
					if (Char == TCHAR('\\') && BlockContent.IsValidIndex(Index + 1))
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

				if (Char == TCHAR('('))
				{
					++ParenthesisDepth;
					continue;
				}
				if (Char == TCHAR(')'))
				{
					ParenthesisDepth = FMath::Max(0, ParenthesisDepth - 1);
					continue;
				}
				if (Char == TCHAR('{'))
				{
					++BraceDepth;
					continue;
				}
				if (Char == TCHAR('}'))
				{
					BraceDepth = FMath::Max(0, BraceDepth - 1);
					continue;
				}
				if (Char == TCHAR('['))
				{
					++BracketDepth;
					continue;
				}
				if (Char == TCHAR(']'))
				{
					BracketDepth = FMath::Max(0, BracketDepth - 1);
					continue;
				}

				if (Char == TCHAR(';') && ParenthesisDepth == 0 && BraceDepth == 0 && BracketDepth == 0)
				{
					break;
				}
			}

			FString Statement = BlockContent.Mid(StatementStartIndex, Index - StatementStartIndex).TrimStartAndEnd();
			if (!Statement.IsEmpty())
			{
				OutStatements.Add(Statement);
			}

			if (BlockContent.IsValidIndex(Index) && BlockContent[Index] == TCHAR(';'))
			{
				++Index;
			}
		}

		return true;
	}

	static bool SplitTopLevelAssignment(const FString& InText, FString& OutLeft, FString& OutRight)
	{
		int32 ParenthesisDepth = 0;
		int32 BraceDepth = 0;
		int32 BracketDepth = 0;
		bool bInString = false;

		for (int32 Index = 0; Index < InText.Len(); ++Index)
		{
			const TCHAR Char = InText[Index];

			if (bInString)
			{
				if (Char == TCHAR('\\') && InText.IsValidIndex(Index + 1))
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

			if (Char == TCHAR('('))
			{
				++ParenthesisDepth;
				continue;
			}

			if (Char == TCHAR(')'))
			{
				ParenthesisDepth = FMath::Max(0, ParenthesisDepth - 1);
				continue;
			}

			if (Char == TCHAR('{'))
			{
				++BraceDepth;
				continue;
			}

			if (Char == TCHAR('}'))
			{
				BraceDepth = FMath::Max(0, BraceDepth - 1);
				continue;
			}

			if (Char == TCHAR('['))
			{
				++BracketDepth;
				continue;
			}

			if (Char == TCHAR(']'))
			{
				BracketDepth = FMath::Max(0, BracketDepth - 1);
				continue;
			}

			if (Char == TCHAR('=') && ParenthesisDepth == 0 && BraceDepth == 0 && BracketDepth == 0)
			{
				OutLeft = InText.Left(Index).TrimStartAndEnd();
				OutRight = InText.Mid(Index + 1).TrimStartAndEnd();
				return true;
			}
		}

		return false;
	}

	static bool SplitDeclarationTypeAndName(const FString& InText, FString& OutTypeToken, FString& OutNameToken)
	{
		const FString Trimmed = InText.TrimStartAndEnd();
		int32 ParenthesisDepth = 0;
		bool bInString = false;
		int32 LastWhitespaceIndex = INDEX_NONE;

		for (int32 Index = 0; Index < Trimmed.Len(); ++Index)
		{
			const TCHAR Char = Trimmed[Index];

			if (bInString)
			{
				if (Char == TCHAR('\\') && Trimmed.IsValidIndex(Index + 1))
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

			if (Char == TCHAR('('))
			{
				++ParenthesisDepth;
				continue;
			}

			if (Char == TCHAR(')'))
			{
				ParenthesisDepth = FMath::Max(0, ParenthesisDepth - 1);
				continue;
			}

			if (ParenthesisDepth == 0 && FChar::IsWhitespace(Char))
			{
				LastWhitespaceIndex = Index;
			}
		}

		if (LastWhitespaceIndex == INDEX_NONE)
		{
			return false;
		}

		OutTypeToken = Trimmed.Left(LastWhitespaceIndex).TrimStartAndEnd();
		OutNameToken = Trimmed.Mid(LastWhitespaceIndex + 1).TrimStartAndEnd();
		return !OutTypeToken.IsEmpty() && !OutNameToken.IsEmpty();
	}

	static bool IsBraceInitializerText(const FString& InText)
	{
		const FString Trimmed = InText.TrimStartAndEnd();
		return Trimmed.Len() >= 2 && Trimmed.StartsWith(TEXT("{")) && Trimmed.EndsWith(TEXT("}"));
	}

	static bool IsIdentifierToken(const FString& InText)
	{
		const FString Trimmed = InText.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return false;
		}

		if (!(FChar::IsAlpha(Trimmed[0]) || Trimmed[0] == TCHAR('_')))
		{
			return false;
		}

		for (int32 Index = 1; Index < Trimmed.Len(); ++Index)
		{
			const TCHAR Char = Trimmed[Index];
			if (!(FChar::IsAlnum(Char) || Char == TCHAR('_')))
			{
				return false;
			}
		}

		return true;
	}

	bool MakeCodeDeclarationStatement(
		const FString& DeclaredType,
		const FString& TargetName,
		const FString& InitializerText,
		FCodeStatement& OutStatement,
		FString& OutError)
	{
		OutStatement = FCodeStatement{};
		OutStatement.bIsDeclaration = true;
		OutStatement.DeclaredType = DeclaredType.TrimStartAndEnd();
		OutStatement.TargetName = TargetName.TrimStartAndEnd();

		if (OutStatement.DeclaredType.IsEmpty() || OutStatement.TargetName.IsEmpty())
		{
			OutError = TEXT("Output declaration initializer requires a type and name.");
			return false;
		}

		const FString TrimmedInitializer = InitializerText.TrimStartAndEnd();
		if (TrimmedInitializer.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Output declaration '%s' has an empty initializer."), *OutStatement.TargetName);
			return false;
		}

		OutStatement.InitializerText = TrimmedInitializer;
		if (IsBraceInitializerText(TrimmedInitializer))
		{
			OutStatement.bUsesBraceInitializer = true;
			return true;
		}

		return ParseCodeExpression(TrimmedInitializer, OutStatement.Expression, OutError);
	}

	class FCodeExpressionParser
	{
	public:
		explicit FCodeExpressionParser(const FString& InSource)
			: Source(InSource)
		{
			Tokenize();
		}

		bool Parse(TSharedPtr<FCodeExpression>& OutExpression, FString& OutError)
		{
			OutExpression = ParseAdditive(OutError);
			if (!OutExpression)
			{
				return false;
			}

			if (Peek().Type != ECodeTokenType::End)
			{
				OutError = FString::Printf(TEXT("Unexpected token '%s' in Graph expression."), *Peek().Text);
				return false;
			}

			return true;
		}

	private:
		const FString& Source;
		TArray<FCodeToken> Tokens;
		int32 TokenIndex = 0;

		void Tokenize()
		{
			Tokens.Reset();

			int32 Index = 0;
			while (Index < Source.Len())
			{
				const TCHAR Char = Source[Index];
				if (FChar::IsWhitespace(Char))
				{
					++Index;
					continue;
				}

				if (FChar::IsAlpha(Char) || Char == TCHAR('_'))
				{
					const int32 Start = Index++;
					while (Index < Source.Len())
					{
						const TCHAR Inner = Source[Index];
						if (!(FChar::IsAlnum(Inner) || Inner == TCHAR('_')))
						{
							break;
						}
						++Index;
					}

					Tokens.Add({ ECodeTokenType::Identifier, Source.Mid(Start, Index - Start) });
					continue;
				}

				if (FChar::IsDigit(Char) || (Char == TCHAR('.') && Index + 1 < Source.Len() && FChar::IsDigit(Source[Index + 1])))
				{
					const int32 Start = Index++;
					bool bSeenExponent = false;
					while (Index < Source.Len())
					{
						const TCHAR Inner = Source[Index];
						if (FChar::IsDigit(Inner) || Inner == TCHAR('.'))
						{
							++Index;
							continue;
						}

						if (!bSeenExponent && (Inner == TCHAR('e') || Inner == TCHAR('E')))
						{
							bSeenExponent = true;
							++Index;
							if (Index < Source.Len() && (Source[Index] == TCHAR('+') || Source[Index] == TCHAR('-')))
							{
								++Index;
							}
							continue;
						}

						break;
					}

					Tokens.Add({ ECodeTokenType::Number, Source.Mid(Start, Index - Start) });
					continue;
				}

				if (Char == TCHAR('"'))
				{
					FString Value;
					++Index;
					while (Index < Source.Len() && Source[Index] != TCHAR('"'))
					{
						if (Source[Index] == TCHAR('\\') && Index + 1 < Source.Len())
						{
							++Index;
							const TCHAR EscapedChar = Source[Index++];
							switch (EscapedChar)
							{
							case TCHAR('n'):
								Value.AppendChar(TCHAR('\n'));
								break;
							case TCHAR('r'):
								Value.AppendChar(TCHAR('\r'));
								break;
							case TCHAR('t'):
								Value.AppendChar(TCHAR('\t'));
								break;
							case TCHAR('"'):
								Value.AppendChar(TCHAR('"'));
								break;
							case TCHAR('\\'):
								Value.AppendChar(TCHAR('\\'));
								break;
							default:
								Value.AppendChar(EscapedChar);
								break;
							}
							continue;
						}
						Value.AppendChar(Source[Index++]);
					}

					if (Index < Source.Len() && Source[Index] == TCHAR('"'))
					{
						++Index;
					}

					Tokens.Add({ ECodeTokenType::String, Value });
					continue;
				}

				const auto AddSingleToken = [this](const ECodeTokenType Type, const TCHAR CharValue)
				{
					Tokens.Add({ Type, FString(1, &CharValue) });
				};

				switch (Char)
				{
				case TCHAR('('): AddSingleToken(ECodeTokenType::LeftParen, Char); break;
				case TCHAR(')'): AddSingleToken(ECodeTokenType::RightParen, Char); break;
				case TCHAR(','): AddSingleToken(ECodeTokenType::Comma, Char); break;
				case TCHAR('.'): AddSingleToken(ECodeTokenType::Dot, Char); break;
				case TCHAR(':'):
					if (Index + 1 < Source.Len() && Source[Index + 1] == TCHAR(':'))
					{
						Tokens.Add({ ECodeTokenType::ScopeResolution, TEXT("::") });
						++Index;
					}
					else
					{
						AddSingleToken(ECodeTokenType::End, Char);
					}
					break;
				case TCHAR('+'): AddSingleToken(ECodeTokenType::Plus, Char); break;
				case TCHAR('-'): AddSingleToken(ECodeTokenType::Minus, Char); break;
				case TCHAR('*'): AddSingleToken(ECodeTokenType::Star, Char); break;
				case TCHAR('/'): AddSingleToken(ECodeTokenType::Slash, Char); break;
				case TCHAR('='): AddSingleToken(ECodeTokenType::Equals, Char); break;
				default: AddSingleToken(ECodeTokenType::End, Char); break;
				}

				++Index;
			}

			Tokens.Add({ ECodeTokenType::End, TEXT("") });
		}

		const FCodeToken& Peek(const int32 Offset = 0) const
		{
			const int32 Index = FMath::Clamp(TokenIndex + Offset, 0, Tokens.Num() - 1);
			return Tokens[Index];
		}

		bool Match(const ECodeTokenType Type)
		{
			if (Peek().Type == Type)
			{
				++TokenIndex;
				return true;
			}
			return false;
		}

		bool Expect(const ECodeTokenType Type, FString& OutError)
		{
			if (Match(Type))
			{
				return true;
			}

			OutError = FString::Printf(TEXT("Expected token type %d in Graph expression near '%s'."), static_cast<int32>(Type), *Peek().Text);
			return false;
		}

		TSharedPtr<FCodeExpression> ParseAdditive(FString& OutError)
		{
			TSharedPtr<FCodeExpression> Left = ParseMultiplicative(OutError);
			while (Left && (Peek().Type == ECodeTokenType::Plus || Peek().Type == ECodeTokenType::Minus))
			{
				const FString Operator = Peek().Text;
				++TokenIndex;

				TSharedPtr<FCodeExpression> Right = ParseMultiplicative(OutError);
				if (!Right)
				{
					return nullptr;
				}

				TSharedPtr<FCodeExpression> Expression = MakeShared<FCodeExpression>();
				Expression->Kind = ECodeExpressionKind::Binary;
				Expression->Text = Operator;
				Expression->Left = Left;
				Expression->Right = Right;
				Left = Expression;
			}

			return Left;
		}

		TSharedPtr<FCodeExpression> ParseMultiplicative(FString& OutError)
		{
			TSharedPtr<FCodeExpression> Left = ParseUnary(OutError);
			while (Left && (Peek().Type == ECodeTokenType::Star || Peek().Type == ECodeTokenType::Slash))
			{
				const FString Operator = Peek().Text;
				++TokenIndex;

				TSharedPtr<FCodeExpression> Right = ParseUnary(OutError);
				if (!Right)
				{
					return nullptr;
				}

				TSharedPtr<FCodeExpression> Expression = MakeShared<FCodeExpression>();
				Expression->Kind = ECodeExpressionKind::Binary;
				Expression->Text = Operator;
				Expression->Left = Left;
				Expression->Right = Right;
				Left = Expression;
			}

			return Left;
		}

		TSharedPtr<FCodeExpression> ParseUnary(FString& OutError)
		{
			if (Peek().Type == ECodeTokenType::Plus || Peek().Type == ECodeTokenType::Minus)
			{
				const FString Operator = Peek().Text;
				++TokenIndex;
				TSharedPtr<FCodeExpression> Operand = ParseUnary(OutError);
				if (!Operand)
				{
					return nullptr;
				}

				TSharedPtr<FCodeExpression> Expression = MakeShared<FCodeExpression>();
				Expression->Kind = ECodeExpressionKind::Unary;
				Expression->Text = Operator;
				Expression->Left = Operand;
				return Expression;
			}

			return ParsePostfix(OutError);
		}

		TSharedPtr<FCodeExpression> ParsePostfix(FString& OutError)
		{
			TSharedPtr<FCodeExpression> Expression = ParsePrimary(OutError);
			if (!Expression)
			{
				return nullptr;
			}

			while (true)
			{
				if (Match(ECodeTokenType::Dot) || Match(ECodeTokenType::ScopeResolution))
				{
					const bool bWasScopeResolution = Tokens[FMath::Max(0, TokenIndex - 1)].Type == ECodeTokenType::ScopeResolution;
					if (Peek().Type != ECodeTokenType::Identifier)
					{
						OutError = bWasScopeResolution ? TEXT("Expected function name after '::'.") : TEXT("Expected member name after '.'.");
						return nullptr;
					}

					TSharedPtr<FCodeExpression> Member = MakeShared<FCodeExpression>();
					Member->Kind = ECodeExpressionKind::MemberAccess;
					Member->Text = bWasScopeResolution ? FString(TEXT("::")) + Peek().Text : Peek().Text;
					Member->Left = Expression;
					Expression = Member;
					++TokenIndex;
					continue;
				}

				if (Match(ECodeTokenType::LeftParen))
				{
					TSharedPtr<FCodeExpression> Call = MakeShared<FCodeExpression>();
					Call->Kind = ECodeExpressionKind::Call;
					Call->Left = Expression;

					if (!Match(ECodeTokenType::RightParen))
					{
						while (true)
						{
							FCodeCallArgument Argument;
							if (Peek().Type == ECodeTokenType::Identifier && Peek(1).Type == ECodeTokenType::Equals)
							{
								Argument.bIsNamed = true;
								Argument.Name = Peek().Text;
								TokenIndex += 2;
							}

							Argument.Expression = ParseAdditive(OutError);
							if (!Argument.Expression)
							{
								return nullptr;
							}

							Call->Arguments.Add(Argument);

							if (Match(ECodeTokenType::RightParen))
							{
								break;
							}

							if (!Expect(ECodeTokenType::Comma, OutError))
							{
								return nullptr;
							}
						}
					}

					Expression = Call;
					continue;
				}

				break;
			}

			return Expression;
		}

		TSharedPtr<FCodeExpression> ParsePrimary(FString& OutError)
		{
			if (Peek().Type == ECodeTokenType::Identifier)
			{
				TSharedPtr<FCodeExpression> Expression = MakeShared<FCodeExpression>();
				Expression->Kind = ECodeExpressionKind::Name;
				Expression->Text = Peek().Text;
				++TokenIndex;
				return Expression;
			}

			if (Peek().Type == ECodeTokenType::Number)
			{
				TSharedPtr<FCodeExpression> Expression = MakeShared<FCodeExpression>();
				Expression->Kind = ECodeExpressionKind::NumberLiteral;
				Expression->Text = Peek().Text;
				++TokenIndex;
				return Expression;
			}

			if (Peek().Type == ECodeTokenType::String)
			{
				TSharedPtr<FCodeExpression> Expression = MakeShared<FCodeExpression>();
				Expression->Kind = ECodeExpressionKind::StringLiteral;
				Expression->Text = Peek().Text;
				++TokenIndex;
				return Expression;
			}

			if (Match(ECodeTokenType::LeftParen))
			{
				TSharedPtr<FCodeExpression> Expression = ParseAdditive(OutError);
				if (!Expression)
				{
					return nullptr;
				}

				if (!Expect(ECodeTokenType::RightParen, OutError))
				{
					return nullptr;
				}

				return Expression;
			}

			OutError = FString::Printf(TEXT("Unexpected token '%s' in Graph expression."), *Peek().Text);
			return nullptr;
		}
	};

	static bool SplitTopLevelComparison(const FString& InText, FString& OutLeft, FString& OutOperator, FString& OutRight)
	{
		static const TArray<FString> Operators = {
			TEXT(">="),
			TEXT("<="),
			TEXT("=="),
			TEXT("!="),
			TEXT(">"),
			TEXT("<"),
		};

		int32 ParenthesisDepth = 0;
		int32 BraceDepth = 0;
		int32 BracketDepth = 0;
		bool bInString = false;

		for (int32 Index = 0; Index < InText.Len(); ++Index)
		{
			const TCHAR Char = InText[Index];

			if (bInString)
			{
				if (Char == TCHAR('\\') && InText.IsValidIndex(Index + 1))
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

			if (Char == TCHAR('('))
			{
				++ParenthesisDepth;
				continue;
			}
			if (Char == TCHAR(')'))
			{
				ParenthesisDepth = FMath::Max(0, ParenthesisDepth - 1);
				continue;
			}
			if (Char == TCHAR('{'))
			{
				++BraceDepth;
				continue;
			}
			if (Char == TCHAR('}'))
			{
				BraceDepth = FMath::Max(0, BraceDepth - 1);
				continue;
			}
			if (Char == TCHAR('['))
			{
				++BracketDepth;
				continue;
			}
			if (Char == TCHAR(']'))
			{
				BracketDepth = FMath::Max(0, BracketDepth - 1);
				continue;
			}

			if (ParenthesisDepth != 0 || BraceDepth != 0 || BracketDepth != 0)
			{
				continue;
			}

			for (const FString& Operator : Operators)
			{
				if (Index + Operator.Len() <= InText.Len() && InText.Mid(Index, Operator.Len()) == Operator)
				{
					OutLeft = InText.Left(Index).TrimStartAndEnd();
					OutOperator = Operator;
					OutRight = InText.Mid(Index + Operator.Len()).TrimStartAndEnd();
					return !OutLeft.IsEmpty() && !OutRight.IsEmpty();
				}
			}
		}

		return false;
	}

	static bool ParseCodeCondition(const FString& ConditionText, FCodeCondition& OutCondition, FString& OutError)
	{
		FString LeftText;
		FString OperatorText;
		FString RightText;
		if (!SplitTopLevelComparison(ConditionText, LeftText, OperatorText, RightText))
		{
			LeftText = ConditionText.TrimStartAndEnd();
			OperatorText = TEXT("truthy");
		}

		if (LeftText.IsEmpty())
		{
			OutError = TEXT("Graph if condition is empty.");
			return false;
		}

		FCodeExpressionParser LeftParser(LeftText);
		if (!LeftParser.Parse(OutCondition.Left, OutError))
		{
			OutError = FString::Printf(TEXT("In Graph if condition '%s': %s"), *ConditionText, *OutError);
			return false;
		}

		OutCondition.Operator = OperatorText;
		if (OperatorText == TEXT("truthy"))
		{
			return true;
		}

		FCodeExpressionParser RightParser(RightText);
		if (!RightParser.Parse(OutCondition.Right, OutError))
		{
			OutError = FString::Printf(TEXT("In Graph if condition '%s': %s"), *ConditionText, *OutError);
			return false;
		}

		return true;
	}

	static bool ParseIfStatement(const FString& StatementText, FCodeStatement& OutStatement, FString& OutError)
	{
		int32 Index = 0;
		SkipWhitespace(StatementText, Index);
		if (!MatchesKeywordAt(StatementText, Index, TEXT("if")))
		{
			return false;
		}

		Index += 2;
		SkipWhitespace(StatementText, Index);
		int32 ConditionCloseIndex = INDEX_NONE;
		if (!StatementText.IsValidIndex(Index)
			|| StatementText[Index] != TCHAR('(')
			|| !FindMatchingDelimiter(StatementText, Index, TCHAR('('), TCHAR(')'), ConditionCloseIndex))
		{
			OutError = FString::Printf(TEXT("Invalid Graph if statement '%s'."), *StatementText);
			return false;
		}

		const FString ConditionText = StatementText.Mid(Index + 1, ConditionCloseIndex - Index - 1).TrimStartAndEnd();
		Index = ConditionCloseIndex + 1;
		SkipWhitespace(StatementText, Index);

		int32 ThenCloseIndex = INDEX_NONE;
		if (!StatementText.IsValidIndex(Index)
			|| StatementText[Index] != TCHAR('{')
			|| !FindMatchingDelimiter(StatementText, Index, TCHAR('{'), TCHAR('}'), ThenCloseIndex))
		{
			OutError = FString::Printf(TEXT("Invalid Graph if body in '%s'."), *StatementText);
			return false;
		}

		const FString ThenBody = StatementText.Mid(Index + 1, ThenCloseIndex - Index - 1);
		Index = ThenCloseIndex + 1;
		SkipWhitespace(StatementText, Index);

		FString ElseBody;
		if (MatchesKeywordAt(StatementText, Index, TEXT("else")))
		{
			Index += 4;
			SkipWhitespace(StatementText, Index);

			if (MatchesKeywordAt(StatementText, Index, TEXT("if")))
			{
				ElseBody = StatementText.Mid(Index).TrimStartAndEnd();
				Index = StatementText.Len();
			}
			else
			{
				int32 ElseCloseIndex = INDEX_NONE;
				if (!StatementText.IsValidIndex(Index)
					|| StatementText[Index] != TCHAR('{')
					|| !FindMatchingDelimiter(StatementText, Index, TCHAR('{'), TCHAR('}'), ElseCloseIndex))
				{
					OutError = FString::Printf(TEXT("Invalid Graph else body in '%s'."), *StatementText);
					return false;
				}

				ElseBody = StatementText.Mid(Index + 1, ElseCloseIndex - Index - 1);
				Index = ElseCloseIndex + 1;
			}

			SkipWhitespace(StatementText, Index);
		}

		if (Index < StatementText.Len())
		{
			OutError = FString::Printf(TEXT("Unexpected text after Graph if statement: '%s'."), *StatementText.Mid(Index).TrimStartAndEnd());
			return false;
		}

		OutStatement = FCodeStatement{};
		OutStatement.bIsIfStatement = true;
		if (!ParseCodeCondition(ConditionText, OutStatement.Condition, OutError))
		{
			return false;
		}

		if (!ParseCodeStatements(ThenBody, OutStatement.ThenStatements, OutError))
		{
			OutError = FString::Printf(TEXT("In Graph if body: %s"), *OutError);
			return false;
		}

		if (!ElseBody.IsEmpty() && !ParseCodeStatements(ElseBody, OutStatement.ElseStatements, OutError))
		{
			OutError = FString::Printf(TEXT("In Graph else body: %s"), *OutError);
			return false;
		}

		return true;
	}

	bool ParseCodeStatements(const FString& InCode, TArray<FCodeStatement>& OutStatements, FString& OutError)
	{
		OutStatements.Reset();
		TArray<FString> Statements;
		if (!SplitStatements(RemoveComments(InCode), Statements, OutError))
		{
			return false;
		}

		for (const FString& StatementText : Statements)
		{
			int32 StatementProbeIndex = 0;
			SkipWhitespace(StatementText, StatementProbeIndex);
			if (MatchesKeywordAt(StatementText, StatementProbeIndex, TEXT("if")))
			{
				FCodeStatement IfStatement;
				if (!ParseIfStatement(StatementText, IfStatement, OutError))
				{
					return false;
				}

				OutStatements.Add(IfStatement);
				continue;
			}

			const auto ParseDeclarationStatement =
				[&OutError](const FString& DeclaredType, const FString& TargetName, const FString* InitializerText, FCodeStatement& OutStatement) -> bool
			{
				if (!InitializerText)
				{
					OutStatement = FCodeStatement{};
					OutStatement.bIsDeclaration = true;
					OutStatement.DeclaredType = DeclaredType;
					OutStatement.TargetName = TargetName;
					return true;
				}

				return MakeCodeDeclarationStatement(DeclaredType, TargetName, *InitializerText, OutStatement, OutError);
			};

			const TArray<FString> Declarators = SplitTopLevelSegments(StatementText, TCHAR(','));
			if (Declarators.Num() > 1)
			{
				FString FirstLeft;
				FString FirstRight;
				const bool bFirstHasAssignment = SplitTopLevelAssignment(Declarators[0], FirstLeft, FirstRight);
				const FString FirstDeclaratorText = bFirstHasAssignment ? FirstLeft : Declarators[0];

				FString SharedType;
				FString FirstName;
				if (SplitDeclarationTypeAndName(FirstDeclaratorText, SharedType, FirstName))
				{
					for (int32 DeclaratorIndex = 0; DeclaratorIndex < Declarators.Num(); ++DeclaratorIndex)
					{
						const FString& DeclaratorText = Declarators[DeclaratorIndex];
						FCodeStatement Statement;

						if (DeclaratorIndex == 0)
						{
							const FString* InitializerText = bFirstHasAssignment ? &FirstRight : nullptr;
							if (!ParseDeclarationStatement(SharedType, FirstName, InitializerText, Statement))
							{
								OutError = FString::Printf(TEXT("In Graph statement '%s': %s"), *StatementText, *OutError);
								return false;
							}
						}
						else
						{
							FString Left;
							FString Right;
							const bool bHasAssignment = SplitTopLevelAssignment(DeclaratorText, Left, Right);
							const FString NameToken = bHasAssignment ? Left : DeclaratorText;
							if (!IsIdentifierToken(NameToken))
							{
								OutError = FString::Printf(
									TEXT("In Graph statement '%s': '%s' is not a valid declarator in a comma-separated declaration."),
									*StatementText,
									*NameToken.TrimStartAndEnd());
								return false;
							}

							const FString TrimmedNameToken = NameToken.TrimStartAndEnd();
							const FString* InitializerText = bHasAssignment ? &Right : nullptr;
							if (!ParseDeclarationStatement(SharedType, TrimmedNameToken, InitializerText, Statement))
							{
								OutError = FString::Printf(TEXT("In Graph statement '%s': %s"), *StatementText, *OutError);
								return false;
							}
						}

						OutStatements.Add(Statement);
					}

					continue;
				}
			}

			FCodeStatement Statement;

			FString Left;
			FString Right;
			if (!SplitTopLevelAssignment(StatementText, Left, Right))
			{
				if (SplitDeclarationTypeAndName(StatementText, Statement.DeclaredType, Statement.TargetName))
				{
					Statement.bIsDeclaration = true;
					OutStatements.Add(Statement);
					continue;
				}

				FCodeExpressionParser ExpressionParser(StatementText);
				if (!ExpressionParser.Parse(Statement.Expression, OutError))
				{
					OutError = FString::Printf(TEXT("In Graph statement '%s': %s"), *StatementText, *OutError);
					return false;
				}

				Statement.bIsExpressionStatement = true;
				OutStatements.Add(Statement);
				continue;
			}

			Statement.bIsDeclaration = SplitDeclarationTypeAndName(Left, Statement.DeclaredType, Statement.TargetName);
			if (!Statement.bIsDeclaration)
			{
				Statement.TargetName = Left;
			}
			Statement.InitializerText = Right;

			if (IsBraceInitializerText(Right))
			{
				Statement.bUsesBraceInitializer = true;
				OutStatements.Add(Statement);
				continue;
			}

			FCodeExpressionParser ExpressionParser(Right);
			if (!ExpressionParser.Parse(Statement.Expression, OutError))
			{
				OutError = FString::Printf(TEXT("In Graph statement '%s': %s"), *StatementText, *OutError);
				return false;
			}

			OutStatements.Add(Statement);
		}

		return true;
	}

	bool ParseCodeExpression(const FString& InExpression, TSharedPtr<FCodeExpression>& OutExpression, FString& OutError)
	{
		FCodeExpressionParser Parser(InExpression);
		return Parser.Parse(OutExpression, OutError);
	}
}
