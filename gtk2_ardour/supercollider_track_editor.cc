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

#include "pbd/compose.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <glib.h>

#include "ardour/audio_track.h"
#include "ardour/audiofilesource.h"
#include "ardour/midi_model.h"
#include "ardour/midi_region.h"
#include "ardour/midi_source.h"
#include "ardour/midi_track.h"
#include "ardour/playlist.h"
#include "ardour/region.h"
#include "ardour/region_factory.h"
#include "ardour/session.h"
#include "ardour/source_factory.h"
#include "ardour/session_directory.h"
#include "ardour/tempo.h"

#include "gui_thread.h"
#include "editor.h"
#include "public_editor.h"
#include "region_view.h"
#include "supercollider_track_editor.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

SuperColliderTrackEditor::SuperColliderTrackEditor (std::shared_ptr<Route> route)
	: ArdourWindow ("")
	, _synthdef_label (_("SynthDef"))
	, _auto_synthdef_button (_("Auto-fill from source"))
	, _auto_boot_button (_("Auto-start runtime"))
	, _apply_button (_("Apply"))
	, _restart_button (_("Restart"))
	, _render_button (_("Render To Timeline"))
	, _updating (false)
	, _dirty (false)
	, _render_in_progress (false)
{
	const float scale = std::max (1.f, UIConfiguration::instance ().get_ui_scale ());
	set_default_size (620 * scale, 420 * scale);

	add (_vbox);
	_vbox.set_spacing (8);
	_status_label.set_alignment (0.0, 0.5);
	_synthdef_label.set_alignment (0.0, 0.5);
	_controls_box.set_spacing (6);
	_source_buffer = Gtk::TextBuffer::create ();
	_source_view.set_buffer (_source_buffer);
	_source_view.set_editable (true);
	_source_view.set_cursor_visible (true);
	_source_view.set_wrap_mode (Gtk::WRAP_WORD_CHAR);
	_source_view.set_can_focus (true);
	_source_view.set_sensitive (true);
	_source_view.set_size_request (480 * scale, 240 * scale);
	_source_scroller.set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	_source_scroller.add (_source_view);

	_controls_box.pack_start (_synthdef_label, false, false);
	_controls_box.pack_start (_synthdef_entry, true, true);
	_controls_box.pack_start (_auto_synthdef_button, false, false);
	_controls_box.pack_start (_auto_boot_button, false, false);
	_controls_box.pack_start (_apply_button, false, false);
	_controls_box.pack_start (_restart_button, false, false);
	_controls_box.pack_start (_render_button, false, false);

	_vbox.pack_start (_status_label, false, false);
	_vbox.pack_start (_controls_box, false, false);
	_vbox.pack_start (_source_scroller, true, true);

	assert (route);
	_route = route;
	_route->set_supercollider_editor (this);

	_route->PropertyChanged.connect (_connections, invalidator (*this), std::bind (&SuperColliderTrackEditor::route_property_changed, this, _1), gui_context ());
	_route->DropReferences.connect (_connections, invalidator (*this), std::bind (&SuperColliderTrackEditor::route_going_away, this), gui_context ());
	_source_buffer->signal_changed ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::source_or_autofill_changed));
	_synthdef_entry.signal_changed ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::mark_dirty));
	_auto_synthdef_button.signal_toggled ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::source_or_autofill_changed));
	_auto_boot_button.signal_toggled ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::mark_dirty));
	_apply_button.signal_clicked ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::apply_changes));
	_restart_button.signal_clicked ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::restart_runtime));
	_render_button.signal_clicked ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::render_to_timeline));
	Editor* editor = dynamic_cast<Editor*> (&PublicEditor::instance ());
	if (editor) {
		editor->get_selection ().RegionsChanged.connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::region_selection_changed));
	}

	_vbox.show_all ();
	set_position (UIConfiguration::instance ().get_default_window_position ());
	sync_editor ();
	update_title ();
}

SuperColliderTrackEditor::~SuperColliderTrackEditor ()
{
	hide ();
	if (_route) {
		_route->set_supercollider_editor (nullptr);
		_route.reset ();
	}
	_connections.drop_connections ();
}

void
SuperColliderTrackEditor::toggle ()
{
	if (!_route) {
		return;
	}

	if (get_visible ()) {
		hide ();
	} else {
		open ();
	}
}

void
SuperColliderTrackEditor::open ()
{
	if (!_route) {
		return;
	}

	sync_editor ();
	update_title ();
	present ();
	_source_view.grab_focus ();
}

void
SuperColliderTrackEditor::route_property_changed (const PBD::PropertyChange&)
{
	sync_editor ();
	update_title ();
}

void
SuperColliderTrackEditor::route_going_away ()
{
	ENSURE_GUI_THREAD (*this, &SuperColliderTrackEditor::route_going_away);

	if (_route) {
		_route->set_supercollider_editor (nullptr);
		_route.reset ();
	}
	hide ();
}

void
SuperColliderTrackEditor::region_selection_changed ()
{
	if (_dirty) {
		update_title ();
		return;
	}

	sync_editor ();
	update_title ();
}

void
SuperColliderTrackEditor::update_title ()
{
	if (!_route) {
		set_title (_("SuperCollider Track Editor"));
		return;
	}

	std::shared_ptr<ARDOUR::Region> const region = selected_region ();
	if (region) {
		set_title (string_compose ("%1: %2 (%3)", _route->name (), _("SuperCollider Track Editor"), region->name ()));
	} else {
		set_title (string_compose ("%1: %2", _route->name (), _("SuperCollider Track Editor")));
	}
}

std::shared_ptr<ARDOUR::Region>
SuperColliderTrackEditor::selected_region () const
{
	std::shared_ptr<SuperColliderTrack> const sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		return std::shared_ptr<ARDOUR::Region> ();
	}

	Editor* editor = dynamic_cast<Editor*> (&PublicEditor::instance ());
	if (!editor) {
		return std::shared_ptr<ARDOUR::Region> ();
	}

	std::shared_ptr<Playlist> const playlist = sct->playlist ();
	if (!playlist) {
		return std::shared_ptr<ARDOUR::Region> ();
	}

	std::shared_ptr<ARDOUR::Region> match;
	RegionSelection const& selection = editor->get_selection ().regions;
	for (RegionSelection::const_iterator i = selection.begin (); i != selection.end (); ++i) {
		if (!*i) {
			continue;
		}

		std::shared_ptr<ARDOUR::Region> const region = (*i)->region ();
		if (!region || region->playlist () != playlist) {
			continue;
		}

		if (match) {
			return std::shared_ptr<ARDOUR::Region> ();
		}

		match = region;
	}

	return match;
}

void
SuperColliderTrackEditor::sync_editor ()
{
	std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		_status_label.set_text (_("The selected route is not a SuperCollider track."));
	_apply_button.set_sensitive (false);
	_restart_button.set_sensitive (false);
	_render_button.set_sensitive (false);
		return;
	}

	std::string status_text;
	std::shared_ptr<ARDOUR::Region> const region = selected_region ();
	if (_dirty) {
		status_text = _("Runtime status: unsaved changes");
	} else if (sct->supercollider_runtime_running ()) {
		status_text = _("Runtime status: running");
	} else if (!sct->supercollider_runtime_last_error ().empty ()) {
		status_text = string_compose (_("Runtime status: %1"), sct->supercollider_runtime_last_error ());
	} else if (!sct->supercollider_runtime_available ()) {
		status_text = _("Runtime status: sclang not found");
	} else {
		status_text = _("Runtime status: stopped");
	}

	_updating = true;
	std::string const source_text = region ? sct->supercollider_source_for_region (*region) : sct->supercollider_source ();
	std::string synthdef_text = region ? sct->supercollider_synthdef_for_region (*region) : sct->supercollider_synthdef ();
	if (sct->supercollider_auto_synthdef () && !sct->supercollider_generates_midi ()) {
		std::string const inferred = SuperColliderTrack::infer_supercollider_synthdef (source_text);
		if (!inferred.empty ()) {
			synthdef_text = inferred;
		}
	}
	_synthdef_entry.set_text (synthdef_text);
	_auto_synthdef_button.set_active (sct->supercollider_auto_synthdef ());
	_auto_synthdef_button.set_sensitive (!sct->supercollider_generates_midi ());
	if (sct->supercollider_generates_midi ()) {
		_auto_synthdef_button.hide ();
	} else {
		_auto_synthdef_button.show ();
	}
	_synthdef_entry.set_sensitive (!sct->supercollider_auto_synthdef ());
	_auto_boot_button.set_active (sct->supercollider_auto_boot ());
	_auto_boot_button.set_sensitive (sct->supports_live_runtime ());
	_source_buffer->set_text (source_text);
	_status_label.set_text (status_text);
	_render_button.set_label (sct->supercollider_generates_midi () ? _("Render To MIDI Track") : _("Render To Timeline"));
	_apply_button.set_sensitive (_dirty);
	_restart_button.set_sensitive (sct->supports_live_runtime () && sct->supercollider_auto_boot () && sct->supercollider_runtime_available ());
	_render_button.set_sensitive (sct->supercollider_runtime_available ());
	_updating = false;
}

void
SuperColliderTrackEditor::mark_dirty ()
{
	if (_updating) {
		return;
	}

	if (!std::dynamic_pointer_cast<SuperColliderTrack> (_route)) {
		return;
	}

	_dirty = true;
	_status_label.set_text (_("Runtime status: unsaved changes"));
	_apply_button.set_sensitive (true);
}

void
SuperColliderTrackEditor::source_or_autofill_changed ()
{
	if (_updating) {
		return;
	}

	std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		return;
	}

	if (!sct->supercollider_generates_midi () && _auto_synthdef_button.get_active ()) {
		_synthdef_entry.set_sensitive (false);
		std::string const inferred = SuperColliderTrack::infer_supercollider_synthdef (_source_buffer->get_text ());
		if (!inferred.empty () && _synthdef_entry.get_text () != inferred) {
			_updating = true;
			_synthdef_entry.set_text (inferred);
			_updating = false;
		}
	} else {
		_synthdef_entry.set_sensitive (true);
	}

	mark_dirty ();
}

void
SuperColliderTrackEditor::apply_changes ()
{
	std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		return;
	}

	sct->set_supercollider_auto_synthdef (_auto_synthdef_button.get_active ());
	sct->set_supercollider_auto_boot (_auto_boot_button.get_active ());
	std::shared_ptr<ARDOUR::Region> const region = selected_region ();
	if (region) {
		region->set_supercollider_source (_source_buffer->get_text ());
		region->set_supercollider_synthdef (_synthdef_entry.get_text ());
		region->set_name (sct->supercollider_clip_name (*region));
	} else {
		sct->set_supercollider_source (_source_buffer->get_text ());
		if (!sct->supercollider_auto_synthdef ()) {
			sct->set_supercollider_synthdef (_synthdef_entry.get_text ());
		}
	}

	_dirty = false;
	sync_editor ();
}

void
SuperColliderTrackEditor::restart_runtime ()
{
	std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		return;
	}

	if (_dirty) {
		apply_changes ();
	}

	sct->restart_supercollider_runtime ();
	_dirty = false;
	sync_editor ();
}

namespace {

struct RenderClipTask {
	std::string clip_name;
	std::string clip_id;
	std::string synthdef;
	std::string source;
	timepos_t position;
	ARDOUR::samplecnt_t length_samples;
	double duration;
	std::string render_path;
	std::string script_path;
	std::string log_path;
};

struct RenderClipOutput {
	std::string clip_name;
	timepos_t position;
	std::string render_path;
};

struct RenderWorkResult {
	std::vector<RenderClipOutput> clips;
	std::string failure_reason;
};

struct MidiRenderClipTask {
	std::string clip_name;
	std::string clip_id;
	std::string synthdef;
	std::string source;
	timepos_t position;
	ARDOUR::samplepos_t position_samples;
	ARDOUR::samplecnt_t length_samples;
	std::string notes_path;
	std::string script_path;
};

struct MidiRenderClipOutput {
	std::string clip_name;
	timepos_t position;
	Temporal::Beats region_length_beats;
	std::string notes_path;
};

struct MidiRenderWorkResult {
	std::vector<MidiRenderClipOutput> clips;
	std::string failure_reason;
};

struct MidiRenderNote {
	double start_beats;
	double length_beats;
	int note_number;
	int velocity;
	int channel;
};

std::string
sc_string_literal (std::string const& value)
{
	std::string escaped = "\"";

	for (std::string::const_iterator i = value.begin (); i != value.end (); ++i) {
		switch (*i) {
		case '\\':
			escaped += "\\\\";
			break;
		case '"':
			escaped += "\\\"";
			break;
		case '\n':
			escaped += "\\n";
			break;
		case '\r':
			escaped += "\\r";
			break;
		case '\t':
			escaped += "\\t";
			break;
		default:
			escaped += *i;
			break;
		}
	}

	escaped += "\"";
	return escaped;
}

std::string
normalize_language_script_source (std::string const& source)
{
	std::istringstream input (source);
	std::string line;
	std::string normalized;
	bool first = true;

	while (std::getline (input, line)) {
		std::string trimmed = line;
		std::string::size_type begin = trimmed.find_first_not_of (" \t\r");
		if (begin == std::string::npos) {
			begin = 0;
		}
		std::string::size_type end = trimmed.find_last_not_of (" \t\r");
		if (end == std::string::npos) {
			trimmed.clear ();
		} else {
			trimmed = trimmed.substr (begin, end - begin + 1);
		}

		if (trimmed == "(" || trimmed == ")") {
			continue;
		}

		if (!first) {
			normalized += "\n";
		}
		normalized += line;
		first = false;
	}

	return normalized.empty () ? source : normalized;
}

std::string
shell_quote (std::string const& value)
{
	std::string quoted = "'";

	for (std::string::const_iterator i = value.begin (); i != value.end (); ++i) {
		if (*i == '\'') {
			quoted += "'\\''";
		} else {
			quoted += *i;
		}
	}

	quoted += "'";
	return quoted;
}

bool
run_process_with_timeout (std::string const& command, double timeout_seconds, int& exit_status, bool& timed_out, std::string& error_message)
{
	gchar* argv[] = {
		g_strdup ("/bin/sh"),
		g_strdup ("-c"),
		g_strdup (command.c_str ()),
		0
	};
	GError* error = 0;
	GPid pid = 0;

	bool const ok = g_spawn_async (
		0,
		argv,
		0,
		GSpawnFlags (G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD),
		0,
		0,
		&pid,
		&error
	);

	g_free (argv[0]);
	g_free (argv[1]);
	g_free (argv[2]);

	if (!ok) {
		if (error && error->message) {
			error_message = error->message;
		} else {
			error_message = _("could not start SuperCollider render");
		}
		if (error) {
			g_error_free (error);
		}
		return false;
	}

	timed_out = false;
	exit_status = -1;
	int status = 0;
	int const sleep_usecs = 100000;
	int const max_polls = std::max (1, static_cast<int> ((timeout_seconds * 1000000.0) / sleep_usecs));

	for (int poll = 0; poll < max_polls; ++poll) {
		pid_t const wait_result = ::waitpid (pid, &status, WNOHANG);
		if (wait_result == pid) {
			exit_status = status;
			g_spawn_close_pid (pid);
			return true;
		}
		if (wait_result < 0) {
			error_message = _("could not wait for SuperCollider render");
			g_spawn_close_pid (pid);
			return false;
		}
		g_usleep (sleep_usecs);
	}

	timed_out = true;
	::kill (pid, SIGKILL);
	if (::waitpid (pid, &status, 0) == pid) {
		exit_status = status;
	}
	g_spawn_close_pid (pid);
	return true;
}

bool
source_looks_like_language_script (std::string const& source)
{
	static char const* const markers[] = {
		"SynthDef(",
		"Synth.",
		".play",
		"Group.",
		"Server.",
		"s.bind",
		"Routine(",
		"Task(",
		"TempoClock",
		"~",
		0
	};

	for (char const* const* marker = markers; *marker; ++marker) {
		if (source.find (*marker) != std::string::npos) {
			return true;
		}
	}

	return false;
}

bool
split_synthdef_prelude (std::string const& source, std::string& prelude, std::string& body)
{
	std::string::size_type const synthdef_pos = source.find ("SynthDef(");
	if (synthdef_pos == std::string::npos) {
		return false;
	}

	std::string::size_type const playback_pos = source.find (".play", synthdef_pos);
	std::string::size_type search_pos = synthdef_pos;
	std::string::size_type last_add_pos = std::string::npos;

	while (true) {
		std::string::size_type const add_pos = source.find (".add;", search_pos);
		if (add_pos == std::string::npos) {
			break;
		}
		if (playback_pos != std::string::npos && add_pos > playback_pos) {
			break;
		}
		last_add_pos = add_pos;
		search_pos = add_pos + 5;
	}

	if (last_add_pos == std::string::npos) {
		return false;
	}

	std::string::size_type split_pos = last_add_pos + 5;
	while (split_pos < source.size ()) {
		char const c = source[split_pos];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			++split_pos;
			continue;
		}
		if (c == ')' || c == ';') {
			++split_pos;
			continue;
		}
		break;
	}

	prelude = source.substr (0, split_pos);
	body = source.substr (split_pos);
	return true;
}

bool
parse_sc_midi_notes (std::string const& path, std::vector<MidiRenderNote>& notes, std::string& error)
{
	std::ifstream input (path.c_str ());
	if (!input) {
		error = _("rendered MIDI note data could not be read");
		return false;
	}

	std::string line;
	unsigned int line_number = 0;
	while (std::getline (input, line)) {
		++line_number;
		if (line.empty ()) {
			continue;
		}

		std::vector<std::string> fields;
		std::string current;
		for (std::string::const_iterator i = line.begin (); i != line.end (); ++i) {
			if (*i == '\t') {
				fields.push_back (current);
				current.clear ();
			} else {
				current += *i;
			}
		}
		fields.push_back (current);

		if (fields.size () != 5) {
			error = string_compose (_("rendered MIDI note data is malformed on line %1"), line_number);
			return false;
		}

		try {
			MidiRenderNote note;
			note.start_beats = std::stod (fields[0]);
			note.length_beats = std::stod (fields[1]);
			note.note_number = std::stoi (fields[2]);
			note.velocity = std::stoi (fields[3]);
			note.channel = std::stoi (fields[4]);
			notes.push_back (note);
		} catch (...) {
			error = string_compose (_("rendered MIDI note data is malformed on line %1"), line_number);
			return false;
		}
	}

	return true;
}

std::vector<std::shared_ptr<ARDOUR::Region> >
selected_regions_for_track (std::shared_ptr<SuperColliderTrack> const& sct)
{
	std::vector<std::shared_ptr<ARDOUR::Region> > regions;
	if (!sct) {
		return regions;
	}

	Editor* editor = dynamic_cast<Editor*> (&PublicEditor::instance ());
	if (!editor) {
		return regions;
	}

	std::shared_ptr<Playlist> const playlist = sct->playlist ();
	if (!playlist) {
		return regions;
	}

	RegionSelection const& selection = editor->get_selection ().regions;
	if (selection.empty ()) {
		return regions;
	}

	playlist->foreach_region ([&regions, &selection] (std::shared_ptr<ARDOUR::Region> region) {
		if (region && selection.contains (region)) {
			regions.push_back (region);
		}
	});

	return regions;
}

}

void
SuperColliderTrackEditor::render_to_timeline ()
{
	std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		return;
	}

	if (sct->supercollider_generates_midi ()) {
		render_to_midi_track ();
		return;
	}

	if (_render_in_progress.exchange (true)) {
		_status_label.set_text (_("Render status: render already in progress"));
		return;
	}

	if (_dirty) {
		apply_changes ();
	}

	std::shared_ptr<Playlist> const source_playlist = sct->playlist ();
	if (!source_playlist) {
		_render_in_progress = false;
		_status_label.set_text (_("Render status: no timeline clips to render"));
		return;
	}

	std::vector<std::shared_ptr<Region> > clips = selected_regions_for_track (sct);
	bool const using_selection = !clips.empty ();
	if (!using_selection) {
		source_playlist->foreach_region ([&clips] (std::shared_ptr<Region> region) {
			if (region) {
				clips.push_back (region);
			}
		});
	}

	if (clips.empty ()) {
		_render_in_progress = false;
		_status_label.set_text (_("Render status: add a region to the SuperCollider track timeline first"));
		return;
	}

	std::sort (clips.begin (), clips.end (), [] (std::shared_ptr<Region> const& a, std::shared_ptr<Region> const& b) {
		return a->position_sample () < b->position_sample ();
	});

	Session& session = sct->session ();
	std::string const render_track_name = string_compose (_("%1 Render"), sct->name ());
	std::shared_ptr<AudioTrack> render_track = std::dynamic_pointer_cast<AudioTrack> (session.route_by_name (render_track_name));

	if (!render_track) {
		uint32_t output_chan = 2;
		if ((Config->get_output_auto_connect() & AutoConnectMaster) && session.master_out ()) {
			output_chan = session.master_out ()->n_inputs ().n_audio ();
		}

		AudioTrackList tracks = session.new_audio_track (2, output_chan, 0, 1, render_track_name, PresentationInfo::max_order, Normal, true, false);
		if (tracks.empty ()) {
			_render_in_progress = false;
			_status_label.set_text (_("Render status: could not create companion audio track"));
			return;
		}

		render_track = tracks.front ();
	}

	std::shared_ptr<Playlist> const render_playlist = render_track->playlist ();
	if (!render_playlist) {
		_render_in_progress = false;
		_status_label.set_text (_("Render status: companion audio track has no playlist"));
		return;
	}

	std::string const runtime_path = sct->supercollider_runtime_path ();
	if (runtime_path.empty ()) {
		_render_in_progress = false;
		_status_label.set_text (_("Render status: sclang not found"));
		return;
	}

	std::vector<RenderClipTask> render_tasks;
	std::string const sound_path = session.session_directory ().sound_path ();
	double const sample_rate = session.sample_rate ();
	std::string const track_id = sct->id ().to_s ();
	std::string const track_name = sct->name ();

	for (std::vector<std::shared_ptr<Region> >::const_iterator i = clips.begin (); i != clips.end (); ++i) {
		std::shared_ptr<Region> const clip = *i;
		double const duration = static_cast<double> (clip->length_samples ()) / sample_rate;
		if (duration <= 0.0) {
			continue;
		}

		std::string const base_name = string_compose ("scardour-%1-%2", track_id, clip->id ().to_s ());
		RenderClipTask task;
		task.clip_name = clip->name ();
		task.clip_id = clip->id ().to_s ();
		task.synthdef = sct->supercollider_synthdef_for_region (*clip);
		task.source = normalize_language_script_source (sct->supercollider_source_for_region (*clip));
		task.position = clip->position ();
		task.length_samples = clip->length_samples ();
		task.duration = duration;
		task.render_path = Glib::build_filename (sound_path, base_name + ".wav");
		task.script_path = Glib::build_filename (sound_path, base_name + ".scd");
		task.log_path = Glib::build_filename (sound_path, base_name + ".log");
		render_tasks.push_back (task);
	}

	if (render_tasks.empty ()) {
		_render_in_progress = false;
		_status_label.set_text (_("Render status: no non-empty clips to render"));
		return;
	}

	if (using_selection) {
		_status_label.set_text (string_compose (_("Render status: rendering %1 selected clip(s)..."), render_tasks.size ()));
	} else {
		_status_label.set_text (string_compose (_("Render status: rendering %1 clip(s)..."), render_tasks.size ()));
	}
	_apply_button.set_sensitive (false);
	_restart_button.set_sensitive (false);
	_render_button.set_sensitive (false);

	std::thread ([this, render_tasks, runtime_path, sample_rate, track_id, track_name, render_track_name] () {
		std::shared_ptr<RenderWorkResult> result (new RenderWorkResult);

		for (std::vector<RenderClipTask>::const_iterator i = render_tasks.begin (); i != render_tasks.end (); ++i) {
			bool const source_is_language_script = source_looks_like_language_script (i->source);
			std::string synthdef_prelude;
			std::string synthdef_body;
			bool const split_synthdef = source_is_language_script && split_synthdef_prelude (i->source, synthdef_prelude, synthdef_body);
			std::ofstream script (i->script_path.c_str (), std::ios::out | std::ios::trunc);
			if (!script) {
				result->failure_reason = _("could not write SuperCollider render script");
				break;
			}

			script
				<< "~scardourOutputPath = " << sc_string_literal (i->render_path) << ";\n"
				<< "~scardourSampleRate = " << sample_rate << ";\n"
				<< "~scardourDuration = " << i->duration << ";\n"
				<< "Server.killAll;\n"
				<< "s = Server.local;\n"
				<< "s.options.sampleRate = ~scardourSampleRate;\n"
				<< "s.options.numOutputBusChannels = 2;\n"
				<< "s.options.numInputBusChannels = 2;\n"
				<< "s.waitForBoot({\n"
				<< "    s.record(path: ~scardourOutputPath, numChannels: 2);\n"
				<< "    ~scardourTracks = IdentityDictionary.new;\n"
				<< "    ~ardourTracks = ~scardourTracks;\n"
				<< "    ~scardourTrackId = " << sc_string_literal (track_id) << ";\n"
				<< "    ~scardourTrackName = " << sc_string_literal (track_name) << ";\n"
				<< "    ~scardourSynthDef = " << sc_string_literal (i->synthdef) << ";\n"
				<< "    ~scardourRegionId = " << sc_string_literal (i->clip_id) << ";\n"
				<< "    ~scardourRegionName = " << sc_string_literal (i->clip_name) << ";\n"
				<< "    ~scardourRegionStart = 0;\n"
				<< "    ~scardourRegionEnd = " << i->length_samples << ";\n"
				<< "    ~scardourRegionDuration = ~scardourDuration;\n"
				<< "    ~ardourTrackId = ~scardourTrackId;\n"
				<< "    ~ardourTrackName = ~scardourTrackName;\n"
				<< "    ~ardourSynthDef = ~scardourSynthDef;\n"
				<< "    ~ardourRegionId = ~scardourRegionId;\n"
				<< "    ~ardourRegionName = ~scardourRegionName;\n"
				<< "    ~ardourRegionStart = ~scardourRegionStart;\n"
				<< "    ~ardourRegionEnd = ~scardourRegionEnd;\n"
				<< "    ~ardourRegionDuration = ~scardourRegionDuration;\n"
				<< "    ~scardourTrackGroup = Group.tail(s);\n"
				<< "    ~ardourTrackGroup = ~scardourTrackGroup;\n"
				<< "    ~scardourRenderPlayer = nil;\n"
				<< "    ~ardourRenderPlayer = nil;\n";

			if (source_is_language_script) {
				if (split_synthdef) {
					script
						<< synthdef_prelude << "\n"
						<< "    s.sync;\n"
						<< "    ~scardourRenderResult = {\n"
						<< synthdef_body << "\n"
						<< "    }.value;\n";
				} else {
					script
						<< "    ~scardourRenderResult = {\n"
						<< i->source << "\n"
						<< "    }.value;\n";
				}
				script
					<< "    if (~scardourRenderResult.notNil) {\n"
					<< "        ~scardourRenderPlayer = ~scardourRenderResult;\n"
					<< "        ~ardourRenderPlayer = ~scardourRenderPlayer;\n"
					<< "    };\n"
					<< "    if (~tone.notNil) {\n"
					<< "        ~scardourRenderPlayer = ~tone;\n"
					<< "        ~ardourRenderPlayer = ~scardourRenderPlayer;\n"
					<< "    };\n";
			} else {
				script
					<< "    ~scardourRenderPlayer = {\n"
					<< i->source << "\n"
					<< "    }.play(target: ~scardourTrackGroup);\n"
					<< "    ~ardourRenderPlayer = ~scardourRenderPlayer;\n";
			}

			script
				<< "    SystemClock.sched(~scardourDuration.max(0.05), {\n"
				<< "        if (~scardourRenderPlayer.notNil) {\n"
				<< "            if (~scardourRenderPlayer.respondsTo(\\stop)) { ~scardourRenderPlayer.stop; };\n"
				<< "            if (~scardourRenderPlayer.respondsTo(\\release)) { ~scardourRenderPlayer.release; };\n"
				<< "            if (~scardourRenderPlayer.respondsTo(\\free)) { ~scardourRenderPlayer.free; };\n"
				<< "            ~scardourRenderPlayer = nil;\n"
				<< "            ~ardourRenderPlayer = nil;\n"
				<< "        };\n"
				<< "        if (~tone.notNil) {\n"
				<< "            if (~tone.respondsTo(\\release)) { ~tone.release; };\n"
				<< "            if (~tone.respondsTo(\\free)) { ~tone.free; };\n"
				<< "            ~tone = nil;\n"
				<< "        };\n"
				<< "        if (~scardourTrackGroup.notNil) {\n"
				<< "            ~scardourTrackGroup.freeAll;\n"
				<< "            ~scardourTrackGroup.free;\n"
				<< "        };\n"
				<< "        CmdPeriod.run;\n"
				<< "        s.stopRecording;\n"
				<< "        s.freeAll;\n"
				<< "        s.quit;\n"
				<< "        0.exit;\n"
				<< "        nil;\n"
				<< "    });\n"
				<< "});\n"
				<< "s.boot;\n";

			script.close ();

			std::string const command =
				shell_quote (runtime_path) + " " +
				shell_quote (i->script_path) + " > " +
				shell_quote (i->log_path) + " 2>&1";
			int exit_status = 0;
			bool timed_out = false;
			std::string spawn_error;
			double const timeout_seconds = std::max (15.0, i->duration + 10.0);
			bool const ok = run_process_with_timeout (command, timeout_seconds, exit_status, timed_out, spawn_error);

			if (!ok || timed_out || exit_status != 0 || !g_file_test (i->render_path.c_str (), G_FILE_TEST_EXISTS)) {
				result->failure_reason = _("SuperCollider render failed");
				if (!ok && !spawn_error.empty ()) {
					result->failure_reason = spawn_error;
				} else if (timed_out) {
					result->failure_reason = string_compose (_("SuperCollider render timed out after %1 seconds"), static_cast<int> (timeout_seconds));
				} else {
					std::ifstream log_input (i->log_path.c_str ());
					if (log_input) {
						std::string log_contents ((std::istreambuf_iterator<char> (log_input)), std::istreambuf_iterator<char> ());
						if (!log_contents.empty ()) {
							result->failure_reason = log_contents;
						}
					}
				}
			} else {
				RenderClipOutput output;
				output.clip_name = i->clip_name;
				output.position = i->position;
				output.render_path = i->render_path;
				result->clips.push_back (output);
			}

			if (!result->failure_reason.empty ()) {
				break;
			}
		}

		Gtkmm2ext::UI::instance()->call_slot (invalidator (*this), [this, result, render_track_name] () {
			_render_in_progress = false;

			std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
			if (!sct) {
				return;
			}

			Session& session = sct->session ();
			std::shared_ptr<AudioTrack> render_track = std::dynamic_pointer_cast<AudioTrack> (session.route_by_name (render_track_name));

			if (!render_track) {
				uint32_t output_chan = 2;
				if ((Config->get_output_auto_connect() & AutoConnectMaster) && session.master_out ()) {
					output_chan = session.master_out ()->n_inputs ().n_audio ();
				}

				AudioTrackList tracks = session.new_audio_track (2, output_chan, 0, 1, render_track_name, PresentationInfo::max_order, Normal, true, false);
				if (tracks.empty ()) {
					_status_label.set_text (_("Render status: could not create companion audio track"));
					_apply_button.set_sensitive (_dirty);
					_restart_button.set_sensitive (sct->supercollider_auto_boot () && sct->supercollider_runtime_available ());
					_render_button.set_sensitive (sct->supercollider_runtime_available ());
					return;
				}

				render_track = tracks.front ();
			}

			std::shared_ptr<Playlist> const render_playlist = render_track->playlist ();
			if (!render_playlist) {
				_status_label.set_text (_("Render status: companion audio track has no playlist"));
				_apply_button.set_sensitive (_dirty);
				_restart_button.set_sensitive (sct->supercollider_auto_boot () && sct->supercollider_runtime_available ());
				_render_button.set_sensitive (sct->supercollider_runtime_available ());
				return;
			}

			unsigned int rendered = 0;
			bool command_started = false;

			for (std::vector<RenderClipOutput>::const_iterator i = result->clips.begin (); i != result->clips.end (); ++i) {
				SourceList sources;

				try {
					std::shared_ptr<Source> left = session.audio_source_by_path_and_channel (i->render_path, 0);
					if (!left) {
						left = SourceFactory::createExternal (DataType::AUDIO, session, i->render_path, 0, Source::Flag (0), true, true);
					}
					sources.push_back (left);

					std::shared_ptr<Source> right = session.audio_source_by_path_and_channel (i->render_path, 1);
					if (!right) {
						right = SourceFactory::createExternal (DataType::AUDIO, session, i->render_path, 1, Source::Flag (0), true, true);
					}
					sources.push_back (right);
				} catch (failed_constructor const&) {
					result->failure_reason = _("rendered file could not be imported");
					break;
				}

				if (!sources[0] || !sources[1]) {
					result->failure_reason = _("rendered file could not be imported");
					break;
				}

				PBD::PropertyList plist;
				plist.add (ARDOUR::Properties::start, timepos_t (Temporal::AudioTime));
				plist.add (ARDOUR::Properties::length, sources[0]->length ());
				plist.add (ARDOUR::Properties::name, string_compose (_("SC Render: %1"), i->clip_name));
				plist.add (ARDOUR::Properties::layer, 0);
				plist.add (ARDOUR::Properties::whole_file, true);
				plist.add (ARDOUR::Properties::external, true);
				plist.add (ARDOUR::Properties::opaque, true);

				std::shared_ptr<Region> const rendered_region = RegionFactory::create (sources, plist);
				if (!command_started) {
					PublicEditor::instance ().begin_reversible_command (_("Render SuperCollider To Timeline"));
					command_started = true;
				}
				render_playlist->clear_changes ();
				render_playlist->clear_owned_changes ();
				render_playlist->add_region (rendered_region, i->position);
				render_playlist->rdiff_and_add_command (&session);
				++rendered;
			}

			if (command_started) {
				if (rendered > 0) {
					PublicEditor::instance ().commit_reversible_command ();
				} else {
					PublicEditor::instance ().abort_reversible_command ();
				}
			}

			if (!result->failure_reason.empty ()) {
				if (rendered > 0) {
					_status_label.set_text (string_compose (_("Render status: placed %1 rendered clip(s) on %2 before %3"), rendered, render_track->name (), result->failure_reason));
				} else {
					_status_label.set_text (string_compose (_("Render status: %1"), result->failure_reason));
				}
				_apply_button.set_sensitive (_dirty);
				_restart_button.set_sensitive (sct->supercollider_auto_boot () && sct->supercollider_runtime_available ());
				_render_button.set_sensitive (sct->supercollider_runtime_available ());
				return;
			}

			_status_label.set_text (string_compose (_("Render status: placed %1 rendered clip(s) on %2"), rendered, render_track->name ()));
			_apply_button.set_sensitive (_dirty);
			_restart_button.set_sensitive (sct->supercollider_auto_boot () && sct->supercollider_runtime_available ());
			_render_button.set_sensitive (sct->supercollider_runtime_available ());
		});
	}).detach ();
}

void
SuperColliderTrackEditor::render_to_midi_track ()
{
	std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		return;
	}

	if (_render_in_progress.exchange (true)) {
		_status_label.set_text (_("Render status: render already in progress"));
		return;
	}

	if (_dirty) {
		apply_changes ();
	}

	std::shared_ptr<Playlist> const source_playlist = sct->playlist ();
	if (!source_playlist) {
		_render_in_progress = false;
		_status_label.set_text (_("Render status: no timeline clips to render"));
		return;
	}

	std::vector<std::shared_ptr<Region> > clips = selected_regions_for_track (sct);
	bool const using_selection = !clips.empty ();
	if (!using_selection) {
		source_playlist->foreach_region ([&clips] (std::shared_ptr<Region> region) {
			if (region) {
				clips.push_back (region);
			}
		});
	}

	if (clips.empty ()) {
		_render_in_progress = false;
		_status_label.set_text (_("Render status: add a region to the SuperCollider track timeline first"));
		return;
	}

	std::sort (clips.begin (), clips.end (), [] (std::shared_ptr<Region> const& a, std::shared_ptr<Region> const& b) {
		return a->position_sample () < b->position_sample ();
	});

	std::string const runtime_path = sct->supercollider_runtime_path ();
	if (runtime_path.empty ()) {
		_render_in_progress = false;
		_status_label.set_text (_("Render status: sclang not found"));
		return;
	}

	Session& session = sct->session ();
	Temporal::TempoMap::SharedPtr const tempo_map = Temporal::TempoMap::use ();
	std::string const sound_path = session.session_directory ().sound_path ();
	std::string const track_id = sct->id ().to_s ();
	std::string const track_name = sct->name ();

	std::vector<MidiRenderClipTask> render_tasks;
	for (std::vector<std::shared_ptr<Region> >::const_iterator i = clips.begin (); i != clips.end (); ++i) {
		std::shared_ptr<Region> const clip = *i;
		if (clip->length_samples () == 0) {
			continue;
		}

		std::string const base_name = string_compose ("scardour-midi-%1-%2", track_id, clip->id ().to_s ());
		MidiRenderClipTask task;
		task.clip_name = clip->name ();
		task.clip_id = clip->id ().to_s ();
		task.synthdef = sct->supercollider_synthdef_for_region (*clip);
		task.source = normalize_language_script_source (sct->supercollider_source_for_region (*clip));
		task.position = clip->position ();
		task.position_samples = clip->position_sample ();
		task.length_samples = clip->length_samples ();
		task.notes_path = Glib::build_filename (sound_path, base_name + ".notes");
		task.script_path = Glib::build_filename (sound_path, base_name + ".scd");
		render_tasks.push_back (task);
	}

	if (render_tasks.empty ()) {
		_render_in_progress = false;
		_status_label.set_text (_("Render status: no non-empty clips to render"));
		return;
	}

	std::string const render_track_name = string_compose (_("%1 MIDI Render"), sct->name ());
	if (using_selection) {
		_status_label.set_text (string_compose (_("Render status: rendering %1 selected clip(s) to MIDI..."), render_tasks.size ()));
	} else {
		_status_label.set_text (string_compose (_("Render status: rendering %1 clip(s) to MIDI..."), render_tasks.size ()));
	}
	_apply_button.set_sensitive (false);
	_restart_button.set_sensitive (false);
	_render_button.set_sensitive (false);

	std::thread ([this, render_tasks, runtime_path, track_id, track_name, render_track_name, tempo_map] () {
		std::shared_ptr<MidiRenderWorkResult> result (new MidiRenderWorkResult);

		for (std::vector<MidiRenderClipTask>::const_iterator i = render_tasks.begin (); i != render_tasks.end (); ++i) {
			std::ofstream script (i->script_path.c_str (), std::ios::out | std::ios::trunc);
			if (!script) {
				result->failure_reason = _("could not write SuperCollider MIDI render script");
				break;
			}

			script
				<< "~scardourOutputPath = " << sc_string_literal (i->notes_path) << ";\n"
				<< "~scardourEventValue = { |event, symbolKey, stringKey, fallback|\n"
				<< "    var value = event[symbolKey];\n"
				<< "    if (value.isNil) { value = event.at(stringKey); };\n"
				<< "    if (value.isNil) { value = fallback; };\n"
				<< "    value;\n"
				<< "};\n"
				<< "~scardourTrackId = " << sc_string_literal (track_id) << ";\n"
				<< "~scardourTrackName = " << sc_string_literal (track_name) << ";\n"
				<< "~scardourSynthDef = " << sc_string_literal (i->synthdef) << ";\n"
				<< "~scardourRegionId = " << sc_string_literal (i->clip_id) << ";\n"
				<< "~scardourRegionName = " << sc_string_literal (i->clip_name) << ";\n"
				<< "~ardourTrackId = ~scardourTrackId;\n"
				<< "~ardourTrackName = ~scardourTrackName;\n"
				<< "~ardourSynthDef = ~scardourSynthDef;\n"
				<< "~ardourRegionId = ~scardourRegionId;\n"
				<< "~ardourRegionName = ~scardourRegionName;\n"
				<< "~scardourMidiNotes = List.new;\n"
				<< "~ardourMidiNotes = ~scardourMidiNotes;\n"
				<< i->source << "\n"
				<< "~ardourMidiNotes = ~ardourMidiNotes ? ~scardourMidiNotes;\n"
				<< "~scardourMidiNotes = ~scardourMidiNotes ? ~ardourMidiNotes;\n"
				<< "~scardourMidiNotesToWrite = ~scardourMidiNotes ? Array.new;\n"
				<< "File.use(~scardourOutputPath, \"w\", { |file|\n"
				<< "    ~scardourMidiNotesToWrite.do({ |event|\n"
				<< "        var start = ~scardourEventValue.(event, \\start, \"start\", ~scardourEventValue.(event, \\time, \"time\", 0)).asFloat;\n"
				<< "        var length = ~scardourEventValue.(event, \\length, \"length\", ~scardourEventValue.(event, \\dur, \"dur\", 0.25)).asFloat.max(0.001);\n"
				<< "        var note = ~scardourEventValue.(event, \\note, \"note\", ~scardourEventValue.(event, \\pitch, \"pitch\", 60)).asInteger.clip(0, 127);\n"
				<< "        var velocity = ~scardourEventValue.(event, \\velocity, \"velocity\", ~scardourEventValue.(event, \\vel, \"vel\", 100)).asInteger.clip(1, 127);\n"
				<< "        var channel = ~scardourEventValue.(event, \\channel, \"channel\", 0).asInteger.clip(0, 15);\n"
				<< "        file.write(\"%\\t%\\t%\\t%\\t%\\n\".format(start, length, note, velocity, channel));\n"
				<< "    });\n"
				<< "});\n"
				<< "0.exit;\n";

			script.close ();

			gchar* argv[] = {
				g_strdup (runtime_path.c_str ()),
				g_strdup (i->script_path.c_str ()),
				0
			};
			gchar* standard_output = 0;
			gchar* standard_error = 0;
			gint exit_status = 0;
			GError* error = 0;

			bool const ok = g_spawn_sync (
				0,
				argv,
				0,
				G_SPAWN_SEARCH_PATH,
				0,
				0,
				&standard_output,
				&standard_error,
				&exit_status,
				&error
			);

			g_free (argv[0]);
			g_free (argv[1]);

			if (!ok || exit_status != 0 || !g_file_test (i->notes_path.c_str (), G_FILE_TEST_EXISTS)) {
				result->failure_reason = _("SuperCollider MIDI render failed");
				if (error && error->message) {
					result->failure_reason = error->message;
				} else if (standard_error && *standard_error) {
					result->failure_reason = standard_error;
				}
			} else {
				MidiRenderClipOutput output;
				output.clip_name = i->clip_name;
				output.position = i->position;
				output.notes_path = i->notes_path;
				Temporal::Beats const start_beats = tempo_map->quarters_at (timepos_t (i->position_samples));
				Temporal::Beats const end_beats = tempo_map->quarters_at (timepos_t (i->position_samples + i->length_samples));
				output.region_length_beats = end_beats - start_beats;
				result->clips.push_back (output);
			}

			if (error) {
				g_error_free (error);
			}
			g_free (standard_output);
			g_free (standard_error);

			if (!result->failure_reason.empty ()) {
				break;
			}
		}

		Gtkmm2ext::UI::instance()->call_slot (invalidator (*this), [this, result, render_track_name] () {
			_render_in_progress = false;

			std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
			if (!sct) {
				return;
			}

			Session& session = sct->session ();
			std::shared_ptr<MidiTrack> render_track = std::dynamic_pointer_cast<MidiTrack> (session.route_by_name (render_track_name));

			if (!render_track) {
				ChanCount one_midi_channel;
				one_midi_channel.set (DataType::MIDI, 1);
				std::list<std::shared_ptr<MidiTrack> > tracks = session.new_midi_track (one_midi_channel, one_midi_channel, false, std::shared_ptr<PluginInfo> (), 0, std::shared_ptr<RouteGroup> (), 1, render_track_name, PresentationInfo::max_order, Normal, true, false);
				if (tracks.empty ()) {
					_status_label.set_text (_("Render status: could not create companion MIDI track"));
					_apply_button.set_sensitive (_dirty);
					_restart_button.set_sensitive (sct->supports_live_runtime () && sct->supercollider_auto_boot () && sct->supercollider_runtime_available ());
					_render_button.set_sensitive (sct->supercollider_runtime_available ());
					return;
				}

				render_track = tracks.front ();
			}

			std::shared_ptr<Playlist> const render_playlist = render_track->playlist ();
			if (!render_playlist) {
				_status_label.set_text (_("Render status: companion MIDI track has no playlist"));
				_apply_button.set_sensitive (_dirty);
				_restart_button.set_sensitive (sct->supports_live_runtime () && sct->supercollider_auto_boot () && sct->supercollider_runtime_available ());
				_render_button.set_sensitive (sct->supercollider_runtime_available ());
				return;
			}

			unsigned int rendered = 0;
			bool command_started = false;

			for (std::vector<MidiRenderClipOutput>::const_iterator i = result->clips.begin (); i != result->clips.end (); ++i) {
				std::vector<MidiRenderNote> notes;
				std::string parse_error;
				if (!parse_sc_midi_notes (i->notes_path, notes, parse_error)) {
					result->failure_reason = parse_error;
					break;
				}

				Temporal::Beats region_length = i->region_length_beats;
				for (std::vector<MidiRenderNote>::const_iterator note = notes.begin (); note != notes.end (); ++note) {
					Temporal::Beats const note_end = Temporal::Beats::from_double (note->start_beats + note->length_beats);
					if (note_end > region_length) {
						region_length = note_end;
					}
				}

				std::shared_ptr<MidiSource> ms = session.create_midi_source_for_session (string_compose ("%1 MIDI", sct->name ()));
				if (!ms) {
					result->failure_reason = _("could not create MIDI source for rendered notes");
					break;
				}

				if (!command_started) {
					PublicEditor::instance ().begin_reversible_command (_("Render SuperCollider To MIDI Track"));
					command_started = true;
				}

				std::shared_ptr<MidiModel> mm = ms->model ();
				MidiModel::NoteDiffCommand* midicmd = mm->new_note_diff_command (_("Render SuperCollider MIDI"));
				for (std::vector<MidiRenderNote>::const_iterator note = notes.begin (); note != notes.end (); ++note) {
					uint8_t const channel = static_cast<uint8_t> (std::max (0, std::min (15, note->channel)));
					uint8_t const note_number = static_cast<uint8_t> (std::max (0, std::min (127, note->note_number)));
					uint8_t const velocity = static_cast<uint8_t> (std::max (1, std::min (127, note->velocity)));
					midicmd->add (std::shared_ptr<Evoral::Note<Temporal::Beats> > (new Evoral::Note<Temporal::Beats> (channel, Temporal::Beats::from_double (note->start_beats), Temporal::Beats::from_double (note->length_beats), note_number, velocity)));
				}
				mm->apply_diff_command_as_subcommand (session, midicmd);

				{
					MidiSource::WriterLock lock (ms->mutex ());
					mm->write_to (ms, lock);
					ms->load_model (lock);
				}

				PBD::PropertyList plist;
				plist.add (ARDOUR::Properties::start, Temporal::Beats ());
				plist.add (ARDOUR::Properties::length, region_length);
				plist.add (ARDOUR::Properties::name, string_compose (_("SC MIDI Render: %1"), i->clip_name));
				plist.add (ARDOUR::Properties::layer, 0);
				plist.add (ARDOUR::Properties::whole_file, true);
				plist.add (ARDOUR::Properties::external, false);
				plist.add (ARDOUR::Properties::opaque, true);

				std::shared_ptr<Region> const rendered_region = RegionFactory::create (ms, plist);
				render_playlist->clear_changes ();
				render_playlist->clear_owned_changes ();
				render_playlist->add_region (rendered_region, i->position);
				render_playlist->rdiff_and_add_command (&session);
				++rendered;
			}

			if (command_started) {
				if (rendered > 0) {
					PublicEditor::instance ().commit_reversible_command ();
				} else {
					PublicEditor::instance ().abort_reversible_command ();
				}
			}

			if (!result->failure_reason.empty ()) {
				if (rendered > 0) {
					_status_label.set_text (string_compose (_("Render status: placed %1 rendered MIDI clip(s) on %2 before %3"), rendered, render_track->name (), result->failure_reason));
				} else {
					_status_label.set_text (string_compose (_("Render status: %1"), result->failure_reason));
				}
				_apply_button.set_sensitive (_dirty);
				_restart_button.set_sensitive (sct->supports_live_runtime () && sct->supercollider_auto_boot () && sct->supercollider_runtime_available ());
				_render_button.set_sensitive (sct->supercollider_runtime_available ());
				return;
			}

			_status_label.set_text (string_compose (_("Render status: placed %1 rendered MIDI clip(s) on %2"), rendered, render_track->name ()));
			_apply_button.set_sensitive (_dirty);
			_restart_button.set_sensitive (sct->supports_live_runtime () && sct->supercollider_auto_boot () && sct->supercollider_runtime_available ());
			_render_button.set_sensitive (sct->supercollider_runtime_available ());
		});
	}).detach ();
}
