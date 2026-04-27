/*
 * Copyright (C) 2026 OpenAI
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <memory>

#include <ytkmm/box.h>
#include <ytkmm/button.h>
#include <ytkmm/checkbutton.h>
#include <ytkmm/entry.h>
#include <ytkmm/filechooserdialog.h>
#include <ytkmm/label.h>
#include <ytkmm/scrolledwindow.h>
#include <ytkmm/textbuffer.h>
#include <ytkmm/textview.h>

#include "ardour/route.h"

#include "ardour_window.h"

class SuperColliderFxEditor : public ArdourWindow
{
public:
	SuperColliderFxEditor (std::shared_ptr<ARDOUR::Route>);
	~SuperColliderFxEditor ();

	void toggle ();
	void open ();

private:
	void route_property_changed (const PBD::PropertyChange&);
	void route_going_away ();
	void sync_editor ();
	void update_title ();
	void mark_dirty ();
	void source_or_autofill_changed ();
	void apply_changes ();
	void load_source_from_file ();
	void save_source_to_file ();

	Gtk::VBox _vbox;
	Gtk::Label _status_label;
	Gtk::Label _detail_label;
	Gtk::Label _diagnostics_label;
	Gtk::HBox _controls_box;
	Gtk::HBox _file_box;
	Gtk::Label _synthdef_label;
	Gtk::Entry _synthdef_entry;
	Gtk::CheckButton _enable_button;
	Gtk::CheckButton _auto_synthdef_button;
	Gtk::Button _apply_button;
	Gtk::Button _load_button;
	Gtk::Button _save_button;
	Gtk::ScrolledWindow _source_scroller;
	Gtk::TextView _source_view;
	Glib::RefPtr<Gtk::TextBuffer> _source_buffer;

	std::shared_ptr<ARDOUR::Route> _route;
	PBD::ScopedConnectionList      _connections;
	bool _updating;
	bool _dirty;
};
