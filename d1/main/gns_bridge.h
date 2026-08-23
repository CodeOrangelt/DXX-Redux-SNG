/*
 * C-callable bridge to GameNetworkingSockets (ICE-based P2P connections).
 *
 * Plain C can't implement GNS's C++ signaling interfaces, so this pair of
 * files exists solely to bridge that gap. Everything else about GNS usage
 * (relaying signaling blobs, deciding when to attempt a connection, acting
 * on a discovered route) is driven from net_udp.c through this C API.
 *
 * Scope: this bridge establishes an ICE/STUN peer-to-peer connection AND
 * carries gameplay traffic over it (gns_bridge_send/gns_bridge_recv), which
 * dxx_sendto()/dxx_recvfrom() in net_udp.c divert into transparently.
 *
 * Carrying the traffic is the whole point, and it is a deliberate change
 * from the original design here, which only harvested the ICE-discovered
 * address and then kept sending game packets from net_udp.c's own UDP
 * socket. That could not work through anything stricter than a full-cone
 * NAT: the hole ICE punches belongs to GNS's socket and its local port, so
 * packets arriving from a different local port are dropped by the peer's
 * NAT. Traffic has to travel over the connection that was actually
 * negotiated.
 */

#ifndef GNS_BRIDGE_H
#define GNS_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Callback: bridge wants a signaling blob delivered to a player over
 * whatever transport net_udp.c already uses to reach them (direct or
 * proxied through the host -- net_udp_send_to_player() already knows
 * which). */
typedef void (*gns_bridge_send_signal_fn)(int to_player, const unsigned char *data, int len);

/* Callback: bridge found a working ICE route to a player. addr points to
 * a struct sockaddr_in (network byte order), addr_len == sizeof(struct sockaddr_in). */
typedef void (*gns_bridge_route_found_fn)(int player_id, const void *addr, int addr_len);

/* Register callbacks. Call before gns_bridge_init(). */
void gns_bridge_set_callbacks(gns_bridge_send_signal_fn send_signal, gns_bridge_route_found_fn route_found);

/* Returns 1 on success, 0 on failure (GNS unavailable -- caller should just
 * carry on without it; nothing else in this header may be called if this
 * returns 0). */
int gns_bridge_init(void);
void gns_bridge_shutdown(void);

/* Pump GNS's internal callback dispatch. Call once per frame. */
void gns_bridge_poll(void);

/* Kick off (or continue, if already in progress) an ICE connection attempt
 * to a given player. Safe to call repeatedly; no-ops if already connecting
 * or connected. */
void gns_bridge_connect_to_player(int player_id);

/* Tear down any GNS connection/state for a player, e.g. on disconnect, so
 * a later reattempt starts clean. */
void gns_bridge_reset_player(int player_id);

/* Feed an out-of-band signaling blob relayed from a given player into GNS. */
void gns_bridge_on_signal_received(int from_player, const unsigned char *data, int len);

/* True once the P2P connection to this player is fully established and
 * usable for gns_bridge_send(). Until then callers must keep using whatever
 * route they had (direct UDP, proxy, or relay). */
int gns_bridge_is_connected(int player_id);

/* Send one datagram to a player over the established P2P connection.
 * Unreliable and unordered, matching the raw-UDP semantics the game's own
 * reliability layer is built on. Returns 1 if handed off to GNS, 0 if not
 * (no connection -- caller must fall back to its other route). */
int gns_bridge_send(int player_id, const void *data, int len);

/* Pull one received datagram, from any connected player. Returns the byte
 * count and sets *from_player, or 0 when nothing is queued. Call repeatedly
 * until it returns 0. */
int gns_bridge_recv(int *from_player, void *buf, int maxlen);

/*
 * Pre-join ICE path -- lets two peers establish a P2P connection *before* the
 * joining client has been assigned a player slot.
 *
 * Flow (tracker brokers both directions):
 *
 *   client                 tracker                  host
 *   ------                 -------                  ----
 *   gns_bridge_connect_to_player(0)
 *   --> TRACKER_PKT_SIGNAL(gameid, blob)
 *                          --> UPID_TRACKER_SIGNAL(client_ip, blob)
 *                                             gns_bridge_accept_prejoin(blob)
 *                                             g_send_prejoin_signal(reply_blob)
 *                          <-- TRACKER_PKT_SIGNAL(gameid, client_ip, reply_blob)
 *   <-- UPID_TRACKER_SIGNAL(reply_blob)
 *   gns_bridge_on_signal_received(0, reply_blob)
 *   ...ICE establishes...
 *   gns_bridge_is_connected(0) == 1
 *   dxx_sendto(GAME_INFO_REQ) goes over GNS to host
 *   ... join proceeds normally ...
 *   host assigns slot N:
 *   gns_bridge_assign_prejoin(N) -- migrates pre-join conn -> g_conn[N]
 */

/* Host: register a callback that delivers ICE reply blobs back to the joining
 * client via the tracker (before a direct path exists).  Call before
 * gns_bridge_init(). */
typedef void (*gns_bridge_send_prejoin_signal_fn)(const unsigned char *data, int len);
void gns_bridge_set_prejoin_signal_callback(gns_bridge_send_prejoin_signal_fn fn);

/* Host: feed an inbound ICE signaling blob (from UPID_TRACKER_SIGNAL) into
 * GNS.  GNS will call OnConnectRequest internally and fire
 * g_send_prejoin_signal() with the reply.  Returns 1 on success. */
int gns_bridge_accept_prejoin(const unsigned char *signal, int signal_len);

/* Host: once the joining client has been assigned player slot N, migrate the
 * pre-join connection into g_conn[N] so normal per-player send/recv works.
 * The pre-join handles are cleared. */
void gns_bridge_assign_prejoin(int player_id);

/* True once the pre-join P2P connection has reached Connected state. */
int gns_bridge_prejoin_ready(void);

/* Send one datagram to the joining client over the pre-join connection.
 * Returns 1 on success, 0 if not ready. */
int gns_bridge_send_prejoin(const void *data, int len);

/* Pull one received datagram from the pre-join connection.
 * Returns byte count or 0 when nothing is queued. */
int gns_bridge_recv_prejoin(void *buf, int maxlen);

#ifdef __cplusplus
}
#endif

#endif /* GNS_BRIDGE_H */
