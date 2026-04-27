/*
 * Copyright (C) 2011-2017 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2021 Ben Loftis <ben@harrisonconsoles.com>
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

#include "gtkmm2ext/actions.h"
#include "gtkmm2ext/gui_thread.h"
#include "gtkmm2ext/utils.h"

#include "canvas/canvas.h"
#include "canvas/debug.h"
#include "canvas/utils.h"

#include "waveview/wave_view.h"

#include "ardour/audioengine.h"
#include "ardour/audioregion.h"
#include "ardour/location.h"
#include "ardour/profile.h"
#include "ardour/region_factory.h"
#include "ardour/session.h"
#include "ardour/source.h"

#include "widgets/ardour_button.h"
#include "widgets/ardour_icon.h"

#include "ardour_ui.h"
#include "audio_clip_editor.h"
#include "audio_clock.h"
#include "boundary.h"
#include "editor_automation_line.h"
#include "editor_cursors.h"
#include "editor_drag.h"
#include "control_point.h"
#include "editor.h"
#include "keyboard.h"
#include "public_editor.h"
#include "region_view.h"
#include "timers.h"
#include "ui_config.h"
#include "verbose_cursor.h"

#include "pbd/i18n.h"

using namespace Gtk;
using namespace Gtkmm2ext;
using namespace ARDOUR;
using namespace ArdourCanvas;
using namespace ArdourWaveView;
using namespace ArdourWidgets;
using std::max;
using std::min;

void
AudioClipEditor::ClipMetric::get_marks (std::vector<ArdourCanvas::Ruler::Mark>& marks, int64_t lower, int64_t upper, int maxchars) const
{
	ace.metric_get_minsec (marks, lower, upper, maxchars);
}

AudioClipEditor::AudioClipEditor (std::string const & name, bool with_transport)
	: CueEditor (name, with_transport)
	, overlay_text (nullptr)
	, selection_rect (nullptr)
	, clip_metric (nullptr)
	, scroll_fraction (0)
{
	load_bindings ();
	register_actions ();

	build_grid_type_menu ();
	build_upper_toolbar ();
	build_canvas ();
	build_lower_toolbar ();

	set_action_defaults ();
}


AudioClipEditor::~AudioClipEditor ()
{
	EC_LOCAL_TEMPO_SCOPE;

	drop_grid ();
	drop_waves ();
	delete clip_metric;
}

void
AudioClipEditor::set_action_defaults ()
{
	EC_LOCAL_TEMPO_SCOPE;

	CueEditor::set_action_defaults ();

	if (grid_actions[Editing::GridTypeMinSec]) {
		grid_actions[Editing::GridTypeMinSec]->set_active (false);
		grid_actions[Editing::GridTypeMinSec]->set_active (true);
	}
}

void
AudioClipEditor::load_shared_bindings ()
{
	EC_LOCAL_TEMPO_SCOPE;

	/* Full shared binding loading must have preceded this in some other EditingContext */
	assert (!need_shared_actions);

	Bindings* b = Bindings::get_bindings (X_("Editing"));

	/* Copy each  shared bindings but give them a new name, which will make them refer to actions
	 * named after this EditingContext (ie. unique to this EC)
	 */

	Bindings* shared_bindings = new Bindings (editor_name(), *b);
	register_common_actions (shared_bindings, editor_name());
	shared_bindings->associate ();

	/* Attach bindings to the canvas for this editing context */

	bindings.push_back (shared_bindings);
}

void
AudioClipEditor::pack_inner (Gtk::Box& box)
{
	EC_LOCAL_TEMPO_SCOPE;

	/* No snap, no grid selections until elastic audio */
	// box.pack_start (snap_box, false, false);
	// box.pack_start (grid_box, false, false);
}

void
AudioClipEditor::pack_outer (Gtk::Box& box)
{
	EC_LOCAL_TEMPO_SCOPE;

	if (with_transport_controls) {
		box.pack_start (play_box, false, false, 12);
	}

	box.pack_start (rec_box, false, false);
}

void
AudioClipEditor::build_lower_toolbar ()
{
	EC_LOCAL_TEMPO_SCOPE;

	_toolbox.pack_start (*_canvas_hscrollbar, false, false);
}

void
AudioClipEditor::build_canvas ()
{
	EC_LOCAL_TEMPO_SCOPE;

	_canvas.set_background_color (UIConfiguration::instance().color ("arrange base"));
	_canvas.signal_event().connect (sigc::mem_fun (*this, &CueEditor::canvas_pre_event), false);
	_canvas.signal_event().connect (sigc::mem_fun (*this, &AudioClipEditor::event_handler), false);
	_canvas.use_nsglview (UIConfiguration::instance().get_nsgl_view_mode () == NSGLHiRes);

	_canvas.PreRender.connect (sigc::mem_fun(*this, &EditingContext::pre_render));

	/* scroll group for items that should not automatically scroll
	 *  (e.g verbose cursor). It shares the canvas coordinate space.
	*/
	no_scroll_group = new ArdourCanvas::Container (_canvas.root());

	h_scroll_group = new ArdourCanvas::ScrollGroup (_canvas.root(), ArdourCanvas::ScrollGroup::ScrollsHorizontally);
	CANVAS_DEBUG_NAME (h_scroll_group, "audioclip h scroll");
	_canvas.add_scroller (*h_scroll_group);


	v_scroll_group = new ArdourCanvas::ScrollGroup (_canvas.root(), ArdourCanvas::ScrollGroup::ScrollsVertically);
	CANVAS_DEBUG_NAME (v_scroll_group, "audioclip v scroll");
	_canvas.add_scroller (*v_scroll_group);

	hv_scroll_group = new ArdourCanvas::ScrollGroup (_canvas.root(),
	                                                 ArdourCanvas::ScrollGroup::ScrollSensitivity (ArdourCanvas::ScrollGroup::ScrollsVertically|
		                ArdourCanvas::ScrollGroup::ScrollsHorizontally));
	CANVAS_DEBUG_NAME (hv_scroll_group, "audioclip hv scroll");
	_canvas.add_scroller (*hv_scroll_group);

	cursor_scroll_group = new ArdourCanvas::ScrollGroup (_canvas.root(), ArdourCanvas::ScrollGroup::ScrollsHorizontally);
	CANVAS_DEBUG_NAME (cursor_scroll_group, "audioclip cursor scroll");
	_canvas.add_scroller (*cursor_scroll_group);

	/*a group to hold global rects like punch/loop indicators */
	global_rect_group = new ArdourCanvas::Container (hv_scroll_group);
	CANVAS_DEBUG_NAME (global_rect_group, "audioclip global rect group");

        transport_loop_range_rect = new ArdourCanvas::Rectangle (global_rect_group, ArdourCanvas::Rect (0.0, 0.0, 0.0, ArdourCanvas::COORD_MAX));
	CANVAS_DEBUG_NAME (transport_loop_range_rect, "audioclip loop rect");
	transport_loop_range_rect->hide();

	/*a group to hold time (measure) lines */
	time_line_group = new ArdourCanvas::Container (h_scroll_group);
	CANVAS_DEBUG_NAME (time_line_group, "audioclip time line group");

	n_timebars = 0;

	clip_metric = new ClipMetric (*this);
	main_ruler = new ArdourCanvas::Ruler (time_line_group, clip_metric, ArdourCanvas::Rect (0, 0, ArdourCanvas::COORD_MAX, timebar_height));
	main_ruler->set_font_description (UIConfiguration::instance ().get_SmallerFont ());
	main_ruler->set_fill_color (UIConfiguration::instance().color (X_("ruler base")));
	main_ruler->set_outline_color (UIConfiguration::instance().color (X_("ruler text")));
	n_timebars++;

	main_ruler->Event.connect (sigc::mem_fun (*this, &CueEditor::ruler_event));

	data_group = new ArdourCanvas::Container (hv_scroll_group);
	CANVAS_DEBUG_NAME (data_group, "cue data group");

	data_group->set_position (ArdourCanvas::Duple (_timeline_origin, timebar_height * n_timebars));
	no_scroll_group->set_position (ArdourCanvas::Duple (_timeline_origin, timebar_height * n_timebars));
	cursor_scroll_group->set_position (ArdourCanvas::Duple (_timeline_origin, timebar_height * n_timebars));
	h_scroll_group->set_position (Duple (_timeline_origin, 0.));

	// _playhead_cursor = new EditorCursor (*this, &Editor::canvas_playhead_cursor_event, X_("playhead"));
	_playhead_cursor = new EditorCursor (*this, X_("playhead"));
	_playhead_cursor->set_sensitive (UIConfiguration::instance().get_sensitize_playhead());
	_playhead_cursor->set_color (UIConfiguration::instance().color ("play head"));
	_playhead_cursor->canvas_item().raise_to_top();
	h_scroll_group->raise_to_top ();

	_canvas.set_name ("AudioClipCanvas");
	_canvas.add_events (Gdk::POINTER_MOTION_HINT_MASK | Gdk::SCROLL_MASK | Gdk::KEY_PRESS_MASK | Gdk::KEY_RELEASE_MASK);
	_canvas.set_can_focus ();
	_canvas_viewport.signal_size_allocate().connect (sigc::mem_fun(*this, &AudioClipEditor::canvas_allocate), false);

	_toolbox.pack_start (_canvas_viewport, true, true);

	/* the lines */

	line_container = new ArdourCanvas::Container (data_group);
	CANVAS_DEBUG_NAME (line_container, "audio clip line container");

	selection_rect = new ArdourCanvas::Rectangle (data_group, ArdourCanvas::Rect (0.0, 0.0, 0.0, 0.0));
	selection_rect->hide ();
	selection_rect->set_outline_color (UIConfiguration::instance().color ("rubber band rect"));
	selection_rect->set_fill_color (UIConfiguration::instance().color_mod ("rubber band rect", "selection rect"));
	CANVAS_DEBUG_NAME (selection_rect, "audio clip selection rect");

	start_line = new StartBoundaryRect (line_container);
	start_line->set_outline_what (ArdourCanvas::Rectangle::RIGHT);
	CANVAS_DEBUG_NAME (start_line, "start boundary rect");

	end_line = new EndBoundaryRect (line_container);
	end_line->set_outline_what (ArdourCanvas::Rectangle::LEFT);
	CANVAS_DEBUG_NAME (end_line, "end boundary rect");

	// loop_line = new Line (line_container);
	// loop_line->set_outline_width (line_width * scale);

	start_line->Event.connect (sigc::bind (sigc::mem_fun (*this, &AudioClipEditor::start_line_event_handler), start_line));
	end_line->Event.connect (sigc::bind (sigc::mem_fun (*this, &AudioClipEditor::end_line_event_handler), end_line));
	// loop_line->Event.connect (sigc::bind (sigc::mem_fun (*this, &AudioClipEditor::line_event_handler), loop_line));

	/* hide lines until there is a region */

	// line_container->hide ();

	_verbose_cursor.reset (new VerboseCursor (*this));

	set_colors ();
}
bool
AudioClipEditor::button_press_handler (ArdourCanvas::Item* item, GdkEvent* event, ItemType item_type)
{
	EC_LOCAL_TEMPO_SCOPE;

	if (event->type != GDK_BUTTON_PRESS) {
		return false;
	}

	switch (event->button.button) {
	case 1:
		return button_press_handler_1 (item, event, item_type);
		break;

	case 2:
		return button_press_handler_2 (item, event, item_type);
		break;

	case 3:
		break;

	default:
		return button_press_dispatch (&event->button);
		break;

	}

	return false;
}

bool
AudioClipEditor::button_press_handler_1 (ArdourCanvas::Item* item, GdkEvent* event, ItemType item_type)
{
	EC_LOCAL_TEMPO_SCOPE;

	switch (item_type) {
	case ClipStartItem: {
		ArdourCanvas::Rectangle* r = dynamic_cast<ArdourCanvas::Rectangle*> (item);
		if (r) {
			_drags->set (new ClipStartDrag (*this, *r), event);
		}
		return true;
		break;
	}

	case ClipEndItem: {
		ArdourCanvas::Rectangle* r = dynamic_cast<ArdourCanvas::Rectangle*> (item);
		if (r) {
			_drags->set (new ClipEndDrag (*this, *r), event);
		}
		return true;
		break;
	}

	default:
		break;
	}

	return false;
}

bool
AudioClipEditor::button_press_handler_2 (ArdourCanvas::Item*, GdkEvent*, ItemType)
{
	EC_LOCAL_TEMPO_SCOPE;

	return true;
}

bool
AudioClipEditor::button_release_handler (ArdourCanvas::Item* item, GdkEvent* event, ItemType item_type)
{
	EC_LOCAL_TEMPO_SCOPE;

	if (!Keyboard::is_context_menu_event (&event->button)) {

		/* see if we're finishing a drag */

		if (_drags->active ()) {
			bool const r = _drags->end_grab (event);
			if (r) {
				/* grab dragged, so do nothing else */
				return true;
			}
		}
	}

	return false;
}

bool
AudioClipEditor::motion_handler (ArdourCanvas::Item*, GdkEvent* event, bool from_autoscroll)
{
	EC_LOCAL_TEMPO_SCOPE;

	if (_drags->active ()) {
		//drags change the snapped_cursor location, because we are snapping the thing being dragged, not the actual mouse cursor
		return _drags->motion_handler (event, from_autoscroll);
	}

	return true;
}

bool
AudioClipEditor::enter_handler (ArdourCanvas::Item* item, GdkEvent* ev, ItemType item_type)
{
	EC_LOCAL_TEMPO_SCOPE;

	choose_canvas_cursor_on_entry (item_type);

	return true;
}

bool
AudioClipEditor::leave_handler (ArdourCanvas::Item* item, GdkEvent* ev, ItemType item_type)
{
	EC_LOCAL_TEMPO_SCOPE;

	set_canvas_cursor (which_mode_cursor());

	return true;
}

bool
AudioClipEditor::start_line_event_handler (GdkEvent* ev, StartBoundaryRect* l)
{
	EC_LOCAL_TEMPO_SCOPE;

	return typed_event (start_line, ev, ClipStartItem);
}

bool
AudioClipEditor::end_line_event_handler (GdkEvent* ev, EndBoundaryRect* l)
{
	EC_LOCAL_TEMPO_SCOPE;

	return typed_event (end_line, ev, ClipEndItem);
}

bool
AudioClipEditor::key_press (GdkEventKey* ev)
{
	EC_LOCAL_TEMPO_SCOPE;

	if (Gtkmm2ext::Keyboard::modifier_state_contains (ev->state, Gtkmm2ext::Keyboard::PrimaryModifier)) {
		switch (ev->keyval) {
		case GDK_z:
		case GDK_Z:
			if (ev->state & GDK_SHIFT_MASK) {
				redo_edit ();
			} else {
				undo_edit ();
			}
			return true;
		case GDK_r:
		case GDK_R:
			redo_edit ();
			return true;
		default:
			break;
		}
	}

	switch (ev->keyval) {
	case GDK_plus:
	case GDK_equal:
	case GDK_KP_Add:
		zoom_in_detail ();
		return true;
	case GDK_minus:
	case GDK_KP_Subtract:
		zoom_out_detail ();
		return true;
	case GDK_0:
	case GDK_KP_0:
		fit_region ();
		return true;
	case GDK_z:
	case GDK_Z:
		zoom_to_selection ();
		return true;
	case GDK_t:
	case GDK_T:
		trim_to_selection ();
		return true;
	case GDK_Delete:
	case GDK_BackSpace:
		silence_selection ();
		return true;
	case GDK_l:
	case GDK_L:
		loop_selection ();
		return true;
	case GDK_Left:
		page_left ();
		return true;
	case GDK_Right:
		page_right ();
		return true;
	case GDK_space:
		play_from_selection ();
		return true;
	default:
		break;
	}

	return false;
}

bool
AudioClipEditor::event_handler (GdkEvent* ev)
{
	EC_LOCAL_TEMPO_SCOPE;

	switch (ev->type) {
	case GDK_BUTTON_PRESS:
		if (ev->button.button == 1) {
			_selection_press_x = event_canvas_x (ev);
			_selection_start_sample = sample_at_view_x (_selection_press_x);
			_selection_end_sample = _selection_start_sample;
			_selecting = true;
			selection_rect->hide ();
			_playhead_cursor->set_position (_selection_start_sample);
			if (_session && _region) {
				_session->request_locate (timeline_sample_from_source_sample (_selection_start_sample), false);
			}
		}
		break;
	case GDK_2BUTTON_PRESS:
		if (ev->button.button == 1) {
			zoom_around (event_canvas_x (ev), true);
			return true;
		}
		break;
	case GDK_SCROLL: {
		const double x = event_canvas_x (ev);
		const guint state = ev->scroll.state;
		if (state & GDK_SHIFT_MASK || ev->scroll.direction == GDK_SCROLL_LEFT || ev->scroll.direction == GDK_SCROLL_RIGHT) {
			if (ev->scroll.direction == GDK_SCROLL_UP || ev->scroll.direction == GDK_SCROLL_LEFT) {
				page_left ();
			} else {
				page_right ();
			}
			return true;
		}

		if (ev->scroll.direction == GDK_SCROLL_UP) {
			zoom_around (x, true);
			return true;
		}
		if (ev->scroll.direction == GDK_SCROLL_DOWN) {
			zoom_around (x, false);
			return true;
		}
		break;
	}
	case GDK_KEY_PRESS:
		return key_press (&ev->key);
	case GDK_MOTION_NOTIFY:
		if (_selecting && (ev->motion.state & GDK_BUTTON1_MASK)) {
			const double x = event_canvas_x (ev);
			_selection_end_sample = sample_at_view_x (x);
			if (fabs (x - _selection_press_x) > 3.0) {
				update_selection_rect ();
			}
			return true;
		}
		break;
	case GDK_BUTTON_RELEASE:
		if (ev->button.button == 1 && _selecting) {
			const double x = event_canvas_x (ev);
			_selection_end_sample = sample_at_view_x (x);
			_selecting = false;
			if (fabs (x - _selection_press_x) <= 3.0) {
				clear_selection_rect ();
			} else {
				update_selection_rect ();
			}
			return true;
		}
		break;
	default:
		break;
	}

	return false;
}

double
AudioClipEditor::event_canvas_x (GdkEvent* ev) const
{
	switch (ev->type) {
	case GDK_BUTTON_PRESS:
	case GDK_2BUTTON_PRESS:
	case GDK_3BUTTON_PRESS:
	case GDK_BUTTON_RELEASE:
		return ev->button.x;
	case GDK_MOTION_NOTIFY:
		return ev->motion.x;
	case GDK_SCROLL:
		return ev->scroll.x;
	default:
		return 0.0;
	}
}

samplepos_t
AudioClipEditor::sample_at_view_x (double x) const
{
	const double canvas_x = std::max<double> (0.0, x - _timeline_origin);
	return _leftmost_sample + llrint (canvas_x * samples_per_pixel);
}

samplepos_t
AudioClipEditor::timeline_sample_from_source_sample (samplepos_t source_sample) const
{
	if (!_region) {
		return source_sample;
	}

	const samplepos_t region_start_in_source = _region->start().samples();
	if (source_sample <= region_start_in_source) {
		return _region->position().samples();
	}

	return _region->position().samples() + (source_sample - region_start_in_source);
}

samplepos_t
AudioClipEditor::source_sample_from_timeline_sample (samplepos_t timeline_sample) const
{
	if (!_region) {
		return timeline_sample;
	}

	if (timeline_sample <= _region->position().samples()) {
		return _region->start().samples();
	}

	return _region->start().samples() + (timeline_sample - _region->position().samples());
}

void
AudioClipEditor::position_lines ()
{
	EC_LOCAL_TEMPO_SCOPE;

	if (!_region) {
		return;
	}

	double start_x1 = sample_to_pixel (_region->start().samples());
	double end_x0 = start_x1 + sample_to_pixel (_region->length().samples());

	start_line->set (ArdourCanvas::Rect (0., 0., start_x1, _visible_canvas_height));
	end_line->set_position (ArdourCanvas::Duple (end_x0, 0.));
	end_line->set (ArdourCanvas::Rect (0., 0., ArdourCanvas::COORD_MAX, _visible_canvas_height));

	if (_selection_start_sample != _selection_end_sample) {
		update_selection_rect ();
	}

	update_loop_rect ();
}

void
AudioClipEditor::update_selection_rect ()
{
	const samplepos_t a = std::min (_selection_start_sample, _selection_end_sample);
	const samplepos_t b = std::max (_selection_start_sample, _selection_end_sample);
	const double x0 = sample_to_pixel_unrounded (a);
	const double x1 = sample_to_pixel_unrounded (b);
	double waveform_top = n_timebars * timebar_height;
	double waveform_bottom = _visible_canvas_height;

	if (!waves.empty ()) {
		waveform_top = waves.front()->position ().y;
		waveform_bottom = waves.back()->position ().y + waves.back()->height ();
	}

	selection_rect->set (ArdourCanvas::Rect (x0, waveform_top, x1, waveform_bottom));
	selection_rect->show ();
}

void
AudioClipEditor::clear_selection_rect ()
{
	_selection_start_sample = 0;
	_selection_end_sample = 0;
	selection_rect->hide ();
}

void
AudioClipEditor::update_loop_rect ()
{
	if (!_session) {
		transport_loop_range_rect->hide ();
		return;
	}

	Location* tll = _session->locations()->auto_loop_location ();
	if (_session->get_play_loop() && tll) {
		const samplepos_t s1 = source_sample_from_timeline_sample (tll->start_sample ());
		const samplepos_t s2 = source_sample_from_timeline_sample (tll->end_sample ());
		const double x1 = sample_to_pixel (s1);
		const double x2 = sample_to_pixel (s2);
		transport_loop_range_rect->set_x0 (x1);
		transport_loop_range_rect->set_x1 (x2);
		transport_loop_range_rect->show ();
	} else {
		transport_loop_range_rect->hide ();
	}
}

void
AudioClipEditor::zoom_around (double x, bool zoom_in)
{
	EC_LOCAL_TEMPO_SCOPE;

	const samplecnt_t current = std::max<samplecnt_t> (1, get_current_zoom ());
	const samplecnt_t target  = zoom_in ? std::max<samplecnt_t> (1, current / 2) : std::max<samplecnt_t> (1, current * 2);

	if (target == current) {
		return;
	}

	const double canvas_x = std::max<double> (0.0, x - _timeline_origin);
	const samplepos_t anchor = _leftmost_sample + llrint (canvas_x * current);
	const samplepos_t target_left = (anchor > llrint (canvas_x * target)) ? anchor - llrint (canvas_x * target) : 0;

	reset_zoom (target);
	reset_x_origin (target_left);
}

void
AudioClipEditor::set_colors ()
{
	EC_LOCAL_TEMPO_SCOPE;

	_canvas.set_background_color (UIConfiguration::instance ().color (X_("theme:bg")));

	start_line->set_fill_color (UIConfiguration::instance().color_mod ("cue editor start rect fill", "cue boundary alpha"));
	start_line->set_outline_color (UIConfiguration::instance().color ("cue editor start rect outline"));

	end_line->set_fill_color (UIConfiguration::instance().color_mod ("cue editor end rect fill", "cue boundary alpha"));
	end_line->set_outline_color (UIConfiguration::instance().color ("cue editor end rect outline"));

	// loop_line->set_outline_color (UIConfiguration::instance ().color (X_("theme:contrasting selection")));

	set_waveform_colors ();
}

void
AudioClipEditor::drop_waves ()
{
	EC_LOCAL_TEMPO_SCOPE;

	for (auto& wave : waves) {
		delete wave;
	}

	waves.clear ();
}

void
AudioClipEditor::set_region (std::shared_ptr<Region> region)
{
	EC_LOCAL_TEMPO_SCOPE;

	CueEditor::set_region (region);

	if (_visible_pending_region) {
		return;
	}

	drop_waves ();

	if (!region) {
		return;
	}

	std::shared_ptr<AudioRegion> r (std::dynamic_pointer_cast<AudioRegion> (region));

	if (!r) {
		return;
	}

	uint32_t    n_chans = r->n_channels ();
	samplecnt_t len;

	len = r->source (0)->length ().samples ();

	for (uint32_t n = 0; n < n_chans; ++n) {
		std::shared_ptr<Region> wr = RegionFactory::get_whole_region_for_source (r->source (n));

		if (!wr) {
			continue;
		}

		std::shared_ptr<AudioRegion> war = std::dynamic_pointer_cast<AudioRegion> (wr);
		if (!war) {
			continue;
		}

		WaveView* wv = new WaveView (data_group, war);
		wv->set_channel (0);
		wv->set_show_zero_line (false);
		wv->set_clip_level (1.0);
		wv->set_amplitude_above_axis (0.82);
		wv->lower_to_bottom ();

		waves.push_back (wv);
	}

	set_spp_from_length (len);
	set_wave_heights ();
	set_waveform_colors ();

	// line_container->show ();
	line_container->raise_to_top ();

	set_session (&r->session ());
	state_connection.disconnect ();

	maybe_set_from_rsu (region->id());

	PBD::PropertyChange interesting_stuff;
	region_changed (interesting_stuff);

	region->PropertyChanged.connect (state_connection, invalidator (*this), std::bind (&AudioClipEditor::region_changed, this, _1), gui_context ());
}

void
AudioClipEditor::canvas_allocate (Gtk::Allocation& alloc)
{
	EC_LOCAL_TEMPO_SCOPE;

	_canvas.size_allocate (alloc);

	_visible_canvas_width = alloc.get_width();
	_visible_canvas_height = alloc.get_height();

	/* no track header here, "track width" is the whole canvas */
	_track_canvas_width = _visible_canvas_width;

	main_ruler->set (ArdourCanvas::Rect (2, 2, alloc.get_width() - 4, timebar_height));

	position_lines ();
	update_fixed_rulers ();

	set_wave_heights ();

	double waveform_top = n_timebars * timebar_height;
	double waveform_bottom = _visible_canvas_height;
	if (!waves.empty ()) {
		waveform_top = waves.front()->position ().y;
		waveform_bottom = waves.back()->position ().y + waves.back()->height ();
	}

	selection_rect->set_y0 (waveform_top);
	selection_rect->set_y1 (waveform_bottom);
	start_line->set_y1 (waveform_bottom - 2.);
	end_line->set_y1 (waveform_bottom - 2.);
	// loop_line->set_y1 (waveform_bottom - 2.);

	catch_pending_show_region ();

	update_grid ();
}

void
AudioClipEditor::set_spp_from_length (samplecnt_t len)
{
	EC_LOCAL_TEMPO_SCOPE;

	if (_visible_canvas_width) {
		set_samples_per_pixel (floor (len / _visible_canvas_width));
	}
}

void
AudioClipEditor::set_wave_heights ()
{
	EC_LOCAL_TEMPO_SCOPE;

	if (waves.empty ()) {
		return;
	}

	uint32_t       n  = 0;
	const Distance w  = _visible_canvas_height - (n_timebars * timebar_height);
	Distance       lane_height = w / waves.size ();
	Distance       ht = lane_height * 0.88;
	Distance       lane_padding = (lane_height - ht) * 0.5;

	for (auto& wave : waves) {
		wave->set_height (ht);
		wave->set_y_position ((n_timebars * timebar_height) + (n * lane_height) + lane_padding);
		++n;
	}
}

void
AudioClipEditor::set_waveform_colors ()
{
	EC_LOCAL_TEMPO_SCOPE;

	Gtkmm2ext::Color clip    = UIConfiguration::instance ().color ("clipped waveform");
	Gtkmm2ext::Color zero    = UIConfiguration::instance ().color ("zero line");
	Gtkmm2ext::Color fill    = UIConfiguration::instance ().color ("waveform fill");
	Gtkmm2ext::Color outline = UIConfiguration::instance ().color ("waveform outline");

	for (auto& wave : waves) {
		wave->set_fill_color (fill);
		wave->set_outline_color (outline);
		wave->set_clip_color (clip);
		wave->set_zero_color (zero);
	}
}

Gtk::Widget&
AudioClipEditor::contents ()
{
	EC_LOCAL_TEMPO_SCOPE;

	return _contents;
}

void
AudioClipEditor::region_changed (const PBD::PropertyChange& what_changed)
{
	EC_LOCAL_TEMPO_SCOPE;
	if (!_region) {
		return;
	}

	const bool geometry_changed =
		what_changed.empty () ||
		what_changed.contains (ARDOUR::Properties::start) ||
		what_changed.contains (ARDOUR::Properties::length);

	const bool position_changed =
		what_changed.empty () ||
		what_changed.contains (ARDOUR::Properties::length);

	if (geometry_changed && _visible_canvas_width > 0) {
		const samplepos_t start = _region->start().samples ();
		const samplecnt_t len = std::max<samplecnt_t> (1, _region->length().samples ());
		const samplecnt_t spp = std::max<samplecnt_t> (1, (samplecnt_t) floor ((double) len / _visible_canvas_width));
		reposition_and_zoom (start, spp);
		instant_save ();
	} else if (position_changed) {
		position_lines ();
	}

	if (has_selection ()) {
		const samplepos_t body_a = _region->start().samples ();
		const samplepos_t body_b = _region->start().samples () + _region->length().samples ();
		const samplepos_t sel_a = std::min (_selection_start_sample, _selection_end_sample);
		const samplepos_t sel_b = std::max (_selection_start_sample, _selection_end_sample);
		if (sel_b <= body_a || sel_a >= body_b) {
			clear_selection_rect ();
		}
	}
}

void
AudioClipEditor::set_samples_per_pixel (samplecnt_t spp)
{
	EC_LOCAL_TEMPO_SCOPE;

	CueEditor::set_samples_per_pixel (spp);

	clip_metric->units_per_pixel = samples_per_pixel;

	position_lines ();

	for (auto& wave : waves) {
		wave->set_samples_per_pixel (samples_per_pixel);
	}

	horizontal_adjustment.set_upper (max_zoom_extent().second.samples() / samples_per_pixel);
	horizontal_adjustment.set_page_size (current_page_samples()/ samples_per_pixel / 10);
	horizontal_adjustment.set_page_increment (current_page_samples()/ samples_per_pixel / 20);
	horizontal_adjustment.set_step_increment (current_page_samples() / samples_per_pixel / 100);
}

samplecnt_t
AudioClipEditor::current_page_samples() const
{
	EC_LOCAL_TEMPO_SCOPE;

	return (samplecnt_t) _track_canvas_width * samples_per_pixel;
}

bool
AudioClipEditor::canvas_enter_leave (GdkEventCrossing* ev)
{
	EC_LOCAL_TEMPO_SCOPE;

	switch (ev->type) {
	case GDK_ENTER_NOTIFY:
		if (ev->detail != GDK_NOTIFY_INFERIOR) {
			_canvas.grab_focus ();
			// ActionManager::set_sensitive (_midi_actions, true);
			within_track_canvas = true;
		}
		break;
	case GDK_LEAVE_NOTIFY:
		if (ev->detail != GDK_NOTIFY_INFERIOR) {
			// ActionManager::set_sensitive (_midi_actions, false);
			within_track_canvas = false;
			ARDOUR_UI::instance()->reset_focus (&_canvas_viewport);
			gdk_window_set_cursor (_canvas_viewport.get_window()->gobj(), nullptr);
		}
	default:
		break;
	}
	return false;
}

void
AudioClipEditor::begin_write ()
{
	EC_LOCAL_TEMPO_SCOPE;

}

void
AudioClipEditor::end_write ()
{
	EC_LOCAL_TEMPO_SCOPE;

}

void
AudioClipEditor::set_overlay_text (std::string const & str)
{
	EC_LOCAL_TEMPO_SCOPE;

	if (!overlay_text) {
		overlay_text = new ArdourCanvas::Text (no_scroll_group);
		Pango::FontDescription font ("Sans 200");
		overlay_text->set_font_description (font);
		overlay_text->set_color (0xff000088);
		overlay_text->set ("0"); /* not shown, used for positioning math */
		overlay_text->set_position (ArdourCanvas::Duple ((_visible_canvas_width / 2.0) - (overlay_text->text_width()/2.), (_visible_canvas_height / 2.0) - (overlay_text->text_height() / 2.)));
	}

	overlay_text->set (str);
	show_overlay_text ();
}

void
AudioClipEditor::show_overlay_text ()
{
	if (overlay_text) {
		overlay_text->show ();
	}
}

void
AudioClipEditor::hide_overlay_text ()
{
	if (overlay_text) {
		overlay_text->hide ();
	}
}

void
AudioClipEditor::show_count_in (std::string const & str)
{
	EC_LOCAL_TEMPO_SCOPE;
	set_overlay_text (str);
}

void
AudioClipEditor::hide_count_in ()
{
	EC_LOCAL_TEMPO_SCOPE;
	hide_overlay_text ();
}

bool
AudioClipEditor::idle_data_captured ()
{
	EC_LOCAL_TEMPO_SCOPE;

	if (!ref.box()) {
		return false;
	}

	CueEditor::idle_data_captured ();

	return false;
}

void
AudioClipEditor::maybe_update ()
{
	EC_LOCAL_TEMPO_SCOPE;

	ARDOUR::TriggerPtr playing_trigger;

	if (ref.trigger()) {

		/* Trigger editor */

		playing_trigger = ref.box()->currently_playing ();

		if (!playing_trigger) {

			if (_drags->active() || !_track || !_track->triggerbox()) {
				return;
			}

			if (_track->triggerbox()->record_enabled() == Recording) {
				_playhead_cursor->set_position (data_capture_duration);
			}

			if (!_region) {
				return;
			}

		} else {

			if (playing_trigger->active ()) {
				if (playing_trigger->the_region()) {

					/* We can't know the precise sample
					 * position because we may be
					 * stretching. So figure out
					 */
					std::shared_ptr<ARDOUR::AudioTrigger> at (std::dynamic_pointer_cast<AudioTrigger> (playing_trigger));
					if (at) {
						const double f = playing_trigger->position_as_fraction ();
						_playhead_cursor->set_position (playing_trigger->the_region()->start().samples() + (f * at->data_length()));
					}
				}
			} else {
				_playhead_cursor->set_position (0);
			}
		}

	} else if (_region) {

		/* Timeline region editor */

		if (!_session) {
			return;
		}

		samplepos_t pos = _session->transport_sample();
		samplepos_t spos = _region->position().samples();
		if (pos < spos) {
			_playhead_cursor->set_position (0);
		} else {
			_playhead_cursor->set_position (source_sample_from_timeline_sample (pos));
		}

	} else {
		_playhead_cursor->set_position (0);
	}

	if (_session->transport_rolling() && follow_playhead() && !_scroll_drag) {
		reset_x_origin_to_follow_playhead ();
	}
}

void
AudioClipEditor::unset_region ()
{
	EC_LOCAL_TEMPO_SCOPE;

	drop_waves ();

	CueEditor::unset_region ();
}

void
AudioClipEditor::unset_trigger ()
{
	EC_LOCAL_TEMPO_SCOPE;
	CueEditor::unset_trigger ();
}

Gdk::Cursor*
AudioClipEditor::which_canvas_cursor (ItemType type) const
{
	EC_LOCAL_TEMPO_SCOPE;

	Gdk::Cursor* cursor = which_mode_cursor ();

	switch (type) {
	case ClipEndItem:
	case ClipStartItem:
		cursor = _cursors->expand_left_right;
		break;

	default:
		break;
	}

	return cursor;
}

void
AudioClipEditor::compute_fixed_ruler_scale ()
{
	EC_LOCAL_TEMPO_SCOPE;

	if (_session == 0) {
		return;
	}

	set_minsec_ruler_scale (_leftmost_sample, _leftmost_sample + current_page_samples());
}

void
AudioClipEditor::update_fixed_rulers ()
{
	EC_LOCAL_TEMPO_SCOPE;
	compute_fixed_ruler_scale ();
}

void
AudioClipEditor::snap_mode_chosen (Editing::SnapMode)
{
}

void
AudioClipEditor::grid_type_chosen (Editing::GridType gt)
{
	EC_LOCAL_TEMPO_SCOPE;

	if (gt != Editing::GridTypeMinSec && grid_actions[gt] && grid_actions[gt]->get_active()) {
		assert (grid_actions[Editing::GridTypeMinSec]);
		grid_actions[Editing::GridTypeMinSec]->set_active (false);
		grid_actions[Editing::GridTypeMinSec]->set_active (true);
	}
}

void
AudioClipEditor::instant_save ()
{
	if (!_region) {
		return;
	}

	EC_LOCAL_TEMPO_SCOPE;

	RegionUISettings rus;
	initialize_region_ui_settings (rus);
	add_region_ui_settings (_region->id(), rus);
}

void
AudioClipEditor::zoom_in_detail ()
{
	EC_LOCAL_TEMPO_SCOPE;

	reset_zoom (std::max<samplecnt_t> (1, get_current_zoom () / 2));
}

void
AudioClipEditor::zoom_out_detail ()
{
	EC_LOCAL_TEMPO_SCOPE;

	reset_zoom (std::max<samplecnt_t> (1, get_current_zoom () * 2));
}

void
AudioClipEditor::fit_region ()
{
	EC_LOCAL_TEMPO_SCOPE;

	full_zoom_clicked ();
}

void
AudioClipEditor::page_left ()
{
	EC_LOCAL_TEMPO_SCOPE;

	const samplecnt_t step = std::max<samplecnt_t> (1, current_page_samples () / 2);
	if (_leftmost_sample <= step) {
		reset_x_origin (0);
		return;
	}

	reset_x_origin (_leftmost_sample - step);
}

void
AudioClipEditor::page_right ()
{
	EC_LOCAL_TEMPO_SCOPE;

	const samplecnt_t step = std::max<samplecnt_t> (1, current_page_samples () / 2);
	const auto        extent = max_zoom_extent ();
	const samplecnt_t visible = current_page_samples ();
	const samplepos_t limit = std::max<samplepos_t> (0, extent.second.samples () - visible);

	reset_x_origin (std::min<samplepos_t> (limit, _leftmost_sample + step));
}

void
AudioClipEditor::zoom_to_selection ()
{
	EC_LOCAL_TEMPO_SCOPE;

	if (!has_selection ()) {
		fit_region ();
		return;
	}

	const samplepos_t a = std::min (_selection_start_sample, _selection_end_sample);
	const samplepos_t b = std::max (_selection_start_sample, _selection_end_sample);
	const samplecnt_t len = std::max<samplecnt_t> (1, b - a);

	reposition_and_zoom (a, std::max<samplecnt_t> (1, (samplecnt_t) floor ((max_extents_scale () * len) / (double) _track_canvas_width)));
}

void
AudioClipEditor::play_from_selection ()
{
	EC_LOCAL_TEMPO_SCOPE;

	if (!_session || !_region) {
		return;
	}

	const samplepos_t start = has_selection () ? std::min (_selection_start_sample, _selection_end_sample) : _playhead_cursor->current_sample ();
	_session->request_locate (timeline_sample_from_source_sample (start), true, MustRoll);
}

void
AudioClipEditor::trim_to_selection ()
{
	EC_LOCAL_TEMPO_SCOPE;

	std::shared_ptr<Region> r (_region);
	if (ref.trigger()) {
		r = ref.trigger()->the_region();
	}

	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (r);
	if (!ar || !has_selection ()) {
		return;
	}

	const samplepos_t sel_a = std::min (_selection_start_sample, _selection_end_sample);
	const samplepos_t sel_b = std::max (_selection_start_sample, _selection_end_sample);
	const samplepos_t body_a = ar->start().samples ();
	const samplepos_t body_b = ar->start().samples () + ar->length().samples ();
	const samplepos_t clip_a = std::max (sel_a, body_a);
	const samplepos_t clip_b = std::min (sel_b, body_b);

	if (clip_b <= clip_a) {
		return;
	}

	PublicEditor& editor = PublicEditor::instance ();
	editor.begin_reversible_command (_("trim region to selection"));
	r->clear_changes ();
	r->trim_front (timepos_t (r->source_position().samples () + clip_a));
	r->trim_end (timepos_t (r->source_position().samples () + clip_b));
	editor.add_command (new PBD::StatefulDiffCommand (r));
	editor.commit_reversible_command ();

	clear_selection_rect ();
}

void
AudioClipEditor::silence_selection ()
{
	EC_LOCAL_TEMPO_SCOPE;

	std::shared_ptr<Region> r (_region);
	if (ref.trigger()) {
		r = ref.trigger()->the_region();
	}

	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (r);
	if (!ar || !has_selection ()) {
		return;
	}

	const samplepos_t sel_a = std::min (_selection_start_sample, _selection_end_sample);
	const samplepos_t sel_b = std::max (_selection_start_sample, _selection_end_sample);
	const samplepos_t body_a = ar->start().samples ();
	const samplepos_t body_b = ar->start().samples () + ar->length().samples ();

	const samplepos_t clip_a = std::max (sel_a, body_a);
	const samplepos_t clip_b = std::min (sel_b, body_b);

	if (clip_b <= clip_a) {
		return;
	}

	const samplepos_t local_a = clip_a - body_a;
	const samplepos_t local_b = clip_b - body_a;
	const samplepos_t region_len = ar->length().samples ();

	std::shared_ptr<AutomationList> env = ar->envelope ();
	if (!env) {
		return;
	}

	PublicEditor& editor = PublicEditor::instance ();
	editor.begin_reversible_command (_("silence region selection"));
	r->clear_changes ();

	ar->set_envelope_active (true);
	if (env->empty ()) {
		ar->set_default_envelope ();
	}

	const samplepos_t pre = (local_a > 0) ? (local_a - 1) : local_a;
	const samplepos_t post = (local_b < region_len) ? std::min<samplepos_t> (region_len, local_b + 1) : local_b;
	const double pre_val = env->eval (timepos_t (pre));
	const double post_val = env->eval (timepos_t (post));

	env->freeze ();
	env->clear (timepos_t (local_a), timepos_t (local_b));
	if (local_a > 0) {
		env->fast_simple_add (timepos_t (pre), pre_val);
	}
	env->fast_simple_add (timepos_t (local_a), 0.0);
	env->fast_simple_add (timepos_t (local_b), 0.0);
	if (local_b < region_len) {
		env->fast_simple_add (timepos_t (post), post_val);
	}
	env->thaw ();

	editor.add_command (new PBD::StatefulDiffCommand (r));
	editor.commit_reversible_command ();

	clear_selection_rect ();
}

void
AudioClipEditor::loop_selection ()
{
	EC_LOCAL_TEMPO_SCOPE;

	if (!_session || !_region || !has_selection ()) {
		return;
	}

	const samplepos_t a = std::min (_selection_start_sample, _selection_end_sample);
	const samplepos_t b = std::max (_selection_start_sample, _selection_end_sample);

	if (b <= a) {
		return;
	}

	const timepos_t start (timeline_sample_from_source_sample (a));
	const timepos_t end (timeline_sample_from_source_sample (b));
	PublicEditor::instance().set_loop_range (start, end, _("loop selection"));
	_session->request_play_loop (true, true);
	update_loop_rect ();
}

void
AudioClipEditor::undo_edit ()
{
	EC_LOCAL_TEMPO_SCOPE;

	if (_session) {
		_session->undo (1);
	}
}

void
AudioClipEditor::redo_edit ()
{
	EC_LOCAL_TEMPO_SCOPE;

	if (_session) {
		_session->redo (1);
	}
}
