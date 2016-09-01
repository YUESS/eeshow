/*
 * gui/sheet.c - Sheet navigation
 *
 * Written 2016 by Werner Almesberger
 * Copyright 2016 by Werner Almesberger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

#include <gtk/gtk.h>

#include "file/git-hist.h"
#include "kicad/sch.h"
#include "kicad/delta.h"
#include "gui/fmt-pango.h"
#include "gui/style.h"
#include "gui/aoi.h"
#include "gui/over.h"
#include "gui/input.h"
#include "gui/help.h"
#include "gui/icons.h"
#include "gui/timer.h"
#include "gui/common.h"


#define	ZOOM_FACTOR	1.26	/* 2^(1/3) */
#define	ZOOM_MAX	1
#define	ZOOM_MIN_SIZE	16
#define	ZOOM_MARGIN	0.95


/* ----- Tools ------------------------------------------------------------- */


static void canvas_coord(const struct gui *gui,
    int ex, int ey, int *x, int *y)
{
	GtkAllocation alloc;
	int sx, sy;

	gtk_widget_get_allocation(gui->da, &alloc);
	sx = ex - alloc.width / 2;
	sy = ey - alloc.height / 2;
	*x = sx / gui->scale + gui->x;
	*y = sy / gui->scale + gui->y;
}


/* ----- Zoom -------------------------------------------------------------- */


static bool zoom_in(struct gui *gui, int x, int y)
{
	float old = gui->scale;

	if (gui->scale == ZOOM_MAX)
		return 0;
	gui->scale *= ZOOM_FACTOR;
	if (gui->scale > ZOOM_MAX)
		gui->scale = ZOOM_MAX;
	gui->x = x + (gui->x - x) * old / gui->scale;
	gui->y = y + (gui->y - y) * old / gui->scale;
	redraw(gui);
	return 1;
}


static bool zoom_out(struct gui *gui, int x, int y)
{
	float old = gui->scale;

	if (gui->curr_sheet->w * gui->scale <= ZOOM_MIN_SIZE)
		return 0;
	gui->scale /= ZOOM_FACTOR;
	if (gui->curr_sheet->w <= ZOOM_MIN_SIZE)
		gui->scale = 1;
	else if (gui->curr_sheet->w * gui->scale <= ZOOM_MIN_SIZE )
		gui->scale = (float) ZOOM_MIN_SIZE / gui->curr_sheet->w;
	gui->x = x + (gui->x - x) * old / gui->scale;
	gui->y = y + (gui->y - y) * old / gui->scale;
	redraw(gui);
	return 1;
}


static void curr_sheet_size(struct gui *gui, int *w, int *h)
{
	const struct gui_sheet *sheet = gui->curr_sheet;
	int ax1, ay1, bx1, by1;

	if (!gui->old_hist) {
		*w = sheet->w;
		*h = sheet->h;
	} else {
		const struct gui_sheet *old =
		    find_corresponding_sheet(gui->old_hist->sheets,
		    gui->new_hist->sheets, sheet);

		/*
		 * We're only interested in differences here, so no need for
		 * the usual "-1" in x1 = x0 + w - 1
		 */
		ax1 = sheet->xmin + sheet->w;
		ay1 = sheet->ymin + sheet->h;
		bx1 = old->xmin + old->w;
		by1 = old->ymin + old->h;
		*w = (ax1 > bx1 ? ax1 : bx1) -
		    (sheet->xmin < old->xmin ? sheet->xmin : old->xmin);
		*h = (ay1 > by1 ? ay1 : by1) -
		    (sheet->ymin < old->ymin ? sheet->ymin : old->ymin);
	}
}


void zoom_to_extents(struct gui *gui)
{
	GtkAllocation alloc;
	int w, h;
	float sw, sh;

	curr_sheet_size(gui, &w, &h);
	gui->x = w / 2;
	gui->y = h / 2;

	gtk_widget_get_allocation(gui->da, &alloc);
	
	sw = w ? (float) ZOOM_MARGIN * alloc.width / w : 1;
	sh = h ? (float) ZOOM_MARGIN * alloc.height / h : 1;
	gui->scale = sw < sh ? sw : sh;

	redraw(gui);
}


/* ----- Revision selection overlays --------------------------------------- */


static bool show_history_details(void *user, bool on, int dx, int dy)
{
	struct gui_hist *h = user;
	struct gui *gui = h->gui;
	char *s;

	if (on) {
		s = vcs_git_long_for_pango(h->vcs_hist, fmt_pango);
		overlay_text_raw(h->over, s);
		free(s);
	} else {
		overlay_text(h->over, "%.40s", vcs_git_summary(h->vcs_hist));
	}
	redraw(gui);
	return 1;
}


static void set_diff_mode(struct gui *gui, enum diff_mode mode)
{
	gui->diff_mode = mode;
	do_revision_overlays(gui);
	redraw(gui);
}


static void show_history_cb(void *user)
{
	struct gui_hist *h = user;
	struct gui *gui = h->gui;
	enum selecting sel = sel_only;

	if (gui->old_hist) {
		if (h == gui->new_hist && gui->diff_mode != diff_new) {
			set_diff_mode(gui, diff_new);
			return;
		}
		if (h == gui->old_hist && gui->diff_mode != diff_old) {
			set_diff_mode(gui, diff_old);
			return;
		}
		sel = h == gui->new_hist ? sel_new : sel_old;
	}
	show_history(gui, sel);
}


static void show_diff_cb(void *user)
{
	struct gui *gui = user;

	if (gui->old_hist)
		set_diff_mode(gui,
		    gui->diff_mode == diff_delta ? diff_new : diff_delta);
	else
		show_history(gui, sel_split);
}


static void toggle_old_new(struct gui *gui)
{
	set_diff_mode(gui, gui->diff_mode == diff_new ? diff_old : diff_new);
}


static void add_delta(struct gui *gui)
{
	struct overlay *over;
	struct overlay_style style;

	over = overlay_add(&gui->hist_overlays, &gui->aois,
	    NULL, show_diff_cb, gui);
	style = overlay_style_default;
	if (gui->old_hist && gui->diff_mode == diff_delta)
		style.frame = RGBA(0, 0, 0, 1);
	style.pad = 4;
	overlay_style(over, &style);
	if (use_delta)
		overlay_icon(over, icon_delta);
	else
		overlay_icon(over, icon_diff);
}


static void revision_overlays_diff(struct gui *gui)
{
	struct gui_hist *new = gui->new_hist;
	struct gui_hist *old = gui->old_hist;
	struct overlay_style style;

	new->over = overlay_add(&gui->hist_overlays, &gui->aois,
	    show_history_details, show_history_cb, new);
	style = overlay_style_diff_new;
	if (gui->diff_mode == diff_new)
		style.frame = RGBA(0, 0, 0, 1);
	overlay_style(new->over, &style);
	show_history_details(new, 0, 0, 0);

	add_delta(gui);

	old->over = overlay_add(&gui->hist_overlays, &gui->aois,
	    show_history_details, show_history_cb, old);
	style = overlay_style_diff_old;
	if (gui->diff_mode == diff_old)
		style.frame = RGBA(0, 0, 0, 1);
	overlay_style(old->over, &style);
	show_history_details(old, 0, 0, 0);
}


void do_revision_overlays(struct gui *gui)
{
	overlay_remove_all(&gui->hist_overlays);

	if (gui->old_hist) {
		revision_overlays_diff(gui);
	} else {
		gui->new_hist->over = overlay_add(&gui->hist_overlays,
		    &gui->aois, show_history_details, show_history_cb,
		    gui->new_hist);
		overlay_style(gui->new_hist->over, &overlay_style_default);
		show_history_details(gui->new_hist, 0, 0, 0);

		add_delta(gui);
	}
}


/* ----- Sheet selection overlays ------------------------------------------ */


static struct gui_sheet *find_parent_sheet(struct gui_sheet *sheets,
    const struct gui_sheet *ref)
{
	struct gui_sheet *parent;
	const struct sch_obj *obj;

	for (parent = sheets; parent; parent = parent->next)
		for (obj = parent->sch->objs; obj; obj = obj->next)
			if (obj->type == sch_obj_sheet &&
			    obj->u.sheet.sheet == ref->sch)
				return parent;
	return NULL;
}


static void close_subsheet(void *user)
{
	struct gui_sheet *sheet = user;
	struct gui *gui = sheet->gui;
	struct gui_sheet *parent;

	parent = find_parent_sheet(gui->new_hist->sheets, sheet);
	if (parent)
		go_to_sheet(gui, parent);
	else
		show_index(gui);
}


static bool hover_sheet(void *user, bool on, int dx, int dy)
{
	struct gui_sheet *sheet = user;
	struct gui *gui = sheet->gui;
	const char *title = sheet->sch->title;

	if (!title)
		title = "(unnamed)";
	if (on) {
		const struct gui_sheet *s;
		int n = 0, this = -1;

		for (s = gui->new_hist->sheets; s; s = s->next) {
			n++;
			if (s == sheet)
				this = n;
		}
		overlay_text(sheet->over, "<b>%s</b>\n<big>%d / %d</big>%s%s",
		    title, this, n,
		    sheet->sch->file ? "\n" : "",
		    sheet->sch->file ? sheet->sch->file : "");
	} else {
		overlay_text(sheet->over, "<b>%s</b>", title);
	}
	redraw(gui);
	return 1;
}


static void sheet_selector_recurse(struct gui *gui, struct gui_sheet *sheet)
{
	struct gui_sheet *parent;

	parent = find_parent_sheet(gui->new_hist->sheets, sheet);
	if (parent)
		sheet_selector_recurse(gui, parent);
	sheet->over = overlay_add(&gui->sheet_overlays, &gui->aois,
	    hover_sheet, close_subsheet, sheet);
	hover_sheet(sheet, 0, 0, 0);
}


static void do_sheet_overlays(struct gui *gui)
{
	overlay_remove_all(&gui->sheet_overlays);
	sheet_selector_recurse(gui, gui->curr_sheet);
}


/* ----- Navigate sheets --------------------------------------------------- */


void go_to_sheet(struct gui *gui, struct gui_sheet *sheet)
{
	aoi_dehover();
	overlay_remove_all(&gui->pop_overlays);
	overlay_remove_all(&gui->pop_underlays);
	if (!sheet->rendered) {
		render_sheet(sheet);
		mark_aois(gui, sheet);
	}
	gui->curr_sheet = sheet;
	if (gui->old_hist)
		render_delta(gui);
	if (gui->vcs_history && !vcs_is_empty(gui->vcs_history))
		do_revision_overlays(gui);
	do_sheet_overlays(gui);
	zoom_to_extents(gui);
}


static bool go_up_sheet(struct gui *gui)
{
	struct gui_sheet *parent;

	parent = find_parent_sheet(gui->new_hist->sheets, gui->curr_sheet);
	if (!parent)
		return 0;
	go_to_sheet(gui, parent);
	return 1;
}


static bool go_prev_sheet(struct gui *gui)
{
	struct gui_sheet *sheet;

	for (sheet = gui->new_hist->sheets; sheet; sheet = sheet->next)
		if (sheet->next && sheet->next == gui->curr_sheet) {
			go_to_sheet(gui, sheet);
			return 1;
		}
	return 0;
}


static bool go_next_sheet(struct gui *gui)
{
	if (!gui->curr_sheet->next)
		return 0;
	go_to_sheet(gui, gui->curr_sheet->next);
	return 1;
}


/* ----- Input ------------------------------------------------------------- */


static bool sheet_click(void *user, int x, int y)
{
	struct gui *gui = user;
	const struct gui_sheet *curr_sheet = gui->curr_sheet;
	int ex, ey;

	canvas_coord(gui, x, y, &ex, &ey);

	if (gui->old_hist && gui->diff_mode == diff_old)
		curr_sheet = find_corresponding_sheet(gui->old_hist->sheets,
		    gui->new_hist->sheets, gui->curr_sheet);

	if (aoi_click(&gui->aois, x, y))
		return 1;
	if (aoi_click(&curr_sheet->aois,
	    ex + curr_sheet->xmin, ey + curr_sheet->ymin))
		return 1;

	overlay_remove_all(&gui->pop_overlays);
	overlay_remove_all(&gui->pop_underlays);
	redraw(gui);
	return 1;
}


static bool sheet_hover_update(void *user, int x, int y)
{
	struct gui *gui = user;
	const struct gui_sheet *curr_sheet = gui->curr_sheet;
	int ex, ey;

	canvas_coord(gui, x, y, &ex, &ey);

	if (gui->old_hist && gui->diff_mode == diff_old)
		curr_sheet = find_corresponding_sheet(gui->old_hist->sheets,
		    gui->new_hist->sheets, gui->curr_sheet);

	if (aoi_hover(&gui->aois, x, y))
		return 1;
	return aoi_hover(&curr_sheet->aois,
	    ex + curr_sheet->xmin, ey + curr_sheet->ymin);
}


static bool sheet_drag_begin(void *user, int x, int y)
{
	dehover_glabel(user);
	return 1;
}


static void sheet_drag_move(void *user, int dx, int dy)
{
	struct gui *gui = user;

	gui->x -= dx / gui->scale;
	gui->y -= dy / gui->scale;
	redraw(gui);
}


static void sheet_drag_end(void *user)
{
	input_update();
}


static void sheet_scroll(void *user, int x, int y, int dy)
{
	struct gui *gui = user;
	int ex, ey;


	canvas_coord(gui, x, y, &ex, &ey);
	if (dy < 0) {
		if (!zoom_in(gui, ex, ey))
			return;
	} else {
		if (!zoom_out(gui, ex, ey))
			return;
	}
	dehover_glabel(gui);
	input_update();
}


static void sheet_key(void *user, int x, int y, int keyval)
{
	struct gui *gui = user;
	struct gui_sheet *sheet = gui->curr_sheet;
	int ex, ey;

	canvas_coord(gui, x, y, &ex, &ey);

	switch (keyval) {
	case '+':
	case '=':
	case GDK_KEY_KP_Add:
	case GDK_KEY_KP_Equal:
		zoom_in(gui, x, y);
		break;
	case '-':
	case GDK_KEY_KP_Subtract:
		zoom_out(gui, x, y);
		break;
	case '*':
	case GDK_KEY_KP_Multiply:
		zoom_to_extents(gui);
		break;

	case GDK_KEY_Home:
	case GDK_KEY_KP_Home:
		if (sheet != gui->new_hist->sheets)
			go_to_sheet(gui, gui->new_hist->sheets);
		break;
	case GDK_KEY_BackSpace:
	case GDK_KEY_Delete:
	case GDK_KEY_KP_Delete:
		go_up_sheet(gui);
		break;
	case GDK_KEY_Page_Up:
	case GDK_KEY_KP_Page_Up:
		go_prev_sheet(gui);
		break;
	case GDK_KEY_Page_Down:
	case GDK_KEY_KP_Page_Down:
		go_next_sheet(gui);
		break;
	case GDK_KEY_Up:
	case GDK_KEY_KP_Up:
		show_history(gui, sel_new);
		break;
	case GDK_KEY_Down:
	case GDK_KEY_KP_Down:
		show_history(gui, sel_old);
		break;
	case GDK_KEY_Tab:
	case GDK_KEY_KP_Tab:
		toggle_old_new(gui);
		break;

	case GDK_KEY_Escape:
		dehover_glabel(user);
		gui->glabel = NULL;
		redraw(gui);
		break;

	case GDK_KEY_e:
		show_extra = !show_extra;
		redraw(gui);
		break;

	case GDK_KEY_n:
		gui->diff_mode = diff_new;
		do_revision_overlays(gui);
		redraw(gui);
		break;
	case GDK_KEY_o:
		gui->diff_mode = diff_old;
		do_revision_overlays(gui);
		redraw(gui);
		break;
	case GDK_KEY_d:
		gui->diff_mode = diff_delta;
		do_revision_overlays(gui);
		redraw(gui);
		break;
	case GDK_KEY_D:	/* Shift + D */
		use_delta = !use_delta;
		do_revision_overlays(gui);
		redraw(gui);
		break;

	case GDK_KEY_h:
	case GDK_KEY_Help:
		help();
		break;

	case GDK_KEY_t:
		timer_toggle();
		redraw(gui);
		break;

	case GDK_KEY_q:
		gtk_main_quit();
	}
}


static const struct input_ops sheet_input_ops = {
	.click		= sheet_click,
	.hover_begin	= sheet_hover_update,
	.hover_update	= sheet_hover_update,
	.hover_click	= sheet_click,
	.scroll		= sheet_scroll,
	.drag_begin	= sheet_drag_begin,
	.drag_move	= sheet_drag_move,
	.drag_end	= sheet_drag_end,
	.key		= sheet_key,
};


/* ----- Initialization ---------------------------------------------------- */


void sheet_setup(struct gui *gui)
{
	input_push(&sheet_input_ops, gui);
}
