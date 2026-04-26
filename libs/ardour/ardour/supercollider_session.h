#pragma once

#include <memory>
#include <map>
#include <set>
#include <string>

#include <glib.h>

#include "pbd/signals.h"

#include "ardour/ardour.h"

namespace ARDOUR {

class Region;
class Session;
class SuperColliderTrack;
class SystemExec;

class SuperColliderSessionRuntime : public PBD::ScopedConnectionList
{
public:
	explicit SuperColliderSessionRuntime (Session&);
	~SuperColliderSessionRuntime ();

	bool runtime_available () const;
	std::string runtime_path () const;
	bool running () const;
	bool track_active (SuperColliderTrack const&) const;
	std::string const& last_error () const { return _last_error; }

	bool activate_track (SuperColliderTrack const&);
	void deactivate_track (SuperColliderTrack const&);
	void stop ();
	void sync_transport ();

private:
	static std::string string_literal (std::string const&);
	static std::string track_key (SuperColliderTrack const&);
	static gboolean transport_poll_cb (gpointer);

	bool ensure_started ();
	bool send_code (std::string const&);
	std::string bootstrap_code () const;
	std::string track_play_region_code (SuperColliderTrack const&, Region const&) const;
	std::string track_stop_code (SuperColliderTrack const&) const;
	bool handle_runtime_line (std::string const&);
	bool deliver_live_midi_event (std::string const&, int, int, int);
	void poll_transport ();
	void sync_transport_state ();
	std::shared_ptr<Region> active_region (SuperColliderTrack const&, samplepos_t) const;
	void runtime_output (std::string, size_t);
	void runtime_terminated ();

	Session& _session;
	std::unique_ptr<SystemExec> _runtime;
	PBD::ScopedConnectionList _runtime_connections;
	std::map<std::string, SuperColliderTrack const*> _active_tracks;
	std::map<std::string, std::string> _active_regions;
	std::map<std::string, samplepos_t> _active_region_ends;
	std::string _last_error;
	std::string _runtime_output_buffer;
	guint _transport_poll_source;
	samplepos_t _last_transport_sample;
	bool _last_transport_rolling;
};

} // namespace ARDOUR
