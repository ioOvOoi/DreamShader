#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

class INetworkingWebSocket;
class IWebSocketServer;
class UMaterial;

namespace UE::DreamShader::Editor::Private
{
	class FDreamShaderPreviewWebSocketServer
	{
	public:
		FDreamShaderPreviewWebSocketServer();
		~FDreamShaderPreviewWebSocketServer();

		bool Startup(uint32 InPort = 17864);
		void Shutdown();
		void Tick();

	private:
		struct FClientPreviewState
		{
			FString RequestId;
			FString SourceFilePath;
			FString AssetPath;
			FString Mesh;
			TWeakObjectPtr<UMaterial> Material;
			int32 Width = 512;
			int32 Height = 512;
			double LastFrameSeconds = 0.0;
			double FrameIntervalSeconds = 0.5;
			int32 FrameIndex = 0;
			int32 LastAckFrameIndex = -1;
			bool bFrameInFlight = false;
			bool bStreaming = false;
		};

		void HandleClientConnected(INetworkingWebSocket* Socket);
		void HandleSocketClosed(INetworkingWebSocket* Socket);
		void HandlePacket(INetworkingWebSocket* Socket, void* Data, int32 Size);
		void HandlePreviewRequest(INetworkingWebSocket* Socket, const TSharedPtr<FJsonObject>& RequestObject);
		void HandlePreviewControl(INetworkingWebSocket* Socket, const TSharedPtr<FJsonObject>& RequestObject);
		void SendPreviewFrame(INetworkingWebSocket* Socket, FClientPreviewState& State, double NowSeconds);
		void SendJson(INetworkingWebSocket* Socket, const TSharedRef<FJsonObject>& JsonObject);

	private:
		TUniquePtr<IWebSocketServer> Server;
		TSet<INetworkingWebSocket*> Clients;
		TMap<INetworkingWebSocket*, FClientPreviewState> PreviewStates;
		uint32 Port = 17864;
		bool bStarted = false;
	};
}
