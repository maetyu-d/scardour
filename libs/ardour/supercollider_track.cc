#include "ardour/supercollider_track.h"

#include <functional>

#include "pbd/compose.h"
#include "pbd/error.h"

#include "ardour/playlist.h"
#include "ardour/region.h"
#include "ardour/session.h"
#include "ardour/supercollider_session.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

SuperColliderTrack::SuperColliderTrack (Session& sess, std::string name, TrackMode mode)
	: MidiTrack (sess, name, mode)
	, _supercollider_source (default_supercollider_source ())
	, _supercollider_synthdef (default_supercollider_synthdef ())
	, _supercollider_auto_boot (true)
	, _runtime_start_pending (false)
{
	_session.SessionLoaded.connect_same_thread (*this, std::bind (&SuperColliderTrack::maybe_start_runtime_after_load, this));
	PlaylistChanged.connect_same_thread (*this, std::bind (&SuperColliderTrack::attach_playlist_observers, this));
}

SuperColliderTrack::~SuperColliderTrack ()
{
	stop_supercollider_runtime ();
}

int
SuperColliderTrack::init ()
{
	if (MidiTrack::init ()) {
		return -1;
	}

	attach_playlist_observers ();
	refresh_timeline_regions ();
	refresh_runtime ();
	return 0;
}

int
SuperColliderTrack::set_state (const XMLNode& node, int version)
{
	node.get_property (X_("supercollider-source"), _supercollider_source);
	node.get_property (X_("supercollider-synthdef"), _supercollider_synthdef);
	node.get_property (X_("supercollider-auto-boot"), _supercollider_auto_boot);

	if (_supercollider_source.empty ()) {
		_supercollider_source = default_supercollider_source ();
	}

	if (_supercollider_synthdef.empty ()) {
		_supercollider_synthdef = default_supercollider_synthdef ();
	}

	if (MidiTrack::set_state (node, version)) {
		return -1;
	}

	attach_playlist_observers ();
	refresh_timeline_regions ();

	if (_session.loading ()) {
		_runtime_start_pending = _supercollider_auto_boot;
	} else {
		refresh_runtime ();
	}

	return 0;
}

void
SuperColliderTrack::set_supercollider_source (std::string const& source)
{
	_supercollider_source = source.empty () ? default_supercollider_source () : source;
	refresh_runtime ();
}

void
SuperColliderTrack::set_supercollider_synthdef (std::string const& synthdef)
{
	_supercollider_synthdef = synthdef.empty () ? default_supercollider_synthdef () : synthdef;
	refresh_timeline_regions ();
	refresh_runtime ();
}

void
SuperColliderTrack::set_supercollider_auto_boot (bool yn)
{
	_supercollider_auto_boot = yn;
	refresh_runtime ();
}

bool
SuperColliderTrack::supercollider_runtime_available () const
{
	return _session.supercollider_runtime ().runtime_available ();
}

std::string
SuperColliderTrack::supercollider_runtime_path () const
{
	return _session.supercollider_runtime ().runtime_path ();
}

bool
SuperColliderTrack::start_supercollider_runtime ()
{
	if (!_supercollider_auto_boot) {
		_supercollider_runtime_last_error.clear ();
		return false;
	}

	std::shared_ptr<Route> const self = std::dynamic_pointer_cast<Route> (shared_from_this ());
	std::string attach_error;
	if (!self || !_session.ensure_supercollider_instrument (self, false, &attach_error)) {
		if (attach_error.empty ()) {
			attach_error = _("SuperColliderAU Audio Unit is missing or could not be attached");
		}
		_supercollider_runtime_last_error = attach_error;
		PBD::error << string_compose (_("SuperCollider track '%1' could not attach SuperColliderAU for live playback: %2"), name (), attach_error) << endmsg;
		return false;
	}

	if (!_session.supercollider_runtime ().activate_track (*this)) {
		_supercollider_runtime_last_error = _session.supercollider_runtime ().last_error ();
		PBD::error << string_compose (_("SuperCollider track '%1' could not join the session runtime: %2"), name (), _supercollider_runtime_last_error) << endmsg;
		return false;
	}

	_supercollider_runtime_last_error.clear ();
	return true;
}

void
SuperColliderTrack::stop_supercollider_runtime ()
{
	_session.supercollider_runtime ().deactivate_track (*this);
}

bool
SuperColliderTrack::restart_supercollider_runtime ()
{
	if (!_supercollider_auto_boot) {
		stop_supercollider_runtime ();
		return false;
	}

	stop_supercollider_runtime ();
	return start_supercollider_runtime ();
}

bool
SuperColliderTrack::supercollider_runtime_running () const
{
	return _supercollider_auto_boot &&
	       _session.supercollider_runtime ().running () &&
	       _session.supercollider_runtime ().track_active (*this);
}

bool
SuperColliderTrack::xml_node_is_supercollider (XMLNode const& node)
{
	bool is_supercollider = false;
	node.get_property (X_("supercollider-track"), is_supercollider);
	return is_supercollider;
}

XMLNode&
SuperColliderTrack::state (bool save_template) const
{
	XMLNode& root (MidiTrack::state (save_template));
	root.set_property (X_("supercollider-track"), true);
	root.set_property (X_("supercollider-source"), _supercollider_source);
	root.set_property (X_("supercollider-synthdef"), _supercollider_synthdef);
	root.set_property (X_("supercollider-auto-boot"), _supercollider_auto_boot);
	return root;
}

std::string
SuperColliderTrack::default_supercollider_source ()
{
	return
		"// SuperCollider track sketch\n"
		"SynthDef(\\ArdourSuperColliderTrack, { |out=0, freq=220, amp=0.15|\n"
		"    var env = EnvGen.kr(Env.perc(0.01, 1.5), doneAction: 2);\n"
		"    var sig = SinOsc.ar(freq * [1, 1.01], 0, amp) * env;\n"
		"    Out.ar(out, sig);\n"
		"}).add;\n"
		"\n"
		"Synth.tail(~ardourTrackGroup, \\ArdourSuperColliderTrack, [\n"
		"    \\freq, 220,\n"
		"    \\amp, 0.15\n"
		"]);\n";
}

std::string
SuperColliderTrack::default_supercollider_synthdef ()
{
	return "ArdourSuperColliderTrack";
}

void
SuperColliderTrack::maybe_start_runtime_after_load ()
{
	if (_runtime_start_pending) {
		_runtime_start_pending = false;
		refresh_runtime ();
	}
}

void
SuperColliderTrack::refresh_runtime ()
{
	if (_session.loading ()) {
		_runtime_start_pending = _supercollider_auto_boot;
		return;
	}

	if (_supercollider_auto_boot) {
		restart_supercollider_runtime ();
	} else {
		_supercollider_runtime_last_error.clear ();
		stop_supercollider_runtime ();
	}
}

void
SuperColliderTrack::attach_playlist_observers ()
{
	_playlist_connection.disconnect ();

	std::shared_ptr<Playlist> const pl = playlist ();
	if (!pl) {
		return;
	}

	pl->ContentsChanged.connect_same_thread (
		_playlist_connection,
		std::bind (&SuperColliderTrack::playlist_contents_changed, this)
	);
}

void
SuperColliderTrack::refresh_timeline_regions ()
{
	std::shared_ptr<Playlist> const pl = playlist ();
	if (!pl) {
		return;
	}

	std::string const clip_name = string_compose (_("SC: %1"), _supercollider_synthdef.empty () ? default_supercollider_synthdef () : _supercollider_synthdef);
	pl->foreach_region ([clip_name] (std::shared_ptr<Region> region) {
		if (!region) {
			return;
		}

		if (region->name () != clip_name) {
			region->set_name (clip_name);
		}
	});
}

void
SuperColliderTrack::playlist_contents_changed ()
{
	refresh_timeline_regions ();
}
