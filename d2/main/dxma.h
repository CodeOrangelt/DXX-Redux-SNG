/*
 * DXMA (sectorgame.com/dxma) mission database integration.
 *
 * Three things live here:
 *  - an in-game browser over the mission list (id/title/author/mode/URL),
 *    seeded from a CSV compiled into the executable so it works offline;
 *  - a manual "check for new maps" refresh that fetches an updated CSV and
 *    caches it in the user's write directory;
 *  - a join-time lookup that guesses which DXMA entry a missing mission
 *    filename corresponds to, so a failed join can offer a download instead
 *    of a dead end.
 *
 * See the block comment at the top of dxma.c for the CSV schema, the
 * filename-matching heuristic and why downloads never go through a shell.
 */

#ifndef _DXMA_H
#define _DXMA_H

#include "pstypes.h"

#define DXMA_GAME_TAG "D2"   // CSV "game" column value this build cares about

typedef struct dxma_mission {
	char id[16];
	char title[128];
	char author[64];
	char mode[32];               // SP / anarchy / coop / etc, as DXMA labels it
	char download_url[512];      // the mission-page "download" redirect
	char direct_download_url[512]; // the actual file, when the CSV has one
} dxma_mission;

// Load the mission database: embedded CSV first, then a cached refreshed
// CSV on top of it if one exists and parses. Safe to call more than once
// (e.g. after a refresh); each call replaces the in-memory table.
// Returns the number of missions loaded.
int dxma_load(void);

int dxma_count(void);
const dxma_mission *dxma_get(int index);   // NULL if index is out of range

// Fetch a fresh CSV from DXMA and cache it in the write directory, then
// reload. Returns 1 on success, 0 on failure (network, parse, or write
// error) -- the previously loaded table is left in place on failure.
// This performs a blocking network request; call it from a menu action,
// not a per-frame path.
int dxma_refresh(void);

// Best-effort match of an 8.3 mission filename (as carried in netgame info,
// e.g. from Netgame.mission_name) against the loaded DXMA table. Compares
// the filename stem against each entry's title-slug and download filename.
// Returns the table index of the best match, or -1 if nothing scores above
// the confidence floor. This is a heuristic, not a lookup by key -- see
// dxma.c for why the CSV cannot support an exact one today.
int dxma_find_match_for_filename(const char *mission_filename);

// Download and extract a mission by table index into MISSION_DIR. Runs
// synchronously and shows its own progress/confirmation dialogs; intended
// to be called directly from a menu action or the join-failure prompt.
// Returns 1 on success.
int dxma_download_mission(int index);

// Opens the mission browser menu (id/title/author, sorted, searchable).
void dxma_missions_menu(void);

#endif
