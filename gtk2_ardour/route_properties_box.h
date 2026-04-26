/*
 * Copyright (C) 2021 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2024 Ben Loftis <ben@harrisonconsoles.com>
 * Copyright (C) 2024 Robin Gareus <robin@gareus.org>
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

#include <vector>

#include <ytkmm/box.h>
#include <ytkmm/button.h>
#include <ytkmm/checkbutton.h>
#include <ytkmm/entry.h>
#include <ytkmm/label.h>
#include <ytkmm/scrolledwindow.h>
#include <ytkmm/textbuffer.h>
#include <ytkmm/textview.h>

#include "ardour/ardour.h"
#include "ardour/session_handle.h"

#include "widgets/frame.h"

namespace ARDOUR {
	class Route;
	class Processor;
	class Session;
	class SuperColliderTrack;
}

class GenericPluginUI;
class ProcessorBox;

class RoutePropertiesBox : public Gtk::HBox, public ARDOUR::SessionHandlePtr
{
public:
	RoutePropertiesBox ();
	~RoutePropertiesBox ();

	void set_session (ARDOUR::Session*);
	void set_route (std::shared_ptr<ARDOUR::Route>, bool force = false);
	void set_force_hide_insert (bool);
	void focus_supercollider_source ();

private:
	void property_changed (const PBD::PropertyChange& what_changed);
	void map_frozen ();
	void ui_actions_ready ();
	void update_processor_box_visibility ();
	void session_going_away ();
	void drop_route ();
	void drop_plugin_uis ();
	void refill_processors ();
	void add_processor_to_display (std::weak_ptr<ARDOUR::Processor> w);
	void idle_refill_processors ();
	void surround_master_added_or_removed ();
	void sync_supercollider_editor ();
	void mark_supercollider_editor_dirty ();
	void supercollider_source_or_autofill_changed ();
	void apply_supercollider_changes ();
	void restart_supercollider_runtime ();

	static int _idle_refill_processors (gpointer);

	Gtk::ScrolledWindow _scroller;
	Gtk::HBox           _box;
	Gtk::VBox           _left_box;

	std::shared_ptr<ARDOUR::Route> _route;
	std::vector <GenericPluginUI*> _proc_uis;

	ProcessorBox*             _insert_box;
	ArdourWidgets::Frame      _supercollider_frame;
	Gtk::VBox                 _supercollider_box;
	Gtk::HBox                 _supercollider_controls;
	Gtk::Label                _supercollider_status;
	Gtk::Label                _supercollider_synthdef_label;
	Gtk::Entry                _supercollider_synthdef_entry;
	Gtk::CheckButton          _supercollider_auto_synthdef_button;
	Gtk::CheckButton          _supercollider_auto_boot_button;
	Gtk::Button               _supercollider_apply_button;
	Gtk::Button               _supercollider_restart_button;
	Gtk::ScrolledWindow       _supercollider_source_scroller;
	Gtk::TextView             _supercollider_source_view;
	Glib::RefPtr<Gtk::TextBuffer> _supercollider_source_buffer;

	ArdourWidgets::Frame _insert_frame;
	bool                 _show_insert;
	bool                 _force_hide_insert;
	bool                 _updating_supercollider_ui;
	bool                 _supercollider_dirty;

	int _idle_refill_processors_id;

	PBD::ScopedConnectionList _processor_connections;
	PBD::ScopedConnectionList _route_connections;
	PBD::ScopedConnectionList _forever_connections;
};
