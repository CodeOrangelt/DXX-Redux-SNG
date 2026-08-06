// See gns_bridge.h for the rationale/scope of this file.

#include "gns_bridge.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>

#include <cstring>
#include <cstdio>

// The game's console logger. Declared by hand rather than including
// console.h, which has no extern "C" guards and would give con_printf C++
// linkage here. Logging has to land in the in-game console -- that is where
// anyone diagnosing a connection actually looks; stderr is invisible.
extern "C" void con_printf(int level, const char *fmt, ...);
#define GNS_CON_NORMAL 0

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
bool g_conn_ready[GNS_BRIDGE_MAX_PLAYERS]; // reached k_ESteamNetworkingConnectionState_Connected

// Round-robin cursor for gns_bridge_recv(), so one chatty peer can't starve
// the others when several connections have messages queued.
int g_recv_cursor = 0;

// Pre-join ICE state.  A single pending connection from a client that is
// trying to join but has not yet been assigned a player slot.
gns_bridge_send_prejoin_signal_fn g_send_prejoin_signal = nullptr;
HSteamNetConnection g_prejoin_conn = k_HSteamNetConnection_Invalid;
bool g_prejoin_ready = false;
int g_pending_prejoin = 0;  // set inside gns_bridge_accept_prejoin() so OnConnectRequest knows to accept

// Sentinel stored as user data on the pre-join connection so SendSignal()
// can distinguish it from a normal per-player connection.
static const int64_t k_prejoin_user_data = -2LL;

// Public STUN servers, needed for ICE to gather server-reflexive candidates
// (i.e. to learn our own public address behind NAT). GNS's open-source
// default for this list is the empty string, which leaves ICE with nothing
// but host candidates -- LAN-only. That default is why P2P never came up
// over the internet and every session stayed on the relay.
const char *const k_stun_servers =
	"stun.l.google.com:19302,"
	"stun1.l.google.com:19302,"
	"stun2.l.google.com:19302";

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
		int64 user_data = SteamNetworkingSockets()->GetConnectionUserData(hConn);

		// Pre-join connection: route signal via the dedicated pre-join callback
		// (net_udp.c sends it to the tracker, which relays it to the client).
		if (user_data == k_prejoin_user_data) {
			if (g_send_prejoin_signal)
				g_send_prejoin_signal((const unsigned char *)pMsg, cbMsg);
			return true;
		}

		if (!g_send_signal)
			return false;
		if (user_data < 0 || user_data >= GNS_BRIDGE_MAX_PLAYERS)
			return false;
		g_send_signal((int)user_data, (const unsigned char *)pMsg, cbMsg);
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

		// Normal in-game inbound (host accepting from an already-slotted player).
		if (g_pending_inbound_player >= 0 && g_pending_inbound_player < GNS_BRIDGE_MAX_PLAYERS) {
			int player_id = g_pending_inbound_player;
			if (g_conn[player_id] != k_HSteamNetConnection_Invalid && g_conn[player_id] != hConn)
				return nullptr;
			g_conn[player_id] = hConn;
			SteamNetworkingSockets()->SetConnectionUserData(hConn, player_id);
			SteamNetworkingSockets()->AcceptConnection(hConn);
			return &g_signaling;
		}

		// Pre-join inbound: a client who is joining but has no player slot yet.
		// gns_bridge_accept_prejoin() sets g_pending_prejoin before calling
		// ReceivedP2PCustomSignal(), which dispatches us synchronously.
		if (g_pending_prejoin) {
			if (g_prejoin_conn != k_HSteamNetConnection_Invalid && g_prejoin_conn != hConn)
				return nullptr; // already handling one joining client at a time
			g_prejoin_conn = hConn;
			SteamNetworkingSockets()->SetConnectionUserData(hConn, k_prejoin_user_data);
			SteamNetworkingSockets()->AcceptConnection(hConn);
			return &g_signaling;
		}

		return nullptr; // unknown origin -- ignore per GNS recommendation
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

const char *StateName(ESteamNetworkingConnectionState s)
{
	switch (s)
	{
	case k_ESteamNetworkingConnectionState_None:               return "None";
	case k_ESteamNetworkingConnectionState_Connecting:         return "Connecting";
	case k_ESteamNetworkingConnectionState_FindingRoute:       return "FindingRoute";
	case k_ESteamNetworkingConnectionState_Connected:          return "Connected";
	case k_ESteamNetworkingConnectionState_ClosedByPeer:       return "ClosedByPeer";
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: return "ProblemDetectedLocally";
	default:                                                   return "?";
	}
}

void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo)
{
	// Pre-join connection handled separately -- it has no player slot.
	if (pInfo->m_hConn == g_prejoin_conn) {
		con_printf(GNS_CON_NORMAL, "GNS: pre-join: %s -> %s%s%s\n",
			StateName(pInfo->m_eOldState),
			StateName(pInfo->m_info.m_eState),
			pInfo->m_info.m_szEndDebug[0] ? " : " : "",
			pInfo->m_info.m_szEndDebug);
		switch (pInfo->m_info.m_eState) {
		case k_ESteamNetworkingConnectionState_Connected:
			g_prejoin_ready = true;
			break;
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
			g_prejoin_conn = k_HSteamNetConnection_Invalid;
			g_prejoin_ready = false;
			break;
		default: break;
		}
		return;
	}

	int player_id = PlayerForConn(pInfo->m_hConn);
	if (player_id < 0)
		return;

	// Every transition is reported, with GNS's own end-of-connection debug
	// string on failure. Without this there is no way to tell "ICE never
	// started" from "ICE gathered candidates but no pair worked" from "the
	// peer never answered", which are three completely different problems
	// with three completely different fixes.
	con_printf(GNS_CON_NORMAL, "GNS: player %d: %s -> %s%s%s\n",
		player_id,
		StateName(pInfo->m_eOldState),
		StateName(pInfo->m_info.m_eState),
		pInfo->m_info.m_szEndDebug[0] ? " : " : "",
		pInfo->m_info.m_szEndDebug);

	switch (pInfo->m_info.m_eState)
	{
	case k_ESteamNetworkingConnectionState_Connected:
	{
		g_conn_ready[player_id] = true;

		// Purely informational now -- net_udp.c logs the upgrade but must
		// NOT redirect its raw UDP socket at this address (see the header
		// comment on why that can't work). Traffic goes over the connection
		// itself, via gns_bridge_send().
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
		g_conn_ready[player_id] = false;
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
	{
		g_conn[i] = k_HSteamNetConnection_Invalid;
		g_conn_ready[i] = false;
	}

	SteamNetworkingErrMsg errMsg;
	if (!GameNetworkingSockets_Init(nullptr, errMsg))
	{
		con_printf(GNS_CON_NORMAL, "GNS bridge: init failed: %s\n", errMsg);
		return 0;
	}

	// Without these, ICE gathers host candidates only and cannot traverse
	// NAT -- see k_stun_servers above. ICE_Enable already defaults to "All"
	// in the open-source build, but it is set explicitly so a future GNS
	// bump can't silently turn P2P off underneath us.
	SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_STUN_ServerList, k_stun_servers);
	SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable,
		k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All);
	SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Penalty, 0);

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
	if (g_prejoin_conn != k_HSteamNetConnection_Invalid) {
		SteamNetworkingSockets()->CloseConnection(g_prejoin_conn, 0, nullptr, false);
		g_prejoin_conn = k_HSteamNetConnection_Invalid;
		g_prejoin_ready = false;
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
	{
		con_printf(GNS_CON_NORMAL, "GNS: player %d: ConnectP2PCustomSignaling FAILED\n", player_id);
		return;
	}

	con_printf(GNS_CON_NORMAL, "GNS: player %d: dialing ICE\n", player_id);

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
	g_conn_ready[player_id] = false;
}

extern "C" int gns_bridge_is_connected(int player_id)
{
	if (player_id < 0 || player_id >= GNS_BRIDGE_MAX_PLAYERS)
		return 0;

	return (g_conn[player_id] != k_HSteamNetConnection_Invalid && g_conn_ready[player_id]) ? 1 : 0;
}

extern "C" int gns_bridge_send(int player_id, const void *data, int len)
{
	if (!gns_bridge_is_connected(player_id) || len <= 0)
		return 0;

	// Unreliable + NoNagle is the closest match to the raw UDP datagram the
	// caller thinks it is sending: the game has its own ack/retransmit layer
	// (net_udp_noloss_*) and its own ordering assumptions, so adding GNS's
	// reliability on top would fight it and add latency.
	EResult r = SteamNetworkingSockets()->SendMessageToConnection(
		g_conn[player_id], data, (uint32)len,
		k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);

	return (r == k_EResultOK) ? 1 : 0;
}

extern "C" int gns_bridge_recv(int *from_player, void *buf, int maxlen)
{
	for (int n = 0; n < GNS_BRIDGE_MAX_PLAYERS; n++)
	{
		int i = (g_recv_cursor + n) % GNS_BRIDGE_MAX_PLAYERS;
		SteamNetworkingMessage_t *msg = nullptr;

		if (g_conn[i] == k_HSteamNetConnection_Invalid || !g_conn_ready[i])
			continue;

		if (SteamNetworkingSockets()->ReceiveMessagesOnConnection(g_conn[i], &msg, 1) != 1)
			continue;

		int len = (int)msg->m_cbSize;
		if (len > maxlen)
			len = maxlen; // oversized for our buffer; truncate rather than overflow

		memcpy(buf, msg->m_pData, len);
		msg->Release();

		if (from_player)
			*from_player = i;

		g_recv_cursor = (i + 1) % GNS_BRIDGE_MAX_PLAYERS;
		return len;
	}

	return 0;
}

extern "C" void gns_bridge_on_signal_received(int from_player, const unsigned char *data, int len)
{
	if (from_player < 0 || from_player >= GNS_BRIDGE_MAX_PLAYERS)
		return;

	g_pending_inbound_player = from_player;
	SteamNetworkingSockets()->ReceivedP2PCustomSignal(data, len, &g_recv_context);
	g_pending_inbound_player = -1;
}

// ---------------------------------------------------------------------------
// Pre-join ICE path
// ---------------------------------------------------------------------------

extern "C" void gns_bridge_set_prejoin_signal_callback(gns_bridge_send_prejoin_signal_fn fn)
{
	g_send_prejoin_signal = fn;
}

extern "C" int gns_bridge_accept_prejoin(const unsigned char *signal, int signal_len)
{
	g_pending_prejoin = 1;
	bool ok = SteamNetworkingSockets()->ReceivedP2PCustomSignal(signal, signal_len, &g_recv_context);
	g_pending_prejoin = 0;
	return ok ? 1 : 0;
}

extern "C" void gns_bridge_assign_prejoin(int player_id)
{
	if (player_id < 0 || player_id >= GNS_BRIDGE_MAX_PLAYERS)
		return;

	// Close any existing per-player connection at this slot first.
	if (g_conn[player_id] != k_HSteamNetConnection_Invalid)
	{
		SteamNetworkingSockets()->CloseConnection(g_conn[player_id], 0, nullptr, false);
		g_conn[player_id] = k_HSteamNetConnection_Invalid;
		g_conn_ready[player_id] = false;
	}

	// Move the pre-join connection into the permanent player slot.
	g_conn[player_id] = g_prejoin_conn;
	g_conn_ready[player_id] = g_prejoin_ready;
	if (g_conn[player_id] != k_HSteamNetConnection_Invalid)
		SteamNetworkingSockets()->SetConnectionUserData(g_conn[player_id], (int64_t)player_id);

	g_prejoin_conn = k_HSteamNetConnection_Invalid;
	g_prejoin_ready = false;

	con_printf(GNS_CON_NORMAL, "GNS: pre-join connection migrated to player slot %d\n", player_id);
}

extern "C" int gns_bridge_prejoin_ready(void)
{
	return (g_prejoin_conn != k_HSteamNetConnection_Invalid && g_prejoin_ready) ? 1 : 0;
}

extern "C" int gns_bridge_send_prejoin(const void *data, int len)
{
	if (!gns_bridge_prejoin_ready() || len <= 0)
		return 0;
	EResult r = SteamNetworkingSockets()->SendMessageToConnection(
		g_prejoin_conn, data, (uint32)len,
		k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
	return (r == k_EResultOK) ? 1 : 0;
}

extern "C" int gns_bridge_recv_prejoin(void *buf, int maxlen)
{
	if (g_prejoin_conn == k_HSteamNetConnection_Invalid || !g_prejoin_ready)
		return 0;
	SteamNetworkingMessage_t *msg = nullptr;
	if (SteamNetworkingSockets()->ReceiveMessagesOnConnection(g_prejoin_conn, &msg, 1) != 1)
		return 0;
	int len = (int)msg->m_cbSize;
	if (len > maxlen) len = maxlen;
	memcpy(buf, msg->m_pData, len);
	msg->Release();
	return len;
}
