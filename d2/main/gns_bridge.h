/*
 * C-callable bridge to GameNetworkingSockets (ICE-based P2P connections).
 *
 * Plain C can't implement GNS's C++ signaling interfaces, so this pair of
 * files exists solely to bridge that gap. Everything else about GNS usage
 * (relaying signaling blobs, deciding when to attempt a connection, acting
 * on a discovered route) is driven from net_udp.c through this C API.
 *
 * Scope: this bridge is used to get an ICE/STUN-verified address for a
 * peer -- it upgrades net_udp.c's own connection_statuses[] direct/proxy
 * mesh with a more accurate address than the naive simultaneous-send
 * holepunch can discover on its own. It does not carry any gameplay
 * traffic itself; PDATA/MDATA/etc. continue over the existing raw UDP path.
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

#ifdef __cplusplus
}
#endif

#endif /* GNS_BRIDGE_H */
