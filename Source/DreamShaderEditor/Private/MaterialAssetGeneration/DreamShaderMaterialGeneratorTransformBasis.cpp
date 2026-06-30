// Copyright (c) 2026 TypeDreamMoon. All rights reserved.
//
// FCodeGraphBuilder transform-basis / transform-target string->enum resolvers used by the UE
// builtin TransformVector / TransformPosition nodes. Pure string-to-enum lookups, no graph state.
// Extracted byte-for-byte from DreamShaderMaterialGeneratorCodeUE.cpp; the member declarations stay
// in the FCodeGraphBuilder class header, so all call sites are unchanged.

#include "DreamShaderMaterialGeneratorCodeShared.h"

namespace UE::DreamShader::Editor::Private
{
	bool FCodeGraphBuilder::TryResolveVectorTransformBasis(const FString& InText, EMaterialVectorCoordTransformSource& OutSource) const
	{
		FString Value = UE::DreamShader::NormalizeSettingKey(InText);
		if (Value == TEXT("tangent"))
		{
			OutSource = TRANSFORMSOURCE_Tangent;
			return true;
		}
		if (Value == TEXT("local"))
		{
			OutSource = TRANSFORMSOURCE_Local;
			return true;
		}
		if (Value == TEXT("world") || Value == TEXT("absoluteworld"))
		{
			OutSource = TRANSFORMSOURCE_World;
			return true;
		}
		if (Value == TEXT("view"))
		{
			OutSource = TRANSFORMSOURCE_View;
			return true;
		}
		if (Value == TEXT("camera"))
		{
			OutSource = TRANSFORMSOURCE_Camera;
			return true;
		}
		if (Value == TEXT("instance") || Value == TEXT("particle") || Value == TEXT("instanceparticle"))
		{
			OutSource = TRANSFORMSOURCE_Instance;
			return true;
		}
		return false;
	}

	bool FCodeGraphBuilder::TryResolveVectorTransformTarget(const FString& InText, EMaterialVectorCoordTransform& OutTarget) const
	{
		FString Value = UE::DreamShader::NormalizeSettingKey(InText);
		if (Value == TEXT("tangent"))
		{
			OutTarget = TRANSFORM_Tangent;
			return true;
		}
		if (Value == TEXT("local"))
		{
			OutTarget = TRANSFORM_Local;
			return true;
		}
		if (Value == TEXT("world") || Value == TEXT("absoluteworld"))
		{
			OutTarget = TRANSFORM_World;
			return true;
		}
		if (Value == TEXT("view"))
		{
			OutTarget = TRANSFORM_View;
			return true;
		}
		if (Value == TEXT("camera"))
		{
			OutTarget = TRANSFORM_Camera;
			return true;
		}
		if (Value == TEXT("instance") || Value == TEXT("particle") || Value == TEXT("instanceparticle"))
		{
			OutTarget = TRANSFORM_Instance;
			return true;
		}
		return false;
	}

	bool FCodeGraphBuilder::TryResolvePositionTransformBasis(const FString& InText, EMaterialPositionTransformSource& OutBasis) const
	{
		FString Value = UE::DreamShader::NormalizeSettingKey(InText);
		if (Value == TEXT("local"))
		{
			OutBasis = TRANSFORMPOSSOURCE_Local;
			return true;
		}
		if (Value == TEXT("world") || Value == TEXT("absoluteworld"))
		{
			OutBasis = TRANSFORMPOSSOURCE_World;
			return true;
		}
		if (Value == TEXT("periodicworld"))
		{
#if DREAMSHADER_UE_VERSION_AT_LEAST(5, 5)
			OutBasis = TRANSFORMPOSSOURCE_PeriodicWorld;
			return true;
#else
			return false;
#endif
		}
		if (Value == TEXT("translatedworld") || Value == TEXT("camerarelativeworld"))
		{
			OutBasis = TRANSFORMPOSSOURCE_TranslatedWorld;
			return true;
		}
		if (Value == TEXT("firstperson") || Value == TEXT("firstpersontranslatedworld"))
		{
#if DREAMSHADER_UE_VERSION_AT_LEAST(5, 6)
			OutBasis = TRANSFORMPOSSOURCE_FirstPersonTranslatedWorld;
			return true;
#else
			return false;
#endif
		}
		if (Value == TEXT("view"))
		{
			OutBasis = TRANSFORMPOSSOURCE_View;
			return true;
		}
		if (Value == TEXT("camera"))
		{
			OutBasis = TRANSFORMPOSSOURCE_Camera;
			return true;
		}
		if (Value == TEXT("instance") || Value == TEXT("particle") || Value == TEXT("instanceparticle"))
		{
			OutBasis = TRANSFORMPOSSOURCE_Instance;
			return true;
		}
		return false;
	}
}
