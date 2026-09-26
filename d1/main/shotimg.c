#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <physfs.h>

#include "shotimg.h"
#include <png.h>
#include "pngfile.h"
#include "pcx.h"
#include "u_mem.h"
#include "pstypes.h"
#include "gr.h"

#define TGA_HEADER_LEN 18
#define TGA_TYPE_TRUECOLOR 2
// Bit 5 of the descriptor byte: set means the first row stored is the top one.
#define TGA_TOP_ORIGIN 0x20
// A shot is one screen; anything larger is not one of ours.
#define TGA_MAX_PIXELS (8192 * 8192)

// read_png hands back malloc'd memory, so this frees with free(), not d_free.
static ubyte *decode_png(const char *path, int *w, int *h, int *channels)
{
	png_data pdata;
	ubyte *pixels = NULL;
	size_t bytes;

	memset(&pdata, 0, sizeof(pdata));
	if (!read_png(path, &pdata))
		return NULL;
	if (pdata.depth == 8 && (pdata.channels == 3 || pdata.channels == 4))
	{
		bytes = (size_t)pdata.width * pdata.height * pdata.channels;
		pixels = d_malloc(bytes);
		if (pixels)
		{
			memcpy(pixels, pdata.data, bytes);
			*w = (int)pdata.width;
			*h = (int)pdata.height;
			*channels = (int)pdata.channels;
		}
	}
	free(pdata.data);
	free(pdata.palette);
	return pixels;
}

static void tga_unflip(const ubyte *raw, ubyte *rgb, int w, int h, int channels, int top_origin)
{
	int x, y;

	for (y = 0; y < h; y++)
	{
		int src_row = top_origin ? y : h - 1 - y;
		const ubyte *src = raw + (size_t)src_row * w * channels;
		ubyte *dst = rgb + (size_t)y * w * channels;

		for (x = 0; x < w; x++)
		{
			dst[x * channels + 0] = src[x * channels + 2];
			dst[x * channels + 1] = src[x * channels + 1];
			dst[x * channels + 2] = src[x * channels + 0];
			if (channels == 4)
				dst[x * channels + 3] = src[x * channels + 3];
		}
	}
}

// The uncompressed BGR(A) TGA that write_bmp() emits without libpng.
static ubyte *decode_tga(const char *path, int *out_w, int *out_h, int *out_channels)
{
	PHYSFS_File *fp = PHYSFS_openRead(path);
	ubyte header[TGA_HEADER_LEN];
	ubyte *raw = NULL, *rgb = NULL;
	int w, h, bpp, channels;

	if (!fp)
		return NULL;
	if (PHYSFS_read(fp, header, sizeof(header), 1) != 1 || header[2] != TGA_TYPE_TRUECOLOR)
		goto fail;

	w = header[12] | (header[13] << 8);
	h = header[14] | (header[15] << 8);
	bpp = header[16];
	channels = bpp / 8;
	if (w < 1 || h < 1 || (bpp != 24 && bpp != 32) || (double)w * h > TGA_MAX_PIXELS)
		goto fail;
	// header[0] is the length of an optional id field sitting before the pixels.
	if (header[0] && !PHYSFS_seek(fp, PHYSFS_tell(fp) + header[0]))
		goto fail;

	raw = d_malloc((size_t)w * h * channels);
	rgb = d_malloc((size_t)w * h * channels);
	if (!raw || !rgb)
		goto fail;
	if (PHYSFS_read(fp, raw, (PHYSFS_uint32)((size_t)w * h * channels), 1) != 1)
		goto fail;

	tga_unflip(raw, rgb, w, h, channels, header[17] & TGA_TOP_ORIGIN);
	*out_w = w;
	*out_h = h;
	*out_channels = channels;
	d_free(raw);
	PHYSFS_close(fp);
	return rgb;

fail:
	if (raw)
		d_free(raw);
	if (rgb)
		d_free(rgb);
	PHYSFS_close(fp);
	return NULL;
}

#define PCX_CHANNELS 3
#define PCX_PALETTE_MAX 63
#define PCX_PALETTE_BYTES 768
#define CHANNEL_MAX 255

// Indexed PCX (the game's own art) expanded to RGB through its palette.
static ubyte *decode_pcx(const char *path, int *w, int *h, int *channels)
{
	grs_bitmap bm;
	ubyte palette[PCX_PALETTE_BYTES];
	ubyte *rgb;
	size_t i, pixels;

	gr_init_bitmap_data(&bm);
	if (pcx_read_bitmap((char *)path, &bm, BM_LINEAR, palette) != PCX_ERROR_NONE)
		return NULL;
	pixels = (size_t)bm.bm_w * bm.bm_h;
	rgb = d_malloc(pixels * PCX_CHANNELS);
	if (rgb)
	{
		for (i = 0; i < pixels; i++)
		{
			int c;

			for (c = 0; c < PCX_CHANNELS; c++)
				rgb[i * PCX_CHANNELS + c] = palette[bm.bm_data[i] * 3 + c] * CHANNEL_MAX / PCX_PALETTE_MAX;
		}
		*w = bm.bm_w;
		*h = bm.bm_h;
		*channels = PCX_CHANNELS;
	}
	gr_free_bitmap_data(&bm);
	return rgb;
}

ubyte *shotimg_decode(const char *path, int *width, int *height, int *channels)
{
	const char *dot = strrchr(path, '.');

	if (dot && !strcasecmp(dot, ".tga"))
		return decode_tga(path, width, height, channels);
	if (dot && !strcasecmp(dot, ".pcx"))
		return decode_pcx(path, width, height, channels);
	return decode_png(path, width, height, channels);
}

ubyte *shotimg_decode_memory(const void *png_bytes, size_t size, int *width, int *height, int *channels)
{
	png_image image;
	ubyte *rgba = NULL;

	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&image, png_bytes, size))
		return NULL;
	image.format = PNG_FORMAT_RGBA;
	rgba = d_malloc(PNG_IMAGE_SIZE(image));
	if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL))
	{
		png_image_free(&image);
		if (rgba)
			d_free(rgba);
		return NULL;
	}
	*width = (int)image.width;
	*height = (int)image.height;
	*channels = 4;
	return rgba;
}
