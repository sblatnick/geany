/*
 *      editor.h - this file is part of Geany, a fast and lightweight IDE
 *
 *      Copyright 2005-2008 Enrico Tröger <enrico(dot)troeger(at)uvena(dot)de>
 *      Copyright 2006-2008 Nick Treleaven <nick(dot)treleaven(at)btinternet(dot)com>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id$
 */

/**
 *  @file editor.h
 *  Callbacks for the Scintilla widget (ScintillaObject).
 *  Most important is the sci-notify callback, handled in on_editor_notification().
 *  This includes auto-indentation, comments, auto-completion, calltips, etc.
 *  Also some general Scintilla-related functions.
 **/


#ifndef GEANY_SCI_CB_H
#define GEANY_SCI_CB_H 1

#include "Scintilla.h"
#include "ScintillaWidget.h"

#define GEANY_WORDCHARS					"_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
#define GEANY_TOGGLE_MARK				"~ "
#define GEANY_MAX_WORD_LENGTH			192
#define GEANY_MAX_AUTOCOMPLETE_WORDS	30

/* Note: Avoid using SSM in files not related to scintilla, use sciwrappers.h instead. */
#define SSM(s, m, w, l) scintilla_send_message(s, m, w, l)


typedef enum
{
	INDENT_NONE = 0,
	INDENT_BASIC,
	INDENT_CURRENTCHARS,
	INDENT_MATCHBRACES
} IndentMode;

/* These are the default prefs when creating a new editor window.
 * Some of these can be overridden per document.
 * Remember to increment abi_version in plugindata.h when changing items. */
typedef struct GeanyEditorPrefs
{
	/* display */
	gboolean	show_white_space;
	gboolean	show_indent_guide;
	gboolean	show_line_endings;
	gint		long_line_type;
	gint		long_line_column;
	gchar		*long_line_color;
	gboolean	show_markers_margin;		/* view menu */
	gboolean	show_linenumber_margin;		/* view menu */
	gboolean	show_scrollbars;			/* hidden pref */
	gboolean	scroll_stop_at_last_line;	/* hidden pref */

	/* behaviour */
	gboolean	line_wrapping;
	gboolean	use_indicators;
	gboolean	folding;
	gboolean	unfold_all_children;
	gint		tab_width;
	gboolean	use_tabs;
	gboolean	use_tab_to_indent;	/* hidden pref */
	IndentMode	indent_mode;
	gboolean	disable_dnd;
	gboolean	smart_home_key;
	gboolean	newline_strip;
	gboolean	auto_complete_symbols;
	gboolean	auto_close_xml_tags;
	gboolean	complete_snippets;
	gint		symbolcompletion_min_chars;
	gint		symbolcompletion_max_height;
	GHashTable	*snippets;
	gboolean	brace_match_ltgt;	/* whether to highlight < and > chars (hidden pref) */
	gboolean	use_gtk_word_boundaries;	/* hidden pref */
	gboolean	complete_snippets_whilst_editing;	/* hidden pref */
	gboolean	detect_tab_mode;
	gint		line_break_column;
	gboolean	auto_continue_multiline;
} GeanyEditorPrefs;

extern GeanyEditorPrefs editor_prefs;


typedef struct
{
	gchar	*current_word;	/* holds word under the mouse or keyboard cursor */
	gint	click_pos;		/* text position where the mouse was clicked */
} EditorInfo;

extern EditorInfo editor_info;




gboolean on_editor_button_press_event(GtkWidget *widget, GdkEventButton *event,
	gpointer user_data);

void on_editor_notification(GtkWidget* editor, gint scn, gpointer lscn, gpointer user_data);

gboolean editor_start_auto_complete(GeanyDocument *doc, gint pos, gboolean force);

void editor_close_block(GeanyDocument *doc, gint pos);

gboolean editor_complete_snippet(GeanyDocument *doc, gint pos);

void editor_auto_latex(GeanyDocument *doc, gint pos);

void editor_show_macro_list(ScintillaObject *sci);

gboolean editor_show_calltip(GeanyDocument *doc, gint pos);

void editor_do_comment_toggle(GeanyDocument *doc);

void editor_do_comment(GeanyDocument *doc, gint line, gboolean allow_empty_lines, gboolean toggle);

gint editor_do_uncomment(GeanyDocument *doc, gint line, gboolean toggle);

void editor_highlight_braces(ScintillaObject *sci, gint cur_pos);

gboolean editor_lexer_is_c_like(gint lexer);

gint editor_lexer_get_type_keyword_idx(gint lexer);

void editor_insert_multiline_comment(GeanyDocument *doc);

void editor_insert_alternative_whitespace(GeanyDocument *doc);

void editor_smart_line_indentation(GeanyDocument *doc, gint pos);

void editor_indentation_by_one_space(GeanyDocument *doc, gint pos, gboolean decrease);

gboolean editor_line_in_view(ScintillaObject *sci, gint line);

void editor_scroll_to_line(ScintillaObject *sci, gint line, gfloat percent_of_view);

void editor_display_current_line(GeanyDocument *doc, gfloat percent_of_view);

void editor_finalize(void);


/* General editing functions */

void editor_find_current_word(ScintillaObject *sci, gint pos, gchar *word, size_t wordlen,
	const gchar *wc);

gchar *editor_get_default_selection(GeanyDocument *doc, gboolean use_current_word, const gchar *wordchars);

void editor_select_word(ScintillaObject *sci);

void editor_select_lines(ScintillaObject *sci, gboolean extra_line);

void editor_select_paragraph(ScintillaObject *sci);

void editor_set_indicator_on_line(GeanyDocument *doc, gint line);

void editor_set_indicator(GeanyDocument *doc, gint start, gint end);

void editor_clear_indicators(GeanyDocument *doc);

void editor_set_font(GeanyDocument *doc, const gchar *font_name, gint size);

const gchar *editor_get_eol_char_name(GeanyDocument *doc);

gint editor_get_eol_char_len(GeanyDocument *doc);

const gchar *editor_get_eol_char(GeanyDocument *doc);

void editor_fold_all(GeanyDocument *doc);

void editor_unfold_all(GeanyDocument *doc);

void editor_replace_tabs(GeanyDocument *doc);

void editor_replace_spaces(GeanyDocument *doc);

void editor_strip_line_trailing_spaces(GeanyDocument *doc, gint line);

void editor_strip_trailing_spaces(GeanyDocument *doc);

void editor_ensure_final_newline(GeanyDocument *doc);

void editor_insert_color(GeanyDocument *doc, const gchar *colour);

void editor_set_use_tabs(GeanyDocument *doc, gboolean use_tabs);

void editor_set_line_wrapping(GeanyDocument *doc, gboolean wrap);

gboolean editor_goto_pos(GeanyDocument *doc, gint pos, gboolean mark);

gboolean on_editor_scroll_event(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);

#endif
