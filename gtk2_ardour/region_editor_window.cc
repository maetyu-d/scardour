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

#include "region_editor_window.h"
#include "audio_clip_editor.h"
#include "audio_region_editor.h"
#include "audio_region_view.h"

#include <sstream>

#include "pbd/i18n.h"

using namespace ARDOUR;

RegionEditorWindow::RegionEditorWindow (Session* s, RegionView* rv)
	: ArdourWindow (_("Region Properties"))
	, _undo_button (_("Undo"))
	, _redo_button (_("Redo"))
	, _play_button (_("Play"))
	, _loop_button (_("Loop"))
	, _trim_button (_("Trim"))
	, _silence_button (_("Silence"))
	, _page_left_button (_("Back"))
	, _page_right_button (_("Forward"))
	, _zoom_out_button (_("-"))
	, _zoom_fit_button (_("Fit"))
	, _zoom_in_button (_("+"))
{
	_contents.set_spacing (3);
	_contents.set_border_width (4);
	_toolbar_box.set_spacing (2);
	_toolbar_left.set_spacing (2);
	_toolbar_right.set_spacing (2);

	AudioRegionView* arv = dynamic_cast<AudioRegionView*> (rv);
	if (arv) {
		std::ostringstream editor_name;
		editor_name << X_("AudioDetailEditor") << this;
		_audio_clip_editor = new AudioClipEditor (editor_name.str (), true);
		_audio_clip_editor->set_region (arv->region ());
		_audio_clip_editor->set_upper_toolbar_visible (false);
		_audio_clip_editor->get_canvas_viewport()->set_size_request (-1, 240);

		auto setup_button = [] (Gtk::Button& button, int width) {
			button.set_size_request (width, 26);
			button.set_can_focus (false);
		};

		setup_button (_undo_button, 76);
		setup_button (_redo_button, 76);
		setup_button (_trim_button, 76);
		setup_button (_silence_button, 82);
		setup_button (_play_button, 68);
		setup_button (_loop_button, 68);
		setup_button (_page_left_button, 76);
		setup_button (_page_right_button, 76);
		setup_button (_zoom_fit_button, 52);
		setup_button (_zoom_out_button, 40);
		setup_button (_zoom_in_button, 40);

		_toolbar_left.pack_start (_undo_button, false, false);
		_toolbar_left.pack_start (_redo_button, false, false);
		_toolbar_left.pack_start (_trim_button, false, false);
		_toolbar_left.pack_start (_silence_button, false, false);
		_toolbar_left.pack_start (_play_button, false, false, 2);
		_toolbar_left.pack_start (_loop_button, false, false);

		_toolbar_right.pack_start (_page_left_button, false, false);
		_toolbar_right.pack_start (_page_right_button, false, false);
		_toolbar_right.pack_start (_zoom_fit_button, false, false, 1);
		_toolbar_right.pack_start (_zoom_out_button, false, false);
		_toolbar_right.pack_start (_zoom_in_button, false, false);

		_undo_button.signal_clicked ().connect (sigc::mem_fun (*_audio_clip_editor, &AudioClipEditor::undo_edit));
		_redo_button.signal_clicked ().connect (sigc::mem_fun (*_audio_clip_editor, &AudioClipEditor::redo_edit));
		_play_button.signal_clicked ().connect (sigc::mem_fun (*_audio_clip_editor, &AudioClipEditor::play_from_selection));
		_loop_button.signal_clicked ().connect (sigc::mem_fun (*_audio_clip_editor, &AudioClipEditor::loop_selection));
		_trim_button.signal_clicked ().connect (sigc::mem_fun (*_audio_clip_editor, &AudioClipEditor::trim_to_selection));
		_silence_button.signal_clicked ().connect (sigc::mem_fun (*_audio_clip_editor, &AudioClipEditor::silence_selection));
		_page_left_button.signal_clicked ().connect (sigc::mem_fun (*_audio_clip_editor, &AudioClipEditor::page_left));
		_page_right_button.signal_clicked ().connect (sigc::mem_fun (*_audio_clip_editor, &AudioClipEditor::page_right));
		_zoom_out_button.signal_clicked ().connect (sigc::mem_fun (*_audio_clip_editor, &AudioClipEditor::zoom_out_detail));
		_zoom_fit_button.signal_clicked ().connect (sigc::mem_fun (*_audio_clip_editor, &AudioClipEditor::fit_region));
		_zoom_in_button.signal_clicked ().connect (sigc::mem_fun (*_audio_clip_editor, &AudioClipEditor::zoom_in_detail));

		_region_editor = new AudioRegionEditor (s, arv);

		_toolbar_spacer.set_text ("");
		_toolbar_box.pack_start (_toolbar_left, false, false);
		_toolbar_box.pack_start (_toolbar_spacer, true, true);
		_toolbar_box.pack_start (_toolbar_right, false, false);

		_editor_pane.pack1 (_audio_clip_editor->contents (), true, false);
		_editor_pane.pack2 (*_region_editor, true, false);
		_editor_pane.set_position (332);

		_contents.pack_start (_toolbar_box, false, false, 0);
		_contents.pack_start (_editor_pane, true, true);
		add (_contents);
		set_title (_("Audio Detail Editor"));
		set_default_size (1240, 1020);
	} else {
		_region_editor = new RegionEditor (s, rv->region());
		add (*_region_editor);
	}

	set_name ("RegionEditorWindow");
}

RegionEditorWindow::~RegionEditorWindow ()
{
	delete _audio_clip_editor;
	delete _region_editor;
}

void
RegionEditorWindow::set_session (Session* s)
{
	ArdourWindow::set_session (s);
	if (s) {
		_region_editor->set_session (s);
	}
}

void
RegionEditorWindow::on_unmap ()
{
	_region_editor->unmap ();
	ArdourWindow::on_unmap ();
}
