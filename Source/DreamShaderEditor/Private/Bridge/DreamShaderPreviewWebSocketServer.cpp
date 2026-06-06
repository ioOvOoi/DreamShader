#include "Bridge/DreamShaderPreviewWebSocketServer.h"

#include "DreamShaderModule.h"
#include "Preview/DreamShaderPreviewRenderer.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "IWebSocketNetworkingModule.h"
#include "IWebSocketServer.h"
#include "INetworkingWebSocket.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "WebSocketNetworkingDelegates.h"

namespace UE::DreamShader::Editor::Private
{
	namespace
	{
		static constexpr uint32 DefaultPreviewWebSocketPort = 17864;

		FString GetStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
		{
			FString Value;
			if (Object.IsValid())
			{
				Object->TryGetStringField(FieldName, Value);
			}
			return Value;
		}

		void SetOptionalStringField(const TSharedRef<FJsonObject>& Object, const TCHAR* FieldName, const FString& Value)
		{
			if (!Value.IsEmpty())
			{
				Object->SetStringField(FieldName, Value);
			}
		}

		double GetFrameIntervalSeconds(const TSharedPtr<FJsonObject>& Object)
		{
			double FrameRate = 2.0;
			if (Object.IsValid())
			{
				Object->TryGetNumberField(TEXT("frameRate"), FrameRate);
			}
			if (FrameRate <= 0.0)
			{
				return 0.0;
			}
			return 1.0 / FMath::Clamp(FrameRate, 0.25, 10.0);
		}
	}

	FDreamShaderPreviewWebSocketServer::FDreamShaderPreviewWebSocketServer() = default;

	FDreamShaderPreviewWebSocketServer::~FDreamShaderPreviewWebSocketServer()
	{
		Shutdown();
	}

	bool FDreamShaderPreviewWebSocketServer::Startup(uint32 InPort)
	{
		if (bStarted)
		{
			return true;
		}

		Port = InPort > 0 ? InPort : DefaultPreviewWebSocketPort;
		IWebSocketNetworkingModule* WebSocketModule = FModuleManager::LoadModulePtr<IWebSocketNetworkingModule>(TEXT("WebSocketNetworking"));
		if (!WebSocketModule)
		{
			UE_LOG(LogDreamShader, Warning, TEXT("DreamShader preview WebSocket server could not load WebSocketNetworking."));
			return false;
		}

		Server = WebSocketModule->CreateServer();
		if (!Server)
		{
			UE_LOG(LogDreamShader, Warning, TEXT("DreamShader preview WebSocket server could not be created."));
			return false;
		}

		Server->SetFilterConnectionCallback(FWebSocketFilterConnectionCallback::CreateLambda([](FString Origin, FString ClientIP)
		{
			(void)Origin;
			return ClientIP == TEXT("127.0.0.1") || ClientIP == TEXT("localhost")
				? EWebsocketConnectionFilterResult::ConnectionAccepted
				: EWebsocketConnectionFilterResult::ConnectionRefused;
		}));

		if (!Server->Init(Port, FWebSocketClientConnectedCallBack::CreateRaw(this, &FDreamShaderPreviewWebSocketServer::HandleClientConnected), TEXT("127.0.0.1")))
		{
			UE_LOG(LogDreamShader, Warning, TEXT("DreamShader preview WebSocket server failed to listen on 127.0.0.1:%u."), Port);
			Server.Reset();
			return false;
		}

		bStarted = true;
		UE_LOG(LogDreamShader, Display, TEXT("DreamShader preview WebSocket server listening on 127.0.0.1:%u."), Port);
		return true;
	}

	void FDreamShaderPreviewWebSocketServer::Shutdown()
	{
		Clients.Reset();
		PreviewStates.Reset();
		Server.Reset();
		bStarted = false;
	}

	void FDreamShaderPreviewWebSocketServer::Tick()
	{
		if (!bStarted || !Server)
		{
			return;
		}

		Server->Tick();
		TArray<INetworkingWebSocket*> ClientSnapshot = Clients.Array();
		const double NowSeconds = FPlatformTime::Seconds();
		for (INetworkingWebSocket* Client : ClientSnapshot)
		{
			if (Client && Clients.Contains(Client))
			{
				Client->Tick();
				if (FClientPreviewState* State = PreviewStates.Find(Client))
				{
					SendPreviewFrame(Client, *State, NowSeconds);
				}
			}
		}
	}

	void FDreamShaderPreviewWebSocketServer::HandleClientConnected(INetworkingWebSocket* Socket)
	{
		if (!Socket)
		{
			return;
		}

		Clients.Add(Socket);
		Socket->SetReceiveCallBack(FWebSocketPacketReceivedCallBack::CreateLambda([this, Socket](void* Data, int32 Size)
		{
			HandlePacket(Socket, Data, Size);
		}));
		Socket->SetSocketClosedCallBack(FWebSocketInfoCallBack::CreateRaw(this, &FDreamShaderPreviewWebSocketServer::HandleSocketClosed, Socket));
		UE_LOG(LogDreamShader, Display, TEXT("DreamShader preview WebSocket client connected: %s"), *Socket->RemoteEndPoint(true));
	}

	void FDreamShaderPreviewWebSocketServer::HandleSocketClosed(INetworkingWebSocket* Socket)
	{
		Clients.Remove(Socket);
		PreviewStates.Remove(Socket);
	}

	void FDreamShaderPreviewWebSocketServer::HandlePacket(INetworkingWebSocket* Socket, void* Data, int32 Size)
	{
		if (!Socket || !Data || Size <= 0)
		{
			return;
		}

		FUTF8ToTCHAR TextConverter(reinterpret_cast<const ANSICHAR*>(Data), Size);
		const FString Text(TextConverter.Length(), TextConverter.Get());
		TSharedPtr<FJsonObject> RequestObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, RequestObject) || !RequestObject.IsValid())
		{
			TSharedRef<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
			ErrorObject->SetStringField(TEXT("type"), TEXT("previewResult"));
			ErrorObject->SetStringField(TEXT("status"), TEXT("error"));
			ErrorObject->SetStringField(TEXT("message"), TEXT("Invalid DreamShader preview request JSON."));
			ErrorObject->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());
			SendJson(Socket, ErrorObject);
			return;
		}

		const FString Type = GetStringField(RequestObject, TEXT("type"));
		const FString Action = GetStringField(RequestObject, TEXT("action"));
		if (Type.Equals(TEXT("previewMaterial"), ESearchCase::IgnoreCase) || Action.Equals(TEXT("previewMaterial"), ESearchCase::IgnoreCase))
		{
			HandlePreviewRequest(Socket, RequestObject);
		}
		else if (Type.Equals(TEXT("previewControl"), ESearchCase::IgnoreCase))
		{
			HandlePreviewControl(Socket, RequestObject);
		}
	}

	void FDreamShaderPreviewWebSocketServer::HandlePreviewRequest(INetworkingWebSocket* Socket, const TSharedPtr<FJsonObject>& RequestObject)
	{
		FDreamShaderPreviewRequest PreviewRequest;
		PreviewRequest.SourceFilePath = GetStringField(RequestObject, TEXT("sourceFile"));
		PreviewRequest.Mesh = GetStringField(RequestObject, TEXT("mesh"));

		double Width = PreviewRequest.Width;
		double Height = PreviewRequest.Height;
		RequestObject->TryGetNumberField(TEXT("width"), Width);
		RequestObject->TryGetNumberField(TEXT("height"), Height);
		PreviewRequest.Width = FMath::Clamp(FMath::RoundToInt(Width), 64, 2048);
		PreviewRequest.Height = FMath::Clamp(FMath::RoundToInt(Height), 64, 2048);

		FDreamShaderPreviewResult PreviewResult;
		UMaterial* Material = nullptr;
		const bool bPreviewSucceeded = FDreamShaderPreviewRenderer::ResolvePreviewMaterial(PreviewRequest, PreviewResult, Material);
		const FString Status = bPreviewSucceeded ? TEXT("ready") : TEXT("error");
		TArray64<uint8> FirstFramePngData;
		if (bPreviewSucceeded)
		{
			FString RenderError;
			FString ImagePath;
			const bool bSavedPreview = FDreamShaderPreviewRenderer::SaveMaterialPreviewFrame(
				Material,
				PreviewResult.SourceFilePath,
				PreviewRequest.Width,
				PreviewRequest.Height,
				ImagePath,
				RenderError);
			if (!bSavedPreview || !FDreamShaderPreviewRenderer::RenderMaterialPreviewFrame(Material, PreviewRequest.Width, PreviewRequest.Height, FirstFramePngData, RenderError))
			{
				PreviewResult.bSucceeded = false;
				PreviewResult.Message = RenderError;
			}
			else
			{
				PreviewResult.ImagePath = ImagePath;
				PreviewResult.Message = FString::Printf(TEXT("Streaming preview for %s."), *PreviewResult.AssetPath);
			}
		}
		const bool bStreamingReady = bPreviewSucceeded && PreviewResult.bSucceeded;
		FDreamShaderPreviewRenderer::WritePreviewResult(PreviewResult, bStreamingReady ? TEXT("ready") : TEXT("error"), GetStringField(RequestObject, TEXT("requestId")));

		TSharedRef<FJsonObject> ResultObject = MakeShared<FJsonObject>();
		ResultObject->SetStringField(TEXT("type"), TEXT("previewResult"));
		SetOptionalStringField(ResultObject, TEXT("requestId"), GetStringField(RequestObject, TEXT("requestId")));
		ResultObject->SetStringField(TEXT("status"), bStreamingReady ? TEXT("ready") : TEXT("error"));
		ResultObject->SetStringField(TEXT("sourceFile"), PreviewResult.SourceFilePath);
		ResultObject->SetStringField(TEXT("assetPath"), PreviewResult.AssetPath);
		ResultObject->SetStringField(TEXT("imagePath"), PreviewResult.ImagePath);
		ResultObject->SetStringField(TEXT("mesh"), PreviewResult.Mesh);
		ResultObject->SetStringField(TEXT("message"), PreviewResult.Message);
		ResultObject->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());

		if (bStreamingReady && !FirstFramePngData.IsEmpty())
		{
			ResultObject->SetStringField(TEXT("imageBase64"), FBase64::Encode(FirstFramePngData.GetData(), static_cast<uint32>(FirstFramePngData.Num())));
		}

		SendJson(Socket, ResultObject);
		if (bStreamingReady)
		{
			FClientPreviewState& State = PreviewStates.FindOrAdd(Socket);
			State.RequestId = GetStringField(RequestObject, TEXT("requestId"));
			State.SourceFilePath = PreviewResult.SourceFilePath;
			State.AssetPath = PreviewResult.AssetPath;
			State.Mesh = PreviewResult.Mesh;
			State.Material = Material;
			State.Width = PreviewRequest.Width;
			State.Height = PreviewRequest.Height;
			State.LastFrameSeconds = FPlatformTime::Seconds();
			State.FrameIntervalSeconds = GetFrameIntervalSeconds(RequestObject);
			State.FrameIndex = 0;
			State.LastAckFrameIndex = -1;
			State.bFrameInFlight = false;
			bool bStream = true;
			RequestObject->TryGetBoolField(TEXT("stream"), bStream);
			State.bStreaming = bStream && State.FrameIntervalSeconds > 0.0;
			UE_LOG(LogDreamShader, Display, TEXT("DreamShader preview WebSocket: %s"), *PreviewResult.Message);
		}
		else
		{
			PreviewStates.Remove(Socket);
			UE_LOG(LogDreamShader, Error, TEXT("DreamShader preview WebSocket: %s"), *PreviewResult.Message);
		}
	}

	void FDreamShaderPreviewWebSocketServer::HandlePreviewControl(INetworkingWebSocket* Socket, const TSharedPtr<FJsonObject>& RequestObject)
	{
		FClientPreviewState* State = PreviewStates.Find(Socket);
		if (!State)
		{
			return;
		}
		const FString RequestId = GetStringField(RequestObject, TEXT("requestId"));
		if (!RequestId.IsEmpty() && !State->RequestId.IsEmpty() && RequestId != State->RequestId)
		{
			return;
		}

		bool bStream = State->bStreaming;
		RequestObject->TryGetBoolField(TEXT("stream"), bStream);
		State->FrameIntervalSeconds = GetFrameIntervalSeconds(RequestObject);
		State->bStreaming = bStream && State->FrameIntervalSeconds > 0.0;

		double AckFrameIndex = -1.0;
		if (RequestObject->TryGetNumberField(TEXT("ackFrameIndex"), AckFrameIndex))
		{
			State->LastAckFrameIndex = FMath::RoundToInt(AckFrameIndex);
			State->bFrameInFlight = false;
		}
		else if (State->bStreaming)
		{
			State->bFrameInFlight = false;
		}
	}

	void FDreamShaderPreviewWebSocketServer::SendPreviewFrame(INetworkingWebSocket* Socket, FClientPreviewState& State, double NowSeconds)
	{
		if (!State.bStreaming || State.bFrameInFlight || !State.Material.IsValid() || State.FrameIntervalSeconds <= 0.0 || NowSeconds - State.LastFrameSeconds < State.FrameIntervalSeconds)
		{
			return;
		}

		State.LastFrameSeconds = NowSeconds;
		TArray64<uint8> PngData;
		FString Error;
		if (!FDreamShaderPreviewRenderer::RenderMaterialPreviewFrame(State.Material.Get(), State.Width, State.Height, PngData, Error))
		{
			TSharedRef<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
			ErrorObject->SetStringField(TEXT("type"), TEXT("previewResult"));
			SetOptionalStringField(ErrorObject, TEXT("requestId"), State.RequestId);
			ErrorObject->SetStringField(TEXT("status"), TEXT("error"));
			ErrorObject->SetStringField(TEXT("sourceFile"), State.SourceFilePath);
			ErrorObject->SetStringField(TEXT("assetPath"), State.AssetPath);
			ErrorObject->SetStringField(TEXT("mesh"), State.Mesh);
			ErrorObject->SetStringField(TEXT("message"), Error);
			ErrorObject->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());
			SendJson(Socket, ErrorObject);
			State.bStreaming = false;
			return;
		}

		TSharedRef<FJsonObject> FrameObject = MakeShared<FJsonObject>();
		FrameObject->SetStringField(TEXT("type"), TEXT("previewFrame"));
		SetOptionalStringField(FrameObject, TEXT("requestId"), State.RequestId);
		FrameObject->SetStringField(TEXT("sourceFile"), State.SourceFilePath);
		FrameObject->SetStringField(TEXT("assetPath"), State.AssetPath);
		FrameObject->SetStringField(TEXT("mesh"), State.Mesh);
		FrameObject->SetNumberField(TEXT("frameIndex"), State.FrameIndex++);
		FrameObject->SetStringField(TEXT("imageBase64"), FBase64::Encode(PngData.GetData(), static_cast<uint32>(PngData.Num())));
		FrameObject->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());
		SendJson(Socket, FrameObject);
		State.bFrameInFlight = true;
	}

	void FDreamShaderPreviewWebSocketServer::SendJson(INetworkingWebSocket* Socket, const TSharedRef<FJsonObject>& JsonObject)
	{
		if (!Socket)
		{
			return;
		}

		FString OutputText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputText);
		if (!FJsonSerializer::Serialize(JsonObject, Writer))
		{
			return;
		}

		FTCHARToUTF8 Converter(*OutputText);
		Socket->Send(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length(), false);
		Socket->Flush();
	}
}
