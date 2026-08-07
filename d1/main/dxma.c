/*
 * DXMA mission database integration. See dxma.h for the public API summary.
 *
 * ----------------------------------------------------------------- CSV --
 *
 * CSV columns (from sectorgame.com/dxma's own export):
 *   id, title, mode, game, date, author, mission_url, download_url,
 *   direct_download_url, download_status
 *
 * `game` is one of D1/D2/D3/XL; this build only keeps rows matching
 * DXMA_GAME_TAG. `direct_download_url` is the actual mission archive when
 * DXMA has resolved one, `download_url` is the mission-page redirect (used
 * as a fallback when the direct link is empty).
 *
 * The baseline table is compiled into the binary (see cmake/embed_file.cmake
 * and the dxma_csv_data[] it generates from
 * dxma_missions_complete_with_direct_links.csv at the repo root) so the
 * browser works with no network and survives however the build gets
 * packaged -- AppImage, .exe, or a bare directory. dxma_refresh() can
 * overlay a newer CSV fetched from DXMA, cached in the write directory; if
 * that fetch or parse fails, the embedded baseline is left untouched.
 *
 * ---------------------------------------------------- filename matching --
 *
 * A netgame only advertises its mission by an 8.3 filename stem (see
 * Netgame.mission_name, 9 bytes on the wire in net_udp.c). The CSV has no
 * filename column -- of ~1800 rows, only a literal handful mention a
 * .msn/.hog name in free text at all. So "I need ULTIMA.MSN" cannot be
 * answered by an exact key lookup against this data.
 *
 * What the CSV does reliably have is a title and a download URL whose
 * basename is usually a slugged form of that title (e.g. "Lunar Eclipse 15"
 * -> LUNAR15.zip). dxma_find_match_for_filename() compares the wanted stem
 * against a normalized (lowercased, non-alphanumeric-stripped) form of both
 * the title and the download filename, and returns the best-scoring entry
 * above a confidence floor. It is a heuristic, not a key lookup: it will
 * sometimes miss, and the caller must treat a match as "probably this one",
 * not certainty. A future protocol addition (the host advertising its own
 * DXMA id) would make this exact; nothing here blocks that.
 *
 * -------------------------------------------------------- safe download --
 *
 * The original version of this feature built a shell command line by
 * snprintf'ing the DXMA URL into a `system()` string
 * (`wget -q -O '%s' '%s' || curl ...`). Single-quoting the URL is not
 * sufflicient escaping -- a URL containing a `'` breaks out of the quotes and
 * the remainder is interpreted by the shell. That was a latent problem while
 * the CSV only ever shipped with the build; dxma_refresh() below makes the
 * CSV (and therefore every URL in it) something fetched from the network,
 * which turns the same bug into remote code execution triggered by opening
 * the mission browser or joining a game with a missing map.
 *
 * dxma_spawn_argv() replaces the shell entirely: POSIX uses fork+execvp with
 * an argv array (no string is ever parsed as shell syntax), Windows uses
 * CreateProcess with arguments serialized via Windows' own argv-quoting
 * rules (which is command-line *argument* encoding, not shell interpretation
 * -- there is no cmd.exe in the loop). dxma_url_is_safe() additionally
 * requires https and a sectorgame.com host before anything is ever spawned,
 * and dxma_safe_basename() rejects any download filename containing a path
 * separator or "..", so a malicious CSV entry cannot write outside
 * MISSION_DIR either.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "inferno.h"
#include "dxxerror.h"
#include "u_mem.h"
#include "physfsx.h"
#include "newmenu.h"
#include "gauges.h"
#include "digi.h"
#include "weapon.h"
#include "mission.h"
#include "key.h"
#include "timer.h"
#include "event.h"
#include "window.h"
#include "dxma.h"
#include "gamefont.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dlfcn.h>
#endif

// Symbols from the generated dxma_csv_data.c (cmake/embed_file.cmake output).
extern const unsigned char dxma_csv_data[];
extern const unsigned int dxma_csv_data_len;

#define DXMA_CACHE_FILE   "dxma_missions_cache.csv"
#define DXMA_REFRESH_URL  "https://sectorgame.com/dxma/export/csv"
#define DXMA_LISTING_URL  "https://sectorgame.com/dxma/"
#define MAX_DXMA_MISSIONS 3000
#define DXMA_ROW_TEXT_LEN 256
#define DXMA_FILTER_LEN 48
#define DXMA_REFRESH_MAX_PAGES 80
#define DXMA_REFRESH_TMP_FILE "dxma_refresh_fetch.tmp"

static dxma_mission Missions[MAX_DXMA_MISSIONS];
static int MissionCount = 0;

static int dxma_url_is_safe(const char *url);
static int dxma_try_download(const char *url, const char *dest);

// ------------------------------------------------------------- CSV parsing

// RFC4180-ish CSV field extractor. Handles quoted fields and ""-escaped
// quotes inside them; ordinary commas outside quotes end a field. This is
// what stops mission titles containing commas ("Foo, Part 2") from
// shifting URL text into the mode/author columns.
static const char *dxma_csv_next_field(const char *p, char *out, size_t outsz)
{
	size_t o = 0;
	if (outsz == 0) return p;
	out[0] = '\0';
	if (!p) return p;

	if (*p == '"')
	{
		p++;
		while (*p)
		{
			if (*p == '"' && *(p + 1) == '"')
			{
				if (o + 1 < outsz) out[o++] = '"';
				p += 2;
			}
			else if (*p == '"')
			{
				p++;
				break;
			}
			else
			{
				if (o + 1 < outsz) out[o++] = *p;
				p++;
			}
		}
	}
	else
	{
		while (*p && *p != ',' && *p != '\n' && *p != '\r')
		{
			if (o + 1 < outsz) out[o++] = *p;
			p++;
		}
	}
	out[o] = '\0';
	if (*p == ',') p++;
	return p;
}

static int dxma_parse_csv_line(char *line, dxma_mission *out)
{
	char game_type[16] = {0};
	char field[1024];
	int field_idx = 0;

	if (!line || !line[0]) return 0;
	if (strstr(line, "id,title,mode,game")) return 0;   // header row

	memset(out, 0, sizeof(*out));

	const char *p = line;
	while (*p && field_idx < 12)
	{
		p = dxma_csv_next_field(p, field, sizeof(field));
		switch (field_idx)
		{
		case 0: strncpy(out->id, field, sizeof(out->id) - 1); break;
		case 1: strncpy(out->title, field, sizeof(out->title) - 1); break;
		case 2: strncpy(out->mode, field, sizeof(out->mode) - 1); break;
		case 3: strncpy(game_type, field, sizeof(game_type) - 1); break;
		case 5: strncpy(out->author, field, sizeof(out->author) - 1); break;
		case 7: strncpy(out->download_url, field, sizeof(out->download_url) - 1); break;
		case 8: strncpy(out->direct_download_url, field, sizeof(out->direct_download_url) - 1); break;
		}
		field_idx++;
	}

	if (strcmp(game_type, DXMA_GAME_TAG) != 0)
		return 0;

	return out->id[0] && out->title[0] && (out->download_url[0] || out->direct_download_url[0]);
}

static int dxma_compare(const void *a, const void *b)
{
	return d_stricmp(((const dxma_mission *)a)->title, ((const dxma_mission *)b)->title);
}

static int dxma_mode_is_relevant(const char *mode)
{
	if (!mode || !mode[0])
		return 0;
	return !d_stricmp(mode, "SP") || !d_stricmp(mode, "MP") || !d_stricmp(mode, "CTF") || !d_stricmp(mode, "T");
}

static void dxma_strip_tags(const char *in, char *out, size_t outsz)
{
	size_t j = 0;
	int in_tag = 0;

	for (size_t i = 0; in[i] && j + 1 < outsz; i++)
	{
		char c = in[i];
		if (c == '<')
		{
			in_tag = 1;
			continue;
		}
		if (c == '>')
		{
			in_tag = 0;
			continue;
		}
		if (in_tag)
			continue;
		if (c == '\r' || c == '\n' || c == '\t')
			c = ' ';
		out[j++] = c;
	}
	out[j] = '\0';

	char compact[512];
	char *d = compact;
	int prev_space = 1;
	for (size_t i = 0; out[i] && (size_t)(d - compact) + 1 < sizeof(compact); i++)
	{
		char c = out[i];
		if (isspace((unsigned char)c))
		{
			if (!prev_space)
				*d++ = ' ';
			prev_space = 1;
		}
		else
		{
			*d++ = c;
			prev_space = 0;
		}
	}
	*d = '\0';

	strncpy(out, compact, outsz - 1);
	out[outsz - 1] = '\0';
	for (size_t i = strlen(out); i > 0 && out[i - 1] == ' '; i--)
		out[i - 1] = '\0';
}

static void dxma_unescape_basic_html(char *s)
{
	char out[512];
	char *d = out;
	for (size_t i = 0; s[i] && (size_t)(d - out) + 1 < sizeof(out); i++)
	{
		if (s[i] == '&')
		{
			if (!strncmp(&s[i], "&amp;", 5)) { *d++ = '&'; i += 4; continue; }
			if (!strncmp(&s[i], "&quot;", 6)) { *d++ = '"'; i += 5; continue; }
			if (!strncmp(&s[i], "&#39;", 5)) { *d++ = '\''; i += 4; continue; }
			if (!strncmp(&s[i], "&lt;", 4)) { *d++ = '<'; i += 3; continue; }
			if (!strncmp(&s[i], "&gt;", 4)) { *d++ = '>'; i += 3; continue; }
		}
		*d++ = s[i];
	}
	*d = '\0';
	strncpy(s, out, 511);
	s[511] = '\0';
}

static int dxma_extract_mission_id_from_cell(const char *cell_html)
{
	const char *m = strstr(cell_html, "mission?m=");
	if (!m)
		return -1;
	m += strlen("mission?m=");
	int id = 0;
	while (*m && isdigit((unsigned char)*m))
	{
		id = id * 10 + (*m - '0');
		m++;
	}
	return id > 0 ? id : -1;
}

static int dxma_fetch_url_to_buffer(const char *url, char **out_buf, size_t *out_len)
{
	char real_tmp[PATH_MAX];
	PHYSFS_file *fp = NULL;
	PHYSFS_sint64 sz;
	char *buf = NULL;

	if (!dxma_url_is_safe(url))
		return 0;
	if (!PHYSFSX_getRealPath(DXMA_REFRESH_TMP_FILE, real_tmp))
		return 0;
	if (!dxma_try_download(url, real_tmp))
		return 0;

	fp = PHYSFSX_openReadBuffered(DXMA_REFRESH_TMP_FILE);
	if (!fp)
		return 0;

	sz = PHYSFS_fileLength(fp);
	if (sz <= 0 || sz > 16 * 1024 * 1024)
	{
		PHYSFS_close(fp);
		PHYSFS_delete(DXMA_REFRESH_TMP_FILE);
		return 0;
	}

	buf = d_malloc((size_t)sz + 1);
	if (!buf)
	{
		PHYSFS_close(fp);
		PHYSFS_delete(DXMA_REFRESH_TMP_FILE);
		return 0;
	}

	if (PHYSFS_read(fp, buf, 1, (PHYSFS_uint32)sz) != sz)
	{
		d_free(buf);
		PHYSFS_close(fp);
		PHYSFS_delete(DXMA_REFRESH_TMP_FILE);
		return 0;
	}

	buf[sz] = '\0';
	PHYSFS_close(fp);
	PHYSFS_delete(DXMA_REFRESH_TMP_FILE);

	*out_buf = buf;
	*out_len = (size_t)sz;
	return 1;
}

static int dxma_has_id(const dxma_mission *arr, int count, const char *id)
{
	for (int i = 0; i < count; i++)
		if (!strcmp(arr[i].id, id))
			return 1;
	return 0;
}

static void dxma_copy_csv_safe(char *dst, size_t dstsz, const char *src)
{
	size_t j = 0;
	for (size_t i = 0; src[i] && j + 1 < dstsz; i++)
	{
		char c = src[i];
		if (c == ',' || c == '"' || c == '\n' || c == '\r')
			c = ' ';
		dst[j++] = c;
	}
	dst[j] = '\0';
}

static int dxma_write_cache_from_missions(void)
{
	PHYSFS_file *dst = PHYSFSX_openWriteBuffered(DXMA_CACHE_FILE);
	if (!dst)
		return 0;

	const char *hdr = "id,title,mode,game,date,author,mission_url,download_url,direct_download_url,download_status\n";
	if (PHYSFS_write(dst, hdr, 1, (PHYSFS_uint32)strlen(hdr)) != (PHYSFS_sint64)strlen(hdr))
	{
		PHYSFS_close(dst);
		return 0;
	}

	for (int i = 0; i < MissionCount; i++)
	{
		char title[256], mode[64], author[128], download_url[512], direct_url[512];
		char line[2048];

		dxma_copy_csv_safe(title, sizeof(title), Missions[i].title);
		dxma_copy_csv_safe(mode, sizeof(mode), Missions[i].mode);
		dxma_copy_csv_safe(author, sizeof(author), Missions[i].author);
		dxma_copy_csv_safe(download_url, sizeof(download_url), Missions[i].download_url);
		dxma_copy_csv_safe(direct_url, sizeof(direct_url), Missions[i].direct_download_url);

		snprintf(line, sizeof(line), "%s,%s,%s,%s,,%s,,%s,%s,success\n",
			Missions[i].id, title, mode, DXMA_GAME_TAG, author, download_url, direct_url);
		if (PHYSFS_write(dst, line, 1, (PHYSFS_uint32)strlen(line)) != (PHYSFS_sint64)strlen(line))
		{
			PHYSFS_close(dst);
			return 0;
		}
	}

	PHYSFS_close(dst);
	return 1;
}

static int dxma_refresh_by_scraping(void)
{
	dxma_mission incoming[MAX_DXMA_MISSIONS];
	int incoming_count = 0;
	int saw_any_page = 0;

	for (int page = 1; page <= DXMA_REFRESH_MAX_PAGES; page++)
	{
		char url[256];
		char *html = NULL;
		size_t html_len = 0;
		const char *p;
		int rows = 0;
		int all_known = 1;

		snprintf(url, sizeof(url), "%s?page=%d", DXMA_LISTING_URL, page);
		if (!dxma_fetch_url_to_buffer(url, &html, &html_len))
			break;
		if (!html || html_len == 0)
		{
			if (html) d_free(html);
			break;
		}

		saw_any_page = 1;
		p = html;
		while ((p = strstr(p, "<tr")) != NULL)
		{
			const char *row_end = strstr(p, "</tr>");
			if (!row_end)
				break;

			char row[4096];
			size_t row_len = (size_t)(row_end - p);
			if (row_len >= sizeof(row)) row_len = sizeof(row) - 1;
			memcpy(row, p, row_len);
			row[row_len] = '\0';

			char cells[5][1024];
			char mode[64], game[64], title[256], author[128];
			const char *q = row;
			int c = 0;
			int mission_id;

			while (c < 5)
			{
				const char *td = strstr(q, "<td");
				const char *gt;
				const char *td_end;
				size_t len;
				if (!td) break;
				gt = strchr(td, '>');
				if (!gt) break;
				td_end = strstr(gt + 1, "</td>");
				if (!td_end) break;
				len = (size_t)(td_end - (gt + 1));
				if (len >= sizeof(cells[c])) len = sizeof(cells[c]) - 1;
				memcpy(cells[c], gt + 1, len);
				cells[c][len] = '\0';
				q = td_end + 5;
				c++;
			}

			if (c < 5)
			{
				p = row_end + 5;
				continue;
			}

			dxma_strip_tags(cells[0], mode, sizeof(mode));
			dxma_unescape_basic_html(mode);
			if (!dxma_mode_is_relevant(mode))
			{
				p = row_end + 5;
				continue;
			}

			dxma_strip_tags(cells[1], game, sizeof(game));
			dxma_unescape_basic_html(game);
			if (d_stricmp(game, DXMA_GAME_TAG) != 0)
			{
				p = row_end + 5;
				continue;
			}

			mission_id = dxma_extract_mission_id_from_cell(cells[2]);
			if (mission_id <= 0)
			{
				p = row_end + 5;
				continue;
			}

			rows++;
			char idbuf[16];
			snprintf(idbuf, sizeof(idbuf), "%d", mission_id);

			if (dxma_has_id(Missions, MissionCount, idbuf) || dxma_has_id(incoming, incoming_count, idbuf))
			{
				p = row_end + 5;
				continue;
			}

			all_known = 0;
			if (MissionCount + incoming_count >= MAX_DXMA_MISSIONS)
			{
				p = row_end + 5;
				continue;
			}

			dxma_strip_tags(cells[2], title, sizeof(title));
			dxma_strip_tags(cells[4], author, sizeof(author));
			dxma_unescape_basic_html(title);
			dxma_unescape_basic_html(author);

			dxma_mission *m = &incoming[incoming_count++];
			memset(m, 0, sizeof(*m));
			strncpy(m->id, idbuf, sizeof(m->id) - 1);
			strncpy(m->title, title, sizeof(m->title) - 1);
			strncpy(m->mode, mode, sizeof(m->mode) - 1);
			strncpy(m->author, author, sizeof(m->author) - 1);
			snprintf(m->download_url, sizeof(m->download_url), "https://sectorgame.com/dxma/download?m=%s", m->id);

			p = row_end + 5;
		}

		d_free(html);
		if (rows == 0)
			break;
		if (all_known)
			break;
	}

	if (!saw_any_page)
		return 0;

	if (incoming_count > 0)
	{
		memcpy(&Missions[MissionCount], incoming, sizeof(dxma_mission) * incoming_count);
		MissionCount += incoming_count;
		qsort(Missions, MissionCount, sizeof(dxma_mission), dxma_compare);
		dxma_write_cache_from_missions();
		con_printf(CON_NORMAL, "DXMA: refresh found %d new %s missions via page scraping\n", incoming_count, DXMA_GAME_TAG);
	}
	else
	{
		con_printf(CON_NORMAL, "DXMA: refresh found no new %s missions via page scraping\n", DXMA_GAME_TAG);
	}

	return 1;
}

// Parses CSV text held entirely in memory (used for both the embedded
// baseline and a freshly downloaded refresh, before either is committed to
// `Missions`). Returns the number of rows parsed into `out`, up to `cap`.
static int dxma_parse_csv_buffer(const char *data, size_t len, dxma_mission *out, int cap)
{
	char line[2048];
	size_t i = 0;
	int n = 0;

	while (i < len && n < cap)
	{
		size_t start = i;
		while (i < len && data[i] != '\n') i++;
		size_t linelen = i - start;
		if (linelen >= sizeof(line)) linelen = sizeof(line) - 1;
		memcpy(line, data + start, linelen);
		line[linelen] = '\0';
		if (linelen && line[linelen - 1] == '\r') line[linelen - 1] = '\0';

		if (dxma_parse_csv_line(line, &out[n]))
			n++;
		i++;   // skip the newline
	}
	return n;
}

int dxma_load(void)
{
	int n = dxma_parse_csv_buffer((const char *)dxma_csv_data, dxma_csv_data_len, Missions, MAX_DXMA_MISSIONS);
	MissionCount = n;
	con_printf(CON_NORMAL, "DXMA: %d missions from embedded database\n", n);

	// Merge any cached refresh on top of the embedded baseline. The cache
	// is written after every successful Ctrl+R and contains only missions
	// that were not already in the embedded CSV, so entries are additive.
	// Using merge (not replace) means embedded baseline updates and cached
	// scrape results coexist safely.
	PHYSFS_file *fp = PHYSFSX_openReadBuffered(DXMA_CACHE_FILE);
	if (fp)
	{
		PHYSFS_sint64 sz = PHYSFS_fileLength(fp);
		if (sz > 0 && sz < 8 * 1024 * 1024)
		{
			char *buf = d_malloc((size_t)sz);
			if (buf && PHYSFS_read(fp, buf, 1, (PHYSFS_uint32)sz) == sz)
			{
				dxma_mission tmp[MAX_DXMA_MISSIONS];
				int cn = dxma_parse_csv_buffer(buf, (size_t)sz, tmp, MAX_DXMA_MISSIONS);
				int added = 0;
				for (int i = 0; i < cn && MissionCount < MAX_DXMA_MISSIONS; i++)
					if (!dxma_has_id(Missions, MissionCount, tmp[i].id))
					{
						Missions[MissionCount++] = tmp[i];
						added++;
					}
				if (added > 0)
					con_printf(CON_NORMAL, "DXMA: +%d missions merged from saved cache\n", added);
			}
			if (buf) d_free(buf);
		}
		PHYSFS_close(fp);
	}

	if (MissionCount > 0)
		qsort(Missions, MissionCount, sizeof(dxma_mission), dxma_compare);
	return MissionCount;
}

int dxma_count(void) { return MissionCount; }

const dxma_mission *dxma_get(int index)
{
	if (index < 0 || index >= MissionCount) return NULL;
	return &Missions[index];
}

// ------------------------------------------------------------------ safety

// https + sectorgame.com (or a subdomain of it) only. Every URL in the CSV
// is expected to already look like this; this is a floor under a
// compromised or malicious CSV, not a normal-path check.
static int dxma_url_is_safe(const char *url)
{
	static const char prefix[] = "https://";
	static const char host[] = "sectorgame.com";
	size_t plen = sizeof(prefix) - 1, hlen = sizeof(host) - 1;

	if (!url || d_strnicmp(url, prefix, (int)plen) != 0)
		return 0;

	const char *hstart = url + plen;
	const char *slash = strchr(hstart, '/');
	size_t hostlen = slash ? (size_t)(slash - hstart) : strlen(hstart);

	if (hostlen == hlen && d_strnicmp(hstart, host, (int)hlen) == 0)
		return 1;
	// allow a subdomain: "...sectorgame.com" ending exactly at a dot boundary
	if (hostlen > hlen && d_strnicmp(hstart + hostlen - hlen, host, (int)hlen) == 0
		&& hstart[hostlen - hlen - 1] == '.')
		return 1;
	return 0;
}

// Basename with no path traversal, no separators, no leading dot -- so a
// crafted URL can never write outside MISSION_DIR or overwrite a dotfile.
static int dxma_safe_basename(const char *url, char *out, size_t outsz)
{
	const char *slash = strrchr(url, '/');
	const char *name = slash ? slash + 1 : url;
	if (!name[0] || name[0] == '.' || strstr(name, "..") ||
		strchr(name, '/') || strchr(name, '\\'))
		return 0;
	strncpy(out, name, outsz - 1);
	out[outsz - 1] = '\0';
	return out[0] != '\0';
}

// Spawn argv[0] with the given arguments and wait for it to exit. No shell
// is ever invoked: POSIX uses fork+execvp against the argv array directly;
// Windows serializes argv into a single command line using the platform's
// own argument-quoting rules (MSDN "Parsing C++ Command-Line Arguments") and
// hands that to CreateProcess, which still launches argv[0] directly -- the
// quoting only controls how *that program* re-splits its own arguments, it
// is not shell syntax and metacharacters have no special meaning in it.
// Returns 1 if the process ran and exited 0, else 0.
static int dxma_spawn_argv(char *const argv[])
{
#ifdef _WIN32
	char cmdline[4096] = {0};
	size_t pos = 0;
	for (int i = 0; argv[i]; i++)
	{
		if (i) cmdline[pos++] = ' ';
		cmdline[pos++] = '"';
		for (const char *p = argv[i]; *p && pos < sizeof(cmdline) - 2; p++)
		{
			if (*p == '"' || *p == '\\') cmdline[pos++] = '\\';
			cmdline[pos++] = *p;
		}
		cmdline[pos++] = '"';
		if (pos >= sizeof(cmdline) - 2) return 0;
	}
	cmdline[pos] = '\0';

	STARTUPINFOA si; PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
	memset(&pi, 0, sizeof(pi));

	if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
		return 0;

	WaitForSingleObject(pi.hProcess, 60000);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return code == 0;
#else
	pid_t pid = fork();
	if (pid < 0) return 0;
	if (pid == 0)
	{
		// Child: redirect noise away from the game's own console. Nothing
		// useful to do if these fail (we're about to exec anyway), so the
		// result is deliberately discarded rather than treated as fatal.
		if (!freopen("/dev/null", "w", stdout)) {}
		if (!freopen("/dev/null", "w", stderr)) {}
		execvp(argv[0], argv);
		_exit(127);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0) return 0;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

// ----------------------------------------------------------- libcurl loader
//
// Downloads work "out of the box" whenever libcurl is present on the system
// (it is on essentially every Linux install, and can be shipped alongside
// the game on Windows). We load it at runtime with dlopen/LoadLibrary so no
// build system changes or link-time dependencies are required -- if it is
// missing we fall through to the existing curl/wget spawn path.
//
// The option numbers hardcoded below are libcurl's stable public ABI values
// (see include/curl/curl.h in libcurl); they are safe to use without the
// header. Ranges: 0-9999 = long, 10000-19999 = pointer, 20000-29999 =
// function pointer, so ordinary variadic promotion via a (...) function
// pointer passes them correctly.
#define DXMA_CURLOPT_URL              10002
#define DXMA_CURLOPT_WRITEDATA        10001
#define DXMA_CURLOPT_USERAGENT        10018
#define DXMA_CURLOPT_WRITEFUNCTION    20011
#define DXMA_CURLOPT_FOLLOWLOCATION      52
#define DXMA_CURLOPT_TIMEOUT             13
#define DXMA_CURLOPT_FAILONERROR         45
#define DXMA_CURLOPT_NOSIGNAL            99
#define DXMA_CURLOPT_CONNECTTIMEOUT      78

typedef void DXMA_CURL;
typedef int  DXMA_CURLcode;
typedef size_t (*dxma_curl_write_cb)(void *ptr, size_t size, size_t nmemb, void *userdata);

static void        *dxma_curl_lib          = NULL;
static int          dxma_curl_tried_load   = 0;
static DXMA_CURL   *(*dxma_p_easy_init)(void)                                  = NULL;
static DXMA_CURLcode(*dxma_p_easy_setopt)(DXMA_CURL *, int, ...)               = NULL;
static DXMA_CURLcode(*dxma_p_easy_perform)(DXMA_CURL *)                        = NULL;
static void         (*dxma_p_easy_cleanup)(DXMA_CURL *)                        = NULL;
static const char  *(*dxma_p_easy_strerror)(DXMA_CURLcode)                     = NULL;
static DXMA_CURLcode(*dxma_p_global_init)(long)                                = NULL;

static void *dxma_dl_open(const char *name)
{
#ifdef _WIN32
	return (void *)LoadLibraryA(name);
#else
	return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *dxma_dl_sym(void *handle, const char *name)
{
#ifdef _WIN32
	return (void *)GetProcAddress((HMODULE)handle, name);
#else
	return dlsym(handle, name);
#endif
}

static int dxma_libcurl_load(void)
{
	if (dxma_curl_tried_load) return dxma_curl_lib != NULL;
	dxma_curl_tried_load = 1;

	static const char *candidates[] = {
#ifdef _WIN32
		"libcurl.dll", "libcurl-4.dll", "curl.dll",
#elif defined(__APPLE__)
		"libcurl.4.dylib", "libcurl.dylib",
#else
		"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so.3", "libcurl.so",
#endif
		NULL
	};

	for (int i = 0; candidates[i]; i++)
	{
		dxma_curl_lib = dxma_dl_open(candidates[i]);
		if (dxma_curl_lib)
		{
			con_printf(CON_NORMAL, "DXMA: loaded %s for in-process downloads\n", candidates[i]);
			break;
		}
	}
	if (!dxma_curl_lib) return 0;

	dxma_p_easy_init     = (DXMA_CURL *(*)(void))               dxma_dl_sym(dxma_curl_lib, "curl_easy_init");
	dxma_p_easy_setopt   = (DXMA_CURLcode(*)(DXMA_CURL *, int, ...)) dxma_dl_sym(dxma_curl_lib, "curl_easy_setopt");
	dxma_p_easy_perform  = (DXMA_CURLcode(*)(DXMA_CURL *))      dxma_dl_sym(dxma_curl_lib, "curl_easy_perform");
	dxma_p_easy_cleanup  = (void (*)(DXMA_CURL *))              dxma_dl_sym(dxma_curl_lib, "curl_easy_cleanup");
	dxma_p_easy_strerror = (const char *(*)(DXMA_CURLcode))     dxma_dl_sym(dxma_curl_lib, "curl_easy_strerror");
	dxma_p_global_init   = (DXMA_CURLcode(*)(long))             dxma_dl_sym(dxma_curl_lib, "curl_global_init");

	if (!dxma_p_easy_init || !dxma_p_easy_setopt || !dxma_p_easy_perform || !dxma_p_easy_cleanup)
	{
#ifndef _WIN32
		dlclose(dxma_curl_lib);
#endif
		dxma_curl_lib = NULL;
		return 0;
	}

	// CURL_GLOBAL_DEFAULT (== CURL_GLOBAL_SSL | CURL_GLOBAL_WIN32) == 3
	if (dxma_p_global_init) dxma_p_global_init(3L);
	return 1;
}

static size_t dxma_libcurl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	FILE *fp = (FILE *)userdata;
	return fwrite(ptr, size, nmemb, fp);
}

// In-process download via libcurl. Writes the response body to `dest`.
// Returns 1 on HTTP 2xx + full body written, 0 on any error (libcurl not
// available, network error, HTTP failure, filesystem error).
static int dxma_libcurl_download(const char *url, const char *dest)
{
	if (!dxma_libcurl_load()) return 0;

	DXMA_CURL *curl = dxma_p_easy_init();
	if (!curl) return 0;

	FILE *fp = fopen(dest, "wb");
	if (!fp) { dxma_p_easy_cleanup(curl); return 0; }

	dxma_p_easy_setopt(curl, DXMA_CURLOPT_URL,             url);
	dxma_p_easy_setopt(curl, DXMA_CURLOPT_WRITEDATA,       fp);
	dxma_p_easy_setopt(curl, DXMA_CURLOPT_WRITEFUNCTION,   dxma_libcurl_write_cb);
	dxma_p_easy_setopt(curl, DXMA_CURLOPT_FOLLOWLOCATION,  1L);
	dxma_p_easy_setopt(curl, DXMA_CURLOPT_TIMEOUT,         60L);
	dxma_p_easy_setopt(curl, DXMA_CURLOPT_CONNECTTIMEOUT,  15L);
	dxma_p_easy_setopt(curl, DXMA_CURLOPT_FAILONERROR,     1L);
	dxma_p_easy_setopt(curl, DXMA_CURLOPT_NOSIGNAL,        1L);
	dxma_p_easy_setopt(curl, DXMA_CURLOPT_USERAGENT,       "dxx-redux-sng/dxma");

	DXMA_CURLcode res = dxma_p_easy_perform(curl);
	fflush(fp);
	fclose(fp);
	dxma_p_easy_cleanup(curl);

	if (res != 0)
	{
		remove(dest);
		con_printf(CON_NORMAL, "DXMA: libcurl %s failed: %s\n",
			url,
			dxma_p_easy_strerror ? dxma_p_easy_strerror(res) : "(no strerror)");
		return 0;
	}
	return 1;
}

static int dxma_try_download(const char *url, const char *dest)
{
	if (dxma_libcurl_download(url, dest)) return 1;

#ifdef _WIN32
	// curl.exe ships with Windows 10 1803+; avoids depending on PowerShell
	// (and the more elaborate quoting a -Command string would need).
	char *argv[] = { "curl.exe", "-fsSL", "--max-time", "60", "-o", (char *)dest, (char *)url, NULL };
#else
	char *argv[] = { "curl", "-fsSL", "--max-time", "60", "-o", (char *)dest, (char *)url, NULL };
#endif
	if (dxma_spawn_argv(argv)) return 1;

#ifndef _WIN32
	char *wget_argv[] = { "wget", "-q", "--timeout=60", "-O", (char *)dest, (char *)url, NULL };
	if (dxma_spawn_argv(wget_argv)) return 1;
#endif
	return 0;
}

static int dxma_try_extract_zip(const char *zip_path, const char *dest_dir)
{
#ifdef _WIN32
	// tar.exe has shipped with Windows since build 17063 and understands zip.
	char *argv[] = { "tar.exe", "-xf", (char *)zip_path, "-C", (char *)dest_dir, NULL };
#else
	char *argv[] = { "unzip", "-o", (char *)zip_path, "-d", (char *)dest_dir, NULL };
#endif
	return dxma_spawn_argv(argv);
}

// -------------------------------------------------------------- download

int dxma_download_mission(int index)
{
	const dxma_mission *m = dxma_get(index);
	if (!m) return 0;

	const char *url = m->direct_download_url[0] ? m->direct_download_url : m->download_url;
	if (!dxma_url_is_safe(url))
	{
		con_printf(CON_NORMAL, "DXMA: refusing to fetch untrusted URL: %s\n", url);
		nm_messagebox(NULL, 1, "OK", "This mission's download link does not point to\nsectorgame.com and was not fetched.");
		return 0;
	}

	char filename[256];
	if (!dxma_safe_basename(url, filename, sizeof(filename)))
	{
		nm_messagebox(NULL, 1, "OK", "This mission's download filename is invalid.");
		return 0;
	}

	PHYSFS_mkdir("missions");

	char dest_path[PATH_MAX];
	snprintf(dest_path, sizeof(dest_path), MISSION_DIR "%s", filename);

	if (PHYSFSX_exists(dest_path, 0))
	{
		if (nm_messagebox(NULL, 2, "Overwrite", "Cancel", "File already exists:\n\n%s\n", filename) != 0)
			return 0;
		PHYSFS_delete(dest_path);
	}
	if (nm_messagebox(NULL, 2, "Download", "Cancel", "Download from DXMA:\n\n%s\nby %s\n", m->title, m->author) != 0)
		return 0;

	char real_dest[PATH_MAX];
	if (!PHYSFSX_getRealPath(dest_path, real_dest))
	{
		nm_messagebox(NULL, 1, "OK", "Could not resolve a real path for the missions folder.");
		return 0;
	}

	newmenu_item wm; char msg[256];
	snprintf(msg, sizeof(msg), "Downloading:\n%.60s\n\nPlease wait...", filename);
	wm.type = NM_TYPE_TEXT; wm.text = msg;
	newmenu *wait_menu = newmenu_do3(NULL, NULL, 1, &wm, NULL, NULL, 0, NULL);
	timer_delay(F1_0 / 4);
	event_process();

	con_printf(CON_NORMAL, "DXMA: downloading %s -> %s\n", url, real_dest);
	int ok = dxma_try_download(url, real_dest);

	if (wait_menu) window_close(newmenu_get_window(wait_menu));

	if (!ok)
	{
		nm_messagebox(NULL, 1, "OK", "Download failed for:\n%s\n\nCheck the console log for the exact\nerror. On Linux, libcurl.so.4 is normally used\nautomatically; if it is missing, install curl or\nwget as a fallback.", url);
		return 0;
	}

	const char *dot = strrchr(filename, '.');
	if (dot && d_stricmp(dot, ".zip") == 0)
	{
		char real_dir[PATH_MAX];
		strncpy(real_dir, real_dest, sizeof(real_dir) - 1);
		real_dir[sizeof(real_dir) - 1] = '\0';
		char *d2 = strrchr(real_dir, '.');
		if (d2) *d2 = '\0';
#ifndef _WIN32
		mkdir(real_dir, 0755);
#else
		_mkdir(real_dir);
#endif
		if (!dxma_try_extract_zip(real_dest, real_dir))
			con_printf(CON_NORMAL, "DXMA: extraction failed for %s (archive kept, extract manually)\n", filename);
	}

	nm_messagebox(NULL, 1, "OK", "Mission downloaded!\n\n%s\n\nSaved to missions folder.", m->title);
	return 1;
}

// -------------------------------------------------------------- refresh

int dxma_refresh(void)
{
	if (dxma_url_is_safe(DXMA_REFRESH_URL))
	{
		char tmp_path[PATH_MAX];
		snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", DXMA_CACHE_FILE);

		char real_tmp[PATH_MAX];
		if (PHYSFSX_getRealPath(tmp_path, real_tmp))
		{
			con_printf(CON_NORMAL, "DXMA: attempting legacy CSV refresh from %s\n", DXMA_REFRESH_URL);
			if (dxma_try_download(DXMA_REFRESH_URL, real_tmp))
			{
				PHYSFS_file *fp = PHYSFSX_openReadBuffered(tmp_path);
				if (fp)
				{
					PHYSFS_sint64 sz = PHYSFS_fileLength(fp);
					int good = 0;
					if (sz > 100 && sz < 8 * 1024 * 1024)
					{
						char *buf = d_malloc((size_t)sz);
						if (buf && PHYSFS_read(fp, buf, 1, (PHYSFS_uint32)sz) == sz)
						{
							dxma_mission tmp[MAX_DXMA_MISSIONS];
							int n = dxma_parse_csv_buffer(buf, (size_t)sz, tmp, MAX_DXMA_MISSIONS);
							good = (n >= 10);
						}
						if (buf) d_free(buf);
					}
					PHYSFS_close(fp);

					if (good)
					{
						PHYSFS_delete(DXMA_CACHE_FILE);
						PHYSFS_file *dst;
						PHYSFS_file *src = PHYSFSX_openReadBuffered(tmp_path);
						int copied = 0;
						if (src)
						{
							PHYSFS_sint64 n = PHYSFS_fileLength(src);
							char *buf = n > 0 ? d_malloc((size_t)n) : NULL;
							if (buf && PHYSFS_read(src, buf, 1, (PHYSFS_uint32)n) == n)
							{
								dst = PHYSFSX_openWriteBuffered(DXMA_CACHE_FILE);
								if (dst)
								{
									copied = PHYSFS_write(dst, buf, 1, (PHYSFS_uint32)n) == n;
									PHYSFS_close(dst);
								}
							}
							if (buf) d_free(buf);
							PHYSFS_close(src);
						}
						PHYSFS_delete(tmp_path);
						if (copied)
						{
							dxma_load();
							return 1;
						}
					}
				}
			}
			PHYSFS_delete(tmp_path);
		}
	}

	con_printf(CON_NORMAL, "DXMA: legacy CSV endpoint unavailable; scraping listing pages instead\n");
	return dxma_refresh_by_scraping();
}

// -------------------------------------------------------- filename match

// Lowercase, alnum-only projection, used to compare a mission filename stem
// against a DXMA title or download filename despite punctuation/case/spacing
// differences between them.
static void dxma_normalize(const char *in, char *out, size_t outsz)
{
	size_t j = 0;
	for (size_t i = 0; in[i] && j + 1 < outsz; i++)
		if (isalnum((unsigned char)in[i]))
			out[j++] = (char)tolower((unsigned char)in[i]);
	out[j] = '\0';
}

// Score in [0, min(len_a,len_b)]: length of the longest common run found by
// a simple sliding comparison. Cheap, and sufficient to separate "clearly
// the same slug" from "coincidental short overlap" at this dataset's size.
static int dxma_common_run(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	int best = 0;
	for (size_t i = 0; i < la; i++)
	{
		for (size_t j = 0; j < lb; j++)
		{
			size_t k = 0;
			while (i + k < la && j + k < lb && a[i + k] == b[j + k]) k++;
			if ((int)k > best) best = (int)k;
		}
	}
	return best;
}

int dxma_find_match_for_filename(const char *mission_filename)
{
	if (!mission_filename || !mission_filename[0] || MissionCount == 0) return -1;

	char stem[64];
	strncpy(stem, mission_filename, sizeof(stem) - 1);
	stem[sizeof(stem) - 1] = '\0';
	char *dot = strrchr(stem, '.');
	if (dot) *dot = '\0';

	char wantNorm[64];
	dxma_normalize(stem, wantNorm, sizeof(wantNorm));
	if (!wantNorm[0]) return -1;
	size_t wantLen = strlen(wantNorm);

	int best = -1, bestScore = 0;
	for (int i = 0; i < MissionCount; i++)
	{
		char titleNorm[128], fileNorm[128];
		dxma_normalize(Missions[i].title, titleNorm, sizeof(titleNorm));

		const char *url = Missions[i].direct_download_url[0] ? Missions[i].direct_download_url : Missions[i].download_url;
		const char *slash = strrchr(url, '/');
		dxma_normalize(slash ? slash + 1 : url, fileNorm, sizeof(fileNorm));

		int scoreTitle = dxma_common_run(wantNorm, titleNorm);
		int scoreFile = dxma_common_run(wantNorm, fileNorm);
		int score = scoreTitle > scoreFile ? scoreTitle : scoreFile;

		if (score > bestScore) { bestScore = score; best = i; }
	}

	// Confidence floor: require most of the filename stem to be accounted
	// for, so e.g. "level1" does not "match" every mission in the database.
	if (best >= 0 && bestScore >= 4 && (size_t)bestScore * 100 >= wantLen * 70)
		return best;
	return -1;
}

// -------------------------------------------------------------------- UI

typedef struct { int start_index, per_page, total, page, pages; } dxma_page_state;
static dxma_page_state PageState = {0};
static char *ListText = NULL;
static int InitialSelection = 2;
static int FilterEnabled = 0;
static char FilterText[DXMA_FILTER_LEN] = {0};
static int FilteredIndices[MAX_DXMA_MISSIONS];
static int FilteredCount = 0;

// DOS scan codes for letters and digits are NOT contiguous, so any
// arithmetic like ('a' + (base_key - KEY_A)) produces wrong characters.
// Use the engine's own scan-code table (key_properties, populated in
// arch/sdl/key.c) which already maps every key to its unshifted ASCII.
static int dxma_keycode_to_ascii(int key)
{
	int shifted = (key & KEY_SHIFTED) != 0;
	int base_key = key & 0xFF;
	if (base_key <= 0) return 255;

	unsigned char raw = key_properties[base_key].ascii_value;
	if (raw == 255) return 255;

	if (!shifted) return raw;

	if (raw >= 'a' && raw <= 'z') return raw - 'a' + 'A';
	switch (raw)
	{
	case '1': return '!';
	case '2': return '@';
	case '3': return '#';
	case '4': return '$';
	case '5': return '%';
	case '6': return '^';
	case '7': return '&';
	case '8': return '*';
	case '9': return '(';
	case '0': return ')';
	case '-': return '_';
	case '=': return '+';
	case '[': return '{';
	case ']': return '}';
	case ';': return ':';
	case '\'': return '"';
	case '`': return '~';
	case ',': return '<';
	case '.': return '>';
	case '/': return '?';
	case '\\': return '|';
	default:  return raw;
	}
}

static int dxma_matches_filter(const dxma_mission *m, const char *filter)
{
	if (!filter[0])
		return 1;
	char needle[DXMA_FILTER_LEN];
	char hay_title[256];
	char hay_author[128];
	dxma_normalize(filter, needle, sizeof(needle));
	dxma_normalize(m->title, hay_title, sizeof(hay_title));
	dxma_normalize(m->author, hay_author, sizeof(hay_author));
	return strstr(hay_title, needle) != NULL || strstr(hay_author, needle) != NULL;
}

static void dxma_build_filtered_indices(void)
{
	FilteredCount = 0;
	for (int i = 0; i < MissionCount && FilteredCount < MAX_DXMA_MISSIONS; i++)
		if (!FilterEnabled || dxma_matches_filter(&Missions[i], FilterText))
			FilteredIndices[FilteredCount++] = i;
}

static int dxma_digits(int value)
{
	int d = 1;
	while (value >= 10)
	{
		value /= 10;
		d++;
	}
	return d;
}

static int dxma_measure_text_width(const char *text);  // defined below

// Append spaces to s until its rendered pixel width reaches target_col_px.
// Gives true pixel-accurate column stops in proportional fonts.
static void dxma_pad_to_col(char *s, size_t sz, int target_col_px)
{
	int sp_w = dxma_measure_text_width(" ");
	if (sp_w <= 0) sp_w = 1;
	size_t n = strlen(s);
	int cur = dxma_measure_text_width(s);
	while (cur < target_col_px && n + 1 < sz)
	{
		s[n++] = ' ';
		s[n] = '\0';
		cur += sp_w;
	}
}

// Append src to dst, truncating with "..." if the total rendered width
// of dst would exceed max_total_px.
static void dxma_append_truncated(char *dst, size_t dstsz, const char *src, int max_total_px)
{
	size_t base_len = strlen(dst);
	int cur_w = dxma_measure_text_width(dst);
	int budget = max_total_px - cur_w;
	if (budget <= 0) return;

	if (dxma_measure_text_width(src) <= budget)
	{
		strncat(dst, src, dstsz - base_len - 1);
		return;
	}

	int ellipsis_w = dxma_measure_text_width("...");
	int text_budget = budget - ellipsis_w;
	if (text_budget <= 0)
	{
		if (budget >= ellipsis_w && base_len + 3 < dstsz)
			strcat(dst, "...");
		return;
	}

	// Binary search for the longest prefix that fits within text_budget
	size_t src_len = strlen(src);
	size_t lo = 0, hi = src_len;
	char partial[256];
	while (lo < hi)
	{
		size_t mid = (lo + hi + 1) / 2;
		if (mid >= sizeof(partial)) { hi = sizeof(partial) - 1; continue; }
		memcpy(partial, src, mid);
		partial[mid] = '\0';
		if (dxma_measure_text_width(partial) <= text_budget)
			lo = mid;
		else
			hi = mid - 1;
	}
	if (lo > 0 && base_len + lo + 3 < dstsz)
	{
		memcpy(partial, src, lo);
		partial[lo] = '\0';
		strncat(dst, partial, dstsz - base_len - 1);
		strncat(dst, "...", dstsz - strlen(dst) - 1);
	}
}



// Measure a string's pixel width using GAME_FONT on the screen canvas.
// This is what newmenu itself uses to size items, so the value is what
// really determines whether the mission browser box fits on screen.
static int dxma_measure_text_width(const char *text)
{
	int w = 0, h = 0, aw = 0;
	grs_canvas *save = grd_curcanv;
	grs_font *save_font;
	gr_set_current_canvas(NULL);
	save_font = grd_curcanv->cv_font;
	gr_set_curfont(GAME_FONT);
	gr_get_string_size(text, &w, &h, &aw);
	gr_set_curfont(save_font);
	gr_set_current_canvas(save);
	return w;
}

// Append trailing spaces to s until its rendered pixel width is at least
// target_px. Used so every row in the mission browser has the same width,
// which stops newmenu from resizing its frame as pages / filter change.
static void dxma_pad_row_to_pixels(char *s, size_t sz, int target_px)
{
	int sp_w = dxma_measure_text_width(" ");
	if (sp_w <= 0) sp_w = 1;
	size_t n = strlen(s);
	int cur = dxma_measure_text_width(s);
	while (cur < target_px && n + 1 < sz)
	{
		s[n++] = ' ';
		s[n] = '\0';
		cur += sp_w;
	}
}

// Horizontally centre s within target_px by prepending leading spaces,
// then pad trailing spaces so the total rendered width equals target_px.
// This keeps the newmenu box a fixed size while text appears centred.
static void dxma_center_row(char *s, size_t sz, int target_px)
{
	int sp_w = dxma_measure_text_width(" ");
	if (sp_w <= 0) sp_w = 1;
	int w = dxma_measure_text_width(s);
	int lead_px = (target_px - w) / 2;
	if (lead_px > 0)
	{
		int n_lead = lead_px / sp_w;
		size_t orig_len = strlen(s);
		if (n_lead > 0 && (size_t)n_lead + orig_len + 1 < sz)
		{
			memmove(s + n_lead, s, orig_len + 1);
			for (int k = 0; k < n_lead; k++) s[k] = ' ';
		}
	}
	dxma_pad_row_to_pixels(s, sz, target_px);
}

static int dxma_menu_handler(newmenu *menu, d_event *event, void *userdata)
{
	int citem = newmenu_get_citem(menu);
	newmenu_item *items = newmenu_get_items(menu);
	window *menu_window = newmenu_get_window(menu);

	switch (event->type)
	{
	case EVENT_KEY_COMMAND:
	{
		if (window_get_front() != menu_window) return 0;
		int key = event_key_get(event);
		int ascii = key_ascii();
		int row_base = 3;

		if (key == KEY_CTRLED + KEY_F)
		{
			FilterEnabled = !FilterEnabled;
			PageState.page = 0;
			PageState.start_index = 0;
			InitialSelection = row_base;
			window_close(newmenu_get_window(menu));
			dxma_missions_menu();
			return 1;
		}

		if (FilterEnabled)
		{
			int changed = 0;
			int flen = (int)strlen(FilterText);
			if (key == KEY_BACKSP || key == KEY_DELETE)
			{
				if (flen > 0)
				{
					FilterText[flen - 1] = '\0';
					changed = 1;
				}
			}
			else
			{
				int ch = ascii;
				if (ch == 255)
					ch = dxma_keycode_to_ascii(key);
				if (ch >= 32 && ch < 127 && flen + 1 < DXMA_FILTER_LEN)
				{
					FilterText[flen] = (char)ch;
					FilterText[flen + 1] = '\0';
					changed = 1;
				}
			}
			if (changed)
			{
				PageState.page = 0;
				PageState.start_index = 0;
				InitialSelection = row_base;
				window_close(newmenu_get_window(menu));
				dxma_missions_menu();
				return 1;
			}
		}

		switch (key)
		{
		case KEY_LEFT: case KEY_PAGEUP: case KEY_PAD4:
			if (PageState.page > 0)
			{
				PageState.page--;
				PageState.start_index = PageState.page * PageState.per_page;
				InitialSelection = row_base;
				digi_play_sample(Weapon_info[9].flash_sound, F1_0);
				window_close(newmenu_get_window(menu));
				dxma_missions_menu();
				return 1;
			}
			break;
		case KEY_RIGHT: case KEY_PAGEDOWN: case KEY_PAD6:
			if (PageState.page < PageState.pages - 1)
			{
				PageState.page++;
				PageState.start_index = PageState.page * PageState.per_page;
				InitialSelection = row_base;
				digi_play_sample(Weapon_info[9].flash_sound, F1_0);
				window_close(newmenu_get_window(menu));
				dxma_missions_menu();
				return 1;
			}
			break;
		case KEY_CTRLED + KEY_R:
			window_close(newmenu_get_window(menu));
			if (dxma_refresh())
				nm_messagebox(NULL, 1, "OK", "Mission database refreshed: %d missions.", dxma_count());
			else
				nm_messagebox(NULL, 1, "OK", "Refresh failed (no network, or DXMA unreachable).\nUsing the existing database.");
			dxma_missions_menu();
			return 1;
		}

		if (!FilterEnabled && ascii >= 32 && ascii < 255 &&
			!(citem >= 0 && citem < newmenu_get_nitems(menu) &&
			  (items[citem].type == NM_TYPE_INPUT || items[citem].type == NM_TYPE_INPUT_MENU)))
		{
			int ch = toupper(ascii);
			int first = -1;
			for (int i = 0; i < FilteredCount; i++)
				if (toupper((unsigned char)Missions[FilteredIndices[i]].title[0]) == ch) { first = i; break; }
			if (first > 0)
			{
				PageState.page = first / PageState.per_page;
				PageState.start_index = PageState.page * PageState.per_page;
				InitialSelection = row_base + (first % PageState.per_page);
				window_close(newmenu_get_window(menu));
				dxma_missions_menu();
				return 1;
			}
			return 1;
		}
		break;
	}

	case EVENT_NEWMENU_SELECTED:
	{
		int row_base = 3;
		if (citem < row_base || items[citem].type != NM_TYPE_MENU) return 1;
		int filtered_idx = (citem - row_base) + PageState.start_index;
		if (filtered_idx < 0 || filtered_idx >= FilteredCount) return 1;
		int idx = FilteredIndices[filtered_idx];
		dxma_download_mission(idx);
		return 1;
	}

	case EVENT_WINDOW_CLOSE:
		if (ListText) { d_free(ListText); ListText = NULL; }
		break;

	default: break;
	}
	return 0;
}

void dxma_missions_menu(void)
{
	if (MissionCount == 0) dxma_load();

	if (MissionCount == 0)
	{
		nm_messagebox(NULL, 1, "OK", "No missions available.\nTry refreshing (Ctrl+R) once you have a network connection.");
		return;
	}

	dxma_build_filtered_indices();

	const int per_page = 60;
	if (PageState.total != FilteredCount)
	{
		PageState.per_page = per_page;
		PageState.total = FilteredCount;
		PageState.pages = (FilteredCount + per_page - 1) / per_page;
		if (PageState.pages < 1) PageState.pages = 1;
		if (PageState.page >= PageState.pages) PageState.page = 0;
		PageState.start_index = PageState.page * per_page;
		InitialSelection = 3;
	}

	int on_page = per_page;
	if (PageState.start_index + on_page > FilteredCount)
		on_page = FilteredCount - PageState.start_index;
	if (on_page < 0) on_page = 0;

	int n_items = 3 + per_page;
	newmenu_item *m;
	MALLOC(m, newmenu_item, n_items);
	if (!m) return;
	MALLOC(ListText, char, n_items * DXMA_ROW_TEXT_LEN);
	if (!ListText) { d_free(m); return; }
	memset(m, 0, sizeof(newmenu_item) * n_items);

	// Target ~60 % of screen width. All rows are padded out to exactly this
	// width so the box never resizes between pages/filter states.
	int row_max_px = SWIDTH * 60 / 100;
	if (row_max_px < 200) row_max_px = 200;
	if (row_max_px > SWIDTH - BORDERX * 2 - FSPACX(4))
		row_max_px = SWIDTH - BORDERX * 2 - FSPACX(4);

	m[0].text = ListText; m[0].type = NM_TYPE_TEXT;
	snprintf(m[0].text, DXMA_ROW_TEXT_LEN,
		"Page %d/%d   Ctrl+R refresh   Ctrl+F %s",
		PageState.page + 1, PageState.pages,
		FilterEnabled ? "hide filter" : "show filter");
	dxma_center_row(m[0].text, DXMA_ROW_TEXT_LEN, row_max_px);

	// Centre the visible text before wrapping it in CTF-style color escapes.
	// This keeps control bytes out of the width calculation.
	m[1].text = ListText + DXMA_ROW_TEXT_LEN; m[1].type = NM_TYPE_TEXT;
	{
		char filter_text[DXMA_ROW_TEXT_LEN - 4];
		snprintf(filter_text, sizeof(filter_text), "Filter: %s%s%s",
			FilterText[0] ? FilterText : "(none)",
			FilterEnabled ? "" : " (off)",
			(FilterEnabled && FilterText[0] && FilteredCount == 0) ? " [no matches]" : "");
		dxma_center_row(filter_text, sizeof(filter_text), row_max_px);
		snprintf(m[1].text, DXMA_ROW_TEXT_LEN, "%s%s\x01\x99",
		FilterEnabled ? "\x01\x56" : "\x01\xC0",
		filter_text);
	}

	// Tab stops are defined by newmenu and shared by every row, so each
	// column starts at an identical screen x coordinate.
	m[2].text = ListText + DXMA_ROW_TEXT_LEN * 2; m[2].type = NM_TYPE_TEXT;
	snprintf(m[2].text, DXMA_ROW_TEXT_LEN, "#\tTITLE\t\tMODE\t\tAUTHOR");

	for (int i = 0; i < per_page; i++)
	{
		m[i + 3].text = ListText + DXMA_ROW_TEXT_LEN * (i + 3);
		if (i < on_page)
		{
			int fidx = PageState.start_index + i;
			int idx = FilteredIndices[fidx];
			char idxbuf[16];
			char rowbuf[DXMA_ROW_TEXT_LEN];

			snprintf(idxbuf, sizeof(idxbuf), "%d.", fidx + 1);

			// newmenu's tab stops place TITLE, MODE, and AUTHOR identically
			// on every row, independent of the preceding field's width.
			char title[96] = "", mode[32] = "", author[96] = "";
			dxma_append_truncated(title, sizeof(title), Missions[idx].title,
				FSPACX(127) - FSPACX(18) - FSPACX(2));
			dxma_append_truncated(mode, sizeof(mode), Missions[idx].mode,
				FSPACX(231) - FSPACX(127) - FSPACX(2));
			dxma_append_truncated(author, sizeof(author), Missions[idx].author,
				row_max_px - FSPACX(231) - FSPACX(2));
			snprintf(rowbuf, sizeof(rowbuf), "%s\t%s\t\t%s\t\t%s", idxbuf, title, mode, author);

			m[i + 3].type = NM_TYPE_MENU;
			snprintf(m[i + 3].text, DXMA_ROW_TEXT_LEN, "%s", rowbuf);
		}
		else
		{
			m[i + 3].type = NM_TYPE_TEXT;
			strcpy(m[i + 3].text, " ");
			dxma_pad_row_to_pixels(m[i + 3].text, DXMA_ROW_TEXT_LEN, row_max_px);
		}
	}

	if (InitialSelection >= on_page + 3) InitialSelection = 3;
	newmenu_dotiny("DXMA MISSIONS", NULL, n_items, m, 1, dxma_menu_handler, NULL);
	InitialSelection = 3;
}
