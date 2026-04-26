/*
 * Copyright (C) 2026 OpenAI
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <ytkmm/box.h>
#include <ytkmm/button.h>
#include <ytkmm/checkbutton.h>
#include <ytkmm/comboboxtext.h>
#include <ytkmm/entry.h>
#include <ytkmm/filechooserdialog.h>
#include <ytkmm/label.h>
#include <ytkmm/scrolledwindow.h>
#include <ytkmm/stock.h>
#include <ytkmm/texttag.h>
#include <ytkmm/textbuffer.h>
#include <ytkmm/textview.h>

#include "ardour/route.h"
#include "ardour/supercollider_track.h"

#include "ardour_window.h"

class SuperColliderTrackEditor : public ArdourWindow
{
public:
	SuperColliderTrackEditor (std::shared_ptr<ARDOUR::Route>);
	~SuperColliderTrackEditor ();

	void toggle ();
	void open ();

private:
	void route_property_changed (const PBD::PropertyChange&);
	void route_going_away ();
	void region_selection_changed ();
	void update_title ();
	void sync_editor ();
	std::shared_ptr<ARDOUR::Region> selected_region () const;
	void mark_dirty ();
	void source_or_autofill_changed ();
	void apply_changes ();
	void revert_changes ();
	void restart_runtime ();
	void render_to_timeline ();
	void render_to_midi_track ();
	void load_source_from_file ();
	void save_source_to_file ();
	void insert_template ();
	void goto_line ();
	void find_next ();
	void find_previous ();
	void update_scope_label ();
	void update_cursor_status ();
	void update_line_numbers ();
	void update_syntax_highlighting ();
	void update_current_line_highlight ();
	void cursor_moved (const Gtk::TextBuffer::iterator&, const Glib::RefPtr<Gtk::TextBuffer::Mark>&);
	bool editor_key_press (GdkEventKey*);

	Gtk::VBox _vbox;
	Gtk::Label _status_label;
	Gtk::HBox _controls_box;
	Gtk::HBox _editor_tools_box;
	Gtk::HBox _search_box;
	Gtk::HBox _editor_box;
	Gtk::HBox _footer_box;
	Gtk::Label _synthdef_label;
	Gtk::Entry _synthdef_entry;
	Gtk::CheckButton _auto_synthdef_button;
	Gtk::CheckButton _auto_boot_button;
	Gtk::Button _apply_button;
	Gtk::Button _revert_button;
	Gtk::Button _restart_button;
	Gtk::Button _render_button;
	Gtk::Label _scope_label;
	Gtk::ComboBoxText _template_combo;
	Gtk::Button _insert_template_button;
	Gtk::Button _load_button;
	Gtk::Button _save_button;
	Gtk::Label _search_label;
	Gtk::Entry _search_entry;
	Gtk::Button _find_prev_button;
	Gtk::Button _find_next_button;
	Gtk::Label _goto_label;
	Gtk::Entry _goto_entry;
	Gtk::Button _goto_button;
	Gtk::ScrolledWindow _line_number_scroller;
	Gtk::TextView _line_number_view;
	Glib::RefPtr<Gtk::TextBuffer> _line_number_buffer;
	Gtk::ScrolledWindow _source_scroller;
	Gtk::TextView _source_view;
	Gtk::Label _cursor_status_label;
	Gtk::Label _shortcut_status_label;
	Glib::RefPtr<Gtk::TextBuffer> _source_buffer;

	std::shared_ptr<ARDOUR::Route> _route;
	PBD::ScopedConnectionList      _connections;
	bool _updating;
	bool _dirty;
	std::atomic<bool> _render_in_progress;
};
