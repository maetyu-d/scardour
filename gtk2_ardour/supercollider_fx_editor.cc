/*
 * Copyright (C) 2026 OpenAI
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pbd/compose.h"
#include "pbd/unwind.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <glib.h>

#include "ardour/filesystem_paths.h"

#include "gui_thread.h"
#include "gtkmm2ext/colors.h"
#include "supercollider_fx_editor.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

namespace {

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
find_sclang_path ()
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

std::string
fx_cache_directory ()
{
	std::string const dir = Glib::build_filename (ARDOUR::user_cache_directory (), "supercollider-fx");
	g_mkdir_with_parents (dir.c_str (), 0755);
	return dir;
}

bool
compile_effect_synthdef (std::string const& runtime_path, std::string const& source, std::string const& synthdef_name, std::string& error_out)
{
	std::string const cache_dir = fx_cache_directory ();
	std::string const script_path = Glib::build_filename (cache_dir, "compile-" + synthdef_name + ".scd");

	std::ofstream script (script_path.c_str (), std::ios::out | std::ios::trunc);
	if (!script) {
		error_out = _("Could not create the SuperCollider FX compile script");
		return false;
	}

	script
		<< "~scardourSynthdefName = " << sc_string_literal (synthdef_name) << ".asSymbol;\n"
		<< "~scardourOutputDir = " << sc_string_literal (cache_dir) << ";\n"
		<< source << "\n"
		<< "~scardourSynthDesc = SynthDescLib.global.at(~scardourSynthdefName);\n"
		<< "if (~scardourSynthDesc.isNil) {\n"
		<< "    \"SCArdour FX compile error: SynthDef % not found\".format(~scardourSynthdefName).postln;\n"
		<< "    1.exit;\n"
		<< "};\n"
		<< "~scardourSynthDesc.def.writeDefFile(~scardourOutputDir);\n"
		<< "0.exit;\n";
	script.close ();

	gchar* argv[] = {
		g_strdup (runtime_path.c_str ()),
		g_strdup (script_path.c_str ()),
		0
	};
	gchar* standard_output = 0;
	gchar* standard_error = 0;
	gint exit_status = 0;
	GError* error = 0;

	bool const spawned = g_spawn_sync (
		0,
		argv,
		0,
		G_SPAWN_SEARCH_PATH,
		0,
		0,
		&standard_output,
		&standard_error,
		&exit_status,
		&error);

	for (gchar** arg = argv; *arg; ++arg) {
		g_free (*arg);
	}

	std::string const stdout_text = standard_output ? standard_output : "";
	std::string const stderr_text = standard_error ? standard_error : "";
	g_free (standard_output);
	g_free (standard_error);

	if (!spawned) {
		error_out = error ? error->message : _("Could not launch sclang");
		if (error) {
			g_error_free (error);
		}
		return false;
	}

	if (error) {
		g_error_free (error);
	}

	if (!g_spawn_check_exit_status (exit_status, 0)) {
		error_out = stderr_text.empty () ? stdout_text : stderr_text;
		if (error_out.empty ()) {
			error_out = _("SuperCollider FX compile failed");
		}
		return false;
	}

	return true;
}

}

SuperColliderFxEditor::SuperColliderFxEditor (std::shared_ptr<Route> route)
	: ArdourWindow ("")
	, _synthdef_label (_("SynthDef"))
	, _enable_button (_("Enable FX"))
	, _auto_synthdef_button (_("Auto-fill from source"))
	, _apply_button (_("Apply FX"))
	, _load_button (_("Load .sc"))
	, _save_button (_("Save .sc"))
	, _updating (false)
	, _dirty (false)
{
	const float scale = std::max (1.f, UIConfiguration::instance ().get_ui_scale ());
	Gdk::Color const editor_bg = Gtkmm2ext::gdk_color_from_rgba (UIConfiguration::instance ().color ("gtk_bases"));
	Gdk::Color const editor_fg = Gtkmm2ext::gdk_color_from_rgba (UIConfiguration::instance ().color ("gtk_foreground"));
	Gdk::Color const selection_bg = Gtkmm2ext::gdk_color_from_rgba (UIConfiguration::instance ().color ("gtk_bg_selected"));
	Gdk::Color const selection_fg = Gtkmm2ext::gdk_color_from_rgba (UIConfiguration::instance ().color ("gtk_fg_selected"));

	set_default_size (760 * scale, 520 * scale);
	add (_vbox);
	_vbox.set_spacing (8);
	_controls_box.set_spacing (6);
	_file_box.set_spacing (6);
	_status_label.set_alignment (0.0, 0.5);
	_detail_label.set_alignment (0.0, 0.5);
	_detail_label.set_line_wrap (true);
	_diagnostics_label.set_alignment (0.0, 0.5);
	_diagnostics_label.set_line_wrap (true);
	_synthdef_label.set_alignment (0.0, 0.5);

	_source_buffer = Gtk::TextBuffer::create ();
	_source_view.set_buffer (_source_buffer);
	_source_view.set_editable (true);
	_source_view.set_cursor_visible (true);
	_source_view.set_wrap_mode (Gtk::WRAP_NONE);
	_source_view.modify_font (UIConfiguration::instance ().get_NormalMonospaceFont ());
	_source_view.modify_base (Gtk::STATE_NORMAL, editor_bg);
	_source_view.modify_text (Gtk::STATE_NORMAL, editor_fg);
	_source_view.modify_base (Gtk::STATE_SELECTED, selection_bg);
	_source_view.modify_text (Gtk::STATE_SELECTED, selection_fg);
	_source_scroller.set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	_source_scroller.add (_source_view);

	_controls_box.pack_start (_synthdef_label, false, false);
	_controls_box.pack_start (_synthdef_entry, true, true);
	_controls_box.pack_start (_auto_synthdef_button, false, false);
	_controls_box.pack_start (_enable_button, false, false);
	_controls_box.pack_start (_apply_button, false, false);

	_file_box.pack_start (_load_button, false, false);
	_file_box.pack_start (_save_button, false, false);

	_vbox.pack_start (_status_label, false, false);
	_vbox.pack_start (_detail_label, false, false);
	_vbox.pack_start (_diagnostics_label, false, false);
	_vbox.pack_start (_controls_box, false, false);
	_vbox.pack_start (_file_box, false, false);
	_vbox.pack_start (_source_scroller, true, true);

	_route = route;
	_route->set_supercollider_fx_editor (this);

	_route->PropertyChanged.connect (_connections, invalidator (*this), std::bind (&SuperColliderFxEditor::route_property_changed, this, _1), gui_context ());
	_route->DropReferences.connect (_connections, invalidator (*this), std::bind (&SuperColliderFxEditor::route_going_away, this), gui_context ());
	_route->supercollider_fx_changed.connect (_connections, invalidator (*this), std::bind (&SuperColliderFxEditor::sync_editor, this), gui_context ());
	_source_buffer->signal_changed ().connect (sigc::mem_fun (*this, &SuperColliderFxEditor::source_or_autofill_changed));
	_synthdef_entry.signal_changed ().connect (sigc::mem_fun (*this, &SuperColliderFxEditor::mark_dirty));
	_enable_button.signal_toggled ().connect (sigc::mem_fun (*this, &SuperColliderFxEditor::mark_dirty));
	_auto_synthdef_button.signal_toggled ().connect (sigc::mem_fun (*this, &SuperColliderFxEditor::source_or_autofill_changed));
	_apply_button.signal_clicked ().connect (sigc::mem_fun (*this, &SuperColliderFxEditor::apply_changes));
	_load_button.signal_clicked ().connect (sigc::mem_fun (*this, &SuperColliderFxEditor::load_source_from_file));
	_save_button.signal_clicked ().connect (sigc::mem_fun (*this, &SuperColliderFxEditor::save_source_to_file));

	_vbox.show_all ();
	sync_editor ();
	update_title ();
}

SuperColliderFxEditor::~SuperColliderFxEditor ()
{
	hide ();
	if (_route) {
		_route->set_supercollider_fx_editor (0);
	}
}

void
SuperColliderFxEditor::toggle ()
{
	if (get_visible ()) {
		hide ();
	} else {
		open ();
	}
}

void
SuperColliderFxEditor::open ()
{
	sync_editor ();
	update_title ();
	present ();
	_source_view.grab_focus ();
}

void
SuperColliderFxEditor::route_property_changed (const PBD::PropertyChange&)
{
	sync_editor ();
	update_title ();
}

void
SuperColliderFxEditor::route_going_away ()
{
	ENSURE_GUI_THREAD (*this, &SuperColliderFxEditor::route_going_away);
	if (_route) {
		_route->set_supercollider_fx_editor (0);
		_route.reset ();
	}
	hide ();
}

void
SuperColliderFxEditor::sync_editor ()
{
	if (!_route || _dirty) {
		update_title ();
		return;
	}

	PBD::Unwinder<bool> guard (_updating, true);
	_enable_button.set_active (_route->supercollider_fx_enabled ());
	_auto_synthdef_button.set_active (_route->supercollider_fx_auto_synthdef ());
	_source_buffer->set_text (_route->supercollider_fx_source ());
	_synthdef_entry.set_text (_route->supercollider_fx_synthdef ());
	_synthdef_entry.set_sensitive (!_route->supercollider_fx_auto_synthdef ());

	std::string const summary = _route->supercollider_fx_status_summary ().empty ()
		? (_route->supercollider_fx_enabled () ? _("enabled") : _("disabled"))
		: _route->supercollider_fx_status_summary ();
	_status_label.set_text (string_compose (_("FX status: %1"), summary));
	_detail_label.set_text (_route->supercollider_fx_status_detail ());

	if (!_route->supercollider_fx_diagnostics_path ().empty ()) {
		_diagnostics_label.set_text (string_compose (_("Diagnostics log: %1"), _route->supercollider_fx_diagnostics_path ()));
	} else {
		_diagnostics_label.set_text ("");
	}

	_dirty = false;
	update_title ();
}

void
SuperColliderFxEditor::update_title ()
{
	if (!_route) {
		set_title (_("SuperCollider FX Editor"));
		return;
	}

	set_title (string_compose ("%1: %2", _route->name (), _("SuperCollider FX Editor")));
}

void
SuperColliderFxEditor::mark_dirty ()
{
	if (_updating) {
		return;
	}

	_dirty = true;
	_status_label.set_text (_("FX status: unsaved changes"));
	_detail_label.set_text (_("Apply to compile the SynthDef again and reload the route effect."));
	_diagnostics_label.set_text ("");
}

void
SuperColliderFxEditor::source_or_autofill_changed ()
{
	if (_updating) {
		return;
	}

	{
		PBD::Unwinder<bool> guard (_updating, true);
		if (_auto_synthdef_button.get_active ()) {
			_synthdef_entry.set_sensitive (false);
			std::string const inferred = Route::infer_supercollider_synthdef (_source_buffer->get_text ());
			if (!inferred.empty ()) {
				_synthdef_entry.set_text (inferred);
			}
		} else {
			_synthdef_entry.set_sensitive (true);
		}
	}

	mark_dirty ();
}

void
SuperColliderFxEditor::apply_changes ()
{
	if (!_route) {
		return;
	}

	std::string synthdef = _synthdef_entry.get_text ();
	if (_auto_synthdef_button.get_active ()) {
		std::string const inferred = Route::infer_supercollider_synthdef (_source_buffer->get_text ());
		if (!inferred.empty ()) {
			synthdef = inferred;
		}
	}

	if (_enable_button.get_active () && synthdef.empty ()) {
		_route->set_supercollider_fx_status (_("missing SynthDef name"), _("No SynthDef name could be inferred from the current source."), "", true);
		sync_editor ();
		return;
	}

	_route->set_supercollider_fx_auto_synthdef (_auto_synthdef_button.get_active ());
	_route->set_supercollider_fx_source (_source_buffer->get_text ());
	_route->set_supercollider_fx_synthdef (synthdef);
	_route->set_supercollider_fx_enabled (_enable_button.get_active ());

	if (_enable_button.get_active ()) {
		std::string const runtime_path = find_sclang_path ();
		if (runtime_path.empty ()) {
			_route->set_supercollider_fx_status (_("sclang not found"), _("SCArdour could not find the SuperCollider language binary needed to compile this effect."), "", true);
			sync_editor ();
			return;
		}

		std::string compile_error;
		if (!compile_effect_synthdef (runtime_path, _source_buffer->get_text (), synthdef, compile_error)) {
			std::string const diagnostics_path = Glib::build_filename (fx_cache_directory (), "compile-" + synthdef + ".log");
			_route->set_supercollider_fx_status (_("compile failed"), compile_error, diagnostics_path, true);
			sync_editor ();
			return;
		}

		if (!_route->refresh_supercollider_fx ()) {
			sync_editor ();
			return;
		}
	} else {
		_route->refresh_supercollider_fx ();
	}

	_dirty = false;
	sync_editor ();
}

void
SuperColliderFxEditor::load_source_from_file ()
{
	Gtk::FileChooserDialog chooser (*this, _("Load SuperCollider FX Source"), Gtk::FILE_CHOOSER_ACTION_OPEN);
	chooser.add_button (_("Cancel"), Gtk::RESPONSE_CANCEL);
	chooser.add_button (_("Open"), Gtk::RESPONSE_ACCEPT);
	chooser.set_select_multiple (false);

	if (chooser.run () != Gtk::RESPONSE_ACCEPT) {
		return;
	}

	std::ifstream in (chooser.get_filename ().c_str (), std::ios::in);
	if (!in) {
		_route->set_supercollider_fx_status (_("load failed"), _("Could not open the selected SuperCollider FX file."), "", true);
		sync_editor ();
		return;
	}

	std::stringstream buffer;
	buffer << in.rdbuf ();
	_source_buffer->set_text (buffer.str ());
}

void
SuperColliderFxEditor::save_source_to_file ()
{
	Gtk::FileChooserDialog chooser (*this, _("Save SuperCollider FX Source"), Gtk::FILE_CHOOSER_ACTION_SAVE);
	chooser.add_button (_("Cancel"), Gtk::RESPONSE_CANCEL);
	chooser.add_button (_("Save"), Gtk::RESPONSE_ACCEPT);
	chooser.set_do_overwrite_confirmation (true);

	if (chooser.run () != Gtk::RESPONSE_ACCEPT) {
		return;
	}

	std::ofstream out (chooser.get_filename ().c_str (), std::ios::out | std::ios::trunc);
	if (!out) {
		_route->set_supercollider_fx_status (_("save failed"), _("Could not save the SuperCollider FX source to the selected file."), "", true);
		sync_editor ();
		return;
	}

	out << _source_buffer->get_text ();
	out.close ();
	_route->set_supercollider_fx_status (_("source saved"), chooser.get_filename ());
	sync_editor ();
}
