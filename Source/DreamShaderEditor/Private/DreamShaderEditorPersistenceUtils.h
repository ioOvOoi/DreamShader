// Copyright (c) 2026 TypeDreamMoon. All rights reserved.
//
// Shared JSON/SQLite persistence helpers for the DreamShader editor services (the diagnostics store
// and the workspace service). These were previously duplicated byte-for-byte as anonymous-namespace
// functions in both translation units; in a unity build that happened to group both .cpp files into
// the same blob, the identical definitions in the same (single) anonymous namespace would collide
// with a redefinition error. The grouping is non-deterministic and flips whenever a .cpp is added or
// removed, so the green build was accidental. Defined inline here as a single source of truth.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SQLitePreparedStatement.h"

namespace UE::DreamShader::Editor::Private
{
	inline bool SerializeJsonObject(const TSharedRef<FJsonObject>& Object, FString& OutText)
	{
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutText);
		return FJsonSerializer::Serialize(Object, Writer);
	}

	inline bool BindAndExecute(FSQLitePreparedStatement& Statement)
	{
		const bool bResult = Statement.Execute();
		Statement.Reset();
		Statement.ClearBindings();
		return bResult;
	}
}
