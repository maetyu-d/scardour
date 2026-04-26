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
#include <ytkmm/entry.h>
#include <ytkmm/label.h>
#include <ytkmm/scrolledwindow.h>
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
	void update_title ();
	void sync_editor ();
	void mark_dirty ();
	void apply_changes ();
	void restart_runtime ();
	void render_to_timeline ();

	Gtk::VBox _vbox;
	Gtk::Label _status_label;
	Gtk::HBox _controls_box;
	Gtk::Label _synthdef_label;
	Gtk::Entry _synthdef_entry;
	Gtk::CheckButton _auto_boot_button;
	Gtk::Button _apply_button;
	Gtk::Button _restart_button;
	Gtk::Button _render_button;
	Gtk::ScrolledWindow _source_scroller;
	Gtk::TextView _source_view;
	Glib::RefPtr<Gtk::TextBuffer> _source_buffer;

	std::shared_ptr<ARDOUR::Route> _route;
	PBD::ScopedConnectionList      _connections;
	bool _updating;
	bool _dirty;
	std::atomic<bool> _render_in_progress;
};
