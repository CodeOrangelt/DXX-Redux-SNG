// See gns_bridge.h for the rationale/scope of this file.

#include "gns_bridge.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>

#include <cstring>
#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

#define GNS_BRIDGE_MAX_PLAYERS 8

namespace {

gns_bridge_send_signal_fn g_send_signal = nullptr;
gns_bridge_route_found_fn g_route_found = nullptr;

HSteamNetConnection g_conn[GNS_BRIDGE_MAX_PLAYERS];

// Set immediately before calling ReceivedP2PCustomSignal() for a signal that
// might represent a brand new inbound connection, so OnConnectRequest() can
// find out which of our own player slots it belongs to. Custom signaling
// dispatches OnConnectRequest synchronously from within that call.
int g_pending_inbound_player = -1;

int PlayerForConn(HSteamNetConnection hConn)
{
	for (int i = 0; i < GNS_BRIDGE_MAX_PLAYERS; i++)
		if (g_conn[i] == hConn)
			return i;
	return -1;
}

// Reused for every connection -- SendSignal() gets told which hConn it's
// for, and we recover the player_id via GetConnectionUserData().
class BridgeSignaling final : public ISteamNetworkingConnectionSignaling
{
public:
	bool SendSignal(HSteamNetConnection hConn, const SteamNetConnectionInfo_t &info, const void *pMsg, int cbMsg) override
	{
		(void)info;
		if (!g_send_signal)
			return false;
		int64 player_id = SteamNetworkingSockets()->GetConnectionUserData(hConn);
		if (player_id < 0 || player_id >= GNS_BRIDGE_MAX_PLAYERS)
			return false;
		g_send_signal((int)player_id, (const unsigned char *)pMsg, cbMsg);
		return true;
	}

	void Release() override
	{
		// Shared/static instance -- nothing to free.
	}
};

BridgeSignaling g_signaling;

class BridgeRecvContext final : public ISteamNetworkingSignalingRecvContext
{
public:
	ISteamNetworkingConnectionSignaling *OnConnectRequest(HSteamNetConnection hConn, const SteamNetworkingIdentity &identityPeer, int nLocalVirtualPort) override
	{
		(void)identityPeer;
		(void)nLocalVirtualPort;

		if (g_pending_inbound_player < 0 || g_pending_inbound_player >= GNS_BRIDGE_MAX_PLAYERS)
			return nullptr; // no idea who this is from -- ignore per GNS's own recommendation

		int player_id = g_pending_inbound_player;

		// Already have a connection for this player (e.g. duplicate/retransmitted
		// signal) -- don't leak a second one.
		if (g_conn[player_id] != k_HSteamNetConnection_Invalid && g_conn[player_id] != hConn)
			return nullptr;

		g_conn[player_id] = hConn;
		SteamNetworkingSockets()->SetConnectionUserData(hConn, player_id);
		SteamNetworkingSockets()->AcceptConnection(hConn);
		return &g_signaling;
	}

	void SendRejectionSignal(const SteamNetworkingIdentity &identityPeer, const void *pMsg, int cbMsg) override
	{
		(void)identityPeer;
		(void)pMsg;
		(void)cbMsg;
		// Nothing to do -- we never actively reject, only ignore.
	}
};

BridgeRecvContext g_recv_context;

void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo)
{
	int player_id = PlayerForConn(pInfo->m_hConn);
	if (player_id < 0)
		return;

	switch (pInfo->m_info.m_eState)
	{
	case k_ESteamNetworkingConnectionState_Connected:
	{
		if (g_route_found && pInfo->m_info.m_addrRemote.IsIPv4())
		{
			struct sockaddr_in sin;
			memset(&sin, 0, sizeof(sin));
			sin.sin_family = AF_INET;
			sin.sin_addr.s_addr = htonl(pInfo->m_info.m_addrRemote.GetIPv4());
			sin.sin_port = htons(pInfo->m_info.m_addrRemote.m_port);
			g_route_found(player_id, &sin, (int)sizeof(sin));
		}
		break;
	}

	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
		g_conn[player_id] = k_HSteamNetConnection_Invalid;
		break;

	default:
		break;
	}
}

} // namespace

extern "C" void gns_bridge_set_callbacks(gns_bridge_send_signal_fn send_signal, gns_bridge_route_found_fn route_found)
{
	g_send_signal = send_signal;
	g_route_found = route_found;
}

static bool g_initialized = false;

extern "C" int gns_bridge_init(void)
{
	if (g_initialized)
		return 1; // net_udp_init() can run multiple times per process; keep existing state.

	for (int i = 0; i < GNS_BRIDGE_MAX_PLAYERS; i++)
		g_conn[i] = k_HSteamNetConnection_Invalid;

	SteamNetworkingErrMsg errMsg;
	if (!GameNetworkingSockets_Init(nullptr, errMsg))
	{
		fprintf(stderr, "GNS bridge: init failed: %s\n", errMsg);
		return 0;
	}

	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(OnConnectionStatusChanged);
	g_initialized = true;
	return 1;
}

extern "C" void gns_bridge_shutdown(void)
{
	if (!g_initialized)
		return;

	for (int i = 0; i < GNS_BRIDGE_MAX_PLAYERS; i++)
	{
		if (g_conn[i] != k_HSteamNetConnection_Invalid)
		{
			SteamNetworkingSockets()->CloseConnection(g_conn[i], 0, nullptr, false);
			g_conn[i] = k_HSteamNetConnection_Invalid;
		}
	}
	GameNetworkingSockets_Kill();
	g_initialized = false;
}

extern "C" void gns_bridge_poll(void)
{
	SteamNetworkingSockets()->RunCallbacks();
}

extern "C" void gns_bridge_connect_to_player(int player_id)
{
	if (player_id < 0 || player_id >= GNS_BRIDGE_MAX_PLAYERS)
		return;
	if (g_conn[player_id] != k_HSteamNetConnection_Invalid)
		return; // already connecting/connected

	SteamNetworkingIdentity identity;
	identity.Clear();
	char buf[16];
	snprintf(buf, sizeof(buf), "sng%d", player_id);
	identity.SetGenericString(buf);

	HSteamNetConnection hConn = SteamNetworkingSockets()->ConnectP2PCustomSignaling(&g_signaling, &identity, 0, 0, nullptr);
	if (hConn == k_HSteamNetConnection_Invalid)
		return;

	g_conn[player_id] = hConn;
	SteamNetworkingSockets()->SetConnectionUserData(hConn, player_id);
}

extern "C" void gns_bridge_reset_player(int player_id)
{
	if (player_id < 0 || player_id >= GNS_BRIDGE_MAX_PLAYERS)
		return;
	if (g_conn[player_id] != k_HSteamNetConnection_Invalid)
	{
		SteamNetworkingSockets()->CloseConnection(g_conn[player_id], 0, nullptr, false);
		g_conn[player_id] = k_HSteamNetConnection_Invalid;
	}
}

extern "C" void gns_bridge_on_signal_received(int from_player, const unsigned char *data, int len)
{
	if (from_player < 0 || from_player >= GNS_BRIDGE_MAX_PLAYERS)
		return;

	g_pending_inbound_player = from_player;
	SteamNetworkingSockets()->ReceivedP2PCustomSignal(data, len, &g_recv_context);
	g_pending_inbound_player = -1;
}
