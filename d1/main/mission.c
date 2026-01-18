/*
THE COMPUTER CODE CONTAINED HEREIN IS THE SOLE PROPERTY OF PARALLAX
SOFTWARE CORPORATION ("PARALLAX").  PARALLAX, IN DISTRIBUTING THE CODE TO
END-USERS, AND SUBJECT TO ALL OF THE TERMS AND CONDITIONS HEREIN, GRANTS A
ROYALTY-FREE, PERPETUAL LICENSE TO SUCH END-USERS FOR USE BY SUCH END-USERS
IN USING, DISPLAYING,  AND CREATING DERIVATIVE WORKS THEREOF, SO LONG AS
SUCH USE, DISPLAY OR CREATION IS FOR NON-COMMERCIAL, ROYALTY OR REVENUE
FREE PURPOSES.  IN NO EVENT SHALL THE END-USER USE THE COMPUTER CODE
CONTAINED HEREIN FOR REVENUE-BEARING PURPOSES.  THE END-USER UNDERSTANDS
AND AGREES TO THE TERMS HEREIN AND ACCEPTS THE SAME BY USE OF THIS FILE.
COPYRIGHT 1993-1998 PARALLAX SOFTWARE CORPORATION.  ALL RIGHTS RESERVED.
*/

/*
 *
 * Code to handle multiple missions
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#include "pstypes.h"
#include "strutil.h"
#include "inferno.h"
#include "mission.h"
#include "gameseq.h"
#include "titles.h"
#include "songs.h"
#include "dxxerror.h"
#include "config.h"
#include "newmenu.h"
#include "text.h"
#include "u_mem.h"
#include "ignorecase.h"
#include "key.h"
#include "event.h"

//values that describe where a mission is located
enum mle_loc
{
	ML_CURDIR = 0,
	ML_MISSIONDIR = 1
};

//mission list entry
typedef struct mle {
	char    *filename;          // filename without extension
	int     builtin_hogsize;    // if it's the built-in mission, used for determining the version
	char    mission_name[MISSION_NAME_LEN+1];
	ubyte   anarchy_only_flag;  // if true, mission is anarchy only
	char	*path;				// relative file path
	enum mle_loc	location;           // where the mission is
} mle;

static int num_missions = -1;

Mission *Current_mission = NULL; // currently loaded mission

// Allocate the Level_names, Secret_level_names and Secret_level_table arrays
static int allocate_levels(void)
{
	MALLOC(Level_names, d_fname, Last_level);
	if (!Level_names)
		return 0;
	
	if (Last_secret_level)
	{
		N_secret_levels = -Last_secret_level;

		MALLOC(Secret_level_names, d_fname, N_secret_levels);
		if (!Secret_level_names)
			return 0;
		
		MALLOC(Secret_level_table, ubyte, N_secret_levels);
		if (!Secret_level_table)
			return 0;
	}
	
	return 1;
}

//
//  Special versions of mission routines for d1 builtins
//

int load_mission_d1(void)
{
	int i;

	switch (PHYSFSX_fsize("descent.hog"))
	{
		case D1_SHAREWARE_MISSION_HOGSIZE:
		case D1_SHAREWARE_10_MISSION_HOGSIZE:
			N_secret_levels = 0;
	
			Last_level = 7;
			Last_secret_level = 0;
			
			if (!allocate_levels())
			{
				free_mission();
				return 0;
			}
	
			//build level names
			for (i=0;i<Last_level;i++)
				sprintf(Level_names[i], "level%02d.sdl", i+1);

			strcpy(Briefing_text_filename,BIMD1_BRIEFING_FILE);
			strcpy(Ending_text_filename,BIMD1_ENDING_FILE_SHARE);
	
			break;
		case D1_MAC_SHARE_MISSION_HOGSIZE:
			N_secret_levels = 0;
	
			Last_level = 3;
			Last_secret_level = 0;
	
			if (!allocate_levels())
			{
				free_mission();
				return 0;
			}
			
			//build level names
			for (i=0;i<Last_level;i++)
				sprintf(Level_names[i], "level%02d.sdl", i+1);

			strcpy(Briefing_text_filename,BIMD1_BRIEFING_FILE);
			strcpy(Ending_text_filename,BIMD1_ENDING_FILE_SHARE);

			break;
		case D1_OEM_MISSION_HOGSIZE:
		case D1_OEM_10_MISSION_HOGSIZE:
			N_secret_levels = 1;
	
			Last_level = 15;
			Last_secret_level = -1;
	
			if (!allocate_levels())
			{
				free_mission();
				return 0;
			}
			
			//build level names
			for (i=0; i < Last_level - 1; i++)
				sprintf(Level_names[i], "level%02d.rdl", i+1);
			sprintf(Level_names[i], "saturn%02d.rdl", i+1);
			for (i=0; i < -Last_secret_level; i++)
				sprintf(Secret_level_names[i], "levels%1d.rdl", i+1);
	
			Secret_level_table[0] = 10;

			strcpy(Briefing_text_filename,BIMD1_BRIEFING_FILE_OEM);
			strcpy(Ending_text_filename,BIMD1_ENDING_FILE_OEM);

			break;
		default:
			Int3(); // fall through
		case D1_MISSION_HOGSIZE:
		case D1_10_MISSION_HOGSIZE:
		case D1_MAC_MISSION_HOGSIZE:
			N_secret_levels = 3;
	
			Last_level = BIMD1_LAST_LEVEL;
			Last_secret_level = BIMD1_LAST_SECRET_LEVEL;
	
			if (!allocate_levels())
			{
				free_mission();
				return 0;
			}

			//build level names
			for (i=0;i<Last_level;i++)
				sprintf(Level_names[i], "level%02d.rdl", i+1);
			for (i=0;i<-Last_secret_level;i++)
				sprintf(Secret_level_names[i], "levels%1d.rdl", i+1);
	
			Secret_level_table[0] = 10;
			Secret_level_table[1] = 21;
			Secret_level_table[2] = 24;

			strcpy(Briefing_text_filename,BIMD1_BRIEFING_FILE);
			strcpy(Ending_text_filename,BIMD1_ENDING_FILE);

			break;
	}

	return 1;
}

//compare a string for a token. returns true if match
int istok(char *buf,char *tok)
{
	return d_strnicmp(buf,tok,strlen(tok)) == 0;

}

//adds a terminating 0 after a string at the first white space
void add_term(char *s)
{
	while (*s && !isspace(*s)) s++;

	*s = 0;		//terminate!
}

//returns ptr to string after '=' & white space, or NULL if no '='
//adds 0 after parm at first white space
char *get_value(char *buf)
{
	char *t;

	t = strchr(buf,'=')+1;

	if (t) {
		while (*t && isspace(*t)) t++;

		if (*t)
			return t;
	}

	return NULL;		//error!
}

//reads a line, returns ptr to value of passed parm.  returns NULL if none
char *get_parm_value(char *parm,PHYSFS_file *f)
{
	static char buf[80];

	if (!PHYSFSX_fgets(buf,80,f))
		return NULL;

	if (istok(buf,parm))
		return get_value(buf);
	else
		return NULL;
}

int ml_sort_func(mle *e0,mle *e1)
{
	return d_stricmp(e0->mission_name,e1->mission_name);

}

//returns 1 if file read ok, else 0
int read_mission_file(mle *mission, char *filename, int location)
{
	char filename2[100];
	PHYSFS_file *mfile;

	switch (location) {
		case ML_MISSIONDIR:
			strcpy(filename2,MISSION_DIR);
			break;

		default:
			Int3();		//fall through

		case ML_CURDIR:
			strcpy(filename2,"");
			break;
	}
	strcat(filename2,filename);

	mfile = PHYSFSX_openReadBuffered(filename2);

	if (mfile) {
		char *p;
		char temp[PATH_MAX], *ext;

		strcpy(temp,filename);
		p = strrchr(temp, '/');	// get the filename at the end of the path
		if (!p)
			p = temp;
		else p++;
		
		if ((ext = strchr(p, '.')) == NULL)
			return 0;	//missing extension

		*ext = 0;			//kill extension

		mission->path = d_strdup(temp);
		mission->anarchy_only_flag = 0;
		mission->filename = mission->path + (p - temp);
		mission->location = location;

		p = get_parm_value("name",mfile);

		if (p) {
			char *t;
			if ((t=strchr(p,';'))!=NULL)
				*t=0;
			t = p + strlen(p)-1;
			while (isspace(*t))
				*t-- = 0; // remove trailing whitespace
			if (strlen(p) > MISSION_NAME_LEN)
				p[MISSION_NAME_LEN] = 0;
			strncpy(mission->mission_name, p, MISSION_NAME_LEN + 1);
		}
		else {
			PHYSFS_close(mfile);
			d_free(mission->path);
			return 0;
		}

		p = get_parm_value("type",mfile);

		//get mission type
		if (p)
			mission->anarchy_only_flag = istok(p,"anarchy");

		PHYSFS_close(mfile);

		return 1;
	}

	return 0;
}

void add_d1_builtin_mission_to_list(mle *mission)
{
    int size;
    
	size = PHYSFSX_fsize("descent.hog");
	if (size == -1)
		return;

	switch (size) {
	case D1_SHAREWARE_MISSION_HOGSIZE:
	case D1_SHAREWARE_10_MISSION_HOGSIZE:
	case D1_MAC_SHARE_MISSION_HOGSIZE:
		mission->filename = d_strdup(D1_MISSION_FILENAME);
		strcpy(mission->mission_name, D1_SHAREWARE_MISSION_NAME);
		mission->anarchy_only_flag = 0;
		break;
	case D1_OEM_MISSION_HOGSIZE:
	case D1_OEM_10_MISSION_HOGSIZE:
		mission->filename = d_strdup(D1_MISSION_FILENAME);
		strcpy(mission->mission_name, D1_OEM_MISSION_NAME);
		mission->anarchy_only_flag = 0;
		break;
	default:
		//Warning("Unknown D1 hogsize %d\n", size);
		con_printf(CON_URGENT, "Unknown D1 hogsize %d\n", size);
		Int3();
		// fall through
	case D1_MISSION_HOGSIZE:
	case D1_MISSION_HOGSIZE2:
	case D1_10_MISSION_HOGSIZE:
	case D1_MAC_MISSION_HOGSIZE:
		mission->filename = d_strdup(D1_MISSION_FILENAME);
		strcpy(mission->mission_name, D1_MISSION_NAME);
		mission->anarchy_only_flag = 0;
		break;
	}

	mission->anarchy_only_flag = 0;
	mission->builtin_hogsize = size;
	mission->path = mission->filename;
	num_missions++;
}

void add_missions_to_list(mle *mission_list, char *path, char *rel_path, int anarchy_mode)
{
	char **find, **i, *ext;

	find = PHYSFS_enumerateFiles(path);

	for (i = find; *i != NULL; i++)
	{
		if (strlen(path) + strlen(*i) + 1 >= PATH_MAX)
			continue;	// path is too long

		strcat(rel_path, *i);
		if (PHYSFS_isDirectory(path))
		{
			strcat(rel_path, "/");
			add_missions_to_list(mission_list, path, rel_path, anarchy_mode);
			*(strrchr(path, '/')) = 0;
		}
		else if ((ext = strrchr(*i, '.')) && (!d_strnicmp(ext, ".msn", 4) || !d_strnicmp(ext, ".mn2", 4)))
			if (read_mission_file(&mission_list[num_missions], rel_path, ML_MISSIONDIR))
			{
				if (anarchy_mode || !mission_list[num_missions].anarchy_only_flag)
				{
					mission_list[num_missions].builtin_hogsize = 0;
					num_missions++;
				}
				else
					d_free(mission_list[num_missions].path);
			}
		
		if (num_missions >= MAX_MISSIONS)
		{
			break;
		}

		(strrchr(path, '/'))[1] = 0;	// chop off the entry
	}

	PHYSFS_freeList(find);
}

/* move <mission_name> to <place> on mission list, increment <place> */
void promote (mle *mission_list, char * mission_name, int * top_place)
{
	int i;
	char name[FILENAME_LEN], * t;
	strcpy(name, mission_name);
	if ((t = strchr(name,'.')) != NULL)
		*t = 0; //kill extension
	for (i = *top_place; i < num_missions; i++)
		if (!d_stricmp(mission_list[i].filename, name)) {
			//swap mission positions
			mle temp;

			temp = mission_list[*top_place];
			mission_list[*top_place] = mission_list[i];
			mission_list[i] = temp;
			++(*top_place);
			break;
		}
}

void free_mission(void)
{
    // May become more complex with the editor
    if (Current_mission)
	{
		if (Current_mission->path && !PLAYING_BUILTIN_MISSION)
		{
			char hogpath[PATH_MAX];

			sprintf(hogpath, MISSION_DIR "%s.hog", Current_mission->path);
			PHYSFSX_contfile_close(hogpath);
		}

		if (Current_mission->path)
			d_free(Current_mission->path);

		if (Level_names)
			d_free(Level_names);
		if(Secret_level_names)
			d_free(Secret_level_names);
		if(Secret_level_table)
			d_free(Secret_level_table);
		
        d_free(Current_mission);
    }
}



//fills in the global list of missions.  Returns the number of missions
//in the list.  If anarchy_mode is set, then also add anarchy-only missions.

mle *build_mission_list(int anarchy_mode)
{
	mle *mission_list;
	int top_place;
	char	search_str[PATH_MAX] = MISSION_DIR;

	//now search for levels on disk

//@@Took out this code because after this routine was called once for
//@@a list of single-player missions, a subsequent call for a list of
//@@anarchy missions would not scan again, and thus would not find the
//@@anarchy-only missions.  If we retain the minimum level of install,
//@@we may want to put the code back in, having it always scan for all
//@@missions, and have the code that uses it sort out the ones it wants.
//@@	if (num_missions != -1) {
//@@		if (Current_mission_num != 0)
//@@			load_mission(0);				//set built-in mission as default
//@@		return num_missions;
//@@	}

	MALLOC(mission_list, mle, MAX_MISSIONS);
	num_missions = 0;
	
	add_d1_builtin_mission_to_list(mission_list + num_missions);
	add_missions_to_list(mission_list, search_str, search_str + strlen(search_str), anarchy_mode);
	
	// move original missions (in story-chronological order)
	// to top of mission list
	top_place = 0;
	promote(mission_list, "", &top_place); // original descent 1 mission

	if (num_missions > top_place)
		qsort(&mission_list[top_place],
		      num_missions - top_place,
		      sizeof(*mission_list),
 				(int (*)( const void *, const void * ))ml_sort_func);


	if (num_missions > top_place)
		qsort(&mission_list[top_place],
		      num_missions - top_place,
		      sizeof(*mission_list),
		      (int (*)( const void *, const void * ))ml_sort_func);

	return mission_list;
}

void free_mission_list(mle *mission_list)
{
	int i;

	for (i = 0; i < num_missions; i++)
		d_free(mission_list[i].path);
	
	d_free(mission_list);
	num_missions = 0;
}

void init_extra_robot_movie(char *filename);

//values for built-in mission

//loads the specfied mission from the mission list.
//build_mission_list() must have been called.
//Returns true if mission loaded ok, else false.
int load_mission(mle *mission)
{
	PHYSFS_file *mfile;
	char buf[PATH_MAX], *v;

	if (Current_mission)
		free_mission();
	Current_mission = d_malloc(sizeof(Mission));
	if (!Current_mission) return 0;
	*(mle *) Current_mission = *mission;
	Current_mission->path = d_strdup(mission->path);
	Current_mission->filename = Current_mission->path + (mission->filename - mission->path);
	Current_mission->n_secret_levels = 0;

	//init vars
	Last_level = 0;
	Last_secret_level = 0;
	memset(&Briefing_text_filename, '\0', sizeof(Briefing_text_filename));
	memset(&Ending_text_filename, '\0', sizeof(Ending_text_filename));
	Secret_level_table = NULL;
	Level_names = NULL;
	Secret_level_names = NULL;

	// for Descent 1 missions, load descent.hog
	if (!PHYSFSX_contfile_init("descent.hog", 1))
		Error("descent.hog not available!\n");
	if (!d_stricmp(Current_mission_filename, D1_MISSION_FILENAME))
		return load_mission_d1();

	//read mission from file

	switch (mission->location) {
	case ML_MISSIONDIR:
		strcpy(buf,MISSION_DIR);
		break;
	default:
		Int3();							//fall through
	case ML_CURDIR:
		strcpy(buf,"");
		break;
	}
	strcat(buf, mission->path);
	strcat(buf,".msn");

	PHYSFSEXT_locateCorrectCase(buf);

	mfile = PHYSFSX_openReadBuffered(buf);
	if (mfile == NULL) {
		free_mission();
		return 0;		//error!
	}

	//for non-builtin missions, load HOG
	strcpy(buf+strlen(buf)-4,".hog");		//change extension
	PHYSFSEXT_locateCorrectCase(buf);
	if (PHYSFSX_exists(buf,1))
		PHYSFSX_contfile_init(buf, 0);

	snprintf(Briefing_text_filename, sizeof(Briefing_text_filename), "%s.tex",Current_mission_filename);
	if (!PHYSFSX_exists(Briefing_text_filename,1))
		snprintf(Briefing_text_filename, sizeof(Briefing_text_filename), "%s.txb",Current_mission_filename);
	snprintf(Ending_text_filename, sizeof(Ending_text_filename), "%s.tex",Current_mission_filename);
	if (!PHYSFSX_exists(Ending_text_filename,1))
		snprintf(Ending_text_filename, sizeof(Ending_text_filename), "%s.txb",Current_mission_filename);

	while (PHYSFSX_fgets(buf,sizeof(buf),mfile)) {
		if (istok(buf,"type"))
			continue;						//already have name, go to next line
		else if (istok(buf,"briefing")) {
			if ((v = get_value(buf)) != NULL) {
				add_term(v);
				if (strlen(v) < FILENAME_LEN && strlen(v) > 0)
				{
					char *tmp, *ptr;
					MALLOC(tmp, char, FILENAME_LEN);
					snprintf(tmp, FILENAME_LEN, "%s", v);
					if ((ptr = strrchr(tmp, '.'))) // if there's a filename extension, kill it. No one knows it's the right one.
						*ptr = '\0';
					strncat(tmp, ".tex", sizeof(char)*FILENAME_LEN); // apply tex-extenstion
					if (PHYSFSX_exists(tmp,1)) // check if this file exists ...
						snprintf(Briefing_text_filename, FILENAME_LEN, "%s", tmp); // ... and apply ...
					else // ... otherwise ...
					{
						if ((ptr = strrchr(tmp, '.')))
							*ptr = '\0';
						strncat(tmp, ".txb", sizeof(char)*FILENAME_LEN); // apply txb extension
						if (PHYSFSX_exists(tmp,1)) // check if this file exists ...
							snprintf(Briefing_text_filename, FILENAME_LEN, "%s", tmp); // ... and apply ...
					}
					d_free(tmp);
				}
			}
		}
		else if (istok(buf,"ending")) {
			if ((v = get_value(buf)) != NULL) {
				add_term(v);
				if (strlen(v) < FILENAME_LEN && strlen(v) > 0)
				{
					char *tmp, *ptr;
					MALLOC(tmp, char, FILENAME_LEN);
					snprintf(tmp, FILENAME_LEN, "%s", v);
					if ((ptr = strrchr(tmp, '.'))) // if there's a filename extension, kill it. No one knows it's the right one.
						*ptr = '\0';
					strncat(tmp, ".tex", sizeof(char)*FILENAME_LEN); // apply tex-extenstion
					if (PHYSFSX_exists(tmp,1)) // check if this file exists ...
						snprintf(Briefing_text_filename, FILENAME_LEN, "%s", tmp); // ... and apply ...
					else // ... otherwise ...
					{
						if ((ptr = strrchr(tmp, '.')))
							*ptr = '\0';
						strncat(tmp, ".txb", sizeof(char)*FILENAME_LEN); // apply txb extension
						if (PHYSFSX_exists(tmp,1)) // check if this file exists ...
							snprintf(Ending_text_filename, FILENAME_LEN, "%s", tmp); // ... and apply ...
					}
					d_free(tmp);
				}
			}
		}
		else if (istok(buf,"num_levels")) {

			if ((v=get_value(buf))!=NULL) {
				int n_levels,i;

				n_levels = atoi(v);
				
				Assert(n_levels <= MAX_LEVELS_PER_MISSION);
				n_levels = min(n_levels, MAX_LEVELS_PER_MISSION);
				
				MALLOC(Level_names, d_fname, n_levels);
				if (!Level_names)
				{
					free_mission();
					return 0;
				}

				for (i=0;i<n_levels;i++) {
					PHYSFSX_fgets(buf,sizeof(buf),mfile);
					add_term(buf);
					if (strlen(buf) <= 12) {
						strcpy(Level_names[i],buf);
						Last_level++;
					}
					else
						break;
				}

			}
		}
		else if (istok(buf,"num_secrets")) {
			if ((v=get_value(buf))!=NULL) {
				int i;

				N_secret_levels = atoi(v);

				Assert(N_secret_levels <= MAX_SECRET_LEVELS_PER_MISSION);
				N_secret_levels = min(N_secret_levels, MAX_SECRET_LEVELS_PER_MISSION);

				MALLOC(Secret_level_names, d_fname, N_secret_levels);
				if (!Secret_level_names)
				{
					free_mission();
					return 0;
				}
				
				MALLOC(Secret_level_table, ubyte, N_secret_levels);
				if (!Secret_level_table)
				{
					free_mission();
					return 0;
				}
				
				for (i=0;i<N_secret_levels;i++) {
					char *t;

					PHYSFSX_fgets(buf,sizeof(buf),mfile);
					if ((t=strchr(buf,','))!=NULL) *t++=0;
					else
						break;

					add_term(buf);
					if (strlen(buf) <= 12) {
						strcpy(Secret_level_names[i],buf);
						Secret_level_table[i] = atoi(t);
						if (Secret_level_table[i]<1 || Secret_level_table[i]>Last_level)
							break;
						Last_secret_level--;
					}
					else
						break;
				}

			}
		}

	}

	PHYSFS_close(mfile);

	if (Last_level <= 0) {
		free_mission();		//no valid mission loaded
		return 0;
	}

	return 1;
}

//loads the named mission if exists.
//Returns true if mission loaded ok, else false.
int load_mission_by_name(char *mission_name)
{
	int i;
	mle *mission_list = build_mission_list(1);
	bool found = 0;

	for (i = 0; i < num_missions; i++)
		if (!d_stricmp(mission_name, mission_list[i].filename))
			found = load_mission(mission_list + i);

	free_mission_list(mission_list);
	return found;
}

typedef struct mission_menu
{
	mle *mission_list;
	int (*when_selected)(void);
} mission_menu;

// Search filter for mission selection
static char mission_search_filter[32] = "";

// Helper function to check if mission name matches search filter (case-insensitive)
// Helper function to convert key code to ASCII for alphanumeric keys
static int keycode_to_ascii(int key)
{
	// Numbers - note KEY_0 is 0x0B, not at the end of sequence
	if (key == KEY_1) return '1';
	if (key == KEY_2) return '2';
	if (key == KEY_3) return '3';
	if (key == KEY_4) return '4';
	if (key == KEY_5) return '5';
	if (key == KEY_6) return '6';
	if (key == KEY_7) return '7';
	if (key == KEY_8) return '8';
	if (key == KEY_9) return '9';
	if (key == KEY_0) return '0';
	
	// Letters - NOT sequential, must map individually
	if (key == KEY_A) return 'a';
	if (key == KEY_B) return 'b';
	if (key == KEY_C) return 'c';
	if (key == KEY_D) return 'd';
	if (key == KEY_E) return 'e';
	if (key == KEY_F) return 'f';
	if (key == KEY_G) return 'g';
	if (key == KEY_H) return 'h';
	if (key == KEY_I) return 'i';
	if (key == KEY_J) return 'j';
	if (key == KEY_K) return 'k';
	if (key == KEY_L) return 'l';
	if (key == KEY_M) return 'm';
	if (key == KEY_N) return 'n';
	if (key == KEY_O) return 'o';
	if (key == KEY_P) return 'p';
	if (key == KEY_Q) return 'q';
	if (key == KEY_R) return 'r';
	if (key == KEY_S) return 's';
	if (key == KEY_T) return 't';
	if (key == KEY_U) return 'u';
	if (key == KEY_V) return 'v';
	if (key == KEY_W) return 'w';
	if (key == KEY_X) return 'x';
	if (key == KEY_Y) return 'y';
	if (key == KEY_Z) return 'z';
	
	// Space
	if (key == KEY_SPACEBAR) return ' ';
	
	// Punctuation
	if (key == KEY_MINUS) return '-';
	if (key == KEY_EQUAL) return '=';
	if (key == KEY_LBRACKET) return '[';
	if (key == KEY_RBRACKET) return ']';
	if (key == KEY_SLASH) return '\\';  // This is backslash
	if (key == KEY_DIVIDE) return '/';  // This is forward slash
	if (key == KEY_COMMA) return ',';
	if (key == KEY_PERIOD) return '.';
	if (key == KEY_SEMICOL) return ';';
	if (key == KEY_RAPOSTRO) return '\'';
	if (key == KEY_LAPOSTRO) return '`';
	
	// Numpad
	if (key == KEY_PAD0) return '0';
	if (key == KEY_PAD1) return '1';
	if (key == KEY_PAD2) return '2';
	if (key == KEY_PAD3) return '3';
	if (key == KEY_PAD4) return '4';
	if (key == KEY_PAD5) return '5';
	if (key == KEY_PAD6) return '6';
	if (key == KEY_PAD7) return '7';
	if (key == KEY_PAD8) return '8';
	if (key == KEY_PAD9) return '9';
	
	return 0; // No ASCII equivalent
}

static int mission_matches_search(const char *mission_name, const char *search)
{
	char mission_lower[MISSION_NAME_LEN + 1];
	char search_lower[32];
	int i;
	
	if (!search || !search[0])
		return 1; // Empty search matches everything
	
	// Convert mission name to lowercase
	for (i = 0; mission_name[i] && i < MISSION_NAME_LEN; i++)
		mission_lower[i] = tolower((unsigned char)mission_name[i]);
	mission_lower[i] = '\0';
	
	// Convert search to lowercase
	for (i = 0; search[i] && i < 31; i++)
		search_lower[i] = tolower((unsigned char)search[i]);
	search_lower[i] = '\0';
	
	return strstr(mission_lower, search_lower) != NULL;
}

// Structure for searchable mission menu
typedef struct searchable_mission_menu
{
	mle *mission_list;        // Full mission list
	int full_count;           // Total mission count
	mle **filtered_list;      // Pointers to matching missions
	int filtered_count;       // Number of matching missions
	char **display_names;     // Array of display name pointers
	char search_text[32];     // Current search text
	char title_base[64];      // Base title text
	char title_buffer[128];   // Dynamic title buffer
	int (*when_selected)(void);
	int anarchy_mode;         // For rebuilding the list
} searchable_mission_menu;

// Forward declaration
static void rebuild_filtered_list(searchable_mission_menu *smm, listbox *lb);

int searchable_mission_handler(listbox *lb, d_event *event, searchable_mission_menu *smm)
{
	int citem = listbox_get_citem(lb);

	switch (event->type)
	{
		case EVENT_KEY_COMMAND:
		{
			int key = event_key_get(event);
			int ascii = keycode_to_ascii(key);
			int len = strlen(smm->search_text);
			
			// Handle backspace - clear one character from search
			if (key == KEY_BACKSP)
			{
				if (len > 0)
				{
					smm->search_text[len - 1] = '\0';
					rebuild_filtered_list(smm, lb);
				}
				return 1;
			}
			// Handle Delete - clear entire search
			else if (key == KEY_DELETE)
			{
				smm->search_text[0] = '\0';
				rebuild_filtered_list(smm, lb);
				return 1;
			}
			// Handle printable characters - add to search
			else if (ascii > 0)
			{
				if (len < 30)
				{
					smm->search_text[len] = ascii;
					smm->search_text[len + 1] = '\0';
					rebuild_filtered_list(smm, lb);
				}
				return 1;
			}
			break;
		}
		
		case EVENT_NEWMENU_SELECTED:
			if (citem >= 0 && citem < smm->filtered_count)
			{
				// Chose a mission from filtered list
				strcpy(GameCfg.LastMission, smm->filtered_list[citem]->mission_name);
				
				if (!load_mission(smm->filtered_list[citem]))
				{
					nm_messagebox(NULL, 1, TXT_OK, TXT_MISSION_ERROR);
					return 1;	// stay in listbox so user can select another one
				}
			}
			return !(*smm->when_selected)();

		case EVENT_WINDOW_CLOSE:
			// Save search filter for next time
			strcpy(mission_search_filter, smm->search_text);
			free_mission_list(smm->mission_list);
			d_free(smm->filtered_list);
			d_free(smm->display_names);
			d_free(smm);
			break;
			
		default:
			break;
	}
	
	return 0;
}

static void rebuild_filtered_list(searchable_mission_menu *smm, listbox *lb)
{
	int i;
	
	// Clear filtered list
	smm->filtered_count = 0;
	
	// Rebuild filtered list based on search text
	for (i = 0; i < smm->full_count; i++)
	{
		if (mission_matches_search(smm->mission_list[i].mission_name, smm->search_text))
		{
			smm->filtered_list[smm->filtered_count] = &smm->mission_list[i];
			smm->display_names[smm->filtered_count] = smm->mission_list[i].mission_name;
			smm->filtered_count++;
		}
	}
	
	// Update listbox using accessor functions
	listbox_set_items(lb, smm->filtered_count, smm->display_names);
	
	// Update title to show search status using struct buffer
	if (smm->search_text[0])
		snprintf(smm->title_buffer, sizeof(smm->title_buffer), "%s\nSearch: %s (%d/%d)", 
			smm->title_base, smm->search_text, smm->filtered_count, smm->full_count);
	else
		snprintf(smm->title_buffer, sizeof(smm->title_buffer), "%s\n(%d missions, type to search)", 
			smm->title_base, smm->full_count);
	listbox_set_title(lb, smm->title_buffer);
}

int mission_menu_handler(listbox *lb, d_event *event, mission_menu *mm)
{
	char **list = listbox_get_items(lb);
	int citem = listbox_get_citem(lb);

	switch (event->type)
	{
		case EVENT_NEWMENU_SELECTED:
			if (citem >= 0)
			{
				// Chose a mission
				strcpy(GameCfg.LastMission, list[citem]);
				
				if (!load_mission(mm->mission_list + citem))
				{
					nm_messagebox( NULL, 1, TXT_OK, TXT_MISSION_ERROR);
					return 1;	// stay in listbox so user can select another one
				}
			}
			return !(*mm->when_selected)();
			break;

		case EVENT_WINDOW_CLOSE:
			free_mission_list(mm->mission_list);
			d_free(list);
			d_free(mm);
			break;
			
		default:
			break;
	}
	
	return 0;
}

int select_mission(int anarchy_mode, char *message, int (*when_selected)(void))
{
    mle *mission_list = build_mission_list(anarchy_mode);
	int new_mission_num;

    if (num_missions <= 1)
	{
        new_mission_num = load_mission(mission_list) ? 0 : -1;
		free_mission_list(mission_list);
		(*when_selected)();
		
		return (new_mission_num >= 0);
    }
	else
	{
		searchable_mission_menu *smm;
		int i, default_mission;
		
		// Allocate the searchable mission menu structure
		MALLOC(smm, searchable_mission_menu, 1);
		if (!smm)
		{
			free_mission_list(mission_list);
			return 0;
		}
		
		MALLOC(smm->filtered_list, mle *, num_missions);
		if (!smm->filtered_list)
		{
			d_free(smm);
			free_mission_list(mission_list);
			return 0;
		}
		
		MALLOC(smm->display_names, char *, num_missions);
		if (!smm->display_names)
		{
			d_free(smm->filtered_list);
			d_free(smm);
			free_mission_list(mission_list);
			return 0;
		}
		
		// Initialize structure
		smm->mission_list = mission_list;
		smm->full_count = num_missions;
		smm->when_selected = when_selected;
		smm->anarchy_mode = anarchy_mode;
		strncpy(smm->title_base, message, 63);
		smm->title_base[63] = '\0';
		
		// Restore previous search filter
		strncpy(smm->search_text, mission_search_filter, 31);
		smm->search_text[31] = '\0';
		
		// Build initial filtered list
		smm->filtered_count = 0;
		default_mission = 0;
		for (i = 0; i < num_missions; i++)
		{
			if (mission_matches_search(mission_list[i].mission_name, smm->search_text))
			{
				smm->filtered_list[smm->filtered_count] = &mission_list[i];
				smm->display_names[smm->filtered_count] = mission_list[i].mission_name;
				if (!d_stricmp(mission_list[i].mission_name, GameCfg.LastMission))
					default_mission = smm->filtered_count;
				smm->filtered_count++;
			}
		}
		
		// Build initial title using struct buffer
		if (smm->search_text[0])
			snprintf(smm->title_buffer, sizeof(smm->title_buffer), "%s\nSearch: %s (%d/%d)", 
				smm->title_base, smm->search_text, smm->filtered_count, smm->full_count);
		else
			snprintf(smm->title_buffer, sizeof(smm->title_buffer), "%s\n(%d missions, type to search)", 
				smm->title_base, smm->full_count);
		
		newmenu_listbox1(smm->title_buffer, smm->filtered_count, smm->display_names, 1, default_mission, 
			(int (*)(listbox *, d_event *, void *))searchable_mission_handler, smm);
    }

    return 1;	// presume success
}

#ifdef EDITOR
void create_new_mission(void)
{
	if (Current_mission)
		free_mission();
	
	Current_mission = d_malloc(sizeof(Mission));
	if (!Current_mission)
		return;
	memset(Current_mission, 0, sizeof(Mission));
	
	Current_mission->path = d_strdup("new_mission");
	if (!Current_mission->path)
	{
		free_mission();
		return;
	}

	Current_mission->filename = Current_mission->path;
	
	MALLOC(Level_names, d_fname, 1);
	if (!Level_names)
	{
		free_mission();
		return;
	}

	strcpy(Level_names[0], "GAMESAVE.LVL");
}
#endif
