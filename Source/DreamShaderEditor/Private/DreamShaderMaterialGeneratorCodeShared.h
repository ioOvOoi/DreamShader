#pragma once

#include "DreamShaderMaterialGeneratorPrivate.h"
#include "DreamShaderModule.h"

#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionBreakMaterialAttributes.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionIf.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionScreenPosition.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionTransformPosition.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"
#include "UObject/UnrealType.h"

namespace UE::DreamShader::Editor::Private
{
	inline void ConnectCodeValueToInput(FExpressionInput& Input, const FCodeValue& Value)
	{
		if (Value.Expression)
		{
			Input.Connect(Value.OutputIndex, Value.Expression);
		}
	}

	inline bool TryResolveExpressionOutputIndex(const UMaterialExpression* Expression, const FString& OutputSpecifier, int32& OutIndex)
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

	inline bool IsMaterialAttributesComponentType(const int32 ComponentCount, const bool bIsTextureObject)
	{
		return ComponentCount == 0 && !bIsTextureObject;
	}

	inline bool IsSubstrateComponentType(const int32 ComponentCount, const bool bIsTextureObject)
	{
		return ComponentCount == -1 && !bIsTextureObject;
	}

	inline bool TrySplitMemberTarget(const FString& TargetText, FString& OutBaseName, FString& OutMemberName)
	{
		FString Left;
		FString Right;
		if (!TargetText.TrimStartAndEnd().Split(TEXT("."), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromStart))
		{
			return false;
		}

		OutBaseName = Left.TrimStartAndEnd();
		OutMemberName = Right.TrimStartAndEnd();
		return !OutBaseName.IsEmpty() && !OutMemberName.IsEmpty();
	}

	inline bool ResolveTypeNameForComponentCount(const int32 ComponentCount, FString& OutTypeName)
	{
		switch (ComponentCount)
		{
		case -1: OutTypeName = TEXT("Substrate"); return true;
		case 0: OutTypeName = TEXT("MaterialAttributes"); return true;
		case 1: OutTypeName = TEXT("float"); return true;
		case 2: OutTypeName = TEXT("float2"); return true;
		case 3: OutTypeName = TEXT("float3"); return true;
		case 4: OutTypeName = TEXT("float4"); return true;
		default:
			return false;
		}
	}

	inline bool TryResolveMaterialAttributesBreakOutputIndex(const EMaterialProperty Property, int32& OutOutputIndex)
	{
		switch (Property)
		{
		case MP_BaseColor: OutOutputIndex = 0; return true;
		case MP_Metallic: OutOutputIndex = 1; return true;
		case MP_Specular: OutOutputIndex = 2; return true;
		case MP_Roughness: OutOutputIndex = 3; return true;
		case MP_Anisotropy: OutOutputIndex = 4; return true;
		case MP_EmissiveColor: OutOutputIndex = 5; return true;
		case MP_Opacity: OutOutputIndex = 6; return true;
		case MP_OpacityMask: OutOutputIndex = 7; return true;
		case MP_Normal: OutOutputIndex = 8; return true;
		case MP_Tangent: OutOutputIndex = 9; return true;
		case MP_WorldPositionOffset: OutOutputIndex = 10; return true;
		case MP_SubsurfaceColor: OutOutputIndex = 11; return true;
		case MP_CustomData0: OutOutputIndex = 12; return true;
		case MP_CustomData1: OutOutputIndex = 13; return true;
		case MP_AmbientOcclusion: OutOutputIndex = 14; return true;
		case MP_Refraction: OutOutputIndex = 15; return true;
		case MP_CustomizedUVs0: OutOutputIndex = 16; return true;
		case MP_CustomizedUVs1: OutOutputIndex = 17; return true;
		case MP_CustomizedUVs2: OutOutputIndex = 18; return true;
		case MP_CustomizedUVs3: OutOutputIndex = 19; return true;
		case MP_CustomizedUVs4: OutOutputIndex = 20; return true;
		case MP_CustomizedUVs5: OutOutputIndex = 21; return true;
		case MP_CustomizedUVs6: OutOutputIndex = 22; return true;
		case MP_CustomizedUVs7: OutOutputIndex = 23; return true;
		case MP_PixelDepthOffset: OutOutputIndex = 24; return true;
		case MP_Displacement: OutOutputIndex = 26; return true;
#ifdef MOON_ENGINE
		case MP_MooaEncodedAttribute0: OutOutputIndex = 27; return true;
		case MP_MooaEncodedAttribute1: OutOutputIndex = 28; return true;
		case MP_MooaEncodedAttribute2: OutOutputIndex = 29; return true;
		case MP_MooaEncodedAttribute3: OutOutputIndex = 30; return true;
		case MP_MooaEncodedAttribute4: OutOutputIndex = 31; return true;
#endif
		default:
			return false;
		}
	}

	inline bool IsIdentifierBoundary(const FString& Text, const int32 Index)
	{
		if (!Text.IsValidIndex(Index))
		{
			return true;
		}

		const TCHAR Char = Text[Index];
		return !(FChar::IsAlnum(Char) || Char == TCHAR('_'));
	}

	inline void SkipWhitespace(const FString& Text, int32& InOutIndex)
	{
		while (Text.IsValidIndex(InOutIndex) && FChar::IsWhitespace(Text[InOutIndex]))
		{
			++InOutIndex;
		}
	}

	inline bool FindMatchingDelimiter(
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
}
