/*
 * SNG: the peer book. See peerbook.h for what it is for.
 *
 * One tab-separated line per peer in PEERBOOK_FILE, rewritten whole on save;
 * the book is a few hundred short records, so there is nothing to gain from
 * updating it in place.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "pstypes.h"
#include "u_mem.h"
#include "physfsx.h"
#include "console.h"
#include "peerbook.h"
#include "net_udp.h"

#define PEERBOOK_FILE "peers.tsv"
#define PEERBOOK_LINE_LEN 256

static peer_entry *Peers = NULL;
static int Num_peers = 0;
static int Peers_dirty = 0;

static int peerbook_alloc(void)
{
	if (Peers)
		return 1;
	Peers = d_malloc(sizeof(peer_entry) * PEERBOOK_MAX);
	if (!Peers)
		return 0;
	memset(Peers, 0, sizeof(peer_entry) * PEERBOOK_MAX);
	return 1;
}

// Splits `line` at tabs, writing up to `max` field pointers into `out` and
// terminating each one. Returns how many fields it found.
static int peerbook_split(char *line, char **out, int max)
{
	int n = 0;

	while (n < max)
	{
		char *tab = strchr(line, '\t');

		out[n++] = line;
		if (!tab)
			break;
		*tab = '\0';
		line = tab + 1;
	}
	return n;
}

static void peerbook_copy_field(char *dest, size_t size, const char *src)
{
	snprintf(dest, size, "%s", src ? src : "");
}

void peerbook_load(void)
{
	PHYSFS_file *fp;
	char line[PEERBOOK_LINE_LEN];

	if (!peerbook_alloc())
		return;
	Num_peers = 0;

	fp = PHYSFSX_openReadBuffered(PEERBOOK_FILE);
	if (!fp)
		return;

	while (Num_peers < PEERBOOK_MAX && PHYSFSX_fgets(line, sizeof(line), fp))
	{
		peer_entry *p = &Peers[Num_peers];
		char *field[6];
		int nfields = peerbook_split(line, field, 6);

		if (nfields < 2 || !field[0][0])
			continue;
		memset(p, 0, sizeof(*p));
		peerbook_copy_field(p->name, sizeof(p->name), field[0]);
		peerbook_copy_field(p->addr, sizeof(p->addr), field[1]);
		p->games = nfields > 2 ? atoi(field[2]) : 0;
		p->p2p = nfields > 3 ? atoi(field[3]) : 0;
		p->last_seen = nfields > 4 ? atoi(field[4]) : 0;
		if (nfields > 5)
			peerbook_copy_field(p->note, sizeof(p->note), field[5]);
		Num_peers++;
	}
	PHYSFS_close(fp);
	Peers_dirty = 0;
}

void peerbook_save(void)
{
	PHYSFS_file *fp;
	int i;

	if (!Peers || !Peers_dirty)
		return;

	fp = PHYSFSX_openWriteBuffered(PEERBOOK_FILE);
	if (!fp)
	{
		con_printf(CON_NORMAL, "peerbook: cannot write %s\n", PEERBOOK_FILE);
		return;
	}
	for (i = 0; i < Num_peers; i++)
	{
		const peer_entry *p = &Peers[i];

		PHYSFSX_printf(fp, "%s\t%s\t%d\t%d\t%d\t%s\n",
			p->name, p->addr, p->games, p->p2p, p->last_seen, p->note);
	}
	PHYSFS_close(fp);
	Peers_dirty = 0;
}

void peerbook_close(void)
{
	peerbook_save();
	if (Peers)
		d_free(Peers);
	Num_peers = 0;
}

static int peerbook_find(const char *callsign)
{
	int i;

	for (i = 0; i < Num_peers; i++)
		if (!d_stricmp(Peers[i].name, callsign))
			return i;
	return -1;
}

// Drops the entry not seen for longest, so a full book keeps the peers you
// actually play with rather than whoever you met first.
static int peerbook_oldest(void)
{
	int oldest = 0, i;

	for (i = 1; i < Num_peers; i++)
		if (Peers[i].last_seen < Peers[oldest].last_seen)
			oldest = i;
	return oldest;
}

void peerbook_note_player(const char *callsign, const struct _sockaddr *addr, int via_p2p)
{
	peer_entry *p;
	int index;

	if (!callsign || !callsign[0] || !peerbook_alloc())
		return;

	index = peerbook_find(callsign);
	if (index < 0)
	{
		index = Num_peers < PEERBOOK_MAX ? Num_peers++ : peerbook_oldest();
		memset(&Peers[index], 0, sizeof(Peers[index]));
		snprintf(Peers[index].name, sizeof(Peers[index].name), "%s", callsign);
	}

	p = &Peers[index];
	if (addr)
	{
		char ip[PEERBOOK_ADDR_LEN];

#ifdef IPv6
		const struct sockaddr_in6 *sin = (const struct sockaddr_in6 *)addr;

		if (inet_ntop(AF_INET6, &sin->sin6_addr, ip, sizeof(ip)))
			snprintf(p->addr, sizeof(p->addr), "[%s]:%d", ip, ntohs(sin->sin6_port));
#else
		const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

		if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)))
			snprintf(p->addr, sizeof(p->addr), "%s:%d", ip, ntohs(sin->sin_port));
#endif
	}
	p->games++;
	if (via_p2p)
		p->p2p++;
	p->last_seen = (int)time(NULL);
	Peers_dirty = 1;
	// Written out as it happens: the file is tiny, and a game that ends in
	// a crash should still leave the peer recorded.
	peerbook_save();
}

int peerbook_count(void)
{
	return Num_peers;
}

const peer_entry *peerbook_get(int index)
{
	if (!Peers || index < 0 || index >= Num_peers)
		return NULL;
	return &Peers[index];
}

void peerbook_forget(int index)
{
	if (!Peers || index < 0 || index >= Num_peers)
		return;
	memmove(&Peers[index], &Peers[index + 1], sizeof(peer_entry) * (Num_peers - index - 1));
	Num_peers--;
	Peers_dirty = 1;
}
