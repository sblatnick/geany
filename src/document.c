/*
 *      document.c - this file is part of Geany, a fast and lightweight IDE
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
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * $Id$
 */

/*
 *  Document related actions: new, save, open, etc.
 *  Also Scintilla search actions.
 */

#include "geany.h"

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#include <time.h>

#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#include <ctype.h>
#include <stdlib.h>

/* gstdio.h also includes sys/stat.h */
#include <glib/gstdio.h>

#include "document.h"
#include "documentprivate.h"
#include "filetypes.h"
#include "support.h"
#include "sciwrappers.h"
#include "editor.h"
#include "dialogs.h"
#include "msgwindow.h"
#include "templates.h"
#include "treeviews.h"
#include "ui_utils.h"
#include "utils.h"
#include "encodings.h"
#include "notebook.h"
#include "main.h"
#include "vte.h"
#include "build.h"
#include "symbols.h"
#include "callbacks.h"
#include "geanyobject.h"
#include "highlighting.h"
#include "navqueue.h"
#include "win32.h"
#include "search.h"


GeanyFilePrefs file_prefs;

GPtrArray *documents_array;


/* an undo action, also used for redo actions */
typedef struct
{
	GTrashStack *next;	/* pointer to the next stack element(required for the GTrashStack) */
	guint type;			/* to identify the action */
	gpointer *data; 	/* the old value (before the change), in case of a redo action
						 * it contains the new value */
} undo_action;


/* Whether to colourise the document straight after styling settings are changed.
 * (e.g. when filetype is set or typenames are updated) */
static gboolean delay_colourise = FALSE;


static void document_undo_clear(GeanyDocument *doc);
static void document_redo_add(GeanyDocument *doc, guint type, gpointer data);

static gboolean update_type_keywords(ScintillaObject *sci, gint lang);


/* ignore the case of filenames and paths under WIN32, causes errors if not */
#ifdef G_OS_WIN32
#define filenamecmp(a,b)	strcasecmp((a), (b))
#else
#define filenamecmp(a,b)	strcmp((a), (b))
#endif

/**
 * Find and retrieve the index of the given filename in the %document list.
 *
 * @param realname The filename to search, which should be identical to the
 * string returned by @c tm_get_real_path().
 *
 * @return The matching document, or NULL.
 * @note This is only really useful when passing a @c TMWorkObject::file_name.
 * @see document_find_by_filename().
 **/
GeanyDocument* document_find_by_real_path(const gchar *realname)
{
	guint i;

	if (! realname)
		return NULL;	/* file doesn't exist on disk */

	for (i = 0; i < documents_array->len; i++)
	{
		GeanyDocument *doc = documents[i];

		if (! documents[i]->is_valid || ! doc->real_path) continue;

		if (filenamecmp(realname, doc->real_path) == 0)
		{
			return doc;
		}
	}
	return NULL;
}


/* dereference symlinks, /../ junk in path and return locale encoding */
static gchar *get_real_path_from_utf8(const gchar *utf8_filename)
{
	gchar *locale_name = utils_get_locale_from_utf8(utf8_filename);
	gchar *realname = tm_get_real_path(locale_name);

	g_free(locale_name);
	return realname;
}


/**
 *  Find and retrieve the index of the given filename in the %document list.
 *  This matches either an exact GeanyDocument::file_name string, or variant
 *  filenames with relative elements in the path (e.g. @c "/dir/..//name" will
 *  match @c "/name").
 *
 *  @param utf8_filename The filename to search (in UTF-8 encoding).
 *
 *  @return The matching document, or NULL.
 *  @see document_find_by_real_path().
 **/
GeanyDocument *document_find_by_filename(const gchar *utf8_filename)
{
	guint i;
	GeanyDocument *doc;
	gchar *realname;

	if (! utf8_filename)
		return NULL;

	/* First search GeanyDocument::file_name, so we can find documents with a
	 * filename set but not saved on disk, like vcdiff produces */
	for (i = 0; i < documents_array->len; i++)
	{
		doc = documents[i];

		if (! documents[i]->is_valid || doc->file_name == NULL) continue;

		if (filenamecmp(utf8_filename, doc->file_name) == 0)
		{
			return doc;
		}
	}
	/* Now try matching based on the realpath(), which is unique per file on disk */
	realname = get_real_path_from_utf8(utf8_filename);
	doc = document_find_by_real_path(realname);
	g_free(realname);
	return doc;
}


/* returns the document which has sci, or NULL. */
GeanyDocument *document_find_by_sci(ScintillaObject *sci)
{
	guint i;

	if (sci == NULL)
		return NULL;

	for (i = 0; i < documents_array->len; i++)
	{
		if (documents[i]->is_valid && documents[i]->sci == sci)
			return documents[i];
	}
	return NULL;
}


/* returns the index of the notebook page from the document index */
gint document_get_notebook_page(GeanyDocument *doc)
{
	if (doc == NULL)
		return -1;

	return gtk_notebook_page_num(GTK_NOTEBOOK(main_widgets.notebook),
		GTK_WIDGET(doc->sci));
}


/**
 *  Find and retrieve the index of the given notebook page @a page_num in the %document list.
 *
 *  @param page_num The notebook page number to search.
 *
 *  @return The corresponding document for the given notebook page, or NULL.
 **/
GeanyDocument *document_get_from_page(guint page_num)
{
	ScintillaObject *sci;

	if (page_num >= documents_array->len)
		return NULL;

	sci = (ScintillaObject*)gtk_notebook_get_nth_page(
				GTK_NOTEBOOK(main_widgets.notebook), page_num);

	return document_find_by_sci(sci);
}


/**
 *  Find and retrieve the current %document.
 *
 *  @return A pointer to the current %document or @c NULL if there are no opened documents.
 **/
GeanyDocument *document_get_current(void)
{
	gint cur_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(main_widgets.notebook));

	if (cur_page == -1)
		return NULL;
	else
	{
		ScintillaObject *sci = (ScintillaObject*)
			gtk_notebook_get_nth_page(GTK_NOTEBOOK(main_widgets.notebook), cur_page);

		return document_find_by_sci(sci);
	}
}


void document_init_doclist()
{
	documents_array = g_ptr_array_new();
}


void document_finalize()
{
	g_ptr_array_free(documents_array, TRUE);
}


/**
 *  Update the tab labels, the status bar, the window title and some save-sensitive buttons
 *  according to the document's save state.
 *  This is called by Geany mostly when opening or saving files.
 *
 * @param doc The document to use.
 * @param changed Whether the document state should indicate changes have been made.
 **/
void document_set_text_changed(GeanyDocument *doc, gboolean changed)
{
	if (doc == NULL)
		return;

	doc->changed = changed;

	if (! main_status.quitting)
	{
		ui_update_tab_status(doc);
		ui_save_buttons_toggle(changed);
		ui_set_window_title(doc);
		ui_update_statusbar(doc, -1);
	}
}


/* Apply just the prefs that can change in the Preferences dialog */
void document_apply_update_prefs(GeanyDocument *doc)
{
	ScintillaObject *sci;

	g_return_if_fail(doc != NULL);

	sci = doc->sci;

	sci_set_mark_long_lines(sci, editor_prefs.long_line_type,
		editor_prefs.long_line_column, editor_prefs.long_line_color);

	sci_set_tab_width(sci, editor_prefs.tab_width);

	sci_set_autoc_max_height(sci, editor_prefs.symbolcompletion_max_height);

	sci_set_indentation_guides(sci, editor_prefs.show_indent_guide);
	sci_set_visible_white_spaces(sci, editor_prefs.show_white_space);
	sci_set_visible_eols(sci, editor_prefs.show_line_endings);

	sci_set_folding_margin_visible(sci, editor_prefs.folding);

	doc->auto_indent = (editor_prefs.indent_mode != INDENT_NONE);

	sci_assign_cmdkey(sci, SCK_HOME,
		editor_prefs.smart_home_key ? SCI_VCHOMEWRAP : SCI_HOMEWRAP);
	sci_assign_cmdkey(sci, SCK_END,  SCI_LINEENDWRAP);
}


/* Sets is_valid to FALSE and initializes some members to NULL, to mark it uninitialized.
 * The flag is_valid is set to TRUE in document_create(). */
static void init_doc_struct(GeanyDocument *new_doc)
{
	Document *full_doc = DOCUMENT(new_doc);

	memset(full_doc, 0, sizeof(Document));

	new_doc->is_valid = FALSE;
	new_doc->has_tags = FALSE;
	new_doc->auto_indent = (editor_prefs.indent_mode != INDENT_NONE);
	new_doc->line_wrapping = editor_prefs.line_wrapping;
	new_doc->readonly = FALSE;
	new_doc->file_name = NULL;
	new_doc->file_type = NULL;
	new_doc->tm_file = NULL;
	new_doc->encoding = NULL;
	new_doc->has_bom = FALSE;
	new_doc->sci = NULL;
	new_doc->scroll_percent = -1.0F;
	new_doc->line_breaking = FALSE;
	new_doc->mtime = 0;
	new_doc->changed = FALSE;
	new_doc->last_check = time(NULL);
	new_doc->real_path = NULL;

	full_doc->tag_store = NULL;
	full_doc->tag_tree = NULL;
	full_doc->saved_encoding.encoding = NULL;
	full_doc->saved_encoding.has_bom = FALSE;
	full_doc->undo_actions = NULL;
	full_doc->redo_actions = NULL;
}


/* returns the next free place in the document list,
 * or -1 if the documents_array is full */
static gint document_get_new_idx(void)
{
	guint i;

	for (i = 0; i < documents_array->len; i++)
	{
		if (documents[i]->sci == NULL)
		{
			return (gint) i;
		}
	}
	return -1;
}


static void setup_sci_keys(ScintillaObject *sci)
{
	/* disable some Scintilla keybindings to be able to redefine them cleanly */
	sci_clear_cmdkey(sci, 'A' | (SCMOD_CTRL << 16)); /* select all */
	sci_clear_cmdkey(sci, 'D' | (SCMOD_CTRL << 16)); /* duplicate */
	sci_clear_cmdkey(sci, 'T' | (SCMOD_CTRL << 16)); /* line transpose */
	sci_clear_cmdkey(sci, 'T' | (SCMOD_CTRL << 16) | (SCMOD_SHIFT << 16)); /* line copy */
	sci_clear_cmdkey(sci, 'L' | (SCMOD_CTRL << 16)); /* line cut */
	sci_clear_cmdkey(sci, 'L' | (SCMOD_CTRL << 16) | (SCMOD_SHIFT << 16)); /* line delete */
	sci_clear_cmdkey(sci, SCK_UP | (SCMOD_CTRL << 16)); /* scroll line up */
	sci_clear_cmdkey(sci, SCK_DOWN | (SCMOD_CTRL << 16)); /* scroll line down */

	if (editor_prefs.use_gtk_word_boundaries)
	{
		/* use GtkEntry-like word boundaries */
		sci_assign_cmdkey(sci, SCK_RIGHT | (SCMOD_CTRL << 16), SCI_WORDRIGHTEND);
		sci_assign_cmdkey(sci, SCK_RIGHT | (SCMOD_CTRL << 16) | (SCMOD_SHIFT << 16), SCI_WORDRIGHTENDEXTEND);
		sci_assign_cmdkey(sci, SCK_DELETE | (SCMOD_CTRL << 16), SCI_DELWORDRIGHTEND);
	}
	sci_assign_cmdkey(sci, SCK_UP | (SCMOD_ALT << 16), SCI_LINESCROLLUP);
	sci_assign_cmdkey(sci, SCK_DOWN | (SCMOD_ALT << 16), SCI_LINESCROLLDOWN);
	sci_assign_cmdkey(sci, SCK_UP | (SCMOD_CTRL << 16), SCI_PARAUP);
	sci_assign_cmdkey(sci, SCK_UP | (SCMOD_CTRL << 16) | (SCMOD_SHIFT << 16), SCI_PARAUPEXTEND);
	sci_assign_cmdkey(sci, SCK_DOWN | (SCMOD_CTRL << 16), SCI_PARADOWN);
	sci_assign_cmdkey(sci, SCK_DOWN | (SCMOD_CTRL << 16) | (SCMOD_SHIFT << 16), SCI_PARADOWNEXTEND);

	sci_clear_cmdkey(sci, SCK_BACK | (SCMOD_ALT << 16)); /* clear Alt-Backspace (Undo) */
}


/* Create new editor (the scintilla widget) */
static ScintillaObject *create_new_sci(GeanyDocument *doc)
{
	ScintillaObject	*sci;

	sci = SCINTILLA(scintilla_new());
	scintilla_set_id(sci, doc->index);

	gtk_widget_show(GTK_WIDGET(sci));

	sci_set_codepage(sci, SC_CP_UTF8);
	/*SSM(sci, SCI_SETWRAPSTARTINDENT, 4, 0);*/
	/* disable scintilla provided popup menu */
	sci_use_popup(sci, FALSE);

	setup_sci_keys(sci);

	sci_set_tab_indents(sci, editor_prefs.use_tab_to_indent);
	sci_set_symbol_margin(sci, editor_prefs.show_markers_margin);
	sci_set_lines_wrapped(sci, editor_prefs.line_wrapping);
	sci_set_scrollbar_mode(sci, editor_prefs.show_scrollbars);
	sci_set_caret_policy_x(sci, CARET_JUMPS | CARET_EVEN, 0);
	/*sci_set_caret_policy_y(sci, CARET_JUMPS | CARET_EVEN, 0);*/
	SSM(sci, SCI_AUTOCSETSEPARATOR, '\n', 0);
	/* (dis)allow scrolling past end of document */
	SSM(sci, SCI_SETENDATLASTLINE, editor_prefs.scroll_stop_at_last_line, 0);
	SSM(sci, SCI_SETSCROLLWIDTHTRACKING, 1, 0);

	/* signal for insert-key(works without too, but to update the right status bar) */
	/*g_signal_connect((GtkWidget*) sci, "key-press-event",
					 G_CALLBACK(keybindings_got_event), GINT_TO_POINTER(new_idx));*/
	/* signal for the popup menu */
	g_signal_connect(G_OBJECT(sci), "button-press-event",
					G_CALLBACK(on_editor_button_press_event), doc);
	g_signal_connect(G_OBJECT(sci), "scroll-event",
					G_CALLBACK(on_editor_scroll_event), doc);
	g_signal_connect(G_OBJECT(sci), "motion-notify-event", G_CALLBACK(on_motion_event), NULL);

	return sci;
}


/* Creates a new document and editor, adding a tab in the notebook.
 * @return The index of the created document */
static GeanyDocument *document_create(const gchar *utf8_filename)
{
	PangoFontDescription *pfd;
	gchar *fname;
	GeanyDocument *this;
	gint new_idx;
	gint cur_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(main_widgets.notebook));

	if (cur_pages == 1)
	{
		GeanyDocument *doc = document_get_current();
		/* remove the empty document and open a new one */
		if (doc != NULL && doc->file_name == NULL && ! doc->changed)
			document_remove_page(0);
	}

	new_idx = document_get_new_idx();
	if (new_idx == -1)	/* expand the array, no free places */
	{
		Document *new_doc = g_new0(Document, 1);

		new_idx = documents_array->len;
		g_ptr_array_add(documents_array, new_doc);
	}
	this = documents[new_idx];
	init_doc_struct(this);	/* initialize default document settings */
	this->index = new_idx;

	this->file_name = g_strdup(utf8_filename);

	this->sci = create_new_sci(this);

	document_apply_update_prefs(this);

	pfd = pango_font_description_from_string(interface_prefs.editor_font);
	fname = g_strdup_printf("!%s", pango_font_description_get_family(pfd));
	editor_set_font(this, fname, pango_font_description_get_size(pfd) / PANGO_SCALE);
	pango_font_description_free(pfd);
	g_free(fname);

	treeviews_openfiles_add(this);	/* sets this->iter */

	notebook_new_tab(this);

	/* select document in sidebar */
	{
		GtkTreeSelection *sel;

		sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tv.tree_openfiles));
		gtk_tree_selection_select_iter(sel, &DOCUMENT(this)->iter);
	}

	ui_document_buttons_update();

	this->is_valid = TRUE;	/* do this last to prevent UI updating with NULL items. */
	return this;
}


/**
 *  Remove the given notebook tab at @a page_num and clear all related information
 *  in the document list.
 *
 *  @param page_num The notebook page number to remove.
 *
 *  @return @a TRUE if the document was actually removed or @a FALSE otherwise.
 **/
gboolean document_remove_page(guint page_num)
{
	GeanyDocument *doc = document_get_from_page(page_num);

	if (doc != NULL)
	{
		Document *fdoc = DOCUMENT(doc);

		if (doc->changed && ! dialogs_show_unsaved_file(doc))
		{
			return FALSE;
		}
		/* Checking real_path makes it likely the file exists on disk */
		if (! main_status.closing_all && doc->real_path != NULL)
			ui_add_recent_file(doc->file_name);

		notebook_remove_page(page_num);
		treeviews_remove_document(doc);
		navqueue_remove_file(doc->file_name);
		msgwin_status_add(_("File %s closed."), DOC_FILENAME(doc));
		g_free(doc->encoding);
		g_free(fdoc->saved_encoding.encoding);
		g_free(doc->file_name);
		g_free(doc->real_path);
		tm_workspace_remove_object(doc->tm_file, TRUE, TRUE);

		doc->is_valid = FALSE;
		doc->sci = NULL;
		doc->file_name = NULL;
		doc->real_path = NULL;
		doc->file_type = NULL;
		doc->encoding = NULL;
		doc->has_bom = FALSE;
		doc->tm_file = NULL;
		doc->scroll_percent = -1.0F;
		document_undo_clear(doc);
		if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(main_widgets.notebook)) == 0)
		{
			treeviews_update_tag_list(NULL, FALSE);
			/*on_notebook1_switch_page(GTK_NOTEBOOK(main_widgets.notebook), NULL, 0, NULL);*/
			ui_set_window_title(NULL);
			ui_save_buttons_toggle(FALSE);
			ui_document_buttons_update();
			build_menu_update(NULL);
		}
	}
	else
	{
		geany_debug("Error: page_num: %d", page_num);
		return FALSE;
	}

	return TRUE;
}


/* used to keep a record of the unchanged document state encoding */
static void store_saved_encoding(GeanyDocument *doc)
{
	Document *fdoc = DOCUMENT(doc);

	g_free(fdoc->saved_encoding.encoding);
	fdoc->saved_encoding.encoding = g_strdup(doc->encoding);
	fdoc->saved_encoding.has_bom = doc->has_bom;
}


/* Opens a new empty document only if there are no other documents open */
GeanyDocument *document_new_file_if_non_open()
{
	if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(main_widgets.notebook)) == 0)
		return document_new_file(NULL, NULL, NULL);

	return NULL;
}


/**
 *  Creates a new %document.
 *  After all, the "document-new" signal is emitted for plugins.
 *
 *  @param filename The file name in UTF-8 encoding, or @c NULL to open a file as "untitled".
 *  @param ft The filetype to set or @c NULL to detect it from @a filename if not @c NULL.
 *  @param text The initial content of the file (in UTF-8 encoding), or @c NULL.
 *
 *  @return The new document.
 **/
GeanyDocument *document_new_file(const gchar *filename, GeanyFiletype *ft, const gchar *text)
{
	GeanyDocument *doc = document_create(filename);

	g_assert(doc != NULL);

	sci_set_undo_collection(doc->sci, FALSE); /* avoid creation of an undo action */
	if (text)
		sci_set_text(doc->sci, text);
	else
		sci_clear_all(doc->sci);

	sci_set_eol_mode(doc->sci, file_prefs.default_eol_character);
	/* convert the eol chars in the template text in case they are different from
	 * from file_prefs.default_eol */
	if (text != NULL)
		sci_convert_eols(doc->sci, file_prefs.default_eol_character);

	editor_set_use_tabs(doc, editor_prefs.use_tabs);
	sci_set_undo_collection(doc->sci, TRUE);
	sci_empty_undo_buffer(doc->sci);

	doc->mtime = time(NULL);

	doc->encoding = g_strdup(encodings[file_prefs.default_new_encoding].charset);
	/* store the opened encoding for undo/redo */
	store_saved_encoding(doc);

	/*document_set_filetype(idx, (ft == NULL) ? filetypes[GEANY_FILETYPES_NONE] : ft);*/
	if (ft == NULL && filename != NULL) /* guess the filetype from the filename if one is given */
		ft = filetypes_detect_from_file(doc);

	document_set_filetype(doc, ft);	/* also clears taglist */
	if (ft == NULL)
		highlighting_set_styles(doc->sci, GEANY_FILETYPES_NONE);
	ui_set_window_title(doc);
	build_menu_update(doc);
	document_update_tag_list(doc, FALSE);
	document_set_text_changed(doc, FALSE);
	ui_document_show_hide(doc); /* update the document menu */

	sci_set_line_numbers(doc->sci, editor_prefs.show_linenumber_margin, 0);
	sci_goto_pos(doc->sci, 0, TRUE);

	/* "the" SCI signal (connect after initial setup(i.e. adding text)) */
	g_signal_connect((GtkWidget*) doc->sci, "sci-notify", G_CALLBACK(on_editor_notification), doc);

	if (geany_object)
	{
		g_signal_emit_by_name(geany_object, "document-new", doc);
	}

	msgwin_status_add(_("New file \"%s\" opened."),
		DOC_FILENAME(doc));

	return doc;
}


/**
 *  Open a %document specified by @a locale_filename.
 *  After all, the "document-open" signal is emitted for plugins.
 *
 *  When opening more than one file, either:
 *  -# Use document_open_files().
 *  -# Call document_delay_colourise() before document_open_file() and
 *     document_colourise_new() after opening all files.
 *
 *  This avoids unnecessary recolourising, saving significant processing when a lot of files
 *  are open of a %filetype that supports user typenames, e.g. C.
 *
 *  @param locale_filename The filename of the %document to load, in locale encoding.
 *  @param readonly Whether to open the %document in read-only mode.
 *  @param ft The %filetype for the %document or @c NULL to auto-detect the %filetype.
 *  @param forced_enc The file encoding to use or @c NULL to auto-detect the file encoding.
 *
 *  @return The document opened or NULL.
 **/
GeanyDocument *document_open_file(const gchar *locale_filename, gboolean readonly,
		GeanyFiletype *ft, const gchar *forced_enc)
{
	return document_open_file_full(NULL, locale_filename, 0, readonly, ft, forced_enc);
}


typedef struct
{
	gchar		*data;	/* null-terminated file data */
	gsize		 size;	/* actual file size on disk */
	gsize		 len;	/* string length of data */
	gchar		*enc;
	gboolean	 bom;
	time_t		 mtime;	/* modification time, read by stat::st_mtime */
	gboolean	 readonly;
} FileData;


/* reload file with specified encoding */
static gboolean
handle_forced_encoding(FileData *filedata, const gchar *forced_enc)
{
	GeanyEncodingIndex enc_idx;

	if (utils_str_equal(forced_enc, "UTF-8"))
	{
		if (! g_utf8_validate(filedata->data, filedata->len, NULL))
		{
			return FALSE;
		}
	}
	else
	{
		gchar *converted_text = encodings_convert_to_utf8_from_charset(
										filedata->data, filedata->len, forced_enc, FALSE);
		if (converted_text == NULL)
		{
			return FALSE;
		}
		else
		{
			g_free(filedata->data);
			filedata->data = converted_text;
			filedata->len = strlen(converted_text);
		}
	}
	enc_idx = encodings_scan_unicode_bom(filedata->data, filedata->size, NULL);
	filedata->bom = (enc_idx == GEANY_ENCODING_UTF_8);
	filedata->enc = g_strdup(forced_enc);
	return TRUE;
}


/* detect encoding and convert to UTF-8 if necessary */
static gboolean
handle_encoding(FileData *filedata)
{
	g_return_val_if_fail(filedata->enc == NULL, FALSE);
	g_return_val_if_fail(filedata->bom == FALSE, FALSE);

	if (filedata->size == 0)
	{
		/* we have no data so assume UTF-8, filedata->len can be 0 even we have an empty
		 * e.g. UTF32 file with a BOM(so size is 4, len is 0) */
		filedata->enc = g_strdup("UTF-8");
	}
	else
	{
		/* first check for a BOM */
		GeanyEncodingIndex enc_idx =
			encodings_scan_unicode_bom(filedata->data, filedata->size, NULL);

		if (enc_idx != GEANY_ENCODING_NONE)
		{
			filedata->enc = g_strdup(encodings[enc_idx].charset);
			filedata->bom = TRUE;

			if (enc_idx != GEANY_ENCODING_UTF_8) /* the BOM indicated something else than UTF-8 */
			{
				gchar *converted_text = encodings_convert_to_utf8_from_charset(
										filedata->data, filedata->size, filedata->enc, FALSE);
				if (converted_text != NULL)
				{
					g_free(filedata->data);
					filedata->data = converted_text;
					filedata->len = strlen(converted_text);
				}
				else
				{
					/* there was a problem converting data from BOM encoding type */
					g_free(filedata->enc);
					filedata->enc = NULL;
					filedata->bom = FALSE;
				}
			}
		}

		if (filedata->enc == NULL)	/* either there was no BOM or the BOM encoding failed */
		{
			/* try UTF-8 first */
			if (g_utf8_validate(filedata->data, filedata->len, NULL))
			{
				filedata->enc = g_strdup("UTF-8");
			}
			else
			{
				/* detect the encoding */
				gchar *converted_text = encodings_convert_to_utf8(filedata->data,
					filedata->size, &filedata->enc);

				if (converted_text == NULL)
				{
					return FALSE;
				}
				g_free(filedata->data);
				filedata->data = converted_text;
				filedata->len = strlen(converted_text);
			}
		}
	}
	return TRUE;
}


static void
handle_bom(FileData *filedata)
{
	guint bom_len;

	encodings_scan_unicode_bom(filedata->data, filedata->size, &bom_len);
	g_return_if_fail(bom_len != 0);

	/* use filedata->len here because the contents are already converted into UTF-8 */
	filedata->len -= bom_len;
	/* overwrite the BOM with the remainder of the file contents, plus the NULL terminator. */
	g_memmove(filedata->data, filedata->data + bom_len, filedata->len + 1);
	filedata->data = g_realloc(filedata->data, filedata->len + 1);
}


/* loads textfile data, verifies and converts to forced_enc or UTF-8. Also handles BOM. */
static gboolean load_text_file(const gchar *locale_filename, const gchar *utf8_filename,
		FileData *filedata, const gchar *forced_enc)
{
	GError *err = NULL;
	struct stat st;
	GeanyEncodingIndex tmp_enc_idx;

	filedata->data = NULL;
	filedata->len = 0;
	filedata->enc = NULL;
	filedata->bom = FALSE;
	filedata->readonly = FALSE;

	if (g_stat(locale_filename, &st) != 0)
	{
		ui_set_statusbar(TRUE, _("Could not open file %s (%s)"), utf8_filename, g_strerror(errno));
		return FALSE;
	}

	filedata->mtime = st.st_mtime;

	if (! g_file_get_contents(locale_filename, &filedata->data, NULL, &err))
	{
		ui_set_statusbar(TRUE, "%s", err->message);
		g_error_free(err);
		return FALSE;
	}

	/* use strlen to check for null chars */
	filedata->size = (gsize) st.st_size;
	filedata->len = strlen(filedata->data);

	/* temporarily retrieve the encoding idx based on the BOM to suppress the following warning
	 * if we have a BOM */
	tmp_enc_idx = encodings_scan_unicode_bom(filedata->data, filedata->size, NULL);

	/* check whether the size of the loaded data is equal to the size of the file in the filesystem */
	/* file size may be 0 to allow opening files in /proc/ which have typically a file size
	 * of 0 bytes */
	if (filedata->len != filedata->size && filedata->size != 0 && (
		tmp_enc_idx == GEANY_ENCODING_UTF_8 || /* tmp_enc_idx can be UTF-7/8/16/32, UCS and None */
		tmp_enc_idx == GEANY_ENCODING_UTF_7 || /* filter out UTF-7/8 and None where no NULL bytes */
		tmp_enc_idx == GEANY_ENCODING_NONE))   /* are allowed */
	{
		const gchar *warn_msg = _(
			"The file \"%s\" could not be opened properly and has been truncated. " \
			"This can occur if the file contains a NULL byte. " \
			"Be aware that saving it can cause data loss.\nThe file was set to read-only.");

		if (main_status.main_window_realized)
			dialogs_show_msgbox(GTK_MESSAGE_WARNING, warn_msg, utf8_filename);

		ui_set_statusbar(TRUE, warn_msg, utf8_filename);

		/* set the file to read-only mode because saving it is probably dangerous */
		filedata->readonly = TRUE;
	}

	/* Determine character encoding and convert to UTF-8 */
	if (forced_enc != NULL)
	{
		/* the encoding should be ignored(requested by user), so open the file "as it is" */
		if (utils_str_equal(forced_enc, encodings[GEANY_ENCODING_NONE].charset))
		{
			filedata->bom = FALSE;
			filedata->enc = g_strdup(encodings[GEANY_ENCODING_NONE].charset);
		}
		else if (! handle_forced_encoding(filedata, forced_enc))
		{
			ui_set_statusbar(TRUE, _("The file \"%s\" is not valid %s."), utf8_filename, forced_enc);
			utils_beep();
			g_free(filedata->data);
			return FALSE;
		}
	}
	else if (! handle_encoding(filedata))
	{
		ui_set_statusbar(TRUE,
			_("The file \"%s\" does not look like a text file or the file encoding is not supported."),
			utf8_filename);
		utils_beep();
		g_free(filedata->data);
		return FALSE;
	}

	if (filedata->bom)
		handle_bom(filedata);
	return TRUE;
}


/* Sets the cursor position on opening a file. First it sets the line when cl_options.goto_line
 * is set, otherwise it sets the line when pos is greater than zero and finally it sets the column
 * if cl_options.goto_column is set. */
static void set_cursor_position(GeanyDocument *doc, gint pos)
{
	if (cl_options.goto_line >= 0)
	{	/* goto line which was specified on command line and then undefine the line */
		sci_goto_line(doc->sci, cl_options.goto_line - 1, TRUE);
		doc->scroll_percent = 0.5F;
		cl_options.goto_line = -1;
	}
	else if (pos > 0)
	{
		sci_set_current_position(doc->sci, pos, FALSE);
		doc->scroll_percent = 0.5F;
	}

	if (cl_options.goto_column >= 0)
	{	/* goto column which was specified on command line and then undefine the column */
		gint cur_pos = sci_get_current_position(doc->sci);
		sci_set_current_position(doc->sci, cur_pos + cl_options.goto_column, FALSE);
		doc->scroll_percent = 0.5F;
		cl_options.goto_column = -1;
	}
}


static gboolean detect_use_tabs(ScintillaObject *sci)
{
	gint line;
	gsize tabs = 0, spaces = 0;

	for (line = 0; line < sci_get_line_count(sci); line++)
	{
		gint pos = sci_get_position_from_line(sci, line);
		gchar c;

		c = sci_get_char_at(sci, pos);
		if (c == '\t')
			tabs++;
		else
		if (c == ' ')
		{
			/* check at least 2 spaces */
			if (sci_get_char_at(sci, pos + 1) == ' ')
				spaces++;
		}
	}
	if (spaces == 0 && tabs == 0)
		return editor_prefs.use_tabs;

	/* Skew comparison by a factor of 2 in favour of default editor pref */
	if (editor_prefs.use_tabs)
		return ! (spaces > tabs * 2);
	else
		return (tabs > spaces * 2);
}


static void set_indentation(GeanyDocument *doc)
{
	/* force using tabs for indentation for Makefiles */
	if (FILETYPE_ID(doc->file_type) == GEANY_FILETYPES_MAKE)
		editor_set_use_tabs(doc, TRUE);
	else if (! editor_prefs.detect_tab_mode)
		editor_set_use_tabs(doc, editor_prefs.use_tabs);
	else
	{	/* detect & set tabs/spaces */
		gboolean use_tabs = detect_use_tabs(doc->sci);

		if (use_tabs != editor_prefs.use_tabs)
			ui_set_statusbar(TRUE, _("Setting %s indentation mode."),
				(use_tabs) ? _("Tabs") : _("Spaces"));
		editor_set_use_tabs(doc, use_tabs);
	}
}


/* To open a new file, set doc to NULL; filename should be locale encoded.
 * To reload a file, set the doc for the document to be reloaded; filename should be NULL.
 * pos is the cursor position, which can be overridden by --line and --column.
 * forced_enc can be NULL to detect the file encoding.
 * Returns: doc of the opened file or NULL if an error occurred.
 *
 * When opening more than one file, either:
 * 1. Use document_open_files().
 * 2. Call document_delay_colourise() before document_open_file() and
 *    document_colourise_new() after opening all files.
 *
 * This avoids unnecessary recolourising, saving significant processing when a lot of files
 * are open of a filetype that supports user typenames, e.g. C. */
GeanyDocument *document_open_file_full(GeanyDocument *doc, const gchar *filename, gint pos,
		gboolean readonly, GeanyFiletype *ft, const gchar *forced_enc)
{
	gint editor_mode;
	gboolean reload = (doc == NULL) ? FALSE : TRUE;
	gchar *utf8_filename = NULL;
	gchar *locale_filename = NULL;
	GeanyFiletype *use_ft;
	FileData filedata;

	/*struct timeval tv, tv1;*/
	/*struct timezone tz;*/
	/*gettimeofday(&tv, &tz);*/

	if (reload)
	{
		utf8_filename = g_strdup(doc->file_name);
		locale_filename = utils_get_locale_from_utf8(utf8_filename);
	}
	else
	{
		/* filename must not be NULL when opening a file */
		if (filename == NULL)
		{
			ui_set_statusbar(FALSE, _("Invalid filename"));
			return NULL;
		}

#ifdef G_OS_WIN32
		/* if filename is a shortcut, try to resolve it */
		locale_filename = win32_get_shortcut_target(filename);
#else
		locale_filename = g_strdup(filename);
#endif
		/* try to get the UTF-8 equivalent for the filename, fallback to filename if error */
		utf8_filename = utils_get_utf8_from_locale(locale_filename);

		/* if file is already open, switch to it and go */
		doc = document_find_by_filename(utf8_filename);
		if (doc != NULL)
		{
			ui_add_recent_file(utf8_filename);	/* either add or reorder recent item */
			gtk_notebook_set_current_page(GTK_NOTEBOOK(main_widgets.notebook),
					gtk_notebook_page_num(GTK_NOTEBOOK(main_widgets.notebook),
					(GtkWidget*) doc->sci));
			g_free(utf8_filename);
			g_free(locale_filename);
			document_check_disk_status(doc, TRUE);	/* force a file changed check */
			set_cursor_position(doc, pos);
			return doc;
		}
	}

	/* if default encoding for opening files is set, use it if no forced encoding is set */
	if (file_prefs.default_open_encoding >= 0 && forced_enc == NULL)
		forced_enc = encodings[file_prefs.default_open_encoding].charset;

	if (! load_text_file(locale_filename, utf8_filename, &filedata, forced_enc))
	{
		g_free(utf8_filename);
		g_free(locale_filename);
		return NULL;
	}

	if (! reload) doc = document_create(utf8_filename);
	g_return_val_if_fail(doc != NULL, NULL);	/* really should not happen */

	sci_set_undo_collection(doc->sci, FALSE); /* avoid creation of an undo action */
	sci_empty_undo_buffer(doc->sci);

	/* add the text to the ScintillaObject */
	sci_set_readonly(doc->sci, FALSE);	/* to allow replacing text */
	sci_set_text(doc->sci, filedata.data);	/* NULL terminated data */

	/* detect & set line endings */
	editor_mode = utils_get_line_endings(filedata.data, filedata.len);
	sci_set_eol_mode(doc->sci, editor_mode);
	g_free(filedata.data);

	sci_set_undo_collection(doc->sci, TRUE);

	doc->mtime = filedata.mtime; /* get the modification time from file and keep it */
	g_free(doc->encoding);	/* if reloading, free old encoding */
	doc->encoding = filedata.enc;
	doc->has_bom = filedata.bom;
	store_saved_encoding(doc);	/* store the opened encoding for undo/redo */

	doc->readonly = readonly || filedata.readonly;
	sci_set_readonly(doc->sci, doc->readonly);

	/* update line number margin width */
	sci_set_line_numbers(doc->sci, editor_prefs.show_linenumber_margin, 0);

	/* set the cursor position according to pos, cl_options.goto_line and cl_options.goto_column */
	set_cursor_position(doc, pos);

	if (! reload)
	{
		/* file exists on disk, set real_path */
		g_free(doc->real_path);
		doc->real_path = get_real_path_from_utf8(doc->file_name);

		/* "the" SCI signal (connect after initial setup(i.e. adding text)) */
		g_signal_connect((GtkWidget*) doc->sci, "sci-notify",
					G_CALLBACK(on_editor_notification), doc);

		use_ft = (ft != NULL) ? ft : filetypes_detect_from_file(doc);
	}
	else
	{	/* reloading */
		document_undo_clear(doc);

		/* Unset the filetype so the document gets colourised by document_set_filetype().
		 * (The text could have changed without typenames changing.) */
		doc->file_type = NULL;
		use_ft = ft;
	}
	/* update taglist, typedef keywords and build menu if necessary */
	document_set_filetype(doc, use_ft);

	/* set indentation settings after setting the filetype */
	if (reload)
		editor_set_use_tabs(doc, doc->use_tabs); /* resetup sci */
	else
		set_indentation(doc);

	document_set_text_changed(doc, FALSE);	/* also updates tab state */
	ui_document_show_hide(doc);	/* update the document menu */

	/* finally add current file to recent files menu, but not the files from the last session */
	if (! main_status.opening_session_files)
		ui_add_recent_file(utf8_filename);

	if (! reload && geany_object)
		g_signal_emit_by_name(geany_object, "document-open", doc);

	if (reload)
		ui_set_statusbar(TRUE, _("File %s reloaded."), utf8_filename);
	else
		msgwin_status_add(_("File %s opened(%d%s)."),
				utf8_filename, gtk_notebook_get_n_pages(GTK_NOTEBOOK(main_widgets.notebook)),
				(readonly) ? _(", read-only") : "");

	g_free(utf8_filename);
	g_free(locale_filename);
	/*gettimeofday(&tv1, &tz);*/
	/*geany_debug("%s: %d", filename, (gint)(tv1.tv_usec - tv.tv_usec)); */

	return doc;
}


/* Takes a new line separated list of filename URIs and opens each file.
 * length is the length of the string or -1 if it should be detected */
void document_open_file_list(const gchar *data, gssize length)
{
	gint i;
	gchar *filename;
	gchar **list;

	if (data == NULL) return;

	if (length < 0)
		length = strlen(data);

	switch (utils_get_line_endings(data, length))
	{
		case SC_EOL_CR: list = g_strsplit(data, "\r", 0); break;
		case SC_EOL_CRLF: list = g_strsplit(data, "\r\n", 0); break;
		case SC_EOL_LF: list = g_strsplit(data, "\n", 0); break;
		default: list = g_strsplit(data, "\n", 0);
	}

	document_delay_colourise();

	for (i = 0; ; i++)
	{
		if (list[i] == NULL) break;
		filename = g_filename_from_uri(list[i], NULL, NULL);
		if (filename == NULL) continue;
		document_open_file(filename, FALSE, NULL, NULL);
		g_free(filename);
	}
	document_colourise_new();

	g_strfreev(list);
}


/**
 *  Opens each file in the list @a filenames, ensuring the newly opened documents and
 *  existing documents (if necessary) are only colourised once.
 *  Internally, document_open_file() is called for every list item.
 *
 *  @param filenames A list of filenames to load, in locale encoding.
 *  @param readonly Whether to open the %document in read-only mode.
 *  @param ft The %filetype for the %document or @c NULL to auto-detect the %filetype.
 *  @param forced_enc The file encoding to use or @c NULL to auto-detect the file encoding.
 **/
void document_open_files(const GSList *filenames, gboolean readonly, GeanyFiletype *ft,
		const gchar *forced_enc)
{
	const GSList *item;

	document_delay_colourise();

	for (item = filenames; item != NULL; item = g_slist_next(item))
	{
		document_open_file(item->data, readonly, ft, forced_enc);
	}
	document_colourise_new();
}


/**
 *  Reloads the @a document with the specified file encoding
 *  @a forced_enc or @c NULL to auto-detect the file encoding.
 *
 *  @param doc The document to reload.
 *  @param forced_enc The file encoding to use or @c NULL to auto-detect the file encoding.
 *
 *  @return @a TRUE if the %document was actually reloaded or @a FALSE otherwise.
 **/
gboolean document_reload_file(GeanyDocument *doc, const gchar *forced_enc)
{
	gint pos = 0;
	GeanyDocument *new_doc;

	if (doc == NULL)
		return FALSE;

	/* try to set the cursor to the position before reloading */
	pos = sci_get_current_position(doc->sci);
	new_doc = document_open_file_full(doc, NULL, pos, doc->readonly,
					doc->file_type, forced_enc);
	return (new_doc != NULL);
}


static gboolean document_update_timestamp(GeanyDocument *doc)
{
	struct stat st;
	gchar *locale_filename;

	g_return_val_if_fail(doc != NULL, FALSE);

	locale_filename = utils_get_locale_from_utf8(doc->file_name);
	if (g_stat(locale_filename, &st) != 0)
	{
		ui_set_statusbar(TRUE, _("Could not open file %s (%s)"), doc->file_name,
			g_strerror(errno));
		g_free(locale_filename);
		return FALSE;
	}

	doc->mtime = st.st_mtime; /* get the modification time from file and keep it */
	g_free(locale_filename);
	return TRUE;
}


/* Sets line and column to the given position byte_pos in the document.
 * byte_pos is the position counted in bytes, not characters */
static void get_line_column_from_pos(GeanyDocument *doc, guint byte_pos, gint *line, gint *column)
{
	gint i;
	gint line_start;

	/* for some reason we can use byte count instead of character count here */
	*line = sci_get_line_from_position(doc->sci, byte_pos);
	line_start = sci_get_position_from_line(doc->sci, *line);
	/* get the column in the line */
	*column = byte_pos - line_start;

	/* any non-ASCII characters are encoded with two bytes(UTF-8, always in Scintilla), so
	 * skip one byte(i++) and decrease the column number which is based on byte count */
	for (i = line_start; i < (line_start + *column); i++)
	{
		if (sci_get_char_at(doc->sci, i) < 0)
		{
			(*column)--;
			i++;
		}
	}
}


/*
 * Save the %document, detecting the filetype.
 *
 * @param doc The document for the file to save.
 * @param utf8_fname The new name for the document, in UTF-8, or NULL.
 * @return @c TRUE if the file was saved or @c FALSE if the file could not be saved.
 * @see document_save_file().
 */
gboolean document_save_file_as(GeanyDocument *doc, const gchar *utf8_fname)
{
	gboolean ret;

	if (doc == NULL)
		return FALSE;

	if (utf8_fname)
	{
		g_free(doc->file_name);
		doc->file_name = g_strdup(utf8_fname);
	}

	/* detect filetype */
	if (FILETYPE_ID(doc->file_type) == GEANY_FILETYPES_NONE)
	{
		GeanyFiletype *ft = filetypes_detect_from_file(doc);

		document_set_filetype(doc, ft);
		if (document_get_current() == doc)
		{
			ignore_callback = TRUE;
			filetypes_select_radio_item(doc->file_type);
			ignore_callback = FALSE;
		}
	}
	utils_replace_filename(doc);

	ret = document_save_file(doc, TRUE);
	if (ret)
		ui_add_recent_file(doc->file_name);
	return ret;
}


static gsize save_convert_to_encoding(GeanyDocument *doc, gchar **data, gsize *len)
{
	GError *conv_error = NULL;
	gchar* conv_file_contents = NULL;
	gsize bytes_read;
	gsize conv_len;

	g_return_val_if_fail(data != NULL || *data == NULL, FALSE);
	g_return_val_if_fail(len != NULL, FALSE);

	/* try to convert it from UTF-8 to original encoding */
	conv_file_contents = g_convert(*data, *len - 1, doc->encoding, "UTF-8",
												&bytes_read, &conv_len, &conv_error);

	if (conv_error != NULL)
	{
		gchar *text = g_strdup_printf(
_("An error occurred while converting the file from UTF-8 in \"%s\". The file remains unsaved."),
			doc->encoding);
		gchar *error_text;

		if (conv_error->code == G_CONVERT_ERROR_ILLEGAL_SEQUENCE)
		{
			gchar *context = NULL;
			gint line, column;
			gint context_len;
			gunichar unic;
			/* don't read over the doc length */
			gint max_len = MIN((gint)bytes_read + 6, (gint)*len - 1);
			context = g_malloc(7); /* read 6 bytes from Sci + '\0' */
			sci_get_text_range(doc->sci, bytes_read, max_len, context);

			/* take only one valid Unicode character from the context and discard the leftover */
			unic = g_utf8_get_char_validated(context, -1);
			context_len = g_unichar_to_utf8(unic, context);
			context[context_len] = '\0';
			get_line_column_from_pos(doc, bytes_read, &line, &column);

			error_text = g_strdup_printf(
				_("Error message: %s\nThe error occurred at \"%s\" (line: %d, column: %d)."),
				conv_error->message, context, line + 1, column);
			g_free(context);
		}
		else
			error_text = g_strdup_printf(_("Error message: %s."), conv_error->message);

		geany_debug("encoding error: %s", conv_error->message);
		dialogs_show_msgbox_with_secondary(GTK_MESSAGE_ERROR, text, error_text);
		g_error_free(conv_error);
		g_free(text);
		g_free(error_text);
		return FALSE;
	}
	else
	{
		g_free(*data);
		*data = conv_file_contents;
		*len = conv_len;
	}

	return TRUE;
}


static gint write_data_to_disk(GeanyDocument *doc, const gchar *data, gint len)
{
	FILE *fp;
	gint bytes_written;
	gchar *locale_filename = NULL;
	gint err = 0;

	g_return_val_if_fail(data != NULL, EINVAL);

	locale_filename = utils_get_locale_from_utf8(doc->file_name);
	fp = g_fopen(locale_filename, "wb");
	if (fp == NULL)
	{
		g_free(locale_filename);
		return errno;
	}

	bytes_written = fwrite(data, sizeof(gchar), len, fp);

	if (len != bytes_written)
		err = errno;

	fclose(fp);

	return err;
}


/**
 *  Save the @a document. Saving includes replacing tabs by spaces,
 *  stripping trailing spaces and adding a final new line at the end of the file (all only if
 *  user enabled these features). The filetype is set again or auto-detected if it wasn't
 *  set yet. After all, the "document-save" signal is emitted for plugins.
 *
 *  If the file is not modified, this functions does nothing unless force is set to @c TRUE.
 *
 *  @param doc The %document to save.
 *  @param force Whether to save the file even if it is not modified (e.g. for Save As).
 *
 *  @return @c TRUE if the file was saved or @c FALSE if the file could not or should not be saved.
 **/
gboolean document_save_file(GeanyDocument *doc, gboolean force)
{
	gchar *data;
	gsize len;
	gint err;

	if (doc == NULL)
		return FALSE;

	/* the "changed" flag should exclude the "readonly" flag, but check it anyway for safety */
	if (! force && (! doc->changed || doc->readonly))
		return FALSE;

	if (doc->file_name == NULL)
	{
		ui_set_statusbar(TRUE, _("Error saving file."));
		utils_beep();
		return FALSE;
	}

	/* replaces tabs by spaces but only if the current file is not a Makefile */
	if (file_prefs.replace_tabs && FILETYPE_ID(doc->file_type) != GEANY_FILETYPES_MAKE)
		editor_replace_tabs(doc);
	/* strip trailing spaces */
	if (file_prefs.strip_trailing_spaces)
		editor_strip_trailing_spaces(doc);
	/* ensure the file has a newline at the end */
	if (file_prefs.final_new_line)
		editor_ensure_final_newline(doc);

	len = sci_get_length(doc->sci) + 1;
	if (doc->has_bom && encodings_is_unicode_charset(doc->encoding))
	{	/* always write a UTF-8 BOM because in this moment the text itself is still in UTF-8
		 * encoding, it will be converted to doc->encoding below and this conversion
		 * also changes the BOM */
		data = (gchar*) g_malloc(len + 3);	/* 3 chars for BOM */
		data[0] = (gchar) 0xef;
		data[1] = (gchar) 0xbb;
		data[2] = (gchar) 0xbf;
		sci_get_text(doc->sci, len, data + 3);
		len += 3;
	}
	else
	{
		data = (gchar*) g_malloc(len);
		sci_get_text(doc->sci, len, data);
	}

	/* save in original encoding, skip when it is already UTF-8 or has the encoding "None" */
	if (doc->encoding != NULL && ! utils_str_equal(doc->encoding, "UTF-8") &&
		! utils_str_equal(doc->encoding, encodings[GEANY_ENCODING_NONE].charset))
	{
		if  (! save_convert_to_encoding(doc, &data, &len))
		{
			g_free(data);
			return FALSE;
		}
	}
	else
	{
		len = strlen(data);
	}

	/* actually write the content of data to the file on disk */
	err = write_data_to_disk(doc, data, len);
	g_free(data);

	if (err != 0)
	{
		ui_set_statusbar(TRUE, _("Error saving file (%s)."), g_strerror(err));
		dialogs_show_msgbox_with_secondary(GTK_MESSAGE_ERROR,
			_("Error saving file."), g_strerror(err));
		utils_beep();
		return FALSE;
	}

	/* now the file is on disk, set real_path */
	g_free(doc->real_path);
	doc->real_path = get_real_path_from_utf8(doc->file_name);

	/* store the opened encoding for undo/redo */
	store_saved_encoding(doc);

	/* ignore the following things if we are quitting */
	if (! main_status.quitting)
	{
		gchar *base_name = g_path_get_basename(doc->file_name);

		/* set line numbers again, to reset the margin width, if
		 * there are more lines than before */
		sci_set_line_numbers(doc->sci, editor_prefs.show_linenumber_margin, 0);
		sci_set_savepoint(doc->sci);

		/* stat the file to get the timestamp, otherwise on Windows the actual
		 * timestamp can be ahead of time(NULL) */
		document_update_timestamp(doc);

		/* update filetype-related things */
		document_set_filetype(doc, doc->file_type);

		tm_workspace_update(TM_WORK_OBJECT(app->tm_workspace), TRUE, TRUE, FALSE);
		{
			Document *fdoc = DOCUMENT(doc);
			gtk_label_set_text(GTK_LABEL(fdoc->tab_label), base_name);
			gtk_label_set_text(GTK_LABEL(fdoc->tabmenu_label), base_name);
		}
		msgwin_status_add(_("File %s saved."), doc->file_name);
		ui_update_statusbar(doc, -1);
		g_free(base_name);
#ifdef HAVE_VTE
		vte_cwd(doc->file_name, FALSE);
#endif
	}
	if (geany_object)
	{
		g_signal_emit_by_name(geany_object, "document-save", doc);
	}
	return TRUE;
}


/* special search function, used from the find entry in the toolbar
 * return TRUE if text was found otherwise FALSE
 * return also TRUE if text is empty  */
gboolean document_search_bar_find(GeanyDocument *doc, const gchar *text, gint flags, gboolean inc)
{
	gint start_pos, search_pos;
	struct TextToFind ttf;

	g_return_val_if_fail(text != NULL, FALSE);
	if (doc == NULL)
		return FALSE;
	if (! *text)
		return TRUE;

	start_pos = (inc) ? sci_get_selection_start(doc->sci) :
		sci_get_selection_end(doc->sci);	/* equal if no selection */

	/* search cursor to end */
	ttf.chrg.cpMin = start_pos;
	ttf.chrg.cpMax = sci_get_length(doc->sci);
	ttf.lpstrText = (gchar *)text;
	search_pos = sci_find_text(doc->sci, flags, &ttf);

	/* if no match, search start to cursor */
	if (search_pos == -1)
	{
		ttf.chrg.cpMin = 0;
		ttf.chrg.cpMax = start_pos + strlen(text);
		search_pos = sci_find_text(doc->sci, flags, &ttf);
	}

	if (search_pos != -1)
	{
		gint line = sci_get_line_from_position(doc->sci, ttf.chrgText.cpMin);

		/* unfold maybe folded results */
		sci_ensure_line_is_visible(doc->sci, line);

		sci_set_selection_start(doc->sci, ttf.chrgText.cpMin);
		sci_set_selection_end(doc->sci, ttf.chrgText.cpMax);

		if (! editor_line_in_view(doc->sci, line))
		{	/* we need to force scrolling in case the cursor is outside of the current visible area
			 * GeanyDocument::scroll_percent doesn't work because sci isn't always updated
			 * while searching */
			editor_scroll_to_line(doc->sci, -1, 0.3F);
		}
		return TRUE;
	}
	else
	{
		if (! inc)
		{
			ui_set_statusbar(FALSE, _("\"%s\" was not found."), text);
		}
		utils_beep();
		sci_goto_pos(doc->sci, start_pos, FALSE);	/* clear selection */
		return FALSE;
	}
}


/* General search function, used from the find dialog.
 * Returns -1 on failure or the start position of the matching text.
 * Will skip past any selection, ignoring it. */
gint document_find_text(GeanyDocument *doc, const gchar *text, gint flags, gboolean search_backwards,
		gboolean scroll, GtkWidget *parent)
{
	gint selection_end, selection_start, search_pos;

	g_return_val_if_fail(doc != NULL && text != NULL, -1);
	if (! *text) return -1;
	/* Sci doesn't support searching backwards with a regex */
	if (flags & SCFIND_REGEXP)
		search_backwards = FALSE;

	selection_start = sci_get_selection_start(doc->sci);
	selection_end = sci_get_selection_end(doc->sci);
	if ((selection_end - selection_start) > 0)
	{ /* there's a selection so go to the end */
		if (search_backwards)
			sci_goto_pos(doc->sci, selection_start, TRUE);
		else
			sci_goto_pos(doc->sci, selection_end, TRUE);
	}

	sci_set_search_anchor(doc->sci);
	if (search_backwards)
		search_pos = sci_search_prev(doc->sci, flags, text);
	else
		search_pos = sci_search_next(doc->sci, flags, text);

	if (search_pos != -1)
	{
		/* unfold maybe folded results */
		sci_ensure_line_is_visible(doc->sci,
			sci_get_line_from_position(doc->sci, search_pos));
		if (scroll)
			doc->scroll_percent = 0.3F;
	}
	else
	{
		gint sci_len = sci_get_length(doc->sci);

		/* if we just searched the whole text, give up searching. */
		if ((selection_end == 0 && ! search_backwards) ||
			(selection_end == sci_len && search_backwards))
		{
			ui_set_statusbar(FALSE, _("\"%s\" was not found."), text);
			utils_beep();
			return -1;
		}

		/* we searched only part of the document, so ask whether to wraparound. */
		if (search_prefs.suppress_dialogs ||
			dialogs_show_question_full(parent, GTK_STOCK_FIND, GTK_STOCK_CANCEL,
				_("Wrap search and find again?"), _("\"%s\" was not found."), text))
		{
			gint ret;

			sci_set_current_position(doc->sci, (search_backwards) ? sci_len : 0, FALSE);
			ret = document_find_text(doc, text, flags, search_backwards, scroll, parent);
			if (ret == -1)
			{	/* return to original cursor position if not found */
				sci_set_current_position(doc->sci, selection_start, FALSE);
			}
			return ret;
		}
	}
	return search_pos;
}


/* Replaces the selection if it matches, otherwise just finds the next match.
 * Returns: start of replaced text, or -1 if no replacement was made */
gint document_replace_text(GeanyDocument *doc, const gchar *find_text, const gchar *replace_text,
		gint flags, gboolean search_backwards)
{
	gint selection_end, selection_start, search_pos;

	g_return_val_if_fail(doc != NULL && find_text != NULL && replace_text != NULL, -1);
	if (! *find_text) return -1;

	/* Sci doesn't support searching backwards with a regex */
	if (flags & SCFIND_REGEXP)
		search_backwards = FALSE;

	selection_start = sci_get_selection_start(doc->sci);
	selection_end = sci_get_selection_end(doc->sci);
	if (selection_end == selection_start)
	{
		/* no selection so just find the next match */
		document_find_text(doc, find_text, flags, search_backwards, TRUE, NULL);
		return -1;
	}
	/* there's a selection so go to the start before finding to search through it
	 * this ensures there is a match */
	if (search_backwards)
		sci_goto_pos(doc->sci, selection_end, TRUE);
	else
		sci_goto_pos(doc->sci, selection_start, TRUE);

	search_pos = document_find_text(doc, find_text, flags, search_backwards, TRUE, NULL);
	/* return if the original selected text did not match (at the start of the selection) */
	if (search_pos != selection_start)
		return -1;

	if (search_pos != -1)
	{
		gint replace_len;
		/* search next/prev will select matching text, which we use to set the replace target */
		sci_target_from_selection(doc->sci);
		replace_len = sci_target_replace(doc->sci, replace_text, flags & SCFIND_REGEXP);
		/* select the replacement - find text will skip past the selected text */
		sci_set_selection_start(doc->sci, search_pos);
		sci_set_selection_end(doc->sci, search_pos + replace_len);
	}
	else
	{
		/* no match in the selection */
		utils_beep();
	}
	return search_pos;
}


static void show_replace_summary(GeanyDocument *doc, gint count, const gchar *find_text,
		const gchar *replace_text, gboolean escaped_chars)
{
	gchar *escaped_find_text, *escaped_replace_text, *filename;

	if (count == 0)
	{
		ui_set_statusbar(FALSE, _("No matches found for \"%s\"."), find_text);
		return;
	}

	filename = g_path_get_basename(DOC_FILENAME(doc));

	if (escaped_chars)
	{	/* escape special characters for showing */
		escaped_find_text = g_strescape(find_text, NULL);
		escaped_replace_text = g_strescape(replace_text, NULL);
		ui_set_statusbar(TRUE, ngettext(
			"%s: replaced %d occurrence of \"%s\" with \"%s\".",
			"%s: replaced %d occurrences of \"%s\" with \"%s\".",
			count),	filename, count, escaped_find_text, escaped_replace_text);
		g_free(escaped_find_text);
		g_free(escaped_replace_text);
	}
	else
	{
		ui_set_statusbar(TRUE, ngettext(
			"%s: replaced %d occurrence of \"%s\" with \"%s\".",
			"%s: replaced %d occurrences of \"%s\" with \"%s\".",
			count), filename, count, find_text, replace_text);
	}
	g_free(filename);
}


/* Replace all text matches in a certain range within document.
 * If not NULL, *new_range_end is set to the new range endpoint after replacing,
 * or -1 if no text was found.
 * scroll_to_match is whether to scroll the last replacement in view (which also
 * clears the selection).
 * Returns: the number of replacements made. */
static guint
document_replace_range(GeanyDocument *doc, const gchar *find_text, const gchar *replace_text,
	gint flags, gint start, gint end, gboolean scroll_to_match, gint *new_range_end)
{
	gint count = 0;
	struct TextToFind ttf;
	ScintillaObject *sci;

	if (new_range_end != NULL)
		*new_range_end = -1;
	g_return_val_if_fail(doc != NULL && find_text != NULL && replace_text != NULL, 0);
	if (! *find_text || doc->readonly) return 0;

	sci = doc->sci;

	sci_start_undo_action(sci);
	ttf.chrg.cpMin = start;
	ttf.chrg.cpMax = end;
	ttf.lpstrText = (gchar*)find_text;

	while (TRUE)
	{
		gint search_pos;
		gint find_len = 0, replace_len = 0;

		search_pos = sci_find_text(sci, flags, &ttf);
		find_len = ttf.chrgText.cpMax - ttf.chrgText.cpMin;
		if (search_pos == -1)
			break;	/* no more matches */
		if (find_len == 0 && ! NZV(replace_text))
			break;	/* nothing to do */

		if (search_pos + find_len > end)
			break;	/* found text is partly out of range */
		else
		{
			gint movepastEOL = 0;

			sci_target_start(sci, search_pos);
			sci_target_end(sci, search_pos + find_len);

			if (find_len <= 0)
			{
				gchar chNext = sci_get_char_at(sci, SSM(sci, SCI_GETTARGETEND, 0, 0));

				if (chNext == '\r' || chNext == '\n')
					movepastEOL = 1;
			}
			replace_len = sci_target_replace(sci, replace_text,
				flags & SCFIND_REGEXP);
			count++;
			if (search_pos == end)
				break;	/* Prevent hang when replacing regex $ */

			/* make the next search start after the replaced text */
			start = search_pos + replace_len + movepastEOL;
			if (find_len == 0)
				start = SSM(sci, SCI_POSITIONAFTER, start, 0);	/* prevent '[ ]*' regex rematching part of replaced text */
			ttf.chrg.cpMin = start;
			end += replace_len - find_len;	/* update end of range now text has changed */
			ttf.chrg.cpMax = end;
		}
	}
	sci_end_undo_action(sci);

	if (count > 0)
	{	/* scroll last match in view, will destroy the existing selection */
		if (scroll_to_match)
			sci_goto_pos(sci, ttf.chrg.cpMin, TRUE);

		if (new_range_end != NULL)
			*new_range_end = end;
	}
	return count;
}


void document_replace_sel(GeanyDocument *doc, const gchar *find_text, const gchar *replace_text,
						  gint flags, gboolean escaped_chars)
{
	gint selection_end, selection_start, selection_mode, selected_lines, last_line = 0;
	gint max_column = 0, count = 0;
	gboolean replaced = FALSE;

	g_return_if_fail(doc != NULL && find_text != NULL && replace_text != NULL);
	if (! *find_text) return;

	selection_start = sci_get_selection_start(doc->sci);
	selection_end = sci_get_selection_end(doc->sci);
	/* do we have a selection? */
	if ((selection_end - selection_start) == 0)
	{
		utils_beep();
		return;
	}

	selection_mode = sci_get_selection_mode(doc->sci);
	selected_lines = sci_get_lines_selected(doc->sci);
	/* handle rectangle, multi line selections (it doesn't matter on a single line) */
	if (selection_mode == SC_SEL_RECTANGLE && selected_lines > 1)
	{
		gint first_line, line;

		sci_start_undo_action(doc->sci);

		first_line = sci_get_line_from_position(doc->sci, selection_start);
		/* Find the last line with chars selected (not EOL char) */
		last_line = sci_get_line_from_position(doc->sci,
			selection_end - editor_get_eol_char_len(doc));
		last_line = MAX(first_line, last_line);
		for (line = first_line; line < (first_line + selected_lines); line++)
		{
			gint line_start = sci_get_pos_at_line_sel_start(doc->sci, line);
			gint line_end = sci_get_pos_at_line_sel_end(doc->sci, line);

			/* skip line if there is no selection */
			if (line_start != INVALID_POSITION)
			{
				/* don't let document_replace_range() scroll to match to keep our selection */
				gint new_sel_end;

				count += document_replace_range(doc, find_text, replace_text, flags,
								line_start, line_end, FALSE, &new_sel_end);
				if (new_sel_end != -1)
				{
					replaced = TRUE;
					/* this gets the greatest column within the selection after replacing */
					max_column = MAX(max_column,
						new_sel_end - sci_get_position_from_line(doc->sci, line));
				}
			}
		}
		sci_end_undo_action(doc->sci);
	}
	else	/* handle normal line selection */
	{
		count += document_replace_range(doc, find_text, replace_text, flags,
						selection_start, selection_end, TRUE, &selection_end);
		if (selection_end != -1)
			replaced = TRUE;
	}

	if (replaced)
	{	/* update the selection for the new endpoint */

		if (selection_mode == SC_SEL_RECTANGLE && selected_lines > 1)
		{
			/* now we can scroll to the selection and destroy it because we rebuild it later */
			/*sci_goto_pos(doc->sci, selection_start, FALSE);*/

			/* Note: the selection will be wrapped to last_line + 1 if max_column is greater than
			 * the highest column on the last line. The wrapped selection is completely different
			 * from the original one, so skip the selection at all */
			/* TODO is there a better way to handle the wrapped selection? */
			if ((sci_get_line_length(doc->sci, last_line) - 1) >= max_column)
			{	/* for keeping and adjusting the selection in multi line rectangle selection we
				 * need the last line of the original selection and the greatest column number after
				 * replacing and set the selection end to the last line at the greatest column */
				sci_set_selection_start(doc->sci, selection_start);
				sci_set_selection_end(doc->sci,
					sci_get_position_from_line(doc->sci, last_line) + max_column);
				sci_set_selection_mode(doc->sci, selection_mode);
			}
		}
		else
		{
			sci_set_selection_start(doc->sci, selection_start);
			sci_set_selection_end(doc->sci, selection_end);
		}
	}
	else /* no replacements */
		utils_beep();

	show_replace_summary(doc, count, find_text, replace_text, escaped_chars);
}


/* returns TRUE if at least one replacement was made. */
gboolean document_replace_all(GeanyDocument *doc, const gchar *find_text, const gchar *replace_text,
		gint flags, gboolean escaped_chars)
{
	gint len, count;
	g_return_val_if_fail(doc != NULL && find_text != NULL && replace_text != NULL, FALSE);
	if (! *find_text) return FALSE;

	len = sci_get_length(doc->sci);
	count = document_replace_range(
			doc, find_text, replace_text, flags, 0, len, TRUE, NULL);

	show_replace_summary(doc, count, find_text, replace_text, escaped_chars);
	return (count > 0);
}


void document_update_tag_list(GeanyDocument *doc, gboolean update)
{
	/* We must call treeviews_update_tag_list() before returning,
	 * to ensure that the symbol list is always updated properly (e.g.
	 * when creating a new document with a partial filename set. */
	gboolean success = FALSE;

	/* if the filetype doesn't have a tag parser or it is a new file */
	if (doc == NULL || doc->file_type == NULL ||
		app->tm_workspace == NULL ||
		! filetype_has_tags(doc->file_type) || ! doc->file_name)
	{
		/* set the default (empty) tag list */
		treeviews_update_tag_list(doc, FALSE);
		return;
	}

	if (doc->tm_file == NULL)
	{
		gchar *locale_filename = utils_get_locale_from_utf8(doc->file_name);

		doc->tm_file = tm_source_file_new(locale_filename, FALSE, doc->file_type->name);
		g_free(locale_filename);

		if (doc->tm_file)
		{
			if (!tm_workspace_add_object(doc->tm_file))
			{
				tm_work_object_free(doc->tm_file);
				doc->tm_file = NULL;
			}
			else
			{
				if (update)
					tm_source_file_update(doc->tm_file, TRUE, FALSE, TRUE);
				success = TRUE;
			}
		}
	}
	else
	{
		success = tm_source_file_update(doc->tm_file, TRUE, FALSE, TRUE);
		if (! success)
			geany_debug("tag list updating failed");
	}
	treeviews_update_tag_list(doc, success);
}


/* Caches the list of project typenames, as a space separated GString.
 * Returns: TRUE if typenames have changed.
 * (*types) is set to the list of typenames, or NULL if there are none. */
static gboolean get_project_typenames(const GString **types, gint lang)
{
	static GString *last_typenames = NULL;
	GString *s = NULL;

	if (app->tm_workspace)
	{
		GPtrArray *tags_array = app->tm_workspace->work_object.tags_array;

		if (tags_array)
		{
			s = symbols_find_tags_as_string(tags_array, TM_GLOBAL_TYPE_MASK, lang);
		}
	}

	if (s && last_typenames && g_string_equal(s, last_typenames))
	{
		g_string_free(s, TRUE);
		*types = last_typenames;
		return FALSE;	/* project typenames haven't changed */
	}
	/* cache typename list for next time */
	if (last_typenames)
		g_string_free(last_typenames, TRUE);
	last_typenames = s;

	*types = s;
	if (s == NULL) return FALSE;
	return TRUE;
}


/* If sci is NULL, update project typenames for all documents that support typenames,
 * if typenames have changed.
 * If sci is not NULL, then if sci supports typenames, project typenames are updated
 * if necessary, and typename keywords are set for sci.
 * Returns: TRUE if any scintilla type keywords were updated. */
static gboolean update_type_keywords(ScintillaObject *sci, gint lang)
{
	gboolean ret = FALSE;
	guint n;
	const GString *s;

	if (sci != NULL && editor_lexer_get_type_keyword_idx(sci_get_lexer(sci)) == -1)
		return FALSE;

	if (! get_project_typenames(&s, lang))
	{	/* typenames have not changed */
		if (s != NULL && sci != NULL)
		{
			gint keyword_idx = editor_lexer_get_type_keyword_idx(sci_get_lexer(sci));

			sci_set_keywords(sci, keyword_idx, s->str);
			if (! delay_colourise)
			{
				sci_colourise(sci, 0, -1);
			}
		}
		return FALSE;
	}
	g_return_val_if_fail(s != NULL, FALSE);

	for (n = 0; n < documents_array->len; n++)
	{
		ScintillaObject *wid = documents[n]->sci;

		if (wid)
		{
			gint keyword_idx = editor_lexer_get_type_keyword_idx(sci_get_lexer(wid));

			if (keyword_idx > 0)
			{
				sci_set_keywords(wid, keyword_idx, s->str);
				if (! delay_colourise)
				{
					sci_colourise(wid, 0, -1);
				}
				ret = TRUE;
			}
		}
	}
	return ret;
}


/** Sets the filetype of the document (which controls syntax highlighting and tags)
 * @param doc The document to use.
 * @param type The filetype. */
void document_set_filetype(GeanyDocument *doc, GeanyFiletype *type)
{
	gboolean colourise = FALSE;
	gboolean ft_changed;

	if (type == NULL || doc == NULL)
		return;

	geany_debug("%s : %s (%s)",
		(doc->file_name != NULL) ? doc->file_name : "unknown",
		(type->name != NULL) ? type->name : "unknown",
		(doc->encoding != NULL) ? doc->encoding : "unknown");

	ft_changed = (doc->file_type != type);
	if (ft_changed)	/* filetype has changed */
	{
		doc->file_type = type;

		/* delete tm file object to force creation of a new one */
		if (doc->tm_file != NULL)
		{
			tm_workspace_remove_object(doc->tm_file, TRUE, TRUE);
			doc->tm_file = NULL;
		}
		highlighting_set_styles(doc->sci, type->id);
		build_menu_update(doc);
		colourise = TRUE;
	}

	document_update_tag_list(doc, TRUE);
	if (! delay_colourise)
	{
		/* Check if project typename keywords have changed.
		 * If they haven't, we may need to colourise the document. */
		if (! update_type_keywords(doc->sci, type->lang) && colourise)
			sci_colourise(doc->sci, 0, -1);
	}
	if (ft_changed)
	{
		utils_get_current_function(NULL, NULL);
		ui_update_statusbar(doc, -1);
	}
}


/**
 *  Sets the encoding of a %document.
 *  This function only set the encoding of the %document, it does not any conversions. The new
 *  encoding is used when e.g. saving the file.
 *
 *  @param doc The %document to use.
 *  @param new_encoding The encoding to be set for the %document.
 **/
void document_set_encoding(GeanyDocument *doc, const gchar *new_encoding)
{
	if (doc == NULL || new_encoding == NULL ||
		utils_str_equal(new_encoding, doc->encoding)) return;

	g_free(doc->encoding);
	doc->encoding = g_strdup(new_encoding);

	ui_update_statusbar(doc, -1);
	gtk_widget_set_sensitive(lookup_widget(main_widgets.window, "menu_write_unicode_bom1"),
			encodings_is_unicode_charset(doc->encoding));
}


/* own Undo / Redo implementation to be able to undo / redo changes
 * to the encoding or the Unicode BOM (which are Scintilla independet).
 * All Scintilla events are stored in the undo / redo buffer and are passed through. */

/* Clears the Undo and Redo buffer (to be called when reloading or closing the document) */
void document_undo_clear(GeanyDocument *doc)
{
	Document *fdoc = DOCUMENT(doc);
	undo_action *a;

	while (g_trash_stack_height(&fdoc->undo_actions) > 0)
	{
		a = g_trash_stack_pop(&fdoc->undo_actions);
		if (a != NULL)
		{
			switch (a->type)
			{
				case UNDO_ENCODING: g_free(a->data); break;
				default: break;
			}

			g_free(a);
		}
	}
	fdoc->undo_actions = NULL;

	while (g_trash_stack_height(&fdoc->redo_actions) > 0)
	{
		a = g_trash_stack_pop(&fdoc->redo_actions);
		if (a != NULL)
		{
			switch (a->type)
			{
				case UNDO_ENCODING: g_free(a->data); break;
				default: break;
			}

			g_free(a);
		}
	}
	fdoc->redo_actions = NULL;

	if (! main_status.quitting && doc->sci != NULL)
		document_set_text_changed(doc, FALSE);

	/*geany_debug("%s: new undo stack height: %d, new redo stack height: %d", __func__,
				 *g_trash_stack_height(&doc->undo_actions), g_trash_stack_height(&doc->redo_actions)); */
}


void document_undo_add(GeanyDocument *doc, guint type, gpointer data)
{
	Document *fdoc = DOCUMENT(doc);
	undo_action *action;

	if (doc == NULL)
		return;

	action = g_new0(undo_action, 1);
	action->type = type;
	action->data = data;

	g_trash_stack_push(&fdoc->undo_actions, action);

	document_set_text_changed(doc, TRUE);
	ui_update_popup_reundo_items(doc);

	/*geany_debug("%s: new stack height: %d, added type: %d", __func__,
				 *g_trash_stack_height(&doc->undo_actions), action->type); */
}


gboolean document_can_undo(GeanyDocument *doc)
{
	Document *fdoc = DOCUMENT(doc);

	if (doc == NULL)
		return FALSE;

	if (g_trash_stack_height(&fdoc->undo_actions) > 0 || sci_can_undo(doc->sci))
		return TRUE;
	else
		return FALSE;
}


static void update_changed_state(GeanyDocument *doc)
{
	Document *fdoc = DOCUMENT(doc);

	doc->changed =
		(sci_is_modified(doc->sci) ||
		doc->has_bom != fdoc->saved_encoding.has_bom ||
		! utils_str_equal(doc->encoding, fdoc->saved_encoding.encoding));
	document_set_text_changed(doc, doc->changed);
}


void document_undo(GeanyDocument *doc)
{
	Document *fdoc = DOCUMENT(doc);
	undo_action *action;

	if (doc == NULL)
		return;

	action = g_trash_stack_pop(&fdoc->undo_actions);

	if (action == NULL)
	{
		/* fallback, should not be necessary */
		geany_debug("%s: fallback used", __func__);
		sci_undo(doc->sci);
	}
	else
	{
		switch (action->type)
		{
			case UNDO_SCINTILLA:
			{
				document_redo_add(doc, UNDO_SCINTILLA, NULL);

				sci_undo(doc->sci);
				break;
			}
			case UNDO_BOM:
			{
				document_redo_add(doc, UNDO_BOM, GINT_TO_POINTER(doc->has_bom));

				doc->has_bom = GPOINTER_TO_INT(action->data);
				ui_update_statusbar(doc, -1);
				ui_document_show_hide(doc);
				break;
			}
			case UNDO_ENCODING:
			{
				/* use the "old" encoding */
				document_redo_add(doc, UNDO_ENCODING, g_strdup(doc->encoding));

				document_set_encoding(doc, (const gchar*)action->data);

				ignore_callback = TRUE;
				encodings_select_radio_item((const gchar*)action->data);
				ignore_callback = FALSE;

				g_free(action->data);
				break;
			}
			default: break;
		}
	}
	g_free(action); /* free the action which was taken from the stack */

	update_changed_state(doc);
	ui_update_popup_reundo_items(doc);
	/*geany_debug("%s: new stack height: %d", __func__, g_trash_stack_height(&doc->undo_actions));*/
}


gboolean document_can_redo(GeanyDocument *doc)
{
	Document *fdoc = DOCUMENT(doc);

	if (doc == NULL)
		return FALSE;

	if (g_trash_stack_height(&fdoc->redo_actions) > 0 || sci_can_redo(doc->sci))
		return TRUE;
	else
		return FALSE;
}


void document_redo(GeanyDocument *doc)
{
	Document *fdoc = DOCUMENT(doc);
	undo_action *action;

	if (doc == NULL)
		return;

	action = g_trash_stack_pop(&fdoc->redo_actions);

	if (action == NULL)
	{
		/* fallback, should not be necessary */
		geany_debug("%s: fallback used", __func__);
		sci_redo(doc->sci);
	}
	else
	{
		switch (action->type)
		{
			case UNDO_SCINTILLA:
			{
				document_undo_add(doc, UNDO_SCINTILLA, NULL);

				sci_redo(doc->sci);
				break;
			}
			case UNDO_BOM:
			{
				document_undo_add(doc, UNDO_BOM, GINT_TO_POINTER(doc->has_bom));

				doc->has_bom = GPOINTER_TO_INT(action->data);
				ui_update_statusbar(doc, -1);
				ui_document_show_hide(doc);
				break;
			}
			case UNDO_ENCODING:
			{
				document_undo_add(doc, UNDO_ENCODING, g_strdup(doc->encoding));

				document_set_encoding(doc, (const gchar*)action->data);

				ignore_callback = TRUE;
				encodings_select_radio_item((const gchar*)action->data);
				ignore_callback = FALSE;

				g_free(action->data);
				break;
			}
			default: break;
		}
	}
	g_free(action); /* free the action which was taken from the stack */

	update_changed_state(doc);
	ui_update_popup_reundo_items(doc);
	/*geany_debug("%s: new stack height: %d", __func__, g_trash_stack_height(&doc->redo_actions));*/
}


static void document_redo_add(GeanyDocument *doc, guint type, gpointer data)
{
	Document *fdoc = DOCUMENT(doc);
	undo_action *action;

	if (doc == NULL)
		return;

	action = g_new0(undo_action, 1);
	action->type = type;
	action->data = data;

	g_trash_stack_push(&fdoc->redo_actions, action);

	document_set_text_changed(doc, TRUE);
	ui_update_popup_reundo_items(doc);
}


/* Gets the status colour of the document, or NULL if default widget
 * colouring should be used. */
GdkColor *document_get_status_color(GeanyDocument *doc)
{
	static GdkColor red = {0, 0xFFFF, 0, 0};
	static GdkColor green = {0, 0, 0x7FFF, 0};
	GdkColor *color = NULL;

	if (doc == NULL)
		return NULL;

	if (doc->changed)
		color = &red;
	else if (doc->readonly)
		color = &green;

	return color;	/* return pointer to static GdkColor. */
}


/* useful debugging function (usually debug macros aren't enabled so can't use
 * documents[idx]) */
#ifdef GEANY_DEBUG
GeanyDocument *doc_at(gint idx)
{
	return (idx >= 0 && idx < (gint) documents_array->len) ? documents[idx] : NULL;
}
#endif


static GArray *doc_indexes = NULL;

/* Cache the current document indexes and prevent any colourising until
 * document_colourise_new() is called. */
void document_delay_colourise()
{
	gint n;

	g_return_if_fail(delay_colourise == FALSE);
	g_return_if_fail(doc_indexes == NULL);

	/* make an array containing all the current document indexes */
	doc_indexes = g_array_new(FALSE, FALSE, sizeof(gint));
	for (n = 0; n < (gint) documents_array->len; n++)
	{
		if (documents[n]->is_valid)
			g_array_append_val(doc_indexes, n);
	}
	delay_colourise = TRUE;
}


/* Colourise only newly opened documents and existing documents whose project typenames
 * keywords have changed.
 * document_delay_colourise() should already have been called. */
void document_colourise_new()
{
	guint n, i;
	/* A bitset representing which docs need [re]colourising.
	 * (use gint8 to save memory because gboolean = gint) */
	gint8 *doc_set = g_newa(gint8, documents_array->len);
	gboolean recolour = FALSE;	/* whether to recolourise existing typenames */

	g_return_if_fail(delay_colourise == TRUE);
	g_return_if_fail(doc_indexes != NULL);

	/* first assume recolourising all docs */
	memset(doc_set, TRUE, documents_array->len * sizeof(gint8));

	/* remove existing docs from the set if they don't use typenames or typenames haven't changed */
	recolour = update_type_keywords(NULL, -2);
	for (i = 0; i < doc_indexes->len; i++)
	{
		ScintillaObject *sci;

		n = g_array_index(doc_indexes, gint, i);
		sci = documents[n]->sci;
		if (! recolour || (sci && editor_lexer_get_type_keyword_idx(sci_get_lexer(sci)) == -1))
		{
			doc_set[n] = FALSE;
		}
	}
	/* colourise all in the doc_set */
	for (n = 0; n < documents_array->len; n++)
	{
		if (doc_set[n] && documents[n]->is_valid)
			sci_colourise(documents[n]->sci, 0, -1);
	}
	delay_colourise = FALSE;
	g_array_free(doc_indexes, TRUE);
	doc_indexes = NULL;

	/* now that the current document is colourised, fold points are now accurate,
	 * so force an update of the current function/tag. */
	utils_get_current_function(NULL, NULL);
	ui_update_statusbar(NULL, -1);
}


GeanyDocument *document_clone(GeanyDocument *old_doc, const gchar *utf8_filename)
{
	/* create a new file and copy file content and properties */
	gint len;
	gchar *text;
	GeanyDocument *doc;

	len = sci_get_length(old_doc->sci) + 1;
	text = (gchar*) g_malloc(len);
	sci_get_text(old_doc->sci, len, text);
	/* use old file type (or maybe NULL for auto detect would be better?) */
	doc = document_new_file(utf8_filename, old_doc->file_type, text);
	g_free(text);

	/* copy file properties */
	doc->line_wrapping = old_doc->line_wrapping;
	doc->readonly = old_doc->readonly;
	doc->has_bom = old_doc->has_bom;
	document_set_encoding(doc, old_doc->encoding);
	sci_set_lines_wrapped(doc->sci, doc->line_wrapping);
	sci_set_readonly(doc->sci, doc->readonly);

	ui_document_show_hide(doc);
	return doc;
}


/* @note If successful, this should always be followed up with a call to
 * document_close_all().
 * @return TRUE if all files were saved or had their changes discarded. */
gboolean document_account_for_unsaved(void)
{
	gint p;
	guint i, len = documents_array->len;

	for (p = 0; p < gtk_notebook_get_n_pages(GTK_NOTEBOOK(main_widgets.notebook)); p++)
	{
		GeanyDocument *doc = document_get_from_page(p);

		if (doc->changed)
		{
			if (! dialogs_show_unsaved_file(doc))
				return FALSE;
		}
	}
	/* all documents should now be accounted for, so ignore any changes */
	for (i = 0; i < len; i++)
	{
		if (documents[i]->is_valid && documents[i]->changed)
		{
			documents[i]->changed = FALSE;
		}
	}
	return TRUE;
}


static void force_close_all(void)
{
	guint i, len = documents_array->len;

	/* check all documents have been accounted for */
	for (i = 0; i < len; i++)
	{
		if (documents[i]->is_valid)
		{
			g_return_if_fail(!documents[i]->changed);
		}
	}
	main_status.closing_all = TRUE;

	while (gtk_notebook_get_n_pages(GTK_NOTEBOOK(main_widgets.notebook)) > 0)
	{
		document_remove_page(0);
	}

	main_status.closing_all = FALSE;
}


gboolean document_close_all(void)
{
	if (! document_account_for_unsaved())
		return FALSE;

	force_close_all();

	tm_workspace_update(TM_WORK_OBJECT(app->tm_workspace), TRUE, TRUE, FALSE);
	return TRUE;
}


static gboolean check_reload(GeanyDocument *doc)
{
	gchar *base_name = g_path_get_basename(doc->file_name);
	gboolean want_reload;

	want_reload = dialogs_show_question_full(NULL, _("_Reload"), GTK_STOCK_CANCEL,
		_("Do you want to reload it?"),
		_("The file '%s' on the disk is more recent than\n"
			"the current buffer."), base_name);
	if (want_reload)
	{
		document_reload_file(doc, NULL);
	}
	g_free(base_name);
	return want_reload;
}


/* Set force to force a disk check, otherwise it is ignored if there was a check
 * in the last file_prefs.disk_check_timeout seconds.
 * @return @c TRUE if the file has changed. */
gboolean document_check_disk_status(GeanyDocument *doc, gboolean force)
{
	struct stat st;
	time_t t;
	gchar *locale_filename;
	gboolean ret = FALSE;

	if (file_prefs.disk_check_timeout == 0)
		return FALSE;
	if (doc == NULL)
		return FALSE;
	/* ignore documents that have never been saved to disk */
	if (doc->real_path == NULL) return FALSE;

	t = time(NULL);

	if (! force && doc->last_check > (t - file_prefs.disk_check_timeout))
		return FALSE;

	doc->last_check = t;

	locale_filename = utils_get_locale_from_utf8(doc->file_name);
	if (g_stat(locale_filename, &st) != 0)
	{
		/* file is missing - set unsaved state */
		document_set_text_changed(doc, TRUE);

		if (dialogs_show_question_full(NULL, GTK_STOCK_SAVE, GTK_STOCK_CANCEL,
			_("Try to resave the file?"),
			_("File \"%s\" was not found on disk!"), doc->file_name))
		{
			dialogs_show_save_as();
		}
	}
	else if (doc->mtime > t || st.st_mtime > t)
	{
		geany_debug("Strange: Something is wrong with the time stamps.");
	}
	else if (doc->mtime < st.st_mtime)
	{
		if (check_reload(doc))
		{
			/* Update the modification time */
			doc->mtime = st.st_mtime;
		}
		else
			doc->mtime = st.st_mtime;	/* Ignore this change on disk completely */

		ret = TRUE; /* file has changed */
	}
	g_free(locale_filename);
	return ret;
}


