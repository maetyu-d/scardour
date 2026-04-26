#include "ardour/supercollider_session.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>

#include <glib.h>

#include "ardour/filesystem_paths.h"
#include "ardour/playlist.h"
#include "ardour/region.h"
#include "ardour/audio_backend.h"
#include "ardour/audioengine.h"
#include "ardour/session.h"
#include "ardour/supercollider_track.h"
#include "ardour/system_exec.h"

#include "pbd/compose.h"
#include "pbd/error.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

namespace {

uint64_t sc_runtime_script_counter = 0;

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

}

SuperColliderSessionRuntime::SuperColliderSessionRuntime (Session& session)
	: _session (session)
	, _transport_poll_source (0)
	, _last_transport_sample (std::numeric_limits<samplepos_t>::max ())
	, _last_transport_rolling (false)
{
	_session.TransportStateChange.connect_same_thread (*this, std::bind (&SuperColliderSessionRuntime::sync_transport, this));
	_session.PositionChanged.connect_same_thread (*this, std::bind (&SuperColliderSessionRuntime::poll_transport, this));
	_transport_poll_source = g_timeout_add (25, &SuperColliderSessionRuntime::transport_poll_cb, this);
}

SuperColliderSessionRuntime::~SuperColliderSessionRuntime ()
{
	if (_transport_poll_source != 0) {
		g_source_remove (_transport_poll_source);
		_transport_poll_source = 0;
	}

	drop_connections ();
	stop ();
}

bool
SuperColliderSessionRuntime::runtime_available () const
{
	return !runtime_path ().empty ();
}

std::string
SuperColliderSessionRuntime::runtime_path () const
{
	gchar* path = g_find_program_in_path ("sclang");
	if (path) {
		std::string runtime_path (path);
		g_free (path);
		return runtime_path;
	}

#ifdef __APPLE__
	char const* candidates[] = {
		"/Applications/SuperCollider.app/Contents/MacOS/sclang",
		"/Applications/SuperCollider.app/Contents/Resources/sclang",
		0
	};

	for (char const** candidate = candidates; *candidate; ++candidate) {
		if (g_file_test (*candidate, G_FILE_TEST_IS_EXECUTABLE)) {
			return *candidate;
		}
	}
#endif

	return "";
}

bool
SuperColliderSessionRuntime::running () const
{
	return _runtime && const_cast<SystemExec*> (_runtime.get())->is_running ();
}

bool
SuperColliderSessionRuntime::track_active (SuperColliderTrack const& track) const
{
	return _active_tracks.find (track_key (track)) != _active_tracks.end ();
}

bool
SuperColliderSessionRuntime::activate_track (SuperColliderTrack const& track)
{
	if (!ensure_started ()) {
		return false;
	}

	_active_tracks[track_key (track)] = &track;
	_active_regions.erase (track_key (track));
	_active_region_ends.erase (track_key (track));
	poll_transport ();
	_last_error.clear ();
	return true;
}

void
SuperColliderSessionRuntime::deactivate_track (SuperColliderTrack const& track)
{
	std::string const key = track_key (track);
	if (running () && _active_regions.find (key) != _active_regions.end ()) {
		send_code (track_stop_code (track));
	}

	_active_regions.erase (key);
	_active_region_ends.erase (key);
	_active_tracks.erase (key);
}

void
SuperColliderSessionRuntime::stop ()
{
	_active_tracks.clear ();
	_active_regions.clear ();
	_active_region_ends.clear ();
	_runtime_connections.drop_connections ();
	_last_transport_sample = std::numeric_limits<samplepos_t>::max ();
	_last_transport_rolling = false;

	if (_runtime) {
		_runtime->terminate ();
		_runtime.reset ();
	}
}

void
SuperColliderSessionRuntime::sync_transport ()
{
	poll_transport ();
}

void
SuperColliderSessionRuntime::sync_transport_state ()
{
	if (!running ()) {
		return;
	}

	samplepos_t const sample = _session.transport_sample ();
	bool const rolling = _session.transport_state_rolling ();

	if (_last_transport_sample == sample && _last_transport_rolling == rolling) {
		return;
	}

	_last_transport_sample = sample;
	_last_transport_rolling = rolling;

	send_code (string_compose (
		"~ardourTransportRolling = %1;\n"
		"~ardourTransportSample = %2;\n",
		rolling ? "true" : "false",
		std::to_string (sample)
	));
}

std::string
SuperColliderSessionRuntime::string_literal (std::string const& value)
{
	return sc_string_literal (value);
}

static bool
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

static bool
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

std::string
SuperColliderSessionRuntime::track_key (SuperColliderTrack const& track)
{
	return track.id ().to_s ();
}

bool
SuperColliderSessionRuntime::ensure_started ()
{
	if (running ()) {
		return true;
	}

	std::string const path = runtime_path ();
	if (path.empty ()) {
		_last_error = _("sclang not found");
		return false;
	}

	char** argp = static_cast<char**> (calloc (2, sizeof (char*)));
	argp[0] = strdup (path.c_str ());
	argp[1] = 0;

	_runtime.reset (new SystemExec (argp[0], argp, true));
	_runtime->ReadStdout.connect_same_thread (
		_runtime_connections, std::bind (&SuperColliderSessionRuntime::runtime_output, this, std::placeholders::_1, std::placeholders::_2)
	);
	_runtime->Terminated.connect_same_thread (
		_runtime_connections, std::bind (&SuperColliderSessionRuntime::runtime_terminated, this)
	);

	if (_runtime->start (SystemExec::MergeWithStdin)) {
		_last_error = _("could not launch sclang");
		_runtime_connections.drop_connections ();
		_runtime.reset ();
		return false;
	}

	if (!send_code (bootstrap_code ())) {
		_last_error = _("could not initialize sclang");
		stop ();
		return false;
	}

	sync_transport_state ();
	_last_error.clear ();
	return true;
}

bool
SuperColliderSessionRuntime::send_code (std::string const& code)
{
	if (!running ()) {
		return false;
	}

	std::ostringstream script_name;
	script_name << "sc_runtime_" << ::getpid () << "_" << ++sc_runtime_script_counter << ".scd";
	std::string const script_path = ARDOUR::user_cache_directory () + G_DIR_SEPARATOR_S + script_name.str ();

	if (!g_file_set_contents (script_path.c_str (), code.c_str (), code.size (), 0)) {
		return false;
	}

	std::string const command = string_compose ("%1.load;\n", sc_string_literal (script_path));
	return _runtime->write_to_stdin (command) > 0;
}

std::string
SuperColliderSessionRuntime::bootstrap_code () const
{
	std::string input_device;
	std::string output_device;

	std::shared_ptr<AudioBackend> const backend = _session.engine ().current_backend ();
	if (backend) {
		if (backend->use_separate_input_and_output_devices ()) {
			input_device = backend->input_device_name ();
			output_device = backend->output_device_name ();
		} else {
			input_device = backend->device_name ();
			output_device = backend->device_name ();
		}
	}

	std::string device_setup;
	if (!input_device.empty () && input_device != AudioBackend::get_standard_device_name (AudioBackend::DeviceNone)) {
		device_setup += string_compose ("s.options.inDevice = %1;\n", string_literal (input_device));
	}
	if (!output_device.empty () && output_device != AudioBackend::get_standard_device_name (AudioBackend::DeviceNone)) {
		device_setup += string_compose ("s.options.outDevice = %1;\n", string_literal (output_device));
	}

	return string_compose (
		"(\n"
		"~ardourTracks = ~ardourTracks ? IdentityDictionary.new;\n"
		"Server.default = Server.default ? Server.local;\n"
		"s = Server.default;\n"
		"%1"
		"s.waitForBoot({\n"
		"    \"[Ardour] SuperCollider session ready\".postln;\n"
		"});\n"
		"s.boot;\n"
		")\n",
		device_setup
	);
}

std::string
SuperColliderSessionRuntime::track_play_region_code (SuperColliderTrack const& track, Region const& region) const
{
	std::string const source = track.supercollider_source ();
	std::string player_code;

	if (!source_looks_like_language_script (source)) {
		player_code = string_compose (
			"    state[\\player] = {\n"
			"%1\n"
			"    }.play(target: trackGroup);\n",
			source
		);
	} else {
		std::string prelude;
		std::string body;
		if (split_synthdef_prelude (source, prelude, body)) {
			player_code = prelude + "\n    s.sync;\n" + body + "\n";
		} else {
			player_code = source + "\n";
		}
	}

	if (source_looks_like_language_script (source)) {
		return string_compose (
		"(\n"
		"var trackId = %1;\n"
		"var trackName = %2;\n"
		"var synthdefName = %3;\n"
		"var regionId = %4;\n"
		"var regionName = %5;\n"
		"var regionStart = %6;\n"
		"var regionEnd = %7;\n"
		"~ardourTracks = ~ardourTracks ? IdentityDictionary.new;\n"
		"s = Server.default ? Server.local;\n"
		"s.waitForBoot({\n"
		"    var state = ~ardourTracks[trackId];\n"
		"    var trackGroup;\n"
		"    if (state.notNil) {\n"
		"        if (state[\\group].notNil) {\n"
		"            state[\\group].freeAll;\n"
		"            state[\\group].free;\n"
		"        };\n"
		"    };\n"
		"    trackGroup = Group.tail(s);\n"
		"    ~ardourTrackId = trackId;\n"
		"    ~ardourTrackName = trackName;\n"
		"    ~ardourSynthDef = synthdefName;\n"
		"    ~ardourRegionId = regionId;\n"
		"    ~ardourRegionName = regionName;\n"
		"    ~ardourRegionStart = regionStart;\n"
		"    ~ardourRegionEnd = regionEnd;\n"
		"    ~ardourTrackGroup = trackGroup;\n"
		"    ~tone = nil;\n"
		"    state = (name: trackName, synthdef: synthdefName, group: trackGroup, player: nil, region: regionName, regionId: regionId, regionStart: regionStart, regionEnd: regionEnd);\n"
		"    ~ardourTracks[trackId] = state;\n"
		"%8"
		"    if (~tone.notNil) {\n"
		"        state[\\player] = ~tone;\n"
		"    } {\n"
		"        state[\\player] = trackGroup;\n"
		"    };\n"
		"    state[\\group] = trackGroup;\n"
		"    ~ardourTracks[trackId] = state;\n"
		"});\n"
		")\n",
		string_literal (track_key (track)),
		string_literal (track.name ()),
		string_literal (track.supercollider_synthdef ()),
		string_literal (region.id ().to_s ()),
		string_literal (region.name ()),
		std::to_string (region.position_sample ()),
		std::to_string (region.position_sample () + region.length_samples ()),
		player_code
	);
	}

	return string_compose (
		"(\n"
		"var trackId = %1;\n"
		"var trackName = %2;\n"
		"var synthdefName = %3;\n"
		"var regionId = %4;\n"
		"var regionName = %5;\n"
		"var regionStart = %6;\n"
		"var regionEnd = %7;\n"
		"~ardourTracks = ~ardourTracks ? IdentityDictionary.new;\n"
		"s = Server.default ? Server.local;\n"
		"s.waitForBoot({\n"
		"    var state = ~ardourTracks[trackId];\n"
		"    var trackGroup;\n"
		"    if (state.notNil) {\n"
		"        if (state[\\group].notNil) {\n"
		"            state[\\group].freeAll;\n"
		"            state[\\group].free;\n"
		"        };\n"
		"    };\n"
		"    trackGroup = Group.tail(s);\n"
		"    ~ardourTrackId = trackId;\n"
		"    ~ardourTrackName = trackName;\n"
		"    ~ardourSynthDef = synthdefName;\n"
		"    ~ardourRegionId = regionId;\n"
		"    ~ardourRegionName = regionName;\n"
		"    ~ardourRegionStart = regionStart;\n"
		"    ~ardourRegionEnd = regionEnd;\n"
		"    ~ardourTrackGroup = trackGroup;\n"
		"    ~tone = nil;\n"
		"    state = (name: trackName, synthdef: synthdefName, group: trackGroup, player: nil, region: regionName, regionId: regionId, regionStart: regionStart, regionEnd: regionEnd);\n"
		"    ~ardourTracks[trackId] = state;\n"
		"%8"
		"    state[\\group] = trackGroup;\n"
		"    ~ardourTracks[trackId] = state;\n"
		"});\n"
		")\n",
		string_literal (track_key (track)),
		string_literal (track.name ()),
		string_literal (track.supercollider_synthdef ()),
		string_literal (region.id ().to_s ()),
		string_literal (region.name ()),
		std::to_string (region.position_sample ()),
		std::to_string (region.position_sample () + region.length_samples ()),
		player_code
	);
}

std::string
SuperColliderSessionRuntime::track_stop_code (SuperColliderTrack const& track) const
{
	return string_compose (
		"(\n"
		"var trackId = %1;\n"
		"var state = ~ardourTracks[trackId];\n"
		"if (state.notNil) {\n"
		"    if (state[\\player].notNil) {\n"
		"        state[\\player].release;\n"
		"        s.sendMsg(\"/n_set\", state[\\player].nodeID, \"gate\", 0);\n"
		"        s.sendMsg(\"/n_free\", state[\\player].nodeID);\n"
		"        state[\\player].free;\n"
		"        state[\\player] = nil;\n"
		"    };\n"
		"    if (state[\\group].notNil) {\n"
		"        s.sendMsg(\"/g_deepFree\", state[\\group].nodeID);\n"
		"        s.sendMsg(\"/n_free\", state[\\group].nodeID);\n"
		"        state[\\group].freeAll;\n"
		"        state[\\group].free;\n"
		"    };\n"
		"    state[\\player] = nil;\n"
		"    state[\\group] = nil;\n"
		"    state[\\region] = nil;\n"
		"    state[\\regionId] = nil;\n"
		"    ~ardourTracks[trackId] = state;\n"
		"};\n"
		"    if (~ardourTrackGroup.notNil) {\n"
		"        s.sendMsg(\"/g_deepFree\", ~ardourTrackGroup.nodeID);\n"
		"        s.sendMsg(\"/n_free\", ~ardourTrackGroup.nodeID);\n"
		"        ~ardourTrackGroup.freeAll;\n"
		"        ~ardourTrackGroup.free;\n"
		"        ~ardourTrackGroup = nil;\n"
		"    };\n"
		"    if (~tone.notNil) {\n"
		"        ~tone.release;\n"
		"        s.sendMsg(\"/n_set\", ~tone.nodeID, \"gate\", 0);\n"
		"        s.sendMsg(\"/n_free\", ~tone.nodeID);\n"
		"        ~tone.free;\n"
		"        ~tone = nil;\n"
		"    };\n"
		"    s.freeAll;\n"
		")\n",
		string_literal (track_key (track))
	);
}

gboolean
SuperColliderSessionRuntime::transport_poll_cb (gpointer data)
{
	static_cast<SuperColliderSessionRuntime*> (data)->poll_transport ();
	return G_SOURCE_CONTINUE;
}

std::shared_ptr<Region>
SuperColliderSessionRuntime::active_region (SuperColliderTrack const& track, samplepos_t sample) const
{
	std::shared_ptr<Playlist> const pl = const_cast<SuperColliderTrack&> (track).playlist ();
	if (!pl) {
		return std::shared_ptr<Region> ();
	}

	std::shared_ptr<Region> const region = pl->top_unmuted_region_at (timepos_t (sample));
	if (!region) {
		return std::shared_ptr<Region> ();
	}

	samplepos_t const start = region->position_sample ();
	samplepos_t const end = start + region->length_samples ();
	if (sample < start || sample >= end) {
		return std::shared_ptr<Region> ();
	}

	return region;
}

void
SuperColliderSessionRuntime::poll_transport ()
{
	if (!running ()) {
		return;
	}

	sync_transport_state ();

	if (!_session.transport_state_rolling ()) {
		for (std::map<std::string, SuperColliderTrack const*>::const_iterator i = _active_tracks.begin (); i != _active_tracks.end (); ++i) {
			if (_active_regions.find (i->first) != _active_regions.end ()) {
				send_code (track_stop_code (*i->second));
			}
		}
		_active_regions.clear ();
		_active_region_ends.clear ();
		return;
	}

	samplepos_t const now = _session.transport_sample ();

	for (std::map<std::string, SuperColliderTrack const*>::const_iterator i = _active_tracks.begin (); i != _active_tracks.end (); ++i) {
		std::map<std::string, samplepos_t>::const_iterator const end_it = _active_region_ends.find (i->first);
		if (end_it != _active_region_ends.end () && now >= end_it->second) {
			std::string const current_region_id = _active_regions[i->first];
			if (!current_region_id.empty ()) {
				send_code (track_stop_code (*i->second));
			}
			_active_regions.erase (i->first);
			_active_region_ends.erase (i->first);
		}

		std::shared_ptr<Region> const region = active_region (*i->second, now);
		std::string const next_region_id = region ? region->id ().to_s () : "";
		std::string const current_region_id = _active_regions[i->first];

		if (current_region_id == next_region_id) {
			continue;
		}

		if (!current_region_id.empty ()) {
			send_code (track_stop_code (*i->second));
			_active_region_ends.erase (i->first);
		}

		if (region) {
			send_code (track_play_region_code (*i->second, *region));
			_active_regions[i->first] = next_region_id;
			_active_region_ends[i->first] = region->position_sample () + region->length_samples ();
		} else {
			_active_regions.erase (i->first);
			_active_region_ends.erase (i->first);
		}
	}
}

void
SuperColliderSessionRuntime::runtime_output (std::string text, size_t len)
{
	if (len == 0 || text.empty ()) {
		return;
	}

	PBD::info << string_compose (_("SuperColliderSession: %1"), text.substr (0, len)) << endmsg;
}

void
SuperColliderSessionRuntime::runtime_terminated ()
{
	_runtime_connections.drop_connections ();
	_runtime.reset ();
	_active_tracks.clear ();
	_active_regions.clear ();
	_active_region_ends.clear ();
	_last_error = _("sclang terminated unexpectedly");
}
