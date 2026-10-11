/*
 * SNG: level files for the first-person builder.
 *
 * Writes the builder's level as a plain D1 .RDL (compiled mine version 0,
 * level file version 1, game data version 25) plus a one-level .MSN, both
 * loose in missions/. load_level() falls back to missions/ for level files,
 * so no .HOG is needed -- and none is mounted while we rewrite the file.
 */

#include <string.h>

#include "physfsx.h"
#include "u_mem.h"
#include "inferno.h"
#include "segment.h"
#include "gameseg.h"
#include "object.h"
#include "wall.h"
#include "switch.h"
#include "fuelcen.h"
#include "cntrlcen.h"
#include "polyobj.h"
#include "player.h"
#include "textures.h"
#include "bm.h"
#include "mission.h"
#include "gameseq.h"
#include "gamesave.h"
#include "builder.h"

#define RDL_SIGNATURE		0x504c564c	/* "PLVL" */
#define RDL_LEVEL_VERSION	1
#define RDL_MINE_VERSION	0
#define RDL_GAME_VERSION	25
#define RDL_GAME_SIGNATURE	0x6705
#define GAME_FILEINFO_SIZE	119		/* sizeof(game_fileinfo) on the original DOS build */
#define STARTER_ROOM_SIZE	(40 * F1_0)
#define SIDE_HAS_SPECIAL_BIT	(1 << MAX_SIDES_PER_SEGMENT)

#define LEVEL_PATH	MISSION_DIR BUILDER_LEVEL_FILE
#define MISSION_PATH	MISSION_DIR BUILDER_MISSION ".msn"

extern void write_object(object *obj, PHYSFS_file *f);
extern int control_center_triggers_write(control_center_triggers *cct, PHYSFS_file *fp);

int builder_level_exists(void)
{
	return PHYSFSX_exists(LEVEL_PATH, 1);
}

static short fix_to_short(fix value, int shift)
{
	int scaled = value >> shift;

	if (scaled > 0x7fff)
		return 0x7fff;
	if (scaled < -0x8000)
		return -0x8000;
	return scaled;
}

static ushort fix_to_ushort(fix value, int shift)
{
	int scaled = value < 0 ? 0 : value >> shift;

	return scaled > 0xffff ? 0xffff : scaled;
}

/* Drop vertices no segment references; repeated editing strands a few. */
static void compact_vertices(void)
{
	int *remap;
	int segnum, v, used = 0;

	MALLOC(remap, int, Num_vertices);
	if (!remap)
		return;

	for (v = 0; v < Num_vertices; v++)
		remap[v] = -1;
	for (segnum = 0; segnum < Num_segments; segnum++)
		for (v = 0; v < MAX_VERTICES_PER_SEGMENT; v++)
			remap[Segments[segnum].verts[v]] = 0;

	for (v = 0; v < Num_vertices; v++)
	{
		if (remap[v] < 0)
			continue;
		Vertices[used] = Vertices[v];
		remap[v] = used++;
	}

	for (segnum = 0; segnum < Num_segments; segnum++)
		for (v = 0; v < MAX_VERTICES_PER_SEGMENT; v++)
			Segments[segnum].verts[v] = remap[Segments[segnum].verts[v]];

	Num_vertices = used;
	Highest_vertex_index = used - 1;
	d_free(remap);
}

static ubyte child_mask(segment *seg)
{
	ubyte mask = 0;
	int sidenum;

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		if (seg->children[sidenum] != -1)
			mask |= 1 << sidenum;
	if (seg->special || seg->matcen_num || seg->value)
		mask |= SIDE_HAS_SPECIAL_BIT;
	return mask;
}

static ubyte wall_mask(segment *seg)
{
	ubyte mask = 0;
	int sidenum;

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		if (seg->sides[sidenum].wall_num >= 0)
			mask |= 1 << sidenum;
	return mask;
}

static void write_side_texture(side *sidep, PHYSFS_file *fp)
{
	ushort tmap = sidep->tmap_num;
	int i;

	if (sidep->tmap_num2)
		tmap |= 0x8000;
	PHYSFS_writeSLE16(fp, tmap);
	if (sidep->tmap_num2)
		PHYSFS_writeSLE16(fp, sidep->tmap_num2);

	for (i = 0; i < 4; i++)
	{
		PHYSFS_writeSLE16(fp, fix_to_short(sidep->uvls[i].u, 5));
		PHYSFS_writeSLE16(fp, fix_to_short(sidep->uvls[i].v, 5));
		PHYSFS_writeULE16(fp, fix_to_ushort(sidep->uvls[i].l, 1));
	}
}

static void write_segment(segment *seg, PHYSFS_file *fp)
{
	ubyte mask = child_mask(seg);
	int sidenum, v;

	PHYSFSX_writeU8(fp, mask);
	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		if (mask & (1 << sidenum))
			PHYSFS_writeSLE16(fp, seg->children[sidenum]);
	for (v = 0; v < MAX_VERTICES_PER_SEGMENT; v++)
		PHYSFS_writeSLE16(fp, seg->verts[v]);
	if (mask & SIDE_HAS_SPECIAL_BIT)
	{
		PHYSFSX_writeU8(fp, seg->special);
		PHYSFSX_writeU8(fp, seg->matcen_num);
		PHYSFS_writeSLE16(fp, seg->value);
	}
	PHYSFS_writeULE16(fp, fix_to_ushort(seg->static_light, 4));

	mask = wall_mask(seg);
	PHYSFSX_writeU8(fp, mask);
	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		if (mask & (1 << sidenum))
			PHYSFSX_writeU8(fp, seg->sides[sidenum].wall_num);

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		if (seg->children[sidenum] == -1 || seg->sides[sidenum].wall_num != -1)
			write_side_texture(&seg->sides[sidenum], fp);
}

static void write_mine(PHYSFS_file *fp)
{
	int i;

	PHYSFSX_writeU8(fp, RDL_MINE_VERSION);
	PHYSFS_writeSLE16(fp, Num_vertices);
	PHYSFS_writeSLE16(fp, Num_segments);
	for (i = 0; i < Num_vertices; i++)
		PHYSFSX_writeVector(fp, &Vertices[i]);
	for (i = 0; i < Num_segments; i++)
		write_segment(&Segments[i], fp);
}

static int object_is_saved(object *obj)
{
	switch (obj->type)
	{
		case OBJ_PLAYER:
		case OBJ_GHOST:
		case OBJ_COOP:
		case OBJ_ROBOT:
		case OBJ_HOSTAGE:
		case OBJ_POWERUP:
		case OBJ_CNTRLCEN:
		case OBJ_CLUTTER:
			return 1;
		default:
			return 0;
	}
}

/* A level-file copy of a live object: at rest, and a ghost back to a start. */
static void write_saved_object(object *live, PHYSFS_file *fp)
{
	object copy = *live;

	if (copy.type == OBJ_GHOST)
		copy.type = OBJ_PLAYER;
	if (copy.type == OBJ_PLAYER)
	{
		copy.control_type = CT_FLYING;
		copy.movement_type = MT_PHYSICS;
		copy.render_type = RT_POLYOBJ;
		copy.flags = 0;
	}
	if (copy.movement_type == MT_PHYSICS)
	{
		vm_vec_zero(&copy.mtype.phys_info.velocity);
		vm_vec_zero(&copy.mtype.phys_info.thrust);
		vm_vec_zero(&copy.mtype.phys_info.rotvel);
		vm_vec_zero(&copy.mtype.phys_info.rotthrust);
	}
	copy.last_pos = copy.pos;
	write_object(&copy, fp);
}

/* The console player must be object 0 in the file: InitPlayerObject() and
 * verify_object() both assume it. */
static int write_objects(PHYSFS_file *fp)
{
	int i, count = 0;

	write_saved_object(ConsoleObject, fp);
	count++;
	for (i = 0; i <= Highest_object_index; i++)
	{
		if (&Objects[i] == ConsoleObject || !object_is_saved(&Objects[i]))
			continue;
		write_saved_object(&Objects[i], fp);
		count++;
	}
	return count;
}

static void write_header_entry(PHYSFS_file *fp, int count, int size)
{
	PHYSFS_writeSLE32(fp, -1);
	PHYSFS_writeSLE32(fp, count);
	PHYSFS_writeSLE32(fp, size);
}

static void patch_int(PHYSFS_file *fp, PHYSFS_sint64 where, int value)
{
	PHYSFS_sint64 back = PHYSFS_tell(fp);

	PHYSFS_seek(fp, where);
	PHYSFS_writeSLE32(fp, value);
	PHYSFS_seek(fp, back);
}

/* Layout mirrors save_game_data() / load_game_data() in gamesave.c. Each
 * table header is offset, count, size; offsets and the object count are
 * patched in once known. */
static void write_game_data(PHYSFS_file *fp)
{
	char level_name[LEVEL_NAME_LEN];
	PHYSFS_sint64 table_start, objects_offset;
	int i, n_objects;

	PHYSFS_writeSLE16(fp, RDL_GAME_SIGNATURE);
	PHYSFS_writeSLE16(fp, RDL_GAME_VERSION);
	PHYSFS_writeSLE32(fp, GAME_FILEINFO_SIZE);
	memset(level_name, 0, sizeof(level_name));
	strncpy(level_name, Current_level_name[0] ? Current_level_name : "SNG Builder", sizeof(level_name) - 1);
	PHYSFS_write(fp, level_name, 15, 1);
	PHYSFS_writeSLE32(fp, 1);

	table_start = PHYSFS_tell(fp);
	PHYSFS_writeSLE32(fp, -1);
	PHYSFS_writeSLE32(fp, sizeof(player));
	write_header_entry(fp, 0, sizeof(object));
	write_header_entry(fp, Num_walls, sizeof(wall));
	write_header_entry(fp, 0, sizeof(active_door));
	write_header_entry(fp, Num_triggers, sizeof(trigger));
	write_header_entry(fp, 0, 0);
	write_header_entry(fp, 1, sizeof(control_center_triggers));
	write_header_entry(fp, Num_robot_centers, sizeof(matcen_info));

	/* version 25 reads the mine name NUL-terminated, not newline-terminated */
	PHYSFS_write(fp, level_name, strlen(level_name) + 1, 1);
	PHYSFS_writeSLE16(fp, N_polygon_models);
	PHYSFS_write(fp, Pof_names, sizeof(*Pof_names), N_polygon_models);

	patch_int(fp, table_start, PHYSFS_tell(fp));
	PHYSFS_write(fp, &Players[Player_num], sizeof(player), 1);

	objects_offset = PHYSFS_tell(fp);
	n_objects = write_objects(fp);
	patch_int(fp, table_start + 8, objects_offset);
	patch_int(fp, table_start + 12, n_objects);

	patch_int(fp, table_start + 20, PHYSFS_tell(fp));
	for (i = 0; i < Num_walls; i++)
		wall_write(&Walls[i], RDL_GAME_VERSION, fp);

	patch_int(fp, table_start + 44, PHYSFS_tell(fp));
	for (i = 0; i < Num_triggers; i++)
		trigger_write(&Triggers[i], RDL_GAME_VERSION, fp);

	patch_int(fp, table_start + 68, PHYSFS_tell(fp));
	control_center_triggers_write(&ControlCenterTriggers, fp);

	patch_int(fp, table_start + 80, PHYSFS_tell(fp));
	for (i = 0; i < Num_robot_centers; i++)
		matcen_info_write(&RobotCenters[i], RDL_GAME_VERSION, fp);
}

static int write_level_file(void)
{
	PHYSFS_file *fp;
	PHYSFS_sint64 mine_offset, game_offset;

	PHYSFS_mkdir(MISSION_DIR);
	fp = PHYSFSX_openWriteBuffered(LEVEL_PATH);
	if (!fp)
		return 0;

	PHYSFS_writeSLE32(fp, RDL_SIGNATURE);
	PHYSFS_writeSLE32(fp, RDL_LEVEL_VERSION);
	PHYSFS_writeSLE32(fp, 0);
	PHYSFS_writeSLE32(fp, 0);
	PHYSFS_writeSLE32(fp, 0);

	mine_offset = PHYSFS_tell(fp);
	write_mine(fp);
	game_offset = PHYSFS_tell(fp);
	write_game_data(fp);

	patch_int(fp, 8, mine_offset);
	patch_int(fp, 12, game_offset);
	patch_int(fp, 16, PHYSFS_tell(fp));
	PHYSFS_close(fp);
	return 1;
}

static int write_mission_file(void)
{
	PHYSFS_file *fp;

	PHYSFS_mkdir(MISSION_DIR);
	fp = PHYSFSX_openWriteBuffered(MISSION_PATH);
	if (!fp)
		return 0;
	PHYSFSX_printf(fp, "name = SNG Builder\ntype = normal\nnum_levels = 1\n%s\n", BUILDER_LEVEL_FILE);
	PHYSFS_close(fp);
	return 1;
}

int builder_save_level(void)
{
	compact_vertices();
	return write_level_file() && write_mission_file();
}

/* ---------------------------------------------------------------- starter */

/* A light-emitting texture for the ceiling, so a fresh room isn't dark. */
static int first_light_texture(void)
{
	int t;

	for (t = 0; t < NumTextures; t++)
		if (TmapInfo[t].lighting >= F1_0 / 2)
			return t;
	return 0;
}

static void starter_vertices(void)
{
	static const sbyte corner_sign[MAX_VERTICES_PER_SEGMENT][3] = {
		{ 1,  1, -1}, { 1, -1, -1}, {-1, -1, -1}, {-1,  1, -1},
		{ 1,  1,  1}, { 1, -1,  1}, {-1, -1,  1}, {-1,  1,  1},
	};
	fix half = STARTER_ROOM_SIZE / 2;
	int v;

	for (v = 0; v < MAX_VERTICES_PER_SEGMENT; v++)
	{
		Vertices[v].x = corner_sign[v][0] * half;
		Vertices[v].y = corner_sign[v][1] * half;
		Vertices[v].z = corner_sign[v][2] * half;
		Segments[0].verts[v] = v;
	}
	Num_vertices = MAX_VERTICES_PER_SEGMENT;
	Highest_vertex_index = Num_vertices - 1;
}

static void starter_sides(void)
{
	static const fix corner_u[4] = {0, 0, 2 * F1_0, 2 * F1_0};
	static const fix corner_v[4] = {0, 2 * F1_0, 2 * F1_0, 0};
	segment *seg = &Segments[0];
	int sidenum, k;

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
	{
		side *sidep = &seg->sides[sidenum];

		seg->children[sidenum] = -1;
		sidep->wall_num = -1;
		sidep->tmap_num = 0;
		sidep->tmap_num2 = 0;
		for (k = 0; k < 4; k++)
		{
			sidep->uvls[k].u = corner_u[k];
			sidep->uvls[k].v = corner_v[k];
			sidep->uvls[k].l = F1_0;
		}
	}
	seg->sides[WTOP].tmap_num = first_light_texture();
}

static void starter_player(void)
{
	object *player = &Objects[0];

	memset(player, 0, sizeof(*player));
	player->type = OBJ_PLAYER;
	player->control_type = CT_FLYING;
	player->movement_type = MT_PHYSICS;
	player->render_type = RT_POLYOBJ;
	player->rtype.pobj_info.model_num = Player_ship->model_num;
	player->rtype.pobj_info.tmap_override = -1;
	player->size = Polygon_models[Player_ship->model_num].rad;
	player->shields = 100 * F1_0;
	player->segnum = 0;
	vm_vec_zero(&player->pos);
	player->orient = vmd_identity_matrix;
	player->last_pos = player->pos;
	player->contains_type = -1;
	ConsoleObject = player;
	Highest_object_index = 0;
}

static void starter_reset_tables(void)
{
	memset(&Segments[0], 0, sizeof(Segments[0]));
	Segments[0].objects = -1;
	Segments[0].static_light = F1_0;
	Num_segments = 1;
	Highest_segment_index = 0;
	Num_walls = 0;
	Num_triggers = 0;
	Num_robot_centers = 0;
	memset(&ControlCenterTriggers, 0, sizeof(ControlCenterTriggers));
	strcpy(Current_level_name, "SNG Builder");
}

/* One 40-unit room with a lit ceiling. Built in the global mine arrays;
 * nothing is running yet, and loading the level overwrites them anyway. */
int builder_write_starter_level(void)
{
	starter_reset_tables();
	starter_vertices();
	starter_sides();
	validate_segment(&Segments[0]);
	starter_player();
	return builder_save_level();
}
