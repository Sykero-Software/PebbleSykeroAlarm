// SPDX-License-Identifier: GPL-3.0-only
#include <pebble.h>
#include <string.h>
#include "alarm_calc.h"
#include "alarm_store.h"
#include "escalation.h"
#include "scheduler.h"

static Alarm    s_alarms[MAX_ALARMS];
static int      s_count;
static Config   s_cfg;
static RunState s_rs;

static Window    *s_main_window;
static MenuLayer *s_main_menu;
static Window    *s_list_window;
static MenuLayer *s_list_menu;

#define MAIN_ROW_ALARMS      0
#define MAIN_ROW_LAST_NIGHT  1
#define MAIN_ROW_TEST        2
#define MAIN_ROW_COUNT       3

static void close_to_watchface(void) {
  // Land on the watchface rather than the launcher: the default
  // APP_EXIT_NOT_SPECIFIED returns to wherever the app was launched from.
  exit_reason_set(APP_EXIT_ACTION_PERFORMED_SUCCESSFULLY);
  window_stack_pop_all(true);
}

// "MTWTF--" style; '-' for days the alarm does not repeat. A one-time alarm
// (mask 0) renders as "once".
static void fmt_weekdays(uint8_t mask, char *out, size_t n) {
  static const char k_letters[7] = { 'M', 'T', 'W', 'T', 'F', 'S', 'S' };
  if (mask == 0) {
    snprintf(out, n, "once");
    return;
  }
  if (n < 8) {
    if (n > 0) out[0] = '\0';
    return;
  }
  for (int i = 0; i < 7; i++) {
    out[i] = (mask & (1 << i)) ? k_letters[i] : '-';
  }
  out[7] = '\0';
}

// "in 13 h", "in 45 min", "in 2 d". Hand-rolled formatting; no float maths.
static void fmt_relative(time_t when, time_t now, char *out, size_t n) {
  if (when == 0) {
    snprintf(out, n, "off");
    return;
  }
  long d = (long)(when - now);
  if (d < 0) d = 0;
  if (d < 60 * 60) {
    snprintf(out, n, "in %ld min", d / 60);
  } else if (d < 24 * 60 * 60) {
    snprintf(out, n, "in %ld h", d / 3600);
  } else {
    snprintf(out, n, "in %ld d", d / 86400);
  }
}

static void refresh_list(void) {
  if (s_list_menu) {
    menu_layer_reload_data(s_list_menu);
  }
  if (s_main_menu) {
    menu_layer_reload_data(s_main_menu);
  }
}

static void reload_and_rearm(void) {
  as_save_alarms(s_alarms, s_count);
  as_save_runstate(&s_rs);
  sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, time(NULL));
  refresh_list();
}

// --- Phone config (Clay): AlarmSet plus every other setting. ---

// __typeof__ (not the brief's bare `typeof`): the Pebble SDK builds with
// -std=c99 (strict ISO, no GNU keyword extensions), under which bare `typeof`
// does not exist and fails with "implicit declaration of function 'typeof'"
// -- confirmed by `pebble build`. __typeof__ is GCC's always-available,
// reserved-namespace spelling of the same extension, unaffected by -std=c99.
#define GET_INT(key, field) do { \
    Tuple *tt = dict_find(iter, MESSAGE_KEY_##key); \
    if (tt) { s_cfg.field = (__typeof__(s_cfg.field))tt->value->int32; changed = true; } \
  } while (0)
#define GET_BOOL(key, field) do { \
    Tuple *tt = dict_find(iter, MESSAGE_KEY_##key); \
    if (tt) { s_cfg.field = tt->value->int32 != 0; changed = true; } \
  } while (0)

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t = dict_find(iter, MESSAGE_KEY_AlarmSet);
  if (t) {
    // A CString tuple is not guaranteed NUL-terminated at the buffer end; copy
    // into a bounded buffer and terminate it ourselves.
    char buf[160];
    size_t n = t->length < sizeof(buf) ? t->length : sizeof(buf) - 1;
    memcpy(buf, t->value->cstring, n);
    buf[n] = '\0';
    int parsed = ac_parse_set(buf, s_alarms, MAX_ALARMS);
    s_count = parsed;
    APP_LOG(APP_LOG_LEVEL_INFO, "CFG AlarmSet='%s' -> %d alarms", buf, parsed);
    reload_and_rearm();
  }

  bool changed = false;
  GET_BOOL(SmartEnabled, smart_enabled);
  GET_INT(SmartWindowMin, smart_window_min);
  GET_INT(TimeSemantics, time_semantics);
  GET_INT(Sensitivity, sensitivity);
  GET_INT(SensPercentile, sens_percentile);
  GET_INT(SensMinutes, sens_minutes);
  GET_INT(WakeProfile, wake_profile);
  GET_INT(EscLeadGapS, esc.lead_gap_s);
  GET_INT(EscMinGapS, esc.min_gap_s);
  GET_INT(EscTightenS, esc.tighten_s);
  GET_INT(EscVibStartMs, esc.vib_start_ms);
  GET_INT(EscVibMaxMs, esc.vib_max_ms);
  GET_INT(EscPulsesStart, esc.pulses_start);
  GET_INT(EscPulsesMax, esc.pulses_max);
  GET_INT(EscSoundAfterS, esc.sound_after_s);
  GET_INT(EscSoundRampS, esc.sound_ramp_s);
  GET_INT(EscVolStart, esc.vol_start);
  GET_INT(EscVolMax, esc.vol_max);
  GET_INT(EscCapS, esc.cap_s);
  GET_INT(SnoozeMin, snooze_min);
  GET_INT(SnoozeMax, snooze_max);
  GET_INT(SnoozeRampOffsetS, snooze_ramp_offset_s);
  GET_INT(StopGesture, stop_gesture);
  GET_BOOL(LightPulse, light_pulse);
  GET_BOOL(DstCheck, dst_check);
  GET_INT(IdleExitSec, idle_exit_sec);
  if (changed) {
    as_save_config(&s_cfg);   // esc_clamp runs inside as_save_config
    APP_LOG(APP_LOG_LEVEL_INFO, "CFG updated: smart=%d win=%d sens=%d prof=%d gest=%d",
            (int)s_cfg.smart_enabled, s_cfg.smart_window_min, s_cfg.sensitivity,
            s_cfg.wake_profile, s_cfg.stop_gesture);
    reload_and_rearm();
  }
}

static void outbox_sent(DictionaryIterator *iter, void *context) {}
static void outbox_failed(DictionaryIterator *iter, AppMessageResult r, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "outbox failed: %d", (int)r);
}

static uint16_t list_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_count > 0 ? (uint16_t)s_count : 1;
}

static int16_t list_cell_height(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  return 44;   // two lines: time + weekday/next
}

static void list_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  GRect b = layer_get_bounds(cell);
  bool hl = menu_cell_layer_is_highlighted(cell);
  // Establish the colour for THIS cell before any override. MenuLayer does not
  // re-establish it per cell, so a disabled row would otherwise grey the rows
  // under it.
  graphics_context_set_text_color(gctx, hl ? GColorWhite : GColorBlack);

  if (s_count == 0) {
    graphics_draw_text(gctx, "No alarms.\nSet them on the phone.",
        fonts_get_system_font(FONT_KEY_GOTHIC_18),
        GRect(4, 2, b.size.w - 8, b.size.h - 4),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  const Alarm *a = &s_alarms[ci->row];
  if (!a->enabled) {
    graphics_context_set_text_color(gctx, hl ? GColorLightGray : GColorDarkGray);
  }

  char t[8];
  snprintf(t, sizeof(t), "%02d:%02d", a->minute_of_day / 60, a->minute_of_day % 60);
  graphics_draw_text(gctx, t, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
      GRect(4, -2, b.size.w - 8, 26), GTextOverflowModeFill, GTextAlignmentLeft, NULL);

  // Right-hand markers: skip-next, missed, smart.
  // 16, not 12: "skip " + "missed " + "~" is 13 chars plus the NUL, which a
  // 12-byte buffer truncates to "skip miss" when an alarm is skipped AND missed
  // AND smart is on. Safe but wrong on screen.
  char marks[16];
  marks[0] = '\0';
  snprintf(marks, sizeof(marks), "%s%s%s",
           a->skip_next ? "skip " : "",
           s_rs.missed[ci->row] ? "missed " : "",
           (s_cfg.smart_enabled && PBL_IF_HEALTH_ELSE(1, 0)) ? "~" : "");
  graphics_draw_text(gctx, marks, fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GRect(4, 0, b.size.w - 8, 22), GTextOverflowModeFill, GTextAlignmentRight, NULL);

  char days[12], rel[16], sub[32];
  fmt_weekdays(a->weekday_mask, days, sizeof(days));
  fmt_relative(ac_next_occurrence(a, time(NULL)), time(NULL), rel, sizeof(rel));
  snprintf(sub, sizeof(sub), "%s  %s", days, a->enabled ? rel : "off");
  graphics_draw_text(gctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GRect(4, 20, b.size.w - 8, 22), GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

// Short SELECT toggles the alarm; long SELECT skips its next occurrence. Both are
// operational, not configuration — configuration stays on the phone.
static void list_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  if (s_count == 0) return;
  Alarm *a = &s_alarms[ci->row];
  a->enabled = !a->enabled;
  if (!a->enabled) {
    a->skip_next = false;
  }
  reload_and_rearm();
}

static void list_select_long(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  if (s_count == 0) return;
  Alarm *a = &s_alarms[ci->row];
  if (!a->enabled) return;
  a->skip_next = !a->skip_next;
  reload_and_rearm();
}

static void list_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_list_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_list_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = list_num_rows,
    .get_cell_height = list_cell_height,
    .draw_row = list_draw_row,
    .select_click = list_select,
    .select_long_click = list_select_long,
  });
  menu_layer_set_click_config_onto_window(s_list_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_list_menu));
}

static void list_window_unload(Window *w) {
  menu_layer_destroy(s_list_menu);
  s_list_menu = NULL;
}

static void open_alarm_list(void) {
  if (!s_list_window) {
    s_list_window = window_create();
    window_set_window_handlers(s_list_window, (WindowHandlers){
      .load = list_window_load, .unload = list_window_unload,
    });
  }
  window_stack_push(s_list_window, true);
}

static uint16_t main_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return MAIN_ROW_COUNT;
}

static void main_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  switch (ci->row) {
    case MAIN_ROW_ALARMS: {
      char sub[24];
      time_t when = 0;
      int slot = ac_next_alarm(s_alarms, s_count, time(NULL), &when);
      if (slot < 0) {
        snprintf(sub, sizeof(sub), "none set");
      } else {
        char rel[16];
        fmt_relative(when, time(NULL), rel, sizeof(rel));
        snprintf(sub, sizeof(sub), "next %s", rel);
      }
      menu_cell_basic_draw(gctx, cell, "Alarms", sub, NULL);
      break;
    }
    case MAIN_ROW_LAST_NIGHT:
      menu_cell_basic_draw(gctx, cell, "Last night", "sleep + trigger", NULL);
      break;
    case MAIN_ROW_TEST:
      menu_cell_basic_draw(gctx, cell, "Test alarm", "rings in 2 min", NULL);
      break;
    default:
      break;
  }
}

static void add_test_alarm(void) {
  time_t t = time(NULL) + 120;
  struct tm *tm = localtime(&t);
  Alarm a = {
    .minute_of_day = (uint16_t)(tm->tm_hour * 60 + tm->tm_min),
    .weekday_mask = 0,        // one-time
    .enabled = true,
    .skip_next = false,
  };
  int slot = -1;
  for (int i = 0; i < s_count; i++) {
    if (!s_alarms[i].enabled && s_alarms[i].weekday_mask == 0) { slot = i; break; }
  }
  if (slot < 0 && s_count < MAX_ALARMS) { slot = s_count++; }
  if (slot < 0) { slot = s_count - 1; }   // all full: reuse the last slot
  s_alarms[slot] = a;
  reload_and_rearm();
}

static void main_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  switch (ci->row) {
    case MAIN_ROW_ALARMS:     open_alarm_list(); break;
    case MAIN_ROW_LAST_NIGHT: /* Task 12 pushes the summary window here */ break;
    case MAIN_ROW_TEST:       add_test_alarm(); break;
    default: break;
  }
}

static void main_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_main_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_main_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = main_num_rows,
    .draw_row = main_draw_row,
    .select_click = main_select,
  });
  menu_layer_set_click_config_onto_window(s_main_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_main_menu));
}

static void main_window_unload(Window *w) {
  menu_layer_destroy(s_main_menu);
  s_main_menu = NULL;
}

// --- The ring: escalation, stop gestures, snooze. ---

// Sound can only ever contribute when the platform has a speaker AND the system
// is not muted. speaker_is_muted() covers both the always-on mute (Settings ->
// Sounds & Haptics) and Quiet Time, and an app cannot override either.
static bool sound_available(void) {
#ifdef PBL_SPEAKER
  return !speaker_is_muted();
#else
  return false;
#endif
}

static AppTimer *s_burst_timer;
static AppTimer *s_light_timer;

static void light_off_cb(void *data) {
  s_light_timer = NULL;
  light_enable(false);
}

// Play one escalation burst: `pulses` vibration pulses of `vib_ms`, the backlight
// held for the burst (min ESC_LIGHT_MIN_MS), and a short tone at `volume`.
static void play_burst(const EscStep *s) {
  // Vibration. Max 8 pulses -> 15 segments (on, off, on, …).
  static uint32_t segs[15];
  int n = 0;
  for (int i = 0; i < s->pulses && n < (int)ARRAY_LENGTH(segs); i++) {
    if (i > 0) {
      segs[n++] = ESC_INTRA_PULSE_MS;
    }
    segs[n++] = s->vib_ms;
  }
  if (n > 0) {
    VibePattern pat = { .durations = segs, .num_segments = (uint32_t)n };
    vibes_enqueue_custom_pattern(pat);
  }

  // Backlight only for the burst, never between bursts: light_enable_interaction()
  // would hold it for the system timeout instead, which over a 15 min ring is the
  // difference between seconds and minutes of backlight.
  if (s_cfg.light_pulse) {
    light_enable(true);
    if (s_light_timer) {
      app_timer_cancel(s_light_timer);
    }
    s_light_timer = app_timer_register(s->light_ms, light_off_cb, NULL);
  }

#ifdef PBL_SPEAKER
  if (s->volume > 0 && !speaker_is_muted()) {
    // A rising two-note figure, repeated. midi 76 = E5, 79 = G5.
    static const SpeakerNote k_notes[4] = {
      { .midi_note = 76, .waveform = SpeakerWaveformTriangle, .duration_ms = 180, .velocity = 0 },
      { .midi_note = 0,  .waveform = SpeakerWaveformTriangle, .duration_ms = 60,  .velocity = 0 },
      { .midi_note = 79, .waveform = SpeakerWaveformTriangle, .duration_ms = 260, .velocity = 0 },
      { .midi_note = 0,  .waveform = SpeakerWaveformTriangle, .duration_ms = 80,  .velocity = 0 },
    };
    speaker_play_notes(k_notes, ARRAY_LENGTH(k_notes), s->volume);
  }
#endif
}

static Window    *s_ring_window;
static TextLayer *s_ring_time;
static TextLayer *s_ring_sub;
static TextLayer *s_ring_up;
static TextLayer *s_ring_down;
static TextLayer *s_ring_hint;
static Layer     *s_ring_progress;

static bool     s_ringing;
static uint32_t s_hold_ms;          // long-press progress, 0..STOP_HOLD_MS
static AppTimer *s_hold_timer;
static AppTimer *s_hint_timer;

#define STOP_HOLD_MS       2000
// window_long_click_subscribe's own recognition delay: kept short so the
// progress bar appears (already partly filled, at STOP_HOLD_DELAY_MS/
// STOP_HOLD_MS) almost as soon as the button goes down, rather than only
// after the full STOP_HOLD_MS -- which is what made a 2 s hold read as ~4 s
// with zero feedback for the first half.
#define STOP_HOLD_DELAY_MS 300
#define HINT_SHOW_MS   1500

static uint32_t ring_elapsed_s(void) {
  if (s_rs.ring_started_at == 0) {
    return 0;
  }
  EscParams e;
  as_effective_esc(&s_cfg, &e);

  time_t now = time(NULL);
  uint32_t base = (uint32_t)s_rs.snooze_count * s_cfg.snooze_ramp_offset_s;
  // Unlimited snoozing (snooze_max == 0) must never go silent: past a certain
  // count, base alone already exceeds cap_s and esc_step would report
  // over_cap on the very first burst of the resumed ring even though the user
  // is actively pressing Snooze, not ignoring the alarm. Saturate against full
  // development instead -- an exhausted ramp stays at maximum escalation,
  // never mute. Saturating (not just checking base >= cap_s) also closes a
  // band just below cap_s (e.g. snooze_count=7, base=840 with the default
  // 120s offset and cap_s=900): base alone doesn't reach cap_s yet, but by the
  // time base+d does, the ring would cap out after playing only ~60s at
  // maximum -- capping while the user is actively snoozing, not ignoring it.
  uint16_t full = esc_full_development_s(&e);
  if (base > full) {
    base = full;
  }

  long d = (long)(now - (time_t)s_rs.ring_started_at);
  // ring_started_at is deliberately in the future while a snooze is pending
  // (its expiry), and briefly negative right after an E_RANGE-shifted snooze
  // wakeup fires a minute or two early -- clamp rather than wrap the unsigned
  // return.
  if (d < 0) {
    d = 0;
  }
  // Defensive bound for a ring_started_at left stale by some path this
  // function did not anticipate. Clamp to cap_s -- do NOT reset to 0. A ring
  // that is legitimately past its cap looks EXACTLY like a "stale" one (both
  // have d > cap_s), so zeroing here would defeat the cap outright: the ramp
  // would restart from its gentlest step and keep restarting forever, the
  // alarm would vibrate/flash until the battery dies, over_cap would never
  // fire, missed[] would never be set, and reload_and_rearm() would never run
  // to re-arm the next alarm. Clamping to cap_s instead means esc_step still
  // sees elapsed_s >= cap_s and reports over_cap, exactly as it should for
  // "this ring is long over". start_ring's from_deadline reset is what
  // actually prevents a stale value in the first place; this is only the
  // belt-and-braces fallback, and it must fail toward the cap, not away from it.
  if (d > (long)e.cap_s) {
    d = (long)e.cap_s;
  }
  return base + (uint32_t)d;
}

static void update_ring_text(void) {
  // Guarded: unreachable today (every path that pops the ring window goes
  // through stop_ring_output first, which stops the burst/tick timers that
  // are this function's only callers), but Task 11's smart window pops
  // windows on its own paths -- once that lands, a timer callback racing a
  // window that has already unloaded (and NULLed these) must not crash.
  if (!s_ring_time || !s_ring_sub) {
    return;
  }
  static char t[8];
  static char sub[48];
  clock_copy_time_string(t, sizeof(t));
  text_layer_set_text(s_ring_time, t);

  EscParams e;
  as_effective_esc(&s_cfg, &e);
  uint32_t el = ring_elapsed_s();
  const char *mute = sound_available() ? "" : "  (muted)";
  if (el >= e.cap_s) {
    snprintf(sub, sizeof(sub), "Alarm missed%s", mute);
  } else if (s_rs.snooze_count > 0) {
    snprintf(sub, sizeof(sub), "Snooze %d%s", s_rs.snooze_count, mute);
  } else {
    snprintf(sub, sizeof(sub), "Alarm%s", mute);
  }
  text_layer_set_text(s_ring_sub, sub);
}

static void burst_cb(void *data);
static void ring_minute_tick(struct tm *t, TimeUnits units);

static void schedule_next_burst(uint16_t gap_s) {
  if (s_burst_timer) {
    app_timer_cancel(s_burst_timer);
  }
  s_burst_timer = app_timer_register(gap_s * 1000, burst_cb, NULL);
}

static void burst_cb(void *data) {
  s_burst_timer = NULL;
  if (!s_ringing) {
    return;
  }
  EscParams e;
  as_effective_esc(&s_cfg, &e);
  uint32_t el = ring_elapsed_s();
  EscStep s = esc_step(&e, el, sound_available());
  update_ring_text();

  if (s.over_cap) {
    // Stop making noise but LEAVE the screen up, so the user can see the alarm
    // was missed. Mark the slot missed; the marker clears the next time this
    // alarm rings and is dismissed normally.
    APP_LOG(APP_LOG_LEVEL_INFO, "RING cap reached at %lu s", (unsigned long)el);
    if (s_rs.pending_slot >= 0 && s_rs.pending_slot < MAX_ALARMS) {
      s_rs.missed[s_rs.pending_slot] = true;
    }
    vibes_cancel();
#ifdef PBL_SPEAKER
    speaker_stop();
#endif
    if (s_light_timer) { app_timer_cancel(s_light_timer); s_light_timer = NULL; }
    if (s_cfg.light_pulse) {
      light_enable(false);
    }
    // Clear s_ringing so a LATER wakeup arriving in this same process (another
    // alarm's WC_DEADLINE, or a stray WC_SNOOZE) is not swallowed by the
    // "already ringing" guard in handle_wakeup_cookie -- a missed alarm must
    // not also silence the next one.
    s_ringing = false;
    // Persist AND re-arm: a missed alarm must not leave the wakeup chain
    // empty. The launch-time sc_rearm and start_ring's own keep-alive are both
    // already spent by the time the cap is reached, so without this a missed
    // alarm with dst_check off could leave zero wakeups scheduled and no
    // alarm would ever fire again. reload_and_rearm both saves RunState and
    // re-schedules from the current alarms/config/state.
    reload_and_rearm();
    return;
  }

  APP_LOG(APP_LOG_LEVEL_INFO, "RING t=%lu gap=%d vib=%d x%d vol=%d light=%d",
          (unsigned long)el, s.gap_s, s.vib_ms, s.pulses, s.volume, s.light_ms);
  play_burst(&s);
  schedule_next_burst(s.gap_s);
}

static void stop_ring_output(void) {
  s_ringing = false;
  if (s_burst_timer) { app_timer_cancel(s_burst_timer); s_burst_timer = NULL; }
  if (s_light_timer) { app_timer_cancel(s_light_timer); s_light_timer = NULL; }
  if (s_hold_timer)  { app_timer_cancel(s_hold_timer);  s_hold_timer = NULL; }
  // Without this, pressing DOWN once (showing the hint) then UP within
  // HINT_SHOW_MS leaves hint_hide_cb pending against a TextLayer that
  // ring_window_unload may since have destroyed.
  if (s_hint_timer)  { app_timer_cancel(s_hint_timer);  s_hint_timer = NULL; }
  vibes_cancel();
#ifdef PBL_SPEAKER
  speaker_stop();
#endif
  if (s_cfg.light_pulse) {
    light_enable(false);
  }
  // The minute tick is only wanted while the ring is up; leaving it subscribed
  // would have ring_minute_tick write to layers ring_window_unload may since
  // have destroyed, once anything pops the ring window without exiting the
  // app outright (Task 11's smart window is exactly such a path).
  tick_timer_service_unsubscribe();
}

static void ring_stop_now(void) {
  stop_ring_output();
  int slot = s_rs.pending_slot;
  if (slot >= 0 && slot < MAX_ALARMS) {
    s_rs.missed[slot] = false;             // dismissed normally: clear the marker
    if (s_alarms[slot].weekday_mask == 0) {
      s_alarms[slot].enabled = false;      // a one-time alarm fires once
    }
    if (s_alarms[slot].skip_next) {
      s_alarms[slot].skip_next = false;    // the skip has been consumed
    }
  }
  s_rs.pending_slot = -1;
  s_rs.ring_started_at = 0;
  s_rs.window_started_at = 0;
  s_rs.snooze_count = 0;
  reload_and_rearm();
  close_to_watchface();
}

static void ring_snooze_now(void) {
  // Out of snoozes (or snoozing disabled) behaves as Stop rather than doing
  // nothing, so the button is never inert.
  if (s_cfg.snooze_min == 0
      || (s_cfg.snooze_max != 0 && s_rs.snooze_count >= s_cfg.snooze_max)) {
    APP_LOG(APP_LOG_LEVEL_INFO, "SNOOZE exhausted -> stop");
    ring_stop_now();
    return;
  }

  uint32_t prev_ring_started_at = s_rs.ring_started_at;
  uint8_t  prev_snooze_count = s_rs.snooze_count;

  stop_ring_output();
  s_rs.snooze_count++;
  // ring_started_at is moved to the snooze expiry so ring_elapsed_s() resumes
  // from the right place, and so sc_rearm can re-derive the snooze wakeup after
  // a relaunch (it reads this field directly — see scheduler.c). Set and saved
  // BEFORE the re-arm below, since sc_rearm reads it.
  time_t until = time(NULL) + (time_t)s_cfg.snooze_min * SECONDS_PER_MINUTE;
  s_rs.ring_started_at = (uint32_t)until;
  as_save_runstate(&s_rs);

  // Re-arm through sc_rearm rather than a manual sc_cancel_all()+sc_schedule()
  // of just this one wakeup (superseding the earlier, narrower amendment).
  // sc_rearm still cancels everything first, which is what kills the stale
  // SC_REENTRY_GAP_S (180 s) keep-alive start_ring armed -- every snooze
  // length on offer is longer than that, so leaving it live would fire ~3 min
  // in and re-ring before the snooze the user asked for. But unlike a bare
  // cancel-then-schedule, sc_rearm ALSO re-arms every other alarm's own
  // WC_DEADLINE at priority 1 (a bare cancel+schedule would silently drop a
  // second alarm's deadline until this snooze's wakeup happened to fire and
  // re-arm it -- e.g. snoozing a 07:00 alarm for 10 minutes would silently
  // swallow a 07:05 alarm), and it derives the snooze wakeup itself from
  // ring_started_at (already set above) via the exact same logic every other
  // caller of sc_rearm uses, so there is exactly one code path that computes
  // that wakeup rather than two that could disagree.
  bool armed = sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, time(NULL));
  APP_LOG(APP_LOG_LEVEL_INFO, "SNOOZE #%d until %lu (ramp offset %d s)",
          s_rs.snooze_count, (unsigned long)until,
          s_rs.snooze_count * s_cfg.snooze_ramp_offset_s);

  if (!armed) {
    // The snooze wakeup itself could not be scheduled (every +/-2 min E_RANGE
    // shift was rejected, or the device is out of wakeup slots). Exiting to
    // the watchface now would silently lose the alarm with nothing on screen
    // to show for it, so don't: undo the snooze bookkeeping, re-arm a plain
    // keep-alive for the continued ring (sc_rearm's cancel just dropped it),
    // and keep ringing -- the ring's own escalation/cap is the fallback that
    // is still guaranteed to fire, and the user can try Snooze again.
    APP_LOG(APP_LOG_LEVEL_ERROR, "SNOOZE could not be armed -- staying ringing");
    s_rs.ring_started_at = prev_ring_started_at;
    s_rs.snooze_count = prev_snooze_count;
    as_save_runstate(&s_rs);
    sc_schedule(time(NULL) + SC_REENTRY_GAP_S, WC_SNOOZE);
    // stop_ring_output() (called above) unsubscribed the minute tick and left
    // whichever of sub/hint was showing untouched -- undo both, since the
    // ring is staying up: re-subscribe so the clock keeps refreshing every
    // minute between bursts, and force back to the subtitle so a hint from
    // right before this press doesn't linger on screen for the rest of the ring.
    tick_timer_service_subscribe(MINUTE_UNIT, ring_minute_tick);
    if (s_ring_hint) {
      layer_set_hidden(text_layer_get_layer(s_ring_hint), true);
    }
    if (s_ring_sub) {
      layer_set_hidden(text_layer_get_layer(s_ring_sub), false);
    }
    s_ringing = true;
    burst_cb(NULL);
    return;
  }

  close_to_watchface();
}

static void hint_hide_cb(void *data) {
  s_hint_timer = NULL;
  // Guarded for the same reason as update_ring_text: a timer callback must
  // not outlive the window that owns the layers it touches.
  if (!s_ring_hint || !s_ring_sub) {
    return;
  }
  layer_set_hidden(text_layer_get_layer(s_ring_hint), true);
  // The hint shares the subtitle's slot (see ring_window_load) -- restore it.
  layer_set_hidden(text_layer_get_layer(s_ring_sub), false);
}

static void show_press_again_hint(void) {
  // Guarded for the same reason as update_ring_text.
  if (!s_ring_hint || !s_ring_sub) {
    return;
  }
  uint8_t need = (s_cfg.stop_gesture == STOP_THREE_TAP) ? 3 : 2;
  static char hint[24];
  // Plain ASCII "x", not the U+00D7 multiplication sign: GOTHIC_18_BOLD has no
  // glyph for it, so it rendered as a tofu box ("2<box> = Stop") on the actual
  // ring screen -- confirmed by pixel-zooming an emulator screenshot. This is
  // the one instruction a half-asleep user actually reads, so it must render.
  snprintf(hint, sizeof(hint), "Press %dx to stop", need);
  text_layer_set_text(s_ring_hint, hint);
  // The hint reuses the subtitle's exact slot (see ring_window_load) rather
  // than needing its own vertical room, which the 144x168 boards do not have
  // once the button labels claim their 22%/78% bands -- so hide one to show
  // the other.
  layer_set_hidden(text_layer_get_layer(s_ring_sub), true);
  layer_set_hidden(text_layer_get_layer(s_ring_hint), false);
  if (s_hint_timer) {
    app_timer_cancel(s_hint_timer);
  }
  s_hint_timer = app_timer_register(HINT_SHOW_MS, hint_hide_cb, NULL);
}

static void ring_down_multi(ClickRecognizerRef rec, void *ctx) {
  uint8_t need = (s_cfg.stop_gesture == STOP_THREE_TAP) ? 3 : 2;
  if (click_number_of_clicks_counted(rec) >= need) {
    ring_stop_now();
  } else {
    // Without this the button is bound only to a multi-click and a single press
    // does nothing at all, which feels broken and makes the user press again too
    // slowly for the 400 ms window.
    show_press_again_hint();
  }
}

static void hold_tick_cb(void *data) {
  s_hold_timer = NULL;
  s_hold_ms += 100;
  layer_mark_dirty(s_ring_progress);
  if (s_hold_ms >= STOP_HOLD_MS) {
    ring_stop_now();
    return;
  }
  s_hold_timer = app_timer_register(100, hold_tick_cb, NULL);
}

static void ring_down_hold_start(ClickRecognizerRef rec, void *ctx) {
  // The button has already been down for STOP_HOLD_DELAY_MS by the time this
  // fires (that is what window_long_click_subscribe's delay_ms means), so the
  // bar starts already partly filled instead of at 0% -- total real hold time
  // to stop is still STOP_HOLD_MS from the physical press, not STOP_HOLD_MS
  // AFTER this callback.
  s_hold_ms = STOP_HOLD_DELAY_MS;
  layer_set_hidden(s_ring_progress, false);
  layer_mark_dirty(s_ring_progress);
  if (s_hold_timer) app_timer_cancel(s_hold_timer);
  s_hold_timer = app_timer_register(100, hold_tick_cb, NULL);
}

static void ring_down_hold_release(ClickRecognizerRef rec, void *ctx) {
  if (s_hold_timer) { app_timer_cancel(s_hold_timer); s_hold_timer = NULL; }
  s_hold_ms = 0;
  layer_set_hidden(s_ring_progress, true);
  layer_mark_dirty(s_ring_progress);
}

static void ring_up(ClickRecognizerRef rec, void *ctx) {
  ring_snooze_now();
}

// BACK and SELECT are bound EXPLICITLY to a no-op. An unbound BACK pops the
// window by default, which would silently dismiss the alarm — the opposite of
// what an alarm clock must do.
static void ring_noop(ClickRecognizerRef rec, void *ctx) {}

static void ring_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, ring_up);
  if (s_cfg.stop_gesture == STOP_LONG_PRESS) {
    window_long_click_subscribe(BUTTON_ID_DOWN, STOP_HOLD_DELAY_MS,
                                ring_down_hold_start, ring_down_hold_release);
  } else {
    uint8_t need = (s_cfg.stop_gesture == STOP_THREE_TAP) ? 3 : 2;
    window_multi_click_subscribe(BUTTON_ID_DOWN, 1, need, 400, false, ring_down_multi);
  }
  window_single_click_subscribe(BUTTON_ID_BACK, ring_noop);
  window_single_click_subscribe(BUTTON_ID_SELECT, ring_noop);
}

static void progress_update(Layer *layer, GContext *gctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(gctx, GColorBlack);
  int w = (int)((uint32_t)b.size.w * s_hold_ms / STOP_HOLD_MS);
  graphics_fill_rect(gctx, GRect(0, 0, w, b.size.h), 0, GCornerNone);
}

// Pick the largest of these three whose measured line-box height fits
// time_h, falling back to the smallest if even that overflows. Mirrors
// PebbleCountdownTimer's alarm_title_font() (src/c/main.c:285-303): board
// screen height varies too much (144..228 px) to hardcode one font size and
// assume it fits -- a fixed BITHAM_42_BOLD clipped its own descenders on
// every 144x168 board (confirmed by pixel-zooming a screenshot: "14:20"
// rendered with the tops of the digits sheared off).
static GFont ring_time_font(int box_w, int time_h, GSize *out) {
  static const char *const keys[] = {
    FONT_KEY_BITHAM_42_BOLD,
    FONT_KEY_BITHAM_30_BLACK,
    FONT_KEY_GOTHIC_28_BOLD,
  };
  const GRect probe = GRect(0, 0, box_w, 200);
  GFont chosen = NULL;
  for (unsigned i = 0; i < ARRAY_LENGTH(keys); i++) {
    GFont f = fonts_get_system_font(keys[i]);
    // "00:00" (not the real time) so the measurement is stable regardless of
    // which digits happen to be showing -- every system digit glyph in these
    // fonts is the same advance/height, so any two-digit HH:MM is equivalent.
    GSize sz = graphics_text_layout_get_content_size(
        "00:00", f, probe, GTextOverflowModeFill, GTextAlignmentCenter);
    chosen = f; *out = sz;
    if (sz.h <= time_h) { break; }   // largest font that fits vertically -> use it
  }
  return chosen;
}

static void ring_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  window_set_background_color(w, GColorWhite);

  // Follows PebbleCountdownTimer's alarm screen (src/c/main.c:344-399): the two
  // button labels right-aligned and positioned vertically AT their physical
  // buttons (UP ~22% of height, DOWN ~78%) -- readable at arm's length by
  // someone who just woke up, instead of GOTHIC_18 tucked into a corner. The
  // clock is the biggest, most important element on screen, so its font is
  // chosen from the space actually available (ring_time_font) rather than
  // fixed -- and on a 144x168 board even that isn't enough room at the normal
  // 28-bold label height, so the labels shrink to GOTHIC_24_BOLD in that case
  // to buy the clock the room instead. Which GESTURE stops the alarm is
  // taught by the hint on the first press (or the progress bar for
  // long-press), not by the button label -- so "Stop" alone is always enough
  // there regardless of which size is picked.
  int btn_h = 34;
  GFont btn_font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  int up_y   = b.size.h * 22 / 100 - btn_h / 2;
  int down_y = b.size.h * 78 / 100 - btn_h / 2;
  // 2 px gap under UP, 2 px reserved above DOWN (the latter is also what
  // keeps the subtitle's descenders -- e.g. "Alarm (muted)" -- off the exact
  // clip edge of the shared band, previously flush with it on a 144x168 board).
  int mid_top = up_y + btn_h + 2;
  int mid_h   = down_y - mid_top - 2;
  int time_h  = mid_h * 6 / 10;

  GSize time_sz;
  GFont time_font = ring_time_font(b.size.w, time_h, &time_sz);
  if (time_sz.h > time_h) {
    // Not even BITHAM_30_BLACK/GOTHIC_28_BOLD's measured line box fit at the
    // normal label height -- shrink the labels to buy the clock more room and
    // re-measure once against the new (larger) time_h.
    btn_h = 28;
    btn_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    up_y   = b.size.h * 22 / 100 - btn_h / 2;
    down_y = b.size.h * 78 / 100 - btn_h / 2;
    mid_top = up_y + btn_h + 2;
    mid_h   = down_y - mid_top - 2;
    time_h  = mid_h * 6 / 10;
    time_font = ring_time_font(b.size.w, time_h, &time_sz);
  }
  int sub_h = mid_h - time_h;

  s_ring_up = text_layer_create(GRect(0, up_y, b.size.w - 6, btn_h));
  text_layer_set_font(s_ring_up, btn_font);
  text_layer_set_text_alignment(s_ring_up, GTextAlignmentRight);
  text_layer_set_text(s_ring_up, "Snooze");
  text_layer_set_background_color(s_ring_up, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_ring_up));

  // "Stop" is constant regardless of s_cfg.stop_gesture -- at either label
  // size, "2x = Stop" would not fit as well as "Stop" alone, and which button
  // stops the alarm is all this label needs to say; the hint (multi-tap) or
  // the progress bar (long-press) teaches the gesture itself.
  s_ring_down = text_layer_create(GRect(0, down_y, b.size.w - 6, btn_h));
  text_layer_set_font(s_ring_down, btn_font);
  text_layer_set_text_alignment(s_ring_down, GTextAlignmentRight);
  text_layer_set_text(s_ring_down, "Stop");
  text_layer_set_background_color(s_ring_down, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_ring_down));

  // Time + subtitle share the band left between the two button labels, split
  // proportionally (60/40) -- verified by screenshot on both boards rather
  // than assumed (see the task report).
  s_ring_time = text_layer_create(GRect(0, mid_top, b.size.w, time_h));
  text_layer_set_font(s_ring_time, time_font);
  text_layer_set_text_alignment(s_ring_time, GTextAlignmentCenter);
  text_layer_set_background_color(s_ring_time, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_ring_time));

  s_ring_sub = text_layer_create(GRect(0, mid_top + time_h, b.size.w, sub_h));
  text_layer_set_font(s_ring_sub, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_ring_sub, GTextAlignmentCenter);
  text_layer_set_background_color(s_ring_sub, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_ring_sub));

  // The hint reuses the subtitle's exact slot (hidden/shown together, never
  // both at once -- see show_press_again_hint/hint_hide_cb): there is no
  // separate room for it once the button labels claim their bands. It
  // temporarily replaces "Alarm"/"Snooze N" for its brief HINT_SHOW_MS,
  // which is an acceptable trade since it is the one instruction a
  // half-asleep user actually needs to read.
  s_ring_hint = text_layer_create(GRect(0, mid_top + time_h, b.size.w, sub_h));
  text_layer_set_font(s_ring_hint, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_ring_hint, GTextAlignmentCenter);
  text_layer_set_background_color(s_ring_hint, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_ring_hint));
  layer_set_hidden(text_layer_get_layer(s_ring_hint), true);

  s_ring_progress = layer_create(GRect(0, b.size.h - 6, b.size.w, 6));
  layer_set_update_proc(s_ring_progress, progress_update);
  layer_add_child(root, s_ring_progress);
  layer_set_hidden(s_ring_progress, true);

  update_ring_text();
}

static void ring_window_unload(Window *w) {
  layer_destroy(s_ring_progress);     s_ring_progress = NULL;
  text_layer_destroy(s_ring_hint);    s_ring_hint = NULL;
  text_layer_destroy(s_ring_down);    s_ring_down = NULL;
  text_layer_destroy(s_ring_up);      s_ring_up = NULL;
  text_layer_destroy(s_ring_sub);     s_ring_sub = NULL;
  text_layer_destroy(s_ring_time);    s_ring_time = NULL;
}

static void ring_minute_tick(struct tm *t, TimeUnits units) {
  update_ring_text();
}

static void start_ring(int slot, bool from_deadline) {
  s_rs.pending_slot = (int8_t)slot;
  // A fresh deadline always means a fresh ring: reset unconditionally rather
  // than probing snooze_count/ring_started_at for "does this look fresh",
  // which is exactly the check that let a day-old snooze_count and
  // ring_started_at survive an over_cap miss and poison every later alarm
  // (elapsed = 86400+ s -> immediate over_cap, forever, silently).
  if (from_deadline) {
    s_rs.snooze_count = 0;
    s_rs.ring_started_at = 0;
  }
  // Only stamp "now" when there is genuinely nothing to resume: a snooze
  // expiry (ring_started_at already holds it) or a mid-ring keep-alive
  // relaunch (ring_started_at already holds the original start) must NOT be
  // overwritten here, or resuming would restart the ramp at t=0 and quietly
  // downgrade a long-running alarm back to its gentlest stage.
  if (s_rs.ring_started_at == 0) {
    s_rs.ring_started_at = (uint32_t)time(NULL);
  }
  // The hard alarm time for this ring cycle (RunState's documented meaning),
  // for Task 11/12 to read. Only stamped on a genuinely fresh deadline, not on
  // a snooze/keep-alive continuation of the same cycle -- ring_started_at at
  // this exact point IS "now" whenever from_deadline just reset it above, so
  // it is also the best available approximation of the actual deadline instant
  // (accurate to within sc_schedule's +/-2 min E_RANGE shift).
  if (from_deadline) {
    s_rs.deadline_at = s_rs.ring_started_at;
  }
  s_rs.window_started_at = 0;
  as_save_runstate(&s_rs);

  // Keep a wakeup live for the whole ring: if anything kills the app mid-ring
  // (another app's wakeup, a phone-initiated launch), the alarm comes back
  // instead of being lost.
  sc_schedule(time(NULL) + SC_REENTRY_GAP_S, WC_SNOOZE);

  APP_LOG(APP_LOG_LEVEL_INFO, "RING start slot=%d deadline=%d sound=%d snooze=%d",
          slot, (int)from_deadline, (int)sound_available(), s_rs.snooze_count);

  if (!s_ring_window) {
    s_ring_window = window_create();
    window_set_window_handlers(s_ring_window, (WindowHandlers){
      .load = ring_window_load, .unload = ring_window_unload,
    });
    window_set_click_config_provider(s_ring_window, ring_click_config);
  }
  window_stack_push(s_ring_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, ring_minute_tick);

  s_ringing = true;
  burst_cb(NULL);   // first burst immediately
}

static void handle_wakeup_cookie(int32_t cookie) {
  time_t now = time(NULL);
  switch (cookie) {
    case WC_DEADLINE:
    case WC_SNOOZE: {
      if (s_ringing) {
        // Still ringing: this fired mid-ring -- the periodic keep-alive
        // itself, or another alarm's deadline coinciding. start_ring's
        // keep-alive only covers the first SC_REENTRY_GAP_S, so without
        // re-arming here a kill anywhere past that window loses the alarm
        // outright, defeating the point of keeping a wakeup live for the
        // whole ring.
        sc_schedule(now + SC_REENTRY_GAP_S, WC_SNOOZE);
        break;
      }
      int slot = s_rs.pending_slot;
      if (slot < 0) {
        time_t when = 0;
        slot = ac_next_alarm(s_alarms, s_count, now - 120, &when);
      }
      if (slot < 0 && s_count > 0) {
        slot = 0;   // last-resort fallback: something to ring beats nothing
      }
      // Bounded against s_count (not just >= 0): with s_count == 0 the
      // fallback above leaves slot at -1, and starting a ring on slot 0 with
      // no alarms configured would have ring_stop_now later mutate
      // s_alarms[0] out of range. This can also happen with s_count > 0: if
      // alarms were deleted (from the phone) while this wakeup was pending,
      // s_rs.pending_slot can point past the shrunk array.
      if (slot >= 0 && slot < s_count) {
        start_ring(slot, cookie == WC_DEADLINE);
      } else {
        // Nothing to ring, but the wakeup was still consumed -- without this,
        // a stale/out-of-range pending_slot would silently drop the wakeup
        // chain here (no ring, no re-arm), and no alarm would ever fire again.
        //
        // Also clear the stale pending_slot itself (found in Task 7's review):
        // with pending_slot left at an out-of-range value (e.g. an alarm the
        // phone just deleted), every LATER WC_DEADLINE takes the `slot < 0`
        // branch above straight to `s_rs.pending_slot`, fails this same
        // `slot < s_count` bound again, and lands right back here -- so no
        // alarm ever rings again. Resetting it (and the ring bookkeeping that
        // goes with a fresh cycle) here is what breaks that loop.
        s_rs.pending_slot = -1;
        s_rs.ring_started_at = 0;
        s_rs.snooze_count = 0;
        as_save_runstate(&s_rs);
        sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, now);
      }
      break;
    }
    case WC_WINDOW:
    case WC_REENTRY:
      // Task 11 opens/continues the smart window here. Until then, re-arm so the
      // deadline stays scheduled.
      sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, now);
      break;
    case WC_DST:
      sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, now);
      break;
    default:
      break;
  }
}

static void wakeup_handler(WakeupId id, int32_t cookie) {
  APP_LOG(APP_LOG_LEVEL_INFO, "WAKEUP while running cookie=%d", (int)cookie);
  handle_wakeup_cookie(cookie);
}

int main(void) {
  as_load_config(&s_cfg);
  as_load_runstate(&s_rs);
  as_load_alarms(s_alarms, &s_count);

  // First-run seed only: a reasonable demo alarm set for a fresh install with
  // nothing stored yet. The phone overwrites this on its first config save.
  if (s_count == 0) {
    s_count = ac_parse_set("07:00|1111100;08:30|0000011;-06:15|1111111", s_alarms, MAX_ALARMS);
    as_save_alarms(s_alarms, s_count);
  }

  // Captures the same launch-wakeup data the pre-existing log line reported
  // (Task 6), plus the cookie itself so it can be dispatched to start_ring below
  // once the main window exists — a single read of the launch event instead of
  // reading it here for logging and again in Step 6's own block.
  int32_t launch_cookie = 0;
  bool launched_by_wakeup = false;
  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    WakeupId id = 0;
    launched_by_wakeup = wakeup_get_launch_event(&id, &launch_cookie);
    APP_LOG(APP_LOG_LEVEL_INFO, "LAUNCHED BY WAKEUP cookie=%d", (int)launch_cookie);
  } else {
    APP_LOG(APP_LOG_LEVEL_INFO, "LAUNCHED reason=%d", (int)launch_reason());
  }

  if (!sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, time(NULL))) {
    // A pending snooze exists (RunState says so) but its wakeup could not be
    // placed. This can happen if the user opens the app manually while a
    // snooze is in flight. There is no UI at this layer to surface it (Task
    // 8/9's config page is where a notification would eventually live), so at
    // minimum this must not be silently discarded -- log it visibly.
    APP_LOG(APP_LOG_LEVEL_ERROR, "launch sc_rearm: pending snooze could not be armed");
  }

  // The handler is only invoked for wakeups that fire while already running; a
  // wakeup that launches the app is instead handled by dispatching
  // launch_cookie below, once the window stack exists.
  wakeup_service_subscribe(wakeup_handler);

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers){
    .load = main_window_load, .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);

  if (launched_by_wakeup) {
    handle_wakeup_cookie(launch_cookie);
  }

  app_message_register_inbox_received(inbox_received);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(512, 128);

  app_event_loop();

  as_save_alarms(s_alarms, s_count);
  as_save_runstate(&s_rs);
  if (s_list_window) window_destroy(s_list_window);
  if (s_ring_window) window_destroy(s_ring_window);
  window_destroy(s_main_window);
  return 0;
}
