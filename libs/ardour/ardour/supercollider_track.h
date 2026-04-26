#pragma once

#include <string>

#include "pbd/signals.h"

#include "ardour/midi_track.h"

namespace ARDOUR {

class Region;

class LIBARDOUR_API SuperColliderTrack : public MidiTrack
{
public:
	enum OutputMode {
		AudioOutput,
		MidiOutput
	};

	SuperColliderTrack (Session&, std::string name = "", TrackMode m = Normal, OutputMode = AudioOutput);
	~SuperColliderTrack ();

	int init ();
	int set_state (const XMLNode&, int version);

	void set_supercollider_source (std::string const&);
	std::string const& supercollider_source () const { return _supercollider_source; }

	void set_supercollider_synthdef (std::string const&);
	std::string const& supercollider_synthdef () const { return _supercollider_synthdef; }

	void set_supercollider_auto_boot (bool yn);
	bool supercollider_auto_boot () const { return _supercollider_auto_boot; }

	OutputMode supercollider_output_mode () const { return _supercollider_output_mode; }
	bool supercollider_generates_midi () const { return _supercollider_output_mode == MidiOutput; }
	bool supports_live_runtime () const { return _supercollider_output_mode == AudioOutput; }

	bool supercollider_runtime_available () const;
	std::string supercollider_runtime_path () const;
	bool start_supercollider_runtime ();
	void stop_supercollider_runtime ();
	bool restart_supercollider_runtime ();
	bool supercollider_runtime_running () const;
	std::string const& supercollider_runtime_last_error () const { return _supercollider_runtime_last_error; }

	static bool xml_node_is_supercollider (XMLNode const&);

protected:
	XMLNode& state (bool save_template) const;

private:
	static std::string default_supercollider_source (OutputMode);
	static std::string default_supercollider_synthdef (OutputMode);
	static OutputMode parse_output_mode (std::string const&);
	static std::string output_mode_to_string (OutputMode);

	void maybe_start_runtime_after_load ();
	void refresh_runtime ();
	void attach_playlist_observers ();
	void refresh_timeline_regions ();
	void playlist_contents_changed ();

	std::string _supercollider_source;
	std::string _supercollider_synthdef;
	bool _supercollider_auto_boot;
	OutputMode _supercollider_output_mode;
	std::string _supercollider_runtime_last_error;
	bool _runtime_start_pending;
	PBD::ScopedConnection _playlist_connection;
};

} // namespace ARDOUR
