/*
 * C-callable bridge to SLikeNet: NAT punchthrough plus a peer-to-peer
 * transport for gameplay datagrams.
 *
 * net_udp.c keeps its own UDP socket for LAN broadcast, the tracker and
 * legacy peers. Once a SLikeNet connection to a peer is up, dxx_sendto()
 * diverts that peer's datagrams onto it; until then the raw route is used.
 *
 * Peers are identified by the address net_udp knows them by (their game
 * socket address), so player slot reshuffles never confuse the bridge.
 * They find each other by trading a small signal blob (GUID + port) over
 * whatever route already exists: the tracker before a join, net_udp's
 * direct/proxy mesh after it. The facilitator (tools/slk_facilitator, a
 * NatPunchthroughServer) brokers the punch.
 */

#ifndef SLK_BRIDGE_H
#define SLK_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

struct sockaddr_in;

#define SLK_SIGNAL_SIZE 14
#define SLK_PORT_OFFSET 1	/* SLikeNet binds game port + this */

/* Start on a port; calling again on the same port just forgets all peers.
 * fac_host may be NULL or empty: direct connects still work, punching won't.
 * Returns 1 on success. */
int slk_bridge_start(unsigned short port, const char *fac_host, unsigned short fac_port);
void slk_bridge_shutdown(void);
unsigned short slk_bridge_local_port(void);

/* Pump connection events. Call once per frame. */
void slk_bridge_poll(void);

/* Our signal blob. Returns its length, or 0 if not running. */
int slk_bridge_local_signal(unsigned char *out, int maxlen);

/* Bind a remote peer's signal to the game address we know it by. Any other
 * peer holding that address loses it. Returns 1 if the blob was valid. */
int slk_bridge_learn(const struct sockaddr_in *game_addr, const unsigned char *sig, int len);

/* Connect to a learned peer, direct and punched, retried on a timer. Safe
 * to call every frame. */
void slk_bridge_dial(const struct sockaddr_in *game_addr);

void slk_bridge_forget(const struct sockaddr_in *game_addr);
void slk_bridge_rebind(const struct sockaddr_in *old_addr, const struct sockaddr_in *new_addr);
int slk_bridge_knows(const struct sockaddr_in *game_addr);
int slk_bridge_is_connected(const struct sockaddr_in *game_addr);
int slk_bridge_ping(const struct sockaddr_in *game_addr);

/* Unreliable datagram to a connected peer. Returns 1 if sent, 0 if the
 * caller must fall back to its raw route. */
int slk_bridge_send(const struct sockaddr_in *to, const void *data, int len);

/* Next datagram from any peer. Returns its length or 0. */
int slk_bridge_recv(struct sockaddr_in *from, void *buf, int maxlen);

#ifdef __cplusplus
}
#endif

#endif
