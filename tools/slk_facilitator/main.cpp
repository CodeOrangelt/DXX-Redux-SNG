// NAT punchthrough facilitator for DXX-Redux-SNG's SLikeNet transport.
//
// usage: slk_facilitator [port] [public_ip]
//
// With public_ip, a second socket is bound on port+1 so clients can measure
// their router's port stride; that is what lets punchthrough predict ports
// on symmetric NATs. Both ports must be open for UDP.

#include <slikenet/peerinterface.h>
#include <slikenet/MessageIdentifiers.h>
#include <slikenet/NatPunchthroughServer.h>
#include <slikenet/sleep.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace {

const unsigned short DEFAULT_PORT = 42430;
const unsigned short MAX_PORT = 65534;	// the stride socket needs port+1
const unsigned MAX_CLIENTS = 2048;
const char PASSWORD[] = "DXX-SNG-SLK1";
const int PASSWORD_LEN = sizeof(PASSWORD) - 1;
const unsigned TIMEOUT_MS = 30000;
const unsigned SHUTDOWN_FLUSH_MS = 300;

volatile std::sig_atomic_t g_quit = 0;

void on_signal(int)
{
	g_quit = 1;
}

void log_line(const char *what, const SLNet::Packet *p)
{
	char when[32];
	std::time_t now = std::time(nullptr);
	std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::gmtime(&now));
	std::printf("%s %s %s\n", when, what, p->systemAddress.ToString(true, ':'));
	std::fflush(stdout);
}

struct Logger : public SLNet::NatPunchthroughServerDebugInterface
{
	void OnServerMessage(const char *msg) override
	{
		std::printf("  nat: %s\n", msg);
		std::fflush(stdout);
	}
};

unsigned short parse_port(const char *arg)
{
	char *end;
	long v = std::strtol(arg, &end, 10);

	if (end == arg || *end || v < 1 || v > MAX_PORT)
		return 0;
	return (unsigned short)v;
}

bool start(SLNet::RakPeerInterface *peer, unsigned short port, const char *public_ip)
{
	SLNet::SocketDescriptor sd[2];
	unsigned count = 1;

	sd[0] = SLNet::SocketDescriptor(port, public_ip);
	sd[0].socketFamily = AF_INET;
	if (public_ip)
	{
		sd[1] = SLNet::SocketDescriptor(port + 1, public_ip);
		sd[1].socketFamily = AF_INET;
		count = 2;
	}

	SLNet::StartupResult r = peer->Startup(MAX_CLIENTS, sd, count);
	if (r != SLNet::RAKNET_STARTED)
	{
		std::fprintf(stderr, "startup failed on port %u: %d\n", port, (int)r);
		return false;
	}

	peer->SetIncomingPassword(PASSWORD, PASSWORD_LEN);
	peer->SetMaximumIncomingConnections(MAX_CLIENTS);
	peer->SetTimeoutTime(TIMEOUT_MS, SLNet::UNASSIGNED_SYSTEM_ADDRESS);

	std::printf("slk_facilitator: listening on UDP %u%s, guid %s\n",
		port, public_ip ? " (+1 for port stride)" : "", peer->GetMyGUID().ToString());
	std::fflush(stdout);
	return true;
}

void serve(SLNet::RakPeerInterface *peer)
{
	while (!g_quit)
	{
		SLNet::Packet *p;

		while ((p = peer->Receive()) != nullptr)
		{
			switch (p->data[0])
			{
			case ID_NEW_INCOMING_CONNECTION:
				log_line("connect   ", p);
				break;
			case ID_DISCONNECTION_NOTIFICATION:
				log_line("disconnect", p);
				break;
			case ID_CONNECTION_LOST:
				log_line("lost      ", p);
				break;
			default:
				break;
			}
			peer->DeallocatePacket(p);
		}
		RakSleep(10);
	}
}

} // namespace

int main(int argc, char **argv)
{
	unsigned short port = argc > 1 ? parse_port(argv[1]) : DEFAULT_PORT;
	const char *public_ip = argc > 2 && argv[2][0] ? argv[2] : nullptr;

	if (!port)
	{
		std::fprintf(stderr, "usage: %s [port 1-%u] [public_ip]\n", argv[0], MAX_PORT);
		return 2;
	}

	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);

	SLNet::RakPeerInterface *peer = SLNet::RakPeerInterface::GetInstance();
	SLNet::NatPunchthroughServer *nat = SLNet::NatPunchthroughServer::GetInstance();
	Logger logger;

	peer->AttachPlugin(nat);
	nat->SetDebugInterface(&logger);

	bool started = start(peer, port, public_ip);
	if (started)
		serve(peer);

	peer->Shutdown(started ? SHUTDOWN_FLUSH_MS : 0);
	peer->DetachPlugin(nat);
	SLNet::NatPunchthroughServer::DestroyInstance(nat);
	SLNet::RakPeerInterface::DestroyInstance(peer);
	return started ? 0 : 1;
}
