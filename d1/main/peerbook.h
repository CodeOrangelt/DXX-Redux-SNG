/*
 * SNG: a book of the peers you have played with -- callsign, the address
 * they were last reachable at, and how well a P2P route held up. Kept so a
 * later game can dial someone straight back instead of hunting a tracker.
 *
 * GNS has no stable per-person id to key on: gns_bridge.cpp names a peer
 * "sng<slot>", which is the seat they sat in, not who they are. The
 * callsign is the closest thing to an identity the protocol carries, so
 * that is what an entry is filed under.
 */

#ifndef _PEERBOOK_H
#define _PEERBOOK_H

#include "player.h"
#include "multi.h"	// struct _sockaddr

#define PEERBOOK_MAX 256
#define PEERBOOK_ADDR_LEN 48
#define PEERBOOK_NOTE_LEN 48

typedef struct peer_entry
{
	char name[CALLSIGN_LEN + 1];
	char addr[PEERBOOK_ADDR_LEN];	// "host:port", as manual join takes it
	char note[PEERBOOK_NOTE_LEN];
	int games;			// times seen in a game
	int p2p;			// of those, times a GNS route came up
	int last_seen;			// unix time
} peer_entry;

// Reads the book from the user directory; safe to call more than once.
void peerbook_load(void);
void peerbook_save(void);
void peerbook_close(void);

// Files one sighting of a player. `addr` is their UDP address, and
// `via_p2p` says a GNS route was established to them.
void peerbook_note_player(const char *callsign, const struct _sockaddr *addr, int via_p2p);

int peerbook_count(void);
const peer_entry *peerbook_get(int index);
void peerbook_forget(int index);

#endif
