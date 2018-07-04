/* $OpenBSD$ */

/*
 * Copyright (c) 2008 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Grid data. This is the basic data structure that represents what is shown on
 * screen.
 *
 * A grid is a grid of cells (struct grid_cell). Lines are not allocated until
 * cells in that line are written to. The grid is split into history and
 * viewable data with the history starting at row (line) 0 and extending to
 * (hsize - 1); from hsize to hsize + (sy - 1) is the viewable data. All
 * functions in this file work on absolute coordinates, grid-view.c has
 * functions which work on the screen data.
 */

/* Default grid cell data. */
const struct grid_cell grid_default_cell = {
	0, 0, 8, 8, { { ' ' }, 0, 1, 1 }
};
static const struct grid_cell_entry grid_default_entry = {
	0, { .data = { 0, 8, 8, ' ' } }
};

static void	grid_empty_line(struct grid *, u_int, u_int);
static struct grid_block * grid_block_reflow(
	struct grid_block *gb, u_int sx, u_int **yfixups);


/* Store cell in entry. */
static void
grid_store_cell(struct grid_cell_entry *gce, const struct grid_cell *gc,
    u_char c)
{
	gce->flags = gc->flags;

	gce->data.fg = gc->fg & 0xff;
	if (gc->fg & COLOUR_FLAG_256)
		gce->flags |= GRID_FLAG_FG256;

	gce->data.bg = gc->bg & 0xff;
	if (gc->bg & COLOUR_FLAG_256)
		gce->flags |= GRID_FLAG_BG256;

	gce->data.attr = gc->attr;
	gce->data.data = c;
}

/* Check if a cell should be extended. */
static int
grid_need_extended_cell(const struct grid_cell_entry *gce,
    const struct grid_cell *gc)
{
	if (gce->flags & GRID_FLAG_EXTENDED)
		return (1);
	if (gc->attr > 0xff)
		return (1);
	if (gc->data.size != 1 || gc->data.width != 1)
		return (1);
	if ((gc->fg & COLOUR_FLAG_RGB) || (gc->bg & COLOUR_FLAG_RGB))
		return (1);
	return (0);
}

/* Free up unused extended cells. */
static void
grid_compact_line(struct grid_line *gl)
{
	int			 new_extdsize = 0;
	struct grid_cell	*new_extddata;
	struct grid_cell_entry	*gce;
	struct grid_cell	*gc;
	u_int			 px, idx;

	if (gl->extdsize == 0)
		return;

	for (px = 0; px < gl->cellsize; px++) {
		gce = &gl->celldata[px];
		if (gce->flags & GRID_FLAG_EXTENDED)
			new_extdsize++;
	}

	if (new_extdsize == 0) {
		free(gl->extddata);
		gl->extddata = NULL;
		gl->extdsize = 0;
		return;
	}
	new_extddata = xreallocarray(NULL, new_extdsize, sizeof *gl->extddata);

	idx = 0;
	for (px = 0; px < gl->cellsize; px++) {
		gce = &gl->celldata[px];
		if (gce->flags & GRID_FLAG_EXTENDED) {
			gc = &gl->extddata[gce->offset];
			memcpy(&new_extddata[idx], gc, sizeof *gc);
			gce->offset = idx++;
		}
	}

	free(gl->extddata);
	gl->extddata = new_extddata;
	gl->extdsize = new_extdsize;
}

/* Set cell as extended. */
static struct grid_cell *
grid_extended_cell(struct grid_line *gl, struct grid_cell_entry *gce,
    const struct grid_cell *gc)
{
	struct grid_cell	*gcp;

	gl->flags |= GRID_LINE_EXTENDED;

	if (~gce->flags & GRID_FLAG_EXTENDED) {
		gl->extddata = xreallocarray(gl->extddata, gl->extdsize + 1,
		    sizeof *gl->extddata);
		gce->offset = gl->extdsize++;
		gce->flags = gc->flags | GRID_FLAG_EXTENDED;
	}
	if (gce->offset >= gl->extdsize)
		fatalx("offset too big");

	gcp = &gl->extddata[gce->offset];
	memcpy(gcp, gc, sizeof *gcp);
	return (gcp);
}

#define MAX_BLOCK_LINES   1024

struct grid_block_get_cache {
	u_int			offset;
	struct grid_block	*gb;
};

static void
grid_validate(struct grid *gd)
{
#ifdef DEBUG
	struct grid_block	*gb;
	uint total = 0;

	TAILQ_FOREACH(gb, &gd->blocks, entry) {
		log_debug("%s: gd %p block [%p], size %d",
			  __func__, gd, gb, gb->block_size);
		total += gb->block_size;
	}

	log_debug("%s: gd %p total size %d, hallocated %d",
		  __func__, gd, total, gd->hallocated);
	if (total != gd->hallocated) {
		abort();
	}
#else
	(void)gd;
#endif
}

static struct grid_block *
grid_get_block(struct grid *gd, u_int *py, struct grid_block_get_cache *cache)
{
	struct grid_block	*gb;
	uint			total = gd->hallocated, offset;

	if (cache && cache->gb) {
		if (cache->offset <= *py &&
		    *py < cache->offset + cache->gb->block_size) {
			*py -= cache->offset;
			return cache->gb;
		}
	}

	if (*py < total / 2) {
		offset = 0;

		TAILQ_FOREACH(gb, &gd->blocks, entry) {
			if (offset <= *py && *py < offset + gb->block_size) {
				*py -= offset;
				if (cache) {
					cache->offset = offset;
					cache->gb = gb;
				}
				return gb;
			}

			offset += gb->block_size;
		}
	} else {
		offset = total;

		TAILQ_FOREACH_REVERSE(gb, &gd->blocks, grid_blocks, entry) {
			offset -= gb->block_size;

			if (offset <= *py && *py < offset + gb->block_size) {
				*py -= offset;
				if (cache) {
					cache->offset = offset;
					cache->gb = gb;
				}
				return gb;
			}
		}
	}

	return NULL;
}

/* Add lines, return the first new one. */
static struct grid_line *
grid_block_reflow_add(struct grid_block *gb, u_int n)
{
	struct grid_line	*gl;
	u_int			 sy = gb->block_size + n;

	gb->linedata = xreallocarray(gb->linedata, sy, sizeof *gb->linedata);
	gl = &gb->linedata[gb->block_size];
	memset(gl, 0, n * (sizeof *gl));
	gb->block_size = sy;

	return (gl);
}

static void
grid_reflow_apply_hsize_diff(struct grid *gd, int hsize_diff)
{
	struct grid_block	*gb;

	if (hsize_diff < 0  &&  (u_int)-hsize_diff > gd->hsize) {
		gd->hsize = 0;
		if (!TAILQ_EMPTY(&gd->blocks)) {
			gb = TAILQ_LAST(&gd->blocks, grid_blocks);
			grid_block_reflow_add(gb, (u_int)-hsize_diff);
			gd->hallocated += (u_int)-hsize_diff;
		}
	} else {
		gd->hsize += hsize_diff;
	}
}

static void
grid_reflow_complete(struct grid *gd)
{
	struct grid_block	*gb;
	u_int			*yfixups[1], **yfixup;
	struct grid_block	 *new_gb;
	int			hsize_diff = 0;

	gd->reflowing = 1;

	TAILQ_FOREACH(gb, &gd->blocks, entry) {
		if (!gb->need_reflow)
			continue;

		yfixup = &yfixups[0];
		*yfixup = NULL;

		new_gb = grid_block_reflow(gb, gb->sx, yfixups);

		/*
		 * Relocate the new block content to our old one,
		 * and free its descriptor.
		 */
		free(gb->linedata);
		hsize_diff += new_gb->block_size - gb->block_size;
		gd->hallocated += new_gb->block_size - gb->block_size;
		gb->linedata = new_gb->linedata;
		gb->block_size = new_gb->block_size;
		gb->need_reflow = 0;
		free(new_gb);
	}

	grid_reflow_apply_hsize_diff(gd, hsize_diff);
	gd->reflowing = 0;
}

struct grid_line *
grid_get_linedata(struct grid *gd, u_int py)
{
	u_int			by;
	struct grid_block	*gb;

	if (!gd->reflowing) {
		by = py;
		gb = grid_get_block(gd, &by, NULL);
		if (gb->need_reflow)
			grid_reflow_complete(gd);
	}

	by = py;
	gb = grid_get_block(gd, &by, NULL);

	return &gb->linedata[by];
}

static struct grid_block *
grid_block_new(int sx)
{
	struct grid_block	*gb;

	gb = xmalloc(sizeof *gb);
	gb->sx = sx;
	gb->block_size = 0;
	gb->need_reflow = 0;
	gb->linedata = NULL;

	return gb;
}

static void
grid_append_new_empty_block(struct grid *gd)
{
	struct grid_block	*gb;

	gb = grid_block_new(gd->sx);
	TAILQ_INSERT_TAIL(&gd->blocks, gb, entry);
}

/* Free one line. */
static void
grid_line_free(struct grid_line *gl)
{
	free(gl->celldata);
	gl->celldata = NULL;
	free(gl->extddata);
	gl->extddata = NULL;
}

/* Free one line. */
static void
grid_block_free_line(struct grid_block *gb, u_int py)
{
	grid_line_free(&gb->linedata[py]);
}

/* Free several lines in a block. */
static void
grid_block_free_lines(struct grid_block *gb, u_int py, u_int ny)
{
	u_int	yy;

	for (yy = py; yy < py + ny; yy++)
		grid_block_free_line(gb, yy);
}

static void
grid_block_free(struct grid_block *gb)
{
	grid_block_free_lines(gb, 0, gb->block_size);
	free(gb->linedata);
	free(gb);
}

/* Free several lines in a grid. */
static void
grid_free_lines(struct grid *gd, u_int py, u_int ny)
{
	struct grid_line	*gl;
	u_int	yy;

	for (yy = py; yy < py + ny; yy++) {
		gl = grid_get_linedata(gd, yy);
		grid_line_free(gl);
	}
}

/*
 * Resize the grid according to the total number of lines.
 *
 * Note: Does not update gd->hsize and gd->sy - that's for the caller to decide
 * how.
 */
void
grid_realloc_linedata(struct grid *gd, uint total_goal)
{
	uint			total = gd->hallocated, new_size, i;
	uint			size_to_remove;
	struct grid_block	*gb;

	grid_validate(gd);

	while (total_goal > total) {
		if (TAILQ_EMPTY(&gd->blocks)) {
			grid_append_new_empty_block(gd);
			continue;
		}

		gb = TAILQ_LAST(&gd->blocks, grid_blocks);
		if (gb->block_size >= MAX_BLOCK_LINES) {
			grid_append_new_empty_block(gd);
			continue;
		}

		new_size = gb->block_size + total_goal - total;
		if (new_size > MAX_BLOCK_LINES)
			new_size = MAX_BLOCK_LINES;

		gb->linedata = xreallocarray(
			gb->linedata, new_size, sizeof *gb->linedata);

		for (i = gb->block_size; i < new_size ; i++)
			memset(&gb->linedata[i], 0, sizeof(struct grid_line));

		total += new_size - gb->block_size;
		gb->block_size = new_size;
	}

	while (total_goal < total) {
		if (TAILQ_EMPTY(&gd->blocks))
			break;

		gb = TAILQ_LAST(&gd->blocks, grid_blocks);

		size_to_remove = total - total_goal;
		if (size_to_remove >= gb->block_size) {
			TAILQ_REMOVE(&gd->blocks, gb, entry);
			total -= gb->block_size;

			grid_block_free(gb);
			continue;
		}

		new_size = gb->block_size - size_to_remove;
		grid_block_free_lines(gb, new_size, size_to_remove);

		gb->linedata = xreallocarray(
			gb->linedata, new_size, sizeof *gb->linedata);
		gb->block_size = new_size;

		total -= size_to_remove;
	}

	gd->hallocated = total;
	grid_validate(gd);
}

/*
 * Trim the history from the end, remove `nr_to_remove` lines from it.
 *
 * Note: Does not update gd->hsize and gd->sy - that's for the caller to decide
 * how.
 */
static void
grid_trim_head(struct grid *gd, uint nr_to_remove)
{
	uint			remaining;
	struct grid_block	*gb;

	while (nr_to_remove > 0) {
		if (TAILQ_EMPTY(&gd->blocks))
			break;

		gb = TAILQ_FIRST(&gd->blocks);
		if (gb->block_size <= nr_to_remove) {
			TAILQ_REMOVE(&gd->blocks, gb, entry);
			gd->hallocated -= gb->block_size;
			nr_to_remove -= gb->block_size;
			grid_block_free(gb);
			continue;
		}

		remaining = gb->block_size - nr_to_remove;

		grid_block_free_lines(gb, 0, nr_to_remove);
		memmove(&gb->linedata[0], &gb->linedata[nr_to_remove],
			(remaining) * (sizeof *gb->linedata));
		gb->linedata = xreallocarray(
			gb->linedata, remaining, sizeof *gb->linedata);
		gb->block_size -= nr_to_remove;
		gd->hallocated -= nr_to_remove;
		break;
	}

	grid_validate(gd);
}

/* Copy default into a cell. */
static void
grid_block_clear_cell(struct grid_block *gb, u_int px, u_int py, u_int bg)
{
	struct grid_line	*gl = &gb->linedata[py];
	struct grid_cell_entry	*gce = &gl->celldata[px];
	struct grid_cell	*gc;

	memcpy(gce, &grid_default_entry, sizeof *gce);
	if (bg & COLOUR_FLAG_RGB) {
		gc = grid_extended_cell(gl, gce, &grid_default_cell);
		gc->bg = bg;
	} else {
		if (bg & COLOUR_FLAG_256)
			gce->flags |= GRID_FLAG_BG256;
		gce->data.bg = bg;
	}
}

/* Check grid y position. */
static int
grid_block_check_y(struct grid_block *gb, const char* from, u_int py)
{
	if (py >= gb->block_size) {
		log_debug("%s: y out of range: %u", from, py);
		return (-1);
	}
	return (0);
}


/* Check grid y position. */
static int
grid_check_y(struct grid *gd, const char* from, u_int py)
{
	if (py >= gd->hsize + gd->sy) {
		log_debug("%s: y out of range: %u", from, py);
		return (-1);
	}
	return (0);
}

/* Compare grid cells. Return 1 if equal, 0 if not. */
int
grid_cells_equal(const struct grid_cell *gca, const struct grid_cell *gcb)
{
	if (gca->fg != gcb->fg || gca->bg != gcb->bg)
		return (0);
	if (gca->attr != gcb->attr || gca->flags != gcb->flags)
		return (0);
	if (gca->data.width != gcb->data.width)
		return (0);
	if (gca->data.size != gcb->data.size)
		return (0);
	return (memcmp(gca->data.data, gcb->data.data, gca->data.size) == 0);
}

/* Create a new grid. */
struct grid *
grid_create(u_int sx, u_int sy, u_int hlimit)
{
	struct grid	*gd;

	gd = xmalloc(sizeof *gd);
	gd->sx = sx;
	gd->sy = 0;

	gd->flags = GRID_HISTORY;

	gd->hallocated = 0;
	gd->hscrolled = 0;
	gd->hsize = 0;
	gd->reflowing = 0;
	gd->hlimit = hlimit;

	TAILQ_INIT(&gd->blocks);

	grid_realloc_linedata(gd, sy);

	gd->sy = sy;

	return (gd);
}

/* Destroy grid. */
void
grid_destroy(struct grid *gd)
{
	struct grid_block	*gb, *gb1;

	TAILQ_FOREACH_SAFE(gb, &gd->blocks, entry, gb1) {
		TAILQ_REMOVE(&gd->blocks, gb, entry);

		grid_block_free(gb);
	}

	free(gd);
}

/* Compare grids. */
int
grid_compare(struct grid *ga, struct grid *gb)
{
	struct grid_line	*gla, *glb;
	struct grid_cell	 gca, gcb;
	u_int			 xx, yy;

	if (ga->sx != gb->sx || ga->sy != gb->sy)
		return (1);

	for (yy = 0; yy < ga->sy; yy++) {
		gla = grid_get_linedata(ga, yy);
		glb = grid_get_linedata(gb, yy);
		if (gla->cellsize != glb->cellsize)
			return (1);
		for (xx = 0; xx < gla->cellsize; xx++) {
			grid_get_cell(ga, xx, yy, &gca);
			grid_get_cell(gb, xx, yy, &gcb);
			if (!grid_cells_equal(&gca, &gcb))
				return (1);
		}
	}

	return (0);
}

/*
 * Collect lines from the history if at the limit. Free the top (oldest) 10%
 * and shift up.
 */
void
grid_collect_history(struct grid *gd)
{
	u_int	ny;

	if (gd->hsize == 0 || gd->hsize < gd->hlimit)
		return;

	ny = gd->hlimit / 10;
	if (ny < 1)
		ny = 1;
	if (ny > gd->hsize)
		ny = gd->hsize;

	/*
	 * Free the lines from 0 to ny then move the remaining lines over
	 * them.
	 */
	grid_trim_head(gd, ny);

	gd->hsize -= ny;
	log_debug("%d: new hsize: %d\n", __LINE__, gd->hsize);
	if (gd->hscrolled > gd->hsize)
		gd->hscrolled = gd->hsize;
}

/*
 * Scroll the entire visible screen, moving one line into the history. Just
 * allocate a new line at the bottom and move the history size indicator.
 */
void
grid_scroll_history(struct grid *gd, u_int bg)
{
	u_int	yy;
	struct grid_line *gl;

	yy = gd->hsize + gd->sy;
	grid_realloc_linedata(gd, yy + 1);
	grid_empty_line(gd, yy, bg);

	gd->hscrolled++;
	gl = grid_get_linedata(gd, gd->hsize);
	grid_compact_line(gl);
	gd->hsize++;
}

/* Clear the history. */
void
grid_clear_history(struct grid *gd)
{
	log_debug("grid_clear_history: %d %d, hsize=%d\n", gd->sy, gd->hallocated, gd->hsize);
	grid_trim_head(gd, gd->hsize);
	gd->hscrolled = 0;
	gd->hsize = 0;
	log_debug("grid_clear_history: after %d %d hsize=%d\n", gd->sy, gd->hallocated, gd->hsize);
}

static void
grid_block_move_line(struct grid_block *sgb, struct grid_block *dgb,
        u_int dyb, u_int syb)
{
	grid_line_free(&dgb->linedata[dyb]);

	memcpy(&dgb->linedata[dyb],
	       &sgb->linedata[syb],
	       sizeof dgb->linedata[syb]);
	memset(&sgb->linedata[syb], 0,
	       sizeof sgb->linedata[syb]);
}

static void
grid_move_lines1(struct grid *gd, u_int dy, u_int sy, u_int n)
{
	struct grid_block_get_cache cache_1 = {0, NULL};
	struct grid_block_get_cache cache_2 = {0, NULL};
	u_int  syb, syy, dyb;
	struct grid_block *sgb, *dgb;

	if (sy > dy) {
		for (syy = sy; syy < sy + n; syy++) {
			syb = syy;
			dyb = syy - sy + dy;

			sgb = grid_get_block(gd, &syb, &cache_1);
			dgb = grid_get_block(gd, &dyb, &cache_2);
			grid_block_move_line(sgb, dgb, dyb, syb);
		}
	} else if (sy < dy) {
		for (syy = sy + n - 1; syy >= sy; syy--) {
			syb = syy;
			dyb = syy - sy + dy;

			sgb = grid_get_block(gd, &syb, &cache_1);
			dgb = grid_get_block(gd, &dyb, &cache_2);
			grid_block_move_line(sgb, dgb, dyb, syb);

			if (syy == 0)
				break;
		}
	}
}

/*
 * Scroll a region up, moving the top line into the history.
 *
 *           a    b    c
 *  1   [1] [1]  [1]  [1]
 *  2 u [2] [2]  [2]  [2]
 *  3   [3] [3]  [_]* [4]*
 *  4 l [4] [4]  [4]  [5]*
 *  5   [5] [5]  [5]  [_]
 *  6   [6] [6]  [6]  [6]
 *  7   [x] [_]* [3]  [3]
 *  8   [y] [x]* [x]  [x]
 *  9   [z] [y]* [y]  [y]
 * 10       [z]* [z]  [z]
 */
void
grid_scroll_history_region(struct grid *gd, u_int upper, u_int lower, u_int bg)
{
	u_int			 yy;

	/* Create a space for a new line. */
	yy = gd->hsize + gd->sy;
	grid_realloc_linedata(gd, yy + 1);

	/* Move the entire screen down to free a space for this line. [a] */
	grid_move_lines1(gd, gd->hsize + 1, gd->hsize, gd->sy);

	/* Adjust the region and find its start and end. */
	upper++;
	lower++;

	/* Move the line into the history. [b] */
	grid_move_lines1(gd, gd->hsize, upper, 1);

	/* Then move the region up and clear the bottom line. [c] */
	grid_move_lines1(gd, upper, upper + 1, lower - upper);
	grid_empty_line(gd, lower, bg);

	/* Move the history offset down over the line. */
	gd->hscrolled++;
	gd->hsize++;
}

/* Expand line to fit to cell. */
static void
grid_block_expand_line(struct grid_block *gb, u_int py, u_int sx, u_int bg)
{
	struct grid_line	*gl;
	u_int			 xx;

	gl = &gb->linedata[py];
	if (sx <= gl->cellsize)
		return;

	if (sx < gb->sx / 4)
		sx = gb->sx / 4;
	else if (sx < gb->sx / 2)
		sx = gb->sx / 2;
	else
		sx = gb->sx;

	gl->celldata = xreallocarray(gl->celldata, sx, sizeof *gl->celldata);
	for (xx = gl->cellsize; xx < sx; xx++)
		grid_block_clear_cell(gb, xx, py, bg);
	gl->cellsize = sx;
}


/* Expand line to fit to cell. */
static void
grid_expand_line(struct grid *gd, u_int py, u_int sx, u_int bg)
{
	struct grid_block	*gb = grid_get_block(gd, &py, NULL);

	grid_block_expand_line(gb, py, sx, bg);
}

/* Empty a line and set background colour if needed. */
static void
grid_block_empty_line(struct grid_block *gb, u_int py, u_int bg)
{
	memset(&gb->linedata[py], 0, sizeof gb->linedata[py]);

	if (bg != 8)
		grid_block_expand_line(gb, py, gb->sx, bg);
}

/* Empty a line and set background colour if needed. */
static void
grid_empty_line(struct grid *gd, u_int py, u_int bg)
{
	struct grid_block *gb = grid_get_block(gd, &py, NULL);

	grid_block_empty_line(gb, py, bg);
}

/* Peek at grid line. */
const struct grid_line *
grid_peek_line(struct grid *gd, u_int py)
{
	if (grid_check_y(gd, __func__, py) != 0)
		return (NULL);

	return grid_get_linedata(gd, py);
}

/* Get cell from line. */
static void
grid_get_cell1(struct grid_line *gl, u_int px, struct grid_cell *gc)
{
	struct grid_cell_entry	*gce = &gl->celldata[px];

	if (gce->flags & GRID_FLAG_EXTENDED) {
		if (gce->offset >= gl->extdsize)
			memcpy(gc, &grid_default_cell, sizeof *gc);
		else
			memcpy(gc, &gl->extddata[gce->offset], sizeof *gc);
		return;
	}

	gc->flags = gce->flags & ~(GRID_FLAG_FG256|GRID_FLAG_BG256);
	gc->attr = gce->data.attr;
	gc->fg = gce->data.fg;
	if (gce->flags & GRID_FLAG_FG256)
		gc->fg |= COLOUR_FLAG_256;
	gc->bg = gce->data.bg;
	if (gce->flags & GRID_FLAG_BG256)
		gc->bg |= COLOUR_FLAG_256;
	utf8_set(&gc->data, gce->data.data);
}

/* Set cell at relative position. */
static void
grid_block_set_cell(struct grid_block *gb, u_int px, u_int py, const struct grid_cell *gc)
{
	struct grid_line	*gl;
	struct grid_cell_entry	*gce;

	if (grid_block_check_y(gb, __func__, py) != 0)
		return;

	grid_block_expand_line(gb, py, px + 1, 8);

	gl = &gb->linedata[py];
	if (px + 1 > gl->cellused)
		gl->cellused = px + 1;

	gce = &gl->celldata[px];
	if (grid_need_extended_cell(gce, gc))
		grid_extended_cell(gl, gce, gc);
	else
		grid_store_cell(gce, gc, gc->data.data[0]);
}

static void
grid_line_get_cell(struct grid_line *gl, u_int px, struct grid_cell *gc)
{
	if (px >= gl->cellsize) {
		memcpy(gc, &grid_default_cell, sizeof *gc);
		return;
	}
	return (grid_get_cell1(gl, px, gc));
}

/* Get cell for reading. */
void
grid_get_cell(struct grid *gd, u_int px, u_int py, struct grid_cell *gc)
{
	if (grid_check_y(gd, __func__, py) != 0) {
		memcpy(gc, &grid_default_cell, sizeof *gc);
		return;
	}

	return grid_line_get_cell(grid_get_linedata(gd, py), px, gc);
}

/* Set cell at relative position. */
void
grid_set_cell(struct grid *gd, u_int px, u_int py, const struct grid_cell *gc)
{
	struct grid_block	*gb;

	if (grid_check_y(gd, __func__, py) != 0)
		return;

	gb = grid_get_block(gd, &py, NULL);
	grid_block_set_cell(gb, px, py, gc);
}

/* Set cells at relative position. */
void
grid_set_cells(struct grid *gd, u_int px, u_int py, const struct grid_cell *gc,
    const char *s, size_t slen)
{
	struct grid_line	*gl;
	struct grid_cell_entry	*gce;
	struct grid_cell	*gcp;
	u_int			 i;

	if (grid_check_y(gd, __func__, py) != 0)
		return;

	grid_expand_line(gd, py, px + slen, 8);

	gl = grid_get_linedata(gd, py);
	if (px + slen > gl->cellused)
		gl->cellused = px + slen;

	for (i = 0; i < slen; i++) {
		gce = &gl->celldata[px + i];
		if (grid_need_extended_cell(gce, gc)) {
			gcp = grid_extended_cell(gl, gce, gc);
			utf8_set(&gcp->data, s[i]);
		} else
			grid_store_cell(gce, gc, s[i]);
	}
}

/* Clear area. */
void
grid_clear(struct grid *gd, u_int px, u_int py, u_int nx, u_int ny, u_int bg)
{
	u_int	xx, yy, yb;
	struct grid_block_get_cache cache = {0, NULL};
	struct grid_line *gl;
	struct grid_block *gb;

	if (nx == 0 || ny == 0)
		return;

	if (px == 0 && nx == gd->sx) {
		grid_clear_lines(gd, py, ny, bg);
		return;
	}

	if (grid_check_y(gd, __func__, py) != 0)
		return;
	if (grid_check_y(gd, __func__, py + ny - 1) != 0)
		return;

	for (yy = py; yy < py + ny; yy++) {
		yb = yy;
		gb = grid_get_block(gd, &yb, &cache);
		gl = &gb->linedata[yb];
		if (px + nx >= gd->sx && px < gl->cellused)
			gl->cellused = px;
		if (px > gl->cellsize && bg == 8)
			continue;
		if (px + nx >= gl->cellsize && bg == 8) {
			gl->cellsize = px;
			continue;
		}

		grid_block_expand_line(gb, yb, px + nx, 8); /* default bg first */
		for (xx = px; xx < px + nx; xx++)
			grid_block_clear_cell(gb, xx, yb, bg);
	}
}

/* Clear lines. This just frees and truncates the lines. */
void
grid_clear_lines(struct grid *gd, u_int py, u_int ny, u_int bg)
{
	u_int	yy, yb;
	struct grid_block	*gb;
	struct grid_block_get_cache cache = {0, NULL};

	if (ny == 0)
		return;

	if (grid_check_y(gd, __func__, py) != 0)
		return;
	if (grid_check_y(gd, __func__, py + ny - 1) != 0)
		return;

	for (yy = py; yy < py + ny; yy++) {
		yb = yy;
		gb = grid_get_block(gd, &yb, &cache);
		grid_block_free_line(gb, yb);
		grid_block_empty_line(gb, yb, bg);
	}
}

/* Move a group of lines. */
void
grid_move_lines(struct grid *gd, u_int dy, u_int py, u_int ny, u_int bg)
{
	struct grid_block_get_cache cache = {0, NULL};
	struct grid_block	*gb;
	u_int	yy, yb;

	if (ny == 0 || py == dy)
		return;

	if (grid_check_y(gd, __func__, py) != 0)
		return;
	if (grid_check_y(gd, __func__, py + ny - 1) != 0)
		return;
	if (grid_check_y(gd, __func__, dy) != 0)
		return;
	if (grid_check_y(gd, __func__, dy + ny - 1) != 0)
		return;

	grid_move_lines1(gd, dy, py, ny);

	/*
	 * Wipe any lines that have been moved (without freeing them - they are
	 * still present).
	 */
	for (yy = py; yy < py + ny; yy++) {
		if (yy < dy || yy >= dy + ny) {
			yb = yy;
			gb = grid_get_block(gd, &yb, &cache);
			grid_block_empty_line(gb, yb, bg);
		}
	}
}

/* Move a group of cells. */
static void
grid_block_move_cells(struct grid_block *gb, u_int dx, u_int px, u_int py,
      u_int nx, u_int bg)
{
	struct grid_line	*gl;
	u_int			 xx;

	if (nx == 0 || px == dx)
		return;

	if (grid_block_check_y(gb, __func__, py) != 0)
		return;
	gl = &gb->linedata[py];

	grid_block_expand_line(gb, py, px + nx, 8);
	grid_block_expand_line(gb, py, dx + nx, 8);
	memmove(&gl->celldata[dx], &gl->celldata[px],
	    nx * sizeof *gl->celldata);
	if (dx + nx > gl->cellused)
		gl->cellused = dx + nx;

	/* Wipe any cells that have been moved. */
	for (xx = px; xx < px + nx; xx++) {
		if (xx >= dx && xx < dx + nx)
			continue;
		grid_block_clear_cell(gb, xx, py, bg);
	}
}

void
grid_move_cells(struct grid *gd, u_int dx, u_int px, u_int py, u_int nx,
    u_int bg)
{
	struct grid_block	*gb = grid_get_block(gd, &py, NULL);

	grid_block_move_cells(gb, dx, px, py, nx, bg);
}

/* Get ANSI foreground sequence. */
static size_t
grid_string_cells_fg(const struct grid_cell *gc, int *values)
{
	size_t	n;
	u_char	r, g, b;

	n = 0;
	if (gc->fg & COLOUR_FLAG_256) {
		values[n++] = 38;
		values[n++] = 5;
		values[n++] = gc->fg & 0xff;
	} else if (gc->fg & COLOUR_FLAG_RGB) {
		values[n++] = 38;
		values[n++] = 2;
		colour_split_rgb(gc->fg, &r, &g, &b);
		values[n++] = r;
		values[n++] = g;
		values[n++] = b;
	} else {
		switch (gc->fg) {
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
			values[n++] = gc->fg + 30;
			break;
		case 8:
			values[n++] = 39;
			break;
		case 90:
		case 91:
		case 92:
		case 93:
		case 94:
		case 95:
		case 96:
		case 97:
			values[n++] = gc->fg;
			break;
		}
	}
	return (n);
}

/* Get ANSI background sequence. */
static size_t
grid_string_cells_bg(const struct grid_cell *gc, int *values)
{
	size_t	n;
	u_char	r, g, b;

	n = 0;
	if (gc->bg & COLOUR_FLAG_256) {
		values[n++] = 48;
		values[n++] = 5;
		values[n++] = gc->bg & 0xff;
	} else if (gc->bg & COLOUR_FLAG_RGB) {
		values[n++] = 48;
		values[n++] = 2;
		colour_split_rgb(gc->bg, &r, &g, &b);
		values[n++] = r;
		values[n++] = g;
		values[n++] = b;
	} else {
		switch (gc->bg) {
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
			values[n++] = gc->bg + 40;
			break;
		case 8:
			values[n++] = 49;
			break;
		case 100:
		case 101:
		case 102:
		case 103:
		case 104:
		case 105:
		case 106:
		case 107:
			values[n++] = gc->bg - 10;
			break;
		}
	}
	return (n);
}

/*
 * Returns ANSI code to set particular attributes (colour, bold and so on)
 * given a current state.
 */
static void
grid_string_cells_code(const struct grid_cell *lastgc,
    const struct grid_cell *gc, char *buf, size_t len, int escape_c0)
{
	int	oldc[64], newc[64], s[128];
	size_t	noldc, nnewc, n, i;
	u_int	attr = gc->attr, lastattr = lastgc->attr;
	char	tmp[64];

	struct {
		u_int	mask;
		u_int	code;
	} attrs[] = {
		{ GRID_ATTR_BRIGHT, 1 },
		{ GRID_ATTR_DIM, 2 },
		{ GRID_ATTR_ITALICS, 3 },
		{ GRID_ATTR_UNDERSCORE, 4 },
		{ GRID_ATTR_BLINK, 5 },
		{ GRID_ATTR_REVERSE, 7 },
		{ GRID_ATTR_HIDDEN, 8 },
		{ GRID_ATTR_STRIKETHROUGH, 9 }
	};
	n = 0;

	/* If any attribute is removed, begin with 0. */
	for (i = 0; i < nitems(attrs); i++) {
		if (!(attr & attrs[i].mask) && (lastattr & attrs[i].mask)) {
			s[n++] = 0;
			lastattr &= GRID_ATTR_CHARSET;
			break;
		}
	}
	/* For each attribute that is newly set, add its code. */
	for (i = 0; i < nitems(attrs); i++) {
		if ((attr & attrs[i].mask) && !(lastattr & attrs[i].mask))
			s[n++] = attrs[i].code;
	}

	/* Write the attributes. */
	*buf = '\0';
	if (n > 0) {
		if (escape_c0)
			strlcat(buf, "\\033[", len);
		else
			strlcat(buf, "\033[", len);
		for (i = 0; i < n; i++) {
			if (i + 1 < n)
				xsnprintf(tmp, sizeof tmp, "%d;", s[i]);
			else
				xsnprintf(tmp, sizeof tmp, "%d", s[i]);
			strlcat(buf, tmp, len);
		}
		strlcat(buf, "m", len);
	}

	/* If the foreground colour changed, write its parameters. */
	nnewc = grid_string_cells_fg(gc, newc);
	noldc = grid_string_cells_fg(lastgc, oldc);
	if (nnewc != noldc ||
	    memcmp(newc, oldc, nnewc * sizeof newc[0]) != 0 ||
	    (n != 0 && s[0] == 0)) {
		if (escape_c0)
			strlcat(buf, "\\033[", len);
		else
			strlcat(buf, "\033[", len);
		for (i = 0; i < nnewc; i++) {
			if (i + 1 < nnewc)
				xsnprintf(tmp, sizeof tmp, "%d;", newc[i]);
			else
				xsnprintf(tmp, sizeof tmp, "%d", newc[i]);
			strlcat(buf, tmp, len);
		}
		strlcat(buf, "m", len);
	}

	/* If the background colour changed, append its parameters. */
	nnewc = grid_string_cells_bg(gc, newc);
	noldc = grid_string_cells_bg(lastgc, oldc);
	if (nnewc != noldc ||
	    memcmp(newc, oldc, nnewc * sizeof newc[0]) != 0 ||
	    (n != 0 && s[0] == 0)) {
		if (escape_c0)
			strlcat(buf, "\\033[", len);
		else
			strlcat(buf, "\033[", len);
		for (i = 0; i < nnewc; i++) {
			if (i + 1 < nnewc)
				xsnprintf(tmp, sizeof tmp, "%d;", newc[i]);
			else
				xsnprintf(tmp, sizeof tmp, "%d", newc[i]);
			strlcat(buf, tmp, len);
		}
		strlcat(buf, "m", len);
	}

	/* Append shift in/shift out if needed. */
	if ((attr & GRID_ATTR_CHARSET) && !(lastattr & GRID_ATTR_CHARSET)) {
		if (escape_c0)
			strlcat(buf, "\\016", len); /* SO */
		else
			strlcat(buf, "\016", len);  /* SO */
	}
	if (!(attr & GRID_ATTR_CHARSET) && (lastattr & GRID_ATTR_CHARSET)) {
		if (escape_c0)
			strlcat(buf, "\\017", len); /* SI */
		else
			strlcat(buf, "\017", len);  /* SI */
	}
}

/* Convert cells into a string. */
char *
grid_string_cells(struct grid *gd, u_int px, u_int py, u_int nx,
    struct grid_cell **lastgc, int with_codes, int escape_c0, int trim)
{
	struct grid_cell	 gc;
	static struct grid_cell	 lastgc1;
	const char		*data;
	char			*buf, code[128];
	size_t			 len, off, size, codelen;
	u_int			 xx;
	const struct grid_line	*gl;

	if (lastgc != NULL && *lastgc == NULL) {
		memcpy(&lastgc1, &grid_default_cell, sizeof lastgc1);
		*lastgc = &lastgc1;
	}

	len = 128;
	buf = xmalloc(len);
	off = 0;

	gl = grid_peek_line(gd, py);
	for (xx = px; xx < px + nx; xx++) {
		if (gl == NULL || xx >= gl->cellsize)
			break;
		grid_get_cell(gd, xx, py, &gc);
		if (gc.flags & GRID_FLAG_PADDING)
			continue;

		if (with_codes) {
			grid_string_cells_code(*lastgc, &gc, code, sizeof code,
			    escape_c0);
			codelen = strlen(code);
			memcpy(*lastgc, &gc, sizeof **lastgc);
		} else
			codelen = 0;

		data = gc.data.data;
		size = gc.data.size;
		if (escape_c0 && size == 1 && *data == '\\') {
			data = "\\\\";
			size = 2;
		}

		while (len < off + size + codelen + 1) {
			buf = xreallocarray(buf, 2, len);
			len *= 2;
		}

		if (codelen != 0) {
			memcpy(buf + off, code, codelen);
			off += codelen;
		}
		memcpy(buf + off, data, size);
		off += size;
	}

	if (trim) {
		while (off > 0 && buf[off - 1] == ' ')
			off--;
	}
	buf[off] = '\0';

	return (buf);
}

/*
 * Duplicate a set of lines between two grids. Both source and destination
 * should be big enough.
 */
void
grid_duplicate_lines(struct grid *dst, u_int dy, struct grid *src, u_int sy,
    u_int ny)
{
	struct grid_line	*dstl, *srcl;
	u_int			 yy;

	if (dy + ny > dst->hsize + dst->sy)
		ny = dst->hsize + dst->sy - dy;
	if (sy + ny > src->hsize + src->sy)
		ny = src->hsize + src->sy - sy;
	grid_free_lines(dst, dy, ny);

	for (yy = 0; yy < ny; yy++) {
		srcl = grid_get_linedata(src, sy);
		dstl = grid_get_linedata(dst, dy);

		memcpy(dstl, srcl, sizeof *dstl);
		if (srcl->cellsize != 0) {
			dstl->celldata = xreallocarray(NULL,
			    srcl->cellsize, sizeof *dstl->celldata);
			memcpy(dstl->celldata, srcl->celldata,
			    srcl->cellsize * sizeof *dstl->celldata);
		} else
			dstl->celldata = NULL;

		if (srcl->extdsize != 0) {
			dstl->extdsize = srcl->extdsize;
			dstl->extddata = xreallocarray(NULL, dstl->extdsize,
			    sizeof *dstl->extddata);
			memcpy(dstl->extddata, srcl->extddata, dstl->extdsize *
			    sizeof *dstl->extddata);
		}

		sy++;
		dy++;
	}
}

/* Mark line as dead. */
static void
grid_reflow_dead(struct grid_line *gl)
{
	memset(gl, 0, sizeof *gl);
	gl->flags = GRID_LINE_DEAD;
}

/* Move a line across. */
static struct grid_line *
grid_block_reflow_move(struct grid_block *gb, struct grid_line *from)
{
	struct grid_line	*to;

	to = grid_block_reflow_add(gb, 1);
	memcpy(to, from, sizeof *to);
	grid_reflow_dead(from);

	return (to);
}

/* Join line below onto this one. */
static void
grid_block_reflow_join(struct grid_block *target,
    struct grid_block *gb, u_int sx, u_int yy,
    u_int width, u_int **yfixups, int already)
{
	struct grid_line	*gl, *from = NULL;
	struct grid_cell	 gc;
	u_int			 lines, left, i, to, line, want = 0;
	u_int			 at;
	int			 wrapped = 1;

	/*
	 * Add a new target line.
	 */
	if (!already) {
		to = target->block_size;
		gl = grid_block_reflow_move(target, &gb->linedata[yy]);
	} else {
		to = target->block_size - 1;
		gl = &target->linedata[to];
	}
	at = gl->cellused;

	/*
	 * Loop until no more to consume or the target line is full.
	 */
	lines = 0;
	for (;;) {
		/*
		 * If this is now the last line, there is nothing more to be
		 * done.
		 */
		if (yy + 1 + lines == gb->block_size)
			break;
		line = yy + 1 + lines;

		/* If the next line is empty, skip it. */
		if (~gb->linedata[line].flags & GRID_LINE_WRAPPED)
			wrapped = 0;
		if (gb->linedata[line].cellused == 0) {
			if (!wrapped)
				break;
			lines++;
			continue;
		}

		/*
		 * Is the destination line now full? Copy the first character
		 * separately because we need to leave "from" set to the last
		 * line if this line is full.
		 */
		grid_get_cell1(&gb->linedata[line], 0, &gc);
		if (width + gc.data.width > sx)
			break;
		width += gc.data.width;
		grid_block_set_cell(target, at, to, &gc);
		at++;

		/* Join as much more as possible onto the current line. */
		from = &gb->linedata[line];
		for (want = 1; want < from->cellused; want++) {
			grid_get_cell1(from, want, &gc);
			if (width + gc.data.width > sx)
				break;
			width += gc.data.width;

			grid_block_set_cell(target, at, to, &gc);
			at++;
		}
		lines++;

		/*
		 * If this line wasn't wrapped or we didn't consume the entire
		 * line, don't try to join any further lines.
		 */
		if (!wrapped || want != from->cellused || width == sx)
			break;
	}
	if (lines == 0)
		return;

	/*
	 * If we didn't consume the entire final line, then remove what we did
	 * consume. If we consumed the entire line and it wasn't wrapped,
	 * remove the wrap flag from this line.
	 */
	left = from->cellused - want;
	if (left != 0) {
		grid_block_move_cells(gb, 0, want, yy + lines, left, 8);
		from->cellsize = from->cellused = left;
		lines--;
	} else if (!wrapped)
		gl->flags &= ~GRID_LINE_WRAPPED;

	/* Remove the lines that were completely consumed. */
	for (i = yy + 1; i < yy + 1 + lines; i++) {
		free(gb->linedata[i].celldata);
		free(gb->linedata[i].extddata);
		grid_reflow_dead(&gb->linedata[i]);
	}

	/* Adjust cursor and scroll positions. */
	while (*yfixups != NULL) {
		if (**yfixups > to + lines)
			**yfixups -= lines;
		else if (**yfixups > to)
			**yfixups = to;
		yfixups++;
	}
}

/* Split this line into several new ones */
static void
grid_block_reflow_split(
    struct grid_block *target, struct grid_block *gb, u_int sx, u_int yy,
    u_int at, u_int **yfixups)
{
	struct grid_line	*gl = &gb->linedata[yy], *first;
	struct grid_cell	 gc;
	u_int			 line, lines, width, i, xx;
	u_int			 used = gl->cellused;
	int			 flags = gl->flags;

	/* How many lines do we need to insert? We know we need at least two. */
	if (~gl->flags & GRID_LINE_EXTENDED)
		lines = 1 + (gl->cellused - 1) / sx;
	else {
		lines = 2;
		width = 0;
		for (i = at; i < used; i++) {
			grid_get_cell1(gl, i, &gc);
			if (width + gc.data.width > sx) {
				lines++;
				width = 0;
			}
			width += gc.data.width;
		}
	}

	/* Insert new lines. */
	line = target->block_size + 1;
	first = grid_block_reflow_add(target, lines);

	/* Copy sections from the original line. */
	width = 0;
	xx = 0;
	for (i = at; i < used; i++) {
		grid_get_cell1(gl, i, &gc);
		if (width + gc.data.width > sx) {
			target->linedata[line].flags |= GRID_LINE_WRAPPED;

			line++;
			width = 0;
			xx = 0;
		}
		width += gc.data.width;
		grid_block_set_cell(target, xx, line, &gc);
		xx++;
	}
	if (flags & GRID_LINE_WRAPPED)
		target->linedata[line].flags |= GRID_LINE_WRAPPED;

	/* Move the remainder of the original line. */
	gl->cellsize = gl->cellused = at;
	gl->flags |= GRID_LINE_WRAPPED;
	memcpy(first, gl, sizeof *first);
	grid_reflow_dead(gl);

	/* Adjust the cursor and scroll positions. */
	while (*yfixups != NULL) {
		if (yy <= **yfixups)
			(**yfixups) += lines - 1;
		yfixups++;
	}

	/*
	 * If the original line had the wrapped flag and there is still space
	 * in the last new line, try to join with the next lines.
	 */
	if (width < sx && (flags & GRID_LINE_WRAPPED))
		grid_block_reflow_join(target, gb, sx, yy, width, yfixups, 1);
}

static struct grid_block *
grid_block_reflow(struct grid_block *gb, u_int sx, u_int **yfixups)
{
	struct grid_block	*target;
	struct grid_line	*gl;
	struct grid_cell	 gc;
	u_int			 yy, width, i, at, first;

	target = grid_block_new(sx);

	for (yy = 0; yy < gb->block_size; yy++) {
		gl = &gb->linedata[yy];
		if (gl->flags & GRID_LINE_DEAD)
			continue;

		/*
		 * Work out the width of this line. first is the width of the
		 * first character, at is the point at which the available
		 * width is hit, and width is the full line width.
		 */
		first = at = width = 0;
		if (~gl->flags & GRID_LINE_EXTENDED) {
			first = 1;
			width = gl->cellused;
			if (width > sx)
				at = sx;
			else
				at = width;
		} else {
			for (i = 0; i < gl->cellused; i++) {
				grid_get_cell1(gl, i, &gc);
				if (i == 0)
					first = gc.data.width;
				if (at == 0 && width + gc.data.width > sx)
					at = i;
				width += gc.data.width;
			}
		}

		/*
		 * If the line is exactly right or the first character is wider
		 * than the targe width, just move it across unchanged.
		 */
		if (width == sx || first > sx) {
			grid_block_reflow_move(target, gl);
			continue;
		}

		log_debug("reflow: yy %d width %d > sx %d\n", yy, width, sx);
		/*
		 * If the line is too big, it needs to be split, whether or not
		 * it was previously wrapped.
		 */
		if (width > sx) {
			grid_block_reflow_split(target, gb, sx, yy, at,
				yfixups);
			continue;
		}

		/*
		 * If the line was previously wrapped, join as much as possible
		 * of the next line.
		 */
		if (gl->flags & GRID_LINE_WRAPPED)
			grid_block_reflow_join(target, gb, sx, yy, width,
			       yfixups, 0);
		else
			grid_block_reflow_move(target, gl);
	}

	return target;
}

/* Reflow lines on grid to new width. */
void
grid_reflow(struct grid *gd, u_int sx, u_int *cursor)
{
	u_int			 cy, offset, reflow_offset, rev_hscrolled;
	u_int                    cy_delta = 0, hscrolled_delta = 0;
	u_int                    cy_fixed = 0, hscrolled_fixed = 0;
	int			 hsize_diff = 0;
	struct timeval		 start, tv;
	struct grid_block	*gb, *new_gb;
	u_int                    *yfixups[3], **yfixup;
	u_int                    total = gd->hsize + gd->sy;


	gettimeofday(&start, NULL);

	cy = gd->sy - 1 - *cursor;

	log_debug("%s: %u lines, new width %u, cy=%u, hscrolled=%u, *cursor=%u", __func__,
		  gd->hsize + gd->sy, sx, cy, gd->hscrolled, *cursor);

	/*
	 * Reflow all the blocks:
	 */
	offset = 0;
	reflow_offset = 0;
	rev_hscrolled = total - gd->hscrolled;
	gd->reflowing = 1;

	TAILQ_FOREACH_REVERSE(gb, &gd->blocks, grid_blocks, entry) {
		if (reflow_offset > gd->sy) {
			gb->need_reflow = 1;
			gb->sx = sx;
			continue;
		}

		/* Register yoffsets for fixup lists */
		yfixup = &yfixups[0];

		if (!hscrolled_fixed &&
		    rev_hscrolled >= offset &&
		    rev_hscrolled < offset + gb->block_size)
		{
			hscrolled_delta = gb->block_size - 1
				- (rev_hscrolled - offset);
			*(yfixup++) = &hscrolled_delta;
		}

		if (!cy_fixed &&
		    offset <= cy && cy < offset + gb->block_size)
		{
			cy_delta = gb->block_size - 1 - (cy - offset);
			*(yfixup++) = &cy_delta;
			log_debug("%s: prep cy_delta %u", __func__, cy_delta);
		}

		*yfixup = NULL;

		new_gb = grid_block_reflow(gb, sx, yfixups);

		/* Apply fixups to y offsets */

		yfixup = &yfixups[0];
		while (*yfixup != NULL) {
			if (*yfixups == &cy_delta) {
				log_debug("%s: old cy - %u [%u, %u, %u]", __func__, cy,
					  reflow_offset, new_gb->block_size, cy_delta);
				cy = reflow_offset +
					(new_gb->block_size - 1 - cy_delta);
				log_debug("%s: new cy - %u", __func__, cy);
				cy_fixed = 1;
			} else if (*yfixups == &hscrolled_delta) {
				gd->hscrolled = total - (reflow_offset +
					 (new_gb->block_size - 1
					  - hscrolled_delta));
				hscrolled_fixed = 1;
			}
			yfixup++;
		}

		/*
		 * Relocate the new block content to our old one,
		 * and free its descriptor.
		 */
		free(gb->linedata);
		offset += gb->block_size;
		reflow_offset += new_gb->block_size;
		hsize_diff += new_gb->block_size - gb->block_size;
		gd->hallocated += new_gb->block_size - gb->block_size;
		log_debug("%d: new hsize: %d\n", __LINE__, gd->hsize);
		gb->linedata = new_gb->linedata;
		gb->block_size = new_gb->block_size;
		gb->sx = sx;
		free(new_gb);
	}

	grid_reflow_apply_hsize_diff(gd, hsize_diff);

	/*
	 * Update scrolled and cursor positions.
	 */
	if (gd->hscrolled > gd->hsize)
		gd->hscrolled = gd->hsize;

	if (cy >= gd->sy)
		*cursor = 0;
	else
		*cursor = gd->sy - 1 - cy;
	gd->reflowing = 0;

	log_debug("grid %p: sx=%d sy=%d hscrolled=%d hsize=%d hlimit=%d\n",
		  gd,
		  gd->sx, gd->sy, gd->hscrolled, gd->hsize, gd->hlimit);

	gettimeofday(&tv, NULL);
	timersub(&tv, &start, &tv);
	log_debug("%s: now %u lines (in %llu.%06u seconds)", __func__,
	    gd->hsize + gd->sy, (unsigned long long)tv.tv_sec,
	    (u_int)tv.tv_usec);
}
