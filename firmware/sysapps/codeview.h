/* SPDX-License-Identifier: Apache-2.0
 * Code view (plan 8.4): a monospace text editor with Python syntax colours,
 * line numbers and a cursor, for the Editor.
 *
 * Only the visible lines are drawn (one label draw per line, with LVGL's
 * recolour markup made on the fly), so a long file costs nothing per frame.
 * Typing comes through a 1 px text area kept at the cursor: the on-screen
 * keyboard and a USB keyboard both type into it, and the view applies each
 * key to its own buffer. With a USB keyboard: arrows, Home/End, Page Up and
 * Down, Tab and Shift-Tab (indent), Enter (keeps the indent, adds a level
 * after ':'), Backspace (removes a whole indent level inside indentation).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

typedef struct cv cv_t;

/* A scrolling view filling its parent's width; the cv_t is its user data. */
lv_obj_t *cv_create(lv_obj_t *parent);
cv_t *cv_get(lv_obj_t *view);
/* Replace the text (marks it clean). */
bool cv_set_text(cv_t *cv, const char *text, size_t len);
/* The text, malloc'd, '\n' line ends and a final newline. */
char *cv_text(cv_t *cv, size_t *len);
bool cv_dirty(const cv_t *cv);
void cv_set_clean(cv_t *cv);
/* Called (UI task) after every change and cursor move. */
void cv_set_changed_cb(cv_t *cv, void (*cb)(cv_t *cv, void *arg), void *arg);
/* Take the keyboard (shows the on-screen one if no USB keyboard). */
void cv_focus(cv_t *cv);
void cv_cursor(const cv_t *cv, int *line, int *col);
void cv_goto_line(cv_t *cv, int line);
int cv_line_count(const cv_t *cv);
