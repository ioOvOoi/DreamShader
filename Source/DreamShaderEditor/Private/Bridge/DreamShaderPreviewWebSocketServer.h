#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

class INetworkingWebSocket;
class IWebSocketServer;
class UMaterialInterface;

namespace UE::DreamShader::Editor::Private
{
	class FDreamShaderPreviewRenderContext;

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
			// Orbit camera angles in degrees, updated from drag-to-rotate input over previewControl
			// messages (see HandlePreviewControl) and re-applied to the material's ThumbnailInfo on
			// every KickoffFrame call. Defaults match USceneThumbnailInfo's own so a session that
			// never rotates the camera renders identically to today's baseline framing.
			float OrbitYaw = -157.5f;
			float OrbitPitch = -11.25f;
			TWeakObjectPtr<UMaterialInterface> Material;
			int32 Width = 512;
			int32 Height = 512;
			double LastFrameSeconds = 0.0;
			double FrameIntervalSeconds = 0.5;
			int32 FrameIndex = 0;
			int32 LastAckFrameIndex = -1;
			bool bFrameInFlight = false;
			bool bStreaming = false;
			// Persists the streamed render target + thumbnail scene across every tick of this
			// client's session (see FDreamShaderPreviewRenderContext) instead of recreating them
			// every frame; recreated fresh whenever a new streaming session starts for this client.
			TUniquePtr<FDreamShaderPreviewRenderContext> RenderContext;
		};

		void HandleClientConnected(INetworkingWebSocket* Socket);
		void HandleSocketClosed(INetworkingWebSocket* Socket);
		void HandlePacket(INetworkingWebSocket* Socket, void* Data, int32 Size);
		void HandlePreviewRequest(INetworkingWebSocket* Socket, const TSharedPtr<FJsonObject>& RequestObject);
		void HandlePreviewControl(INetworkingWebSocket* Socket, const TSharedPtr<FJsonObject>& RequestObject);
		void SendPreviewFrame(INetworkingWebSocket* Socket, FClientPreviewState& State, double NowSeconds);
		void SendJson(INetworkingWebSocket* Socket, const TSharedRef<FJsonObject>& JsonObject);
		void SendBinary(INetworkingWebSocket* Socket, const uint8* Data, int64 Length);
		// INetworkingWebSocket::Send()'s third parameter is bPrependSize, not a text/binary opcode
		// selector -- every message this module sends goes out as a raw WS *binary* frame regardless
		// (FWebSocket::OnRawWebSocketWritable always calls lws_write with LWS_WRITE_BINARY), so the
		// WS opcode can't be used to tell a JSON message apart from an image on the client. Instead
		// every message is self-describing: [4-byte length][1-byte type tag][payload], with
		// bPrependSize always requested so the length prefix is present. See preview.js's matching
		// decoder for the client side of this format.
		void SendTagged(INetworkingWebSocket* Socket, uint8 TypeTag, const uint8* Data, int64 Length);

	private:
		TUniquePtr<IWebSocketServer> Server;
		TSet<INetworkingWebSocket*> Clients;
		TMap<INetworkingWebSocket*, FClientPreviewState> PreviewStates;
		uint32 Port = 17864;
		bool bStarted = false;
	};
}
