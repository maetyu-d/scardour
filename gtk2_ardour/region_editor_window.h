/*
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

#include <ytkmm/box.h>
#include <ytkmm/button.h>
#include <ytkmm/label.h>
#include <ytkmm/paned.h>
#include <ytkmm/separator.h>

#include "ardour_window.h"

class AudioClipEditor;
class RegionEditor;
class RegionView;

class RegionEditorWindow : public ArdourWindow
{
public:
	RegionEditorWindow (ARDOUR::Session*, RegionView*);
	~RegionEditorWindow ();

	void set_session (ARDOUR::Session*);

protected:
	void on_unmap ();

private:
	Gtk::VBox    _contents;
	Gtk::HBox    _toolbar_box;
	Gtk::HBox    _toolbar_left;
	Gtk::HBox    _toolbar_right;
	Gtk::Label   _toolbar_spacer;
	Gtk::HSeparator _toolbar_separator;
	Gtk::VPaned  _editor_pane;
	Gtk::Button  _undo_button;
	Gtk::Button  _redo_button;
	Gtk::Button  _play_button;
	Gtk::Button  _loop_button;
	Gtk::Button  _trim_button;
	Gtk::Button  _silence_button;
	Gtk::Button  _page_left_button;
	Gtk::Button  _page_right_button;
	Gtk::Button  _zoom_out_button;
	Gtk::Button  _zoom_fit_button;
	Gtk::Button  _zoom_in_button;
	AudioClipEditor* _audio_clip_editor = nullptr;
	RegionEditor*    _region_editor     = nullptr;
};
