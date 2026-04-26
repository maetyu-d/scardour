/*
 * Copyright (C) 2011-2017 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2024 Ben Loftis <ben@harrisonconsoles.com>
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

#include <cassert>
#include <ytkmm/widget.h>

#include "pbd/compose.h"

#include "ardour/audio_track.h"
#include "ardour/plugin_insert.h"
#include "ardour/port_insert.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/supercollider_track.h"

#include "gtkmm2ext/gui_thread.h"
#include "gtkmm2ext/utils.h"

#include "widgets/tooltips.h"

#include "ardour_ui.h"
#include "mixer_ui.h"
#include "plugin_selector.h"
#include "plugin_ui.h"
#include "port_insert_ui.h"
#include "route_properties_box.h"
#include "timers.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace Gtk;
using namespace ARDOUR;
using namespace ArdourWidgets;

class ProcessorUIFrame : public Gtk::Frame
{
public:
	ProcessorUIFrame (std::shared_ptr<Route>, std::shared_ptr<Processor>, GenericPluginUI*);

private:
	void save_state () const;

	Gtk::HBox    _top;
	Gtk::VBox    _ctrl_box;
	Gtk::Label   _label;
	ArdourButton _collapse_btn;
	ArdourButton _enable_btn;

	std::string                _strip_id;
	std::string                _state_id;
	GenericPluginUI*           _ui;
	std::shared_ptr<Processor> _proc;
	PBD::ScopedConnectionList  _connections;
};

ProcessorUIFrame::ProcessorUIFrame (std::shared_ptr<Route> r, std::shared_ptr<Processor> p, GenericPluginUI* ui)
	: _label (p->display_name ())
	, _collapse_btn (ArdourButton::default_elements, true)
	, _enable_btn (ArdourButton::just_led_default_elements)
	, _strip_id (string_compose ("strip %1", r->id ().to_s ()))
	, _state_id (string_compose ("procuiframe %1", p->id ().to_s ()))
	, _ui (ui)
	, _proc (p)
{
	_label.property_angle () = 90;
	_collapse_btn.set_icon (ArdourIcon::HideEye);
	_collapse_btn.set_name ("processor collapse button");
	_collapse_btn.set_tweaks (ArdourButton::Square);
	_enable_btn.set_tweaks (ArdourButton::ExpandtoSquare);

	ui->set_no_show_all ();

	_ctrl_box.pack_start (_collapse_btn, false, false, 4);
	_ctrl_box.pack_start (_label, true, true);
	_ctrl_box.pack_start (_enable_btn, false, false, 4);
	_top.pack_start (_ctrl_box, false, false);
	_top.pack_start (*ui, true, true);
	add (_top);
	show_all ();

	set_tooltip (&_collapse_btn, _("Collapse"));
	set_tooltip (&_enable_btn, _("Bypass"));

	_enable_btn.set_active (_proc->enabled ());
	_collapse_btn.signal_clicked.connect ([&] () { _ui->set_visible (!_collapse_btn.get_active ()); save_state (); });
	_enable_btn.signal_clicked.connect ([&] () { _proc->enable (!_proc->enabled ()); });

	p->ActiveChanged.connect (_connections, invalidator (*this), [&] () { _enable_btn.set_active (_proc->enabled ()); }, gui_context ());

	bool            visible = true;
	GUIObjectState& st      = *ARDOUR_UI::instance ()->gui_object_state;
	XMLNode*        strip   = st.get_or_add_node (_strip_id);
	XMLNode*        n       = GUIObjectState::get_node (strip, _state_id);
	if (n) {
		n->get_property (X_("visible"), visible);
	}
	_collapse_btn.set_active (!visible);
	_ui->set_visible (visible);
}

void
ProcessorUIFrame::save_state () const
{
	GUIObjectState& st    = *ARDOUR_UI::instance ()->gui_object_state;
	XMLNode*        strip = st.get_or_add_node (_strip_id);
	XMLNode*        state = st.get_or_add_node (strip, _state_id);
	state->set_property (X_("visible"), !_collapse_btn.get_active ());
}

/* ****************************************************************************/

RoutePropertiesBox::RoutePropertiesBox ()
	: _insert_box (nullptr)
	, _supercollider_synthdef_label (_("SynthDef"))
	, _supercollider_auto_synthdef_button (_("Auto-fill from source"))
	, _supercollider_auto_boot_button (_("Auto-start runtime"))
	, _supercollider_apply_button (_("Apply"))
	, _supercollider_restart_button (_("Restart"))
	, _show_insert (false)
	, _force_hide_insert (false)
	, _updating_supercollider_ui (false)
	, _supercollider_dirty (false)
	, _idle_refill_processors_id (-1)
{
	_scroller.set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_NEVER);
	_scroller.add (_box);

	/* the return of the ScrolledWindowViewport mess:
	 * remove shadow from scrollWindow's viewport
	 * see http://www.mail-archive.com/gtkmm-list@gnome.org/msg03509.html
	 */
	Gtk::Viewport* viewport = (Gtk::Viewport*) _scroller.get_child();
	viewport->set_shadow_type(Gtk::SHADOW_NONE);
	viewport->set_border_width(0);

	_box.set_spacing (4);
	_left_box.set_spacing (4);
	_supercollider_frame.set_no_show_all ();
	_insert_frame.set_no_show_all ();
	_supercollider_status.set_alignment (0.0, 0.5);
	_supercollider_synthdef_label.set_alignment (0.0, 0.5);
	_supercollider_source_buffer = Gtk::TextBuffer::create ();
	_supercollider_source_view.set_buffer (_supercollider_source_buffer);
	_supercollider_source_view.set_editable (true);
	_supercollider_source_view.set_cursor_visible (true);
	_supercollider_source_view.set_can_focus (true);
	_supercollider_source_view.set_wrap_mode (Gtk::WRAP_WORD_CHAR);
	_supercollider_source_view.set_size_request (420, 180);
	_supercollider_source_scroller.set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	_supercollider_source_scroller.add (_supercollider_source_view);
	_supercollider_controls.set_spacing (6);
	_supercollider_controls.pack_start (_supercollider_synthdef_label, false, false);
	_supercollider_controls.pack_start (_supercollider_synthdef_entry, true, true);
	_supercollider_controls.pack_start (_supercollider_auto_synthdef_button, false, false);
	_supercollider_controls.pack_start (_supercollider_auto_boot_button, false, false);
	_supercollider_controls.pack_start (_supercollider_apply_button, false, false);
	_supercollider_controls.pack_start (_supercollider_restart_button, false, false);
	_supercollider_box.set_spacing (6);
	_supercollider_box.pack_start (_supercollider_status, false, false);
	_supercollider_box.pack_start (_supercollider_controls, false, false);
	_supercollider_box.pack_start (_supercollider_source_scroller, true, true);
	_supercollider_frame.add (_supercollider_box);
	_supercollider_frame.set_label (_("SuperCollider"));
	_supercollider_frame.set_padding (4);

	_left_box.pack_start (_supercollider_frame, false, false, 4);
	_left_box.pack_start (_insert_frame, false, false, 4);
	pack_start (_left_box, false, false, 4);
	pack_start (_scroller, true, true);
	show_all();
	_supercollider_frame.hide ();

	ARDOUR_UI::instance()->ActionsReady.connect_same_thread (_forever_connections, std::bind (&RoutePropertiesBox::ui_actions_ready, this));
	_supercollider_source_buffer->signal_changed().connect (sigc::mem_fun (*this, &RoutePropertiesBox::supercollider_source_or_autofill_changed));
	_supercollider_synthdef_entry.signal_changed().connect (sigc::mem_fun (*this, &RoutePropertiesBox::mark_supercollider_editor_dirty));
	_supercollider_auto_synthdef_button.signal_toggled().connect (sigc::mem_fun (*this, &RoutePropertiesBox::supercollider_source_or_autofill_changed));
	_supercollider_auto_boot_button.signal_toggled().connect (sigc::mem_fun (*this, &RoutePropertiesBox::mark_supercollider_editor_dirty));
	_supercollider_apply_button.signal_clicked().connect (sigc::mem_fun (*this, &RoutePropertiesBox::apply_supercollider_changes));
	_supercollider_restart_button.signal_clicked().connect (sigc::mem_fun (*this, &RoutePropertiesBox::restart_supercollider_runtime));
}

RoutePropertiesBox::~RoutePropertiesBox ()
{
}

void
RoutePropertiesBox::ui_actions_ready ()
{
	Glib::RefPtr<ToggleAction> tact = ActionManager::get_toggle_action (X_("Editor"), X_("show-editor-mixer"));
	tact->signal_toggled().connect (sigc::mem_fun (*this, &RoutePropertiesBox::update_processor_box_visibility));
}

void
RoutePropertiesBox::session_going_away ()
{
	ENSURE_GUI_THREAD (*this, &RoutePropertiesBox::session_going_away);
	SessionHandlePtr::session_going_away ();

	_insert_frame.remove ();
	_supercollider_frame.remove ();
	drop_plugin_uis ();
	drop_route ();
	delete _insert_box;
	_insert_box = nullptr;
}

void
RoutePropertiesBox::set_session (ARDOUR::Session* s) {
	SessionHandlePtr::set_session (s);
	if (!s) {
		return;
	}
	delete _insert_box;
	_insert_box = new ProcessorBox (_session, std::bind (&Mixer_UI::plugin_selector, Mixer_UI::instance()), Mixer_UI::instance()->selection(), 0);
	_insert_box->show_all ();

	float ui_scale = std::max<float> (1.f, UIConfiguration::instance().get_ui_scale());
	_insert_frame.remove ();
	_insert_frame.add (*_insert_box);
	_insert_frame.set_padding (4);
	_insert_frame.set_size_request (144 * ui_scale, 236 * ui_scale);
	_supercollider_frame.set_size_request (350 * ui_scale, 280 * ui_scale);

	_session->SurroundMasterAddedOrRemoved.connect (_session_connections, invalidator (*this), std::bind (&RoutePropertiesBox::surround_master_added_or_removed, this), gui_context());
}

void
RoutePropertiesBox::surround_master_added_or_removed ()
{
	set_route (_route, true);
}

void
RoutePropertiesBox::set_route (std::shared_ptr<Route> r, bool force_update)
{
	if (r == _route && !force_update) {
		return;
	}

	if (!r) {
		drop_route ();
		return;
	}

	_route = r;
	_supercollider_dirty = false;
	_route_connections.drop_connections ();

	_route->processors_changed.connect (_route_connections, invalidator (*this), std::bind (&RoutePropertiesBox::idle_refill_processors, this), gui_context());
	_route->PropertyChanged.connect (_route_connections, invalidator (*this), std::bind (&RoutePropertiesBox::property_changed, this, _1), gui_context ());
	_route->DropReferences.connect (_route_connections, invalidator (*this), std::bind (&RoutePropertiesBox::drop_route, this), gui_context());

	std::shared_ptr<AudioTrack> at = std::dynamic_pointer_cast<AudioTrack>(_route);
	if (at) {
		at->FreezeChange.connect (_route_connections, invalidator (*this), std::bind (&RoutePropertiesBox::map_frozen, this), gui_context());
	}

	_insert_box->set_route (r);
	sync_supercollider_editor ();
	refill_processors ();
}

void
RoutePropertiesBox::map_frozen ()
{
	ENSURE_GUI_THREAD (*this, &RoutePropertiesBox::map_frozen)
		std::shared_ptr<AudioTrack> at = std::dynamic_pointer_cast<AudioTrack>(_route);
	if (at && _insert_box) {
		switch (at->freeze_state()) {
			case AudioTrack::Frozen:
				_insert_box->set_sensitive (false);
				break;
			default:
				_insert_box->set_sensitive (true);
				break;
		}
	}
}

void
RoutePropertiesBox::update_processor_box_visibility ()
{
	_show_insert = !ActionManager::get_toggle_action (X_("Editor"), X_("show-editor-mixer"))->get_active ();
	if (_force_hide_insert || !_show_insert || _proc_uis.empty ()) {
		_insert_frame.hide ();
	} else {
		_insert_frame.show ();
	}

#ifndef MIXBUS
	if (!_force_hide_insert && (_show_insert || !_proc_uis.empty ())) {
		float ui_scale = std::max<float> (1.f, UIConfiguration::instance().get_ui_scale());
		set_size_request (-1, 365 * ui_scale); // match with SelectionPropertiesBox
	} else {
		set_size_request (-1, -1);
	}
#endif
}

void
RoutePropertiesBox::set_force_hide_insert (bool yn)
{
	_force_hide_insert = yn;
	update_processor_box_visibility ();
}

void
RoutePropertiesBox::focus_supercollider_source ()
{
	_supercollider_source_view.grab_focus ();
}

void
RoutePropertiesBox::property_changed (const PBD::PropertyChange& what_changed)
{
	sync_supercollider_editor ();
}

void
RoutePropertiesBox::drop_route ()
{
	drop_plugin_uis ();
	_route.reset ();
	_route_connections.drop_connections ();
	_supercollider_frame.hide ();
	_supercollider_dirty = false;
	if (_idle_refill_processors_id >= 0) {
		g_source_destroy (g_main_context_find_source_by_id (NULL, _idle_refill_processors_id));
		_idle_refill_processors_id = -1;
	}
}

void
RoutePropertiesBox::drop_plugin_uis ()
{
	std::list<Gtk::Widget*> children = _box.get_children ();
	for (auto const& child : children) {
		child->hide ();
		_box.remove (*child);
		delete child;
	}

	for (auto const& ui : _proc_uis) {
		ui->stop_updating (0);
		delete ui;
	}

	_processor_connections.drop_connections ();
	_proc_uis.clear ();
	_insert_frame.hide ();
}

void
RoutePropertiesBox::add_processor_to_display (std::weak_ptr<Processor> w)
{
	std::shared_ptr<Processor> p = w.lock ();
	std::shared_ptr<PlugInsertBase> pib = std::dynamic_pointer_cast<PlugInsertBase> (p);
	if (!pib) {
		return;
	}
#ifdef MIXBUS
	if (std::dynamic_pointer_cast<PluginInsert> (pib)->channelstrip () != Processor::None) {
		return;
	}
#endif
	GenericPluginUI* plugin_ui = new GenericPluginUI (pib, true, true);
	if (plugin_ui->empty ()) {
		delete plugin_ui;
		return;
	}
	//pib->DropReferences.connect (_processor_connections, invalidator (*this), std::bind (&RoutePropertiesBox::refill_processors, this), gui_context());
	_proc_uis.push_back (plugin_ui);
#if 0
	ArdourWidgets::Frame* frame = new ArdourWidgets::Frame ();
	frame->set_label (p->display_name ());
	frame->add (*plugin_ui);
	frame->set_padding (0);
	_box.pack_start (*frame, false, false);
	plugin_ui->show ();
#else
	ProcessorUIFrame* frame = new ProcessorUIFrame (_route, p, plugin_ui);
	_box.pack_start (*frame, false, false);
#endif
}

int
RoutePropertiesBox::_idle_refill_processors (gpointer arg)
{
	static_cast<RoutePropertiesBox*>(arg)->refill_processors ();
	return 0;
}

void
RoutePropertiesBox::idle_refill_processors ()
{
	if (_idle_refill_processors_id < 0) {
		_idle_refill_processors_id = g_idle_add_full (G_PRIORITY_HIGH_IDLE + 10, _idle_refill_processors, this, NULL);
	}
}

void
RoutePropertiesBox::refill_processors ()
{
	if (!_session || _session->deletion_in_progress()) {
		return;
	}
	drop_plugin_uis ();

	assert (_route);

	if (!_route) {
		_idle_refill_processors_id = -1;
		return;
	}

	_route->foreach_processor (sigc::mem_fun (*this, &RoutePropertiesBox::add_processor_to_display));
	if (_proc_uis.empty ()) {
		_scroller.hide ();
	} else {
		float ui_scale = std::max<float> (1.f, UIConfiguration::instance().get_ui_scale());
		int h = 100 * ui_scale;
		for (auto const& ui : _proc_uis) {
			h = std::max<int> (h, ui->get_preferred_height () + /* frame label */ 34 * ui_scale);
		}
		h = std::min<int> (h, 300 * ui_scale);
		_box.set_size_request (-1, h);
		_scroller.show_all ();
	}
	update_processor_box_visibility ();
	_idle_refill_processors_id = -1;
}

void
RoutePropertiesBox::sync_supercollider_editor ()
{
	std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		_supercollider_frame.hide ();
		return;
	}

	std::string status_text;
	if (_supercollider_dirty) {
		status_text = _("Runtime status: unsaved changes");
	} else if (sct->supercollider_runtime_running ()) {
		status_text = _("Runtime status: running");
	} else if (!sct->supercollider_runtime_last_error ().empty ()) {
		status_text = string_compose (_("Runtime status: %1"), sct->supercollider_runtime_last_error ());
	} else if (!sct->supercollider_runtime_available ()) {
		status_text = _("Runtime status: sclang not found on PATH");
	} else {
		status_text = _("Runtime status: stopped");
	}

	_updating_supercollider_ui = true;
	_supercollider_synthdef_entry.set_text (sct->supercollider_synthdef ());
	_supercollider_auto_synthdef_button.set_active (sct->supercollider_auto_synthdef ());
	_supercollider_auto_synthdef_button.set_sensitive (!sct->supercollider_generates_midi ());
	if (sct->supercollider_generates_midi ()) {
		_supercollider_auto_synthdef_button.hide ();
	} else {
		_supercollider_auto_synthdef_button.show ();
	}
	_supercollider_synthdef_entry.set_sensitive (!sct->supercollider_auto_synthdef ());
	_supercollider_auto_boot_button.set_active (sct->supercollider_auto_boot ());
	_supercollider_auto_boot_button.set_sensitive (sct->supports_live_runtime ());
	_supercollider_source_buffer->set_text (sct->supercollider_source ());
	_supercollider_status.set_text (status_text);
	_supercollider_apply_button.set_sensitive (_supercollider_dirty);
	_supercollider_restart_button.set_sensitive (sct->supports_live_runtime () && sct->supercollider_auto_boot () && sct->supercollider_runtime_available ());
	_updating_supercollider_ui = false;
	_supercollider_frame.show_all ();
}

void
RoutePropertiesBox::mark_supercollider_editor_dirty ()
{
	if (_updating_supercollider_ui) {
		return;
	}

	if (!std::dynamic_pointer_cast<SuperColliderTrack> (_route)) {
		return;
	}

	_supercollider_dirty = true;
	_supercollider_status.set_text (_("Runtime status: unsaved changes"));
	_supercollider_apply_button.set_sensitive (true);
}

void
RoutePropertiesBox::supercollider_source_or_autofill_changed ()
{
	if (_updating_supercollider_ui) {
		return;
	}

	std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		return;
	}

	if (!sct->supercollider_generates_midi () && _supercollider_auto_synthdef_button.get_active ()) {
		_supercollider_synthdef_entry.set_sensitive (false);
		std::string const inferred = SuperColliderTrack::infer_supercollider_synthdef (_supercollider_source_buffer->get_text ());
		if (!inferred.empty () && _supercollider_synthdef_entry.get_text () != inferred) {
			_updating_supercollider_ui = true;
			_supercollider_synthdef_entry.set_text (inferred);
			_updating_supercollider_ui = false;
		}
	} else {
		_supercollider_synthdef_entry.set_sensitive (true);
	}

	mark_supercollider_editor_dirty ();
}

void
RoutePropertiesBox::apply_supercollider_changes ()
{
	std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		return;
	}

	sct->set_supercollider_auto_synthdef (_supercollider_auto_synthdef_button.get_active ());
	sct->set_supercollider_auto_boot (_supercollider_auto_boot_button.get_active ());
	sct->set_supercollider_source (_supercollider_source_buffer->get_text ());
	if (!sct->supercollider_auto_synthdef ()) {
		sct->set_supercollider_synthdef (_supercollider_synthdef_entry.get_text ());
	}

	_supercollider_dirty = false;
	sync_supercollider_editor ();
}

void
RoutePropertiesBox::restart_supercollider_runtime ()
{
	std::shared_ptr<SuperColliderTrack> sct = std::dynamic_pointer_cast<SuperColliderTrack> (_route);
	if (!sct) {
		return;
	}

	if (_supercollider_dirty) {
		apply_supercollider_changes ();
	}

	sct->restart_supercollider_runtime ();
	sync_supercollider_editor ();
}
