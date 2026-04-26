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
#include <thread>
#include <vector>

#include <glib.h>

#include "ardour/audio_track.h"
#include "ardour/audiofilesource.h"
#include "ardour/playlist.h"
#include "ardour/region.h"
#include "ardour/region_factory.h"
#include "ardour/session.h"
#include "ardour/source_factory.h"
#include "ardour/session_directory.h"

#include "gui_thread.h"
#include "public_editor.h"
#include "supercollider_track_editor.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

SuperColliderTrackEditor::SuperColliderTrackEditor (std::shared_ptr<Route> route)
	: ArdourWindow ("")
	, _synthdef_label (_("SynthDef"))
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
	_source_buffer->signal_changed ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::mark_dirty));
	_synthdef_entry.signal_changed ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::mark_dirty));
	_auto_boot_button.signal_toggled ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::mark_dirty));
	_apply_button.signal_clicked ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::apply_changes));
	_restart_button.signal_clicked ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::restart_runtime));
	_render_button.signal_clicked ().connect (sigc::mem_fun (*this, &SuperColliderTrackEditor::render_to_timeline));

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
SuperColliderTrackEditor::update_title ()
{
	if (!_route) {
		set_title (_("SuperCollider Track Editor"));
		return;
	}

	set_title (string_compose ("%1: %2", _route->name (), _("SuperCollider Track Editor")));
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
	_synthdef_entry.set_text (sct->supercollider_synthdef ());
	_auto_boot_button.set_active (sct->supercollider_auto_boot ());
	_source_buffer->set_text (sct->supercollider_source ());
	_status_label.set_text (status_text);
	_apply_button.set_sensitive (_dirty);
	_restart_button.set_sensitive (sct->supercollider_auto_boot () && sct->supercollider_runtime_available ());
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
SuperColliderTrackEditor::apply_changes ()
{
	std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		return;
	}

	sct->set_supercollider_synthdef (_synthdef_entry.get_text ());
	sct->set_supercollider_auto_boot (_auto_boot_button.get_active ());
	sct->set_supercollider_source (_source_buffer->get_text ());

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
	timepos_t position;
	ARDOUR::samplecnt_t length_samples;
	double duration;
	std::string render_path;
	std::string script_path;
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

	std::string::size_type const add_pos = source.find (".add;", synthdef_pos);
	if (add_pos == std::string::npos) {
		return false;
	}

	std::string::size_type split_pos = add_pos + 5;
	while (split_pos < source.size () && (source[split_pos] == '\n' || source[split_pos] == '\r')) {
		++split_pos;
	}

	prelude = source.substr (0, split_pos);
	body = source.substr (split_pos);
	return true;
}

}

void
SuperColliderTrackEditor::render_to_timeline ()
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

	std::vector<std::shared_ptr<Region> > clips;
	source_playlist->foreach_region ([&clips] (std::shared_ptr<Region> region) {
		if (region) {
			clips.push_back (region);
		}
	});

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
	std::string const synthdef = sct->supercollider_synthdef ();
	std::string const source = sct->supercollider_source ();
	bool const source_is_language_script = source_looks_like_language_script (source);
	std::string synthdef_prelude;
	std::string synthdef_body;
	bool const split_synthdef = source_is_language_script && split_synthdef_prelude (source, synthdef_prelude, synthdef_body);

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
		task.position = clip->position ();
		task.length_samples = clip->length_samples ();
		task.duration = duration;
		task.render_path = Glib::build_filename (sound_path, base_name + ".wav");
		task.script_path = Glib::build_filename (sound_path, base_name + ".scd");
		render_tasks.push_back (task);
	}

	if (render_tasks.empty ()) {
		_render_in_progress = false;
		_status_label.set_text (_("Render status: no non-empty clips to render"));
		return;
	}

	_status_label.set_text (string_compose (_("Render status: rendering %1 clip(s)..."), render_tasks.size ()));
	_apply_button.set_sensitive (false);
	_restart_button.set_sensitive (false);
	_render_button.set_sensitive (false);

	std::thread ([this, render_tasks, runtime_path, sample_rate, track_id, track_name, synthdef, source, source_is_language_script, split_synthdef, synthdef_prelude, synthdef_body, render_track_name] () {
		std::shared_ptr<RenderWorkResult> result (new RenderWorkResult);

		for (std::vector<RenderClipTask>::const_iterator i = render_tasks.begin (); i != render_tasks.end (); ++i) {
			std::ofstream script (i->script_path.c_str (), std::ios::out | std::ios::trunc);
			if (!script) {
				result->failure_reason = _("could not write SuperCollider render script");
				break;
			}

			script
				<< "(\n"
				<< "var outputPath = " << sc_string_literal (i->render_path) << ";\n"
				<< "var sampleRate = " << sample_rate << ";\n"
				<< "var duration = " << i->duration << ";\n"
				<< "Server.killAll;\n"
				<< "s = Server.local;\n"
				<< "s.options.sampleRate = sampleRate;\n"
				<< "s.options.numOutputBusChannels = 2;\n"
				<< "s.options.numInputBusChannels = 2;\n"
				<< "s.waitForBoot({\n"
				<< "    s.record(path: outputPath, numChannels: 2);\n"
				<< "    ~ardourTracks = IdentityDictionary.new;\n"
				<< "    ~ardourTrackId = " << sc_string_literal (track_id) << ";\n"
				<< "    ~ardourTrackName = " << sc_string_literal (track_name) << ";\n"
				<< "    ~ardourSynthDef = " << sc_string_literal (synthdef) << ";\n"
				<< "    ~ardourRegionId = " << sc_string_literal (i->clip_id) << ";\n"
				<< "    ~ardourRegionName = " << sc_string_literal (i->clip_name) << ";\n"
				<< "    ~ardourRegionStart = 0;\n"
				<< "    ~ardourRegionEnd = " << i->length_samples << ";\n"
				<< "    ~ardourRegionDuration = duration;\n"
				<< "    ~ardourTrackGroup = Group.tail(s);\n";

			if (source_is_language_script) {
				if (split_synthdef) {
					script
						<< synthdef_prelude << "\n"
						<< "    s.sync;\n"
						<< synthdef_body << "\n";
				} else {
					script << source << "\n";
				}
			} else {
				script
					<< "    ~ardourRenderPlayer = {\n"
					<< source << "\n"
					<< "    }.play(target: ~ardourTrackGroup);\n";
			}

			script
				<< "    SystemClock.sched(duration.max(0.05), {\n"
				<< "        if (~ardourTrackGroup.notNil) {\n"
				<< "            ~ardourTrackGroup.freeAll;\n"
				<< "            ~ardourTrackGroup.free;\n"
				<< "        };\n"
				<< "        s.stopRecording;\n"
				<< "        s.freeAll;\n"
				<< "        s.quit;\n"
				<< "        0.exit;\n"
				<< "        nil;\n"
				<< "    });\n"
				<< "});\n"
				<< "s.boot;\n"
				<< ")\n";

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

			if (!ok || exit_status != 0 || !g_file_test (i->render_path.c_str (), G_FILE_TEST_EXISTS)) {
				result->failure_reason = _("SuperCollider render failed");
				if (error && error->message) {
					result->failure_reason = error->message;
				} else if (standard_error && *standard_error) {
					result->failure_reason = standard_error;
				}
			} else {
				RenderClipOutput output;
				output.clip_name = i->clip_name;
				output.position = i->position;
				output.render_path = i->render_path;
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
				sources.push_back (SourceFactory::createExternal (DataType::AUDIO, session, i->render_path, 0, Source::Flag (AudioFileSource::NoPeakFile), false));
				sources.push_back (SourceFactory::createExternal (DataType::AUDIO, session, i->render_path, 1, Source::Flag (AudioFileSource::NoPeakFile), false));

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

				SourceFactory::setup_peakfile (sources[0], true);
				SourceFactory::setup_peakfile (sources[1], true);
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
