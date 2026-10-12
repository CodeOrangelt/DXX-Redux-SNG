// See slk_bridge.h for scope.

#include "slk_bridge.h"

#include <slikenet/peerinterface.h>
#include <slikenet/MessageIdentifiers.h>
#include <slikenet/NatPunchthroughClient.h>
#include <slikenet/BitStream.h>
#include <slikenet/GetTime.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

// console.h has no extern "C" guards; declare the one function we need.
extern "C" void con_printf(int level, const char *fmt, ...);
#define SLK_CON_NORMAL 0
#define SLK_CON_DEBUG 2

using SLNet::RakPeerInterface;
using SLNet::RakNetGUID;
using SLNet::SystemAddress;
using SLNet::Packet;

namespace {

const int MAX_PEERS = 16;
const unsigned char ID_SNG_DATAGRAM = ID_USER_PACKET_ENUM;
const unsigned char ID_SNG_LEARNED = ID_USER_PACKET_ENUM + 1;
const int MAX_DATAGRAM = 2048;
const unsigned char SIGNAL_VERSION = 1;
const char PASSWORD[] = "DXX-SNG-SLK1";
const int PASSWORD_LEN = sizeof(PASSWORD) - 1;
const SLNet::TimeMS DIAL_RETRY_MS = 6000;
const SLNet::TimeMS FACILITATOR_RETRY_MS = 15000;
const SLNet::TimeMS PEER_TIMEOUT_MS = 10000;
const SLNet::TimeMS STALE_PEER_MS = 20000;	// signals refresh every 1-2s while a peer matters
const SLNet::TimeMS SHUTDOWN_FLUSH_MS = 30;
const size_t MAX_INBOX = 256;
const unsigned DIRECT_ATTEMPTS = 8;
const unsigned DIRECT_ATTEMPT_GAP_MS = 400;

struct Peer
{
	bool used;
	RakNetGUID guid;
	SystemAddress rak_addr;	// sends go by address: Linux GUIDs are clock-derived and can collide
	bool connected;
	bool learned;
	bool remote_learned;	// far end can attribute our datagrams
	sockaddr_in game_addr;
	unsigned short rak_port;
	SLNet::TimeMS last_dial;
	SLNet::TimeMS last_heard;
};

RakPeerInterface *g_peer = nullptr;
SLNet::NatPunchthroughClient *g_nat = nullptr;
unsigned short g_port = 0;
Peer g_peers[MAX_PEERS];
std::deque<Packet *> g_inbox;

SystemAddress g_fac_addr;
bool g_fac_resolved = false;
bool g_fac_connected = false;
bool g_fac_connecting = false;
SLNet::TimeMS g_fac_last_try = 0;

bool same_addr(const sockaddr_in &a, const sockaddr_in &b)
{
	return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

const char *sys_str(const SystemAddress &sa)
{
	static char buf[64];
	sa.ToString(true, buf, sizeof(buf), ':');
	return buf;
}

const char *game_str(const sockaddr_in &sin)
{
	static char buf[64];
	snprintf(buf, sizeof(buf), "%s:%u", inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
	return buf;
}

void clear_peer(Peer *p)
{
	*p = Peer();
}

Peer *find_guid(const RakNetGUID &guid)
{
	for (Peer &p : g_peers)
		if (p.used && p.guid == guid)
			return &p;
	return nullptr;
}

Peer *find_addr(const sockaddr_in *addr)
{
	if (!addr || addr->sin_family != AF_INET)
		return nullptr;
	for (Peer &p : g_peers)
		if (p.used && p.learned && same_addr(p.game_addr, *addr))
			return &p;
	return nullptr;
}

void drop_peer(Peer *p)
{
	if (p->connected)
		g_peer->CloseConnection(p->rak_addr, true);
	clear_peer(p);
}

void forget_all()
{
	for (Peer &p : g_peers)
		if (p.used)
			drop_peer(&p);
}

Peer *alloc_peer(const RakNetGUID &guid)
{
	Peer *p = find_guid(guid);
	if (p)
		return p;
	for (Peer &slot : g_peers)
	{
		if (slot.used)
			continue;
		clear_peer(&slot);
		slot.used = true;
		slot.guid = guid;
		slot.last_heard = SLNet::GetTimeMS();
		return &slot;
	}
	return nullptr;
}

// A new GUID at a known address means that player restarted.
void release_addr(const sockaddr_in &addr, const Peer *keep)
{
	for (Peer &p : g_peers)
		if (p.used && p.learned && &p != keep && same_addr(p.game_addr, addr))
			drop_peer(&p);
}

// A peer we learned of but never reached (an abandoned join, a player who
// left) would otherwise hold its slot until shutdown.
void expire_stale_peers()
{
	SLNet::TimeMS now = SLNet::GetTimeMS();

	for (Peer &p : g_peers)
		if (p.used && !p.connected && now - p.last_heard > STALE_PEER_MS)
			clear_peer(&p);
}

void tell_learned(Peer *p)
{
	unsigned char msg = ID_SNG_LEARNED;

	if (p->connected && p->learned)
		g_peer->Send((const char *)&msg, 1, HIGH_PRIORITY, RELIABLE, 0, p->rak_addr, false);
}

bool parse_signal(const unsigned char *sig, int len, RakNetGUID *guid, unsigned short *port)
{
	if (len < SLK_SIGNAL_SIZE || sig[0] != 'S' || sig[1] != 'L' || sig[2] != 'K' || sig[3] != SIGNAL_VERSION)
		return false;

	uint64_t g = 0;
	for (int i = 0; i < 8; i++)
		g |= (uint64_t)sig[4 + i] << (8 * i);
	guid->g = g;
	*port = (unsigned short)(sig[12] | (sig[13] << 8));
	return guid->g != SLNet::UNASSIGNED_RAKNET_GUID.g && guid->g != g_peer->GetMyGUID().g;
}

void facilitator_frame()
{
	if (!g_fac_resolved || g_fac_connected || g_fac_connecting)
		return;

	SLNet::TimeMS now = SLNet::GetTimeMS();
	if (g_fac_last_try && now - g_fac_last_try < FACILITATOR_RETRY_MS)
		return;
	g_fac_last_try = now;

	char ip[64];
	g_fac_addr.ToString(false, ip, sizeof(ip));
	if (g_peer->Connect(ip, g_fac_addr.GetPort(), PASSWORD, PASSWORD_LEN) == SLNet::CONNECTION_ATTEMPT_STARTED)
		g_fac_connecting = true;
}

void on_connected(Packet *pkt)
{
	if (pkt->systemAddress == g_fac_addr)
	{
		g_fac_connected = true;
		g_fac_connecting = false;
		con_printf(SLK_CON_NORMAL, "SLikeNet: facilitator %s connected\n", sys_str(pkt->systemAddress));
		g_nat->FindRouterPortStride(g_fac_addr);
		return;
	}

	Peer *p = alloc_peer(pkt->guid);
	if (!p)
	{
		g_peer->CloseConnection(pkt->systemAddress, true);
		return;
	}

	p->connected = true;
	p->rak_addr = pkt->systemAddress;
	p->last_heard = SLNet::GetTimeMS();
	tell_learned(p);
	if (p->learned)
		con_printf(SLK_CON_NORMAL, "SLikeNet: %s connected via %s\n", game_str(p->game_addr), sys_str(pkt->systemAddress));
	else
		con_printf(SLK_CON_DEBUG, "SLikeNet: unannounced peer connected via %s\n", sys_str(pkt->systemAddress));
}

void on_disconnected(Packet *pkt, const char *why)
{
	if (pkt->systemAddress == g_fac_addr)
	{
		g_fac_connected = false;
		g_fac_connecting = false;
		con_printf(SLK_CON_NORMAL, "SLikeNet: facilitator %s %s\n", sys_str(pkt->systemAddress), why);
		return;
	}

	Peer *p = find_guid(pkt->guid);
	if (!p)
		return;

	if (p->learned)
		con_printf(SLK_CON_NORMAL, "SLikeNet: %s %s\n", game_str(p->game_addr), why);
	p->connected = false;
	p->remote_learned = false;
	p->last_dial = 0;
	p->last_heard = SLNet::GetTimeMS();
	if (!p->learned)
		clear_peer(p);
}

void on_remote_learned(Packet *pkt)
{
	Peer *p = find_guid(pkt->guid);

	if (!p || p->remote_learned)
		return;
	p->remote_learned = true;
	if (p->learned)
		con_printf(SLK_CON_NORMAL, "SLikeNet: %s is peer-to-peer\n", game_str(p->game_addr));
}

void on_punch_succeeded(Packet *pkt)
{
	Peer *p = find_guid(pkt->guid);

	con_printf(SLK_CON_NORMAL, "SLikeNet: punched through to %s at %s\n",
		p ? game_str(p->game_addr) : "?", sys_str(pkt->systemAddress));

	bool we_initiated = pkt->length >= 2 && pkt->data[1] == 1;
	if (!we_initiated)
		return;

	char ip[64];
	pkt->systemAddress.ToString(false, ip, sizeof(ip));
	g_peer->Connect(ip, pkt->systemAddress.GetPort(), PASSWORD, PASSWORD_LEN, nullptr, 0, DIRECT_ATTEMPTS, DIRECT_ATTEMPT_GAP_MS);
}

void log_punch_failure(Packet *pkt, const char *why)
{
	RakNetGUID guid = pkt->guid;

	if (pkt->data[0] != ID_NAT_PUNCHTHROUGH_FAILED && pkt->length >= 1 + sizeof(uint64_t))
	{
		SLNet::BitStream bs(pkt->data, pkt->length, false);
		bs.IgnoreBytes(1);
		bs.Read(guid);
	}

	Peer *p = find_guid(guid);
	con_printf(SLK_CON_NORMAL, "SLikeNet: punch to %s failed: %s\n", p ? game_str(p->game_addr) : "?", why);
}

// Returns true if the packet is a datagram now owned by the inbox.
bool dispatch(Packet *pkt)
{
	switch (pkt->data[0])
	{
	case ID_SNG_DATAGRAM:
	{
		const Peer *p = find_guid(pkt->guid);

		if (!p || !p->learned || pkt->length < 2)
			return false;
		if (g_inbox.size() >= MAX_INBOX)
		{
			g_peer->DeallocatePacket(g_inbox.front());
			g_inbox.pop_front();
		}
		g_inbox.push_back(pkt);
		return true;
	}
	case ID_SNG_LEARNED:
		on_remote_learned(pkt);
		break;
	case ID_CONNECTION_REQUEST_ACCEPTED:
	case ID_NEW_INCOMING_CONNECTION:
		on_connected(pkt);
		break;
	case ID_DISCONNECTION_NOTIFICATION:
		on_disconnected(pkt, "disconnected");
		break;
	case ID_CONNECTION_LOST:
		on_disconnected(pkt, "connection lost");
		break;
	case ID_CONNECTION_ATTEMPT_FAILED:
		if (pkt->systemAddress == g_fac_addr)
			on_disconnected(pkt, "unreachable");
		else
			con_printf(SLK_CON_DEBUG, "SLikeNet: connect to %s failed\n", sys_str(pkt->systemAddress));
		break;
	case ID_NO_FREE_INCOMING_CONNECTIONS:
	case ID_INVALID_PASSWORD:
	case ID_INCOMPATIBLE_PROTOCOL_VERSION:
	case ID_CONNECTION_BANNED:
		if (pkt->systemAddress == g_fac_addr)
			g_fac_connecting = false;
		con_printf(SLK_CON_NORMAL, "SLikeNet: %s refused the connection (id %d)\n", sys_str(pkt->systemAddress), pkt->data[0]);
		break;
	case ID_NAT_PUNCHTHROUGH_SUCCEEDED:
		on_punch_succeeded(pkt);
		break;
	case ID_NAT_PUNCHTHROUGH_FAILED:
		log_punch_failure(pkt, "no route found");
		break;
	case ID_NAT_TARGET_NOT_CONNECTED:
		log_punch_failure(pkt, "peer not on facilitator");
		break;
	case ID_NAT_TARGET_UNRESPONSIVE:
		log_punch_failure(pkt, "peer unresponsive");
		break;
	case ID_NAT_CONNECTION_TO_TARGET_LOST:
		log_punch_failure(pkt, "facilitator lost peer");
		break;
	default:
		break;
	}
	return false;
}

void pump()
{
	Packet *pkt;

	while ((pkt = g_peer->Receive()) != nullptr)
		if (!dispatch(pkt))
			g_peer->DeallocatePacket(pkt);
}

void clear_inbox()
{
	for (Packet *pkt : g_inbox)
		g_peer->DeallocatePacket(pkt);
	g_inbox.clear();
}

bool startup(unsigned short port)
{
	SLNet::SocketDescriptor sd(port, nullptr);
	sd.socketFamily = AF_INET;

	SLNet::StartupResult r = g_peer->Startup(MAX_PEERS + 1, &sd, 1);
	if (r == SLNet::RAKNET_STARTED)
		return true;
	if (port == 0)
		return false;

	con_printf(SLK_CON_NORMAL, "SLikeNet: port %u unavailable (%d), using an ephemeral port\n", port, (int)r);
	return startup(0);
}

// Blocking DNS, once per start -- same as the tracker lookup.
void resolve_facilitator(const char *host, unsigned short port)
{
	g_fac_resolved = false;
	g_fac_connected = false;
	g_fac_connecting = false;
	g_fac_last_try = 0;
	g_fac_addr = SLNet::UNASSIGNED_SYSTEM_ADDRESS;

	if (!host || !host[0] || !port)
		return;

	if (!g_fac_addr.FromStringExplicitPort(host, port, 4))
	{
		con_printf(SLK_CON_NORMAL, "SLikeNet: cannot resolve facilitator %s -- direct connects only\n", host);
		g_fac_addr = SLNet::UNASSIGNED_SYSTEM_ADDRESS;
		return;
	}
	g_fac_resolved = true;
}

int send_to(Peer *p, const void *data, int len)
{
	unsigned char buf[MAX_DATAGRAM];

	if (!p || !p->connected || !p->remote_learned || len <= 0 || len + 1 > (int)sizeof(buf))
		return 0;

	buf[0] = ID_SNG_DATAGRAM;
	memcpy(buf + 1, data, len);
	return g_peer->Send((const char *)buf, len + 1, IMMEDIATE_PRIORITY, UNRELIABLE, 0, p->rak_addr, false) != 0;
}

} // namespace

extern "C" int slk_bridge_start(unsigned short port, const char *fac_host, unsigned short fac_port)
{
	if (g_peer && g_port == port)
	{
		clear_inbox();
		forget_all();
		return 1;
	}
	if (g_peer)
		slk_bridge_shutdown();

	g_peer = RakPeerInterface::GetInstance();
	g_nat = SLNet::NatPunchthroughClient::GetInstance();
	g_peer->AttachPlugin(g_nat);

	if (!startup(port))
	{
		con_printf(SLK_CON_NORMAL, "SLikeNet: startup failed -- peer-to-peer transport disabled\n");
		slk_bridge_shutdown();
		return 0;
	}

	for (Peer &p : g_peers)
		clear_peer(&p);

	g_port = port;
	g_peer->SetIncomingPassword(PASSWORD, PASSWORD_LEN);
	g_peer->SetMaximumIncomingConnections(MAX_PEERS);
	g_peer->SetTimeoutTime(PEER_TIMEOUT_MS, SLNet::UNASSIGNED_SYSTEM_ADDRESS);
	g_peer->SetOccasionalPing(true);

	resolve_facilitator(fac_host, fac_port);
	con_printf(SLK_CON_NORMAL, "SLikeNet: listening on UDP %u, guid %s\n",
		slk_bridge_local_port(), g_peer->GetMyGUID().ToString());
	return 1;
}

extern "C" void slk_bridge_shutdown(void)
{
	if (!g_peer)
		return;

	clear_inbox();
	g_peer->Shutdown(SHUTDOWN_FLUSH_MS);
	g_peer->DetachPlugin(g_nat);
	SLNet::NatPunchthroughClient::DestroyInstance(g_nat);
	RakPeerInterface::DestroyInstance(g_peer);
	g_nat = nullptr;
	g_peer = nullptr;
	g_port = 0;
	g_fac_resolved = false;
	g_fac_connected = false;
	g_fac_connecting = false;
	for (Peer &p : g_peers)
		clear_peer(&p);
}

extern "C" unsigned short slk_bridge_local_port(void)
{
	return g_peer ? g_peer->GetMyBoundAddress().GetPort() : 0;
}

extern "C" void slk_bridge_poll(void)
{
	if (!g_peer)
		return;
	pump();
	expire_stale_peers();
	facilitator_frame();
}

extern "C" int slk_bridge_local_signal(unsigned char *out, int maxlen)
{
	if (!g_peer || maxlen < SLK_SIGNAL_SIZE)
		return 0;

	uint64_t g = g_peer->GetMyGUID().g;
	unsigned short port = slk_bridge_local_port();

	out[0] = 'S';
	out[1] = 'L';
	out[2] = 'K';
	out[3] = SIGNAL_VERSION;
	for (int i = 0; i < 8; i++)
		out[4 + i] = (unsigned char)(g >> (8 * i));
	out[12] = (unsigned char)(port & 0xff);
	out[13] = (unsigned char)(port >> 8);
	return SLK_SIGNAL_SIZE;
}

extern "C" int slk_bridge_learn(const struct sockaddr_in *game_addr, const unsigned char *sig, int len)
{
	RakNetGUID guid;
	unsigned short port;

	if (!g_peer || !game_addr || game_addr->sin_family != AF_INET)
		return 0;
	if (!parse_signal(sig, len, &guid, &port))
		return 0;

	Peer *p = alloc_peer(guid);
	if (!p)
		return 0;

	release_addr(*game_addr, p);
	if (!p->learned || !same_addr(p->game_addr, *game_addr))
		con_printf(SLK_CON_NORMAL, "SLikeNet: learned peer %s (guid %s, port %u)\n", game_str(*game_addr), guid.ToString(), port);

	bool was_learned = p->learned;
	p->learned = true;
	p->last_heard = SLNet::GetTimeMS();
	p->rak_port = port;
	p->game_addr = *game_addr;
	if (!was_learned)
		tell_learned(p);
	return 1;
}

extern "C" void slk_bridge_dial(const struct sockaddr_in *game_addr)
{
	if (!g_peer)
		return;

	Peer *p = find_addr(game_addr);
	if (!p || !p->learned || p->connected)
		return;

	SLNet::TimeMS now = SLNet::GetTimeMS();
	if (p->last_dial && now - p->last_dial < DIAL_RETRY_MS)
		return;
	p->last_dial = now;

	if (p->rak_port)
		g_peer->Connect(inet_ntoa(p->game_addr.sin_addr), p->rak_port, PASSWORD, PASSWORD_LEN, nullptr, 0, DIRECT_ATTEMPTS, DIRECT_ATTEMPT_GAP_MS);
	if (g_fac_connected)
		g_nat->OpenNAT(p->guid, g_fac_addr);

	con_printf(SLK_CON_DEBUG, "SLikeNet: dialing %s%s\n", game_str(p->game_addr), g_fac_connected ? " (direct + punch)" : " (direct only)");
}

extern "C" void slk_bridge_forget(const struct sockaddr_in *game_addr)
{
	if (!g_peer)
		return;

	Peer *p = find_addr(game_addr);
	if (p)
		drop_peer(p);
}

extern "C" void slk_bridge_rebind(const struct sockaddr_in *old_addr, const struct sockaddr_in *new_addr)
{
	if (!g_peer || !new_addr || new_addr->sin_family != AF_INET)
		return;

	Peer *p = find_addr(old_addr);
	if (!p || same_addr(p->game_addr, *new_addr))
		return;

	release_addr(*new_addr, p);
	p->game_addr = *new_addr;
}

extern "C" int slk_bridge_knows(const struct sockaddr_in *game_addr)
{
	if (!g_peer)
		return 0;

	Peer *p = find_addr(game_addr);
	return p && p->learned;
}

extern "C" int slk_bridge_is_connected(const struct sockaddr_in *game_addr)
{
	if (!g_peer)
		return 0;

	Peer *p = find_addr(game_addr);
	return p && p->connected && p->remote_learned;
}

extern "C" int slk_bridge_ping(const struct sockaddr_in *game_addr)
{
	if (!g_peer)
		return -1;

	Peer *p = find_addr(game_addr);
	if (!p || !p->connected)
		return -1;
	return g_peer->GetAveragePing(p->rak_addr);
}

extern "C" int slk_bridge_send(const struct sockaddr_in *to, const void *data, int len)
{
	if (!g_peer)
		return 0;
	return send_to(find_addr(to), data, len);
}

extern "C" int slk_bridge_recv(struct sockaddr_in *from, void *buf, int maxlen)
{
	if (!g_peer)
		return 0;

	if (g_inbox.empty())
		pump();

	while (!g_inbox.empty())
	{
		Packet *pkt = g_inbox.front();
		g_inbox.pop_front();

		const Peer *p = find_guid(pkt->guid);
		int len = (int)pkt->length - 1;

		if (!p || !p->learned || len > maxlen)
		{
			g_peer->DeallocatePacket(pkt);
			continue;
		}

		memcpy(buf, pkt->data + 1, len);
		*from = p->game_addr;
		g_peer->DeallocatePacket(pkt);
		return len;
	}

	return 0;
}
