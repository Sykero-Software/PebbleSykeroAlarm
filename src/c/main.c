// SPDX-License-Identifier: GPL-3.0-only
#include <pebble.h>
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

#define STOP_HOLD_MS   2000
#define HINT_SHOW_MS   1500

static uint32_t ring_elapsed_s(void) {
  if (s_rs.ring_started_at == 0) {
    return 0;
  }
  time_t now = time(NULL);
  uint32_t base = (uint32_t)s_rs.snooze_count * s_cfg.snooze_ramp_offset_s;
  long d = (long)(now - (time_t)s_rs.ring_started_at);
  if (d < 0) d = 0;
  return base + (uint32_t)d;
}

static void update_ring_text(void) {
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
    light_enable(false);
    as_save_runstate(&s_rs);
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
  vibes_cancel();
#ifdef PBL_SPEAKER
  speaker_stop();
#endif
  light_enable(false);
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
  EscParams e;
  as_effective_esc(&s_cfg, &e);
  // Out of snoozes (or snoozing disabled) behaves as Stop rather than doing
  // nothing, so the button is never inert.
  if (s_cfg.snooze_min == 0
      || (s_cfg.snooze_max != 0 && s_rs.snooze_count >= s_cfg.snooze_max)) {
    APP_LOG(APP_LOG_LEVEL_INFO, "SNOOZE exhausted -> stop");
    ring_stop_now();
    return;
  }
  stop_ring_output();
  s_rs.snooze_count++;
  // ring_started_at is moved to the snooze expiry so ring_elapsed_s() resumes
  // from the right place, and so sc_rearm can re-derive the snooze wakeup after
  // a relaunch (it reads this field directly — see scheduler.c).
  time_t until = time(NULL) + (time_t)s_cfg.snooze_min * SECONDS_PER_MINUTE;
  s_rs.ring_started_at = (uint32_t)until;
  as_save_runstate(&s_rs);
  // start_ring armed a WC_SNOOZE keep-alive SC_REENTRY_GAP_S (180 s) out. Every
  // snooze length on offer is longer than that, so without cancelling it first
  // the stale wakeup would fire ~3 min in and re-ring long before the snooze the
  // user asked for. There is no selective cancel, so drop everything and let the
  // snooze wakeup below (and the sc_rearm on the next launch) restore the rest.
  sc_cancel_all();
  sc_schedule(until, WC_SNOOZE);
  APP_LOG(APP_LOG_LEVEL_INFO, "SNOOZE #%d until %lu (ramp offset %d s)",
          s_rs.snooze_count, (unsigned long)until,
          s_rs.snooze_count * s_cfg.snooze_ramp_offset_s);
  close_to_watchface();
}

static void hint_hide_cb(void *data) {
  s_hint_timer = NULL;
  layer_set_hidden(text_layer_get_layer(s_ring_hint), true);
}

static void show_press_again_hint(void) {
  uint8_t need = (s_cfg.stop_gesture == STOP_THREE_TAP) ? 3 : 2;
  static char hint[24];
  snprintf(hint, sizeof(hint), "Press %d\xC3\x97 to stop", need);
  text_layer_set_text(s_ring_hint, hint);
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
  s_hold_ms = 0;
  layer_set_hidden(s_ring_progress, false);
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
    window_long_click_subscribe(BUTTON_ID_DOWN, STOP_HOLD_MS,
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

static void ring_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  window_set_background_color(w, GColorWhite);

  s_ring_time = text_layer_create(GRect(0, b.size.h / 2 - 44, b.size.w, 44));
  text_layer_set_font(s_ring_time, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_ring_time, GTextAlignmentCenter);
  text_layer_set_background_color(s_ring_time, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_ring_time));

  s_ring_sub = text_layer_create(GRect(0, b.size.h / 2, b.size.w, 24));
  text_layer_set_font(s_ring_sub, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_ring_sub, GTextAlignmentCenter);
  text_layer_set_background_color(s_ring_sub, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_ring_sub));

  s_ring_up = text_layer_create(GRect(0, 6, b.size.w - 4, 20));
  text_layer_set_font(s_ring_up, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_ring_up, GTextAlignmentRight);
  text_layer_set_text(s_ring_up, "Snooze");
  text_layer_set_background_color(s_ring_up, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_ring_up));

  s_ring_down = text_layer_create(GRect(0, b.size.h - 28, b.size.w - 4, 20));
  text_layer_set_font(s_ring_down, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_ring_down, GTextAlignmentRight);
  text_layer_set_text(s_ring_down,
      s_cfg.stop_gesture == STOP_LONG_PRESS ? "Hold = Stop"
    : s_cfg.stop_gesture == STOP_THREE_TAP  ? "3\xC3\x97 = Stop"
                                            : "2\xC3\x97 = Stop");
  text_layer_set_background_color(s_ring_down, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_ring_down));

  s_ring_hint = text_layer_create(GRect(0, b.size.h - 52, b.size.w - 4, 20));
  text_layer_set_font(s_ring_hint, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_ring_hint, GTextAlignmentRight);
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
  layer_destroy(s_ring_progress);
  text_layer_destroy(s_ring_hint);
  text_layer_destroy(s_ring_down);
  text_layer_destroy(s_ring_up);
  text_layer_destroy(s_ring_sub);
  text_layer_destroy(s_ring_time);
}

static void ring_minute_tick(struct tm *t, TimeUnits units) {
  update_ring_text();
}

static void start_ring(int slot, bool from_deadline) {
  s_rs.pending_slot = (int8_t)slot;
  if (s_rs.ring_started_at == 0 || s_rs.snooze_count == 0) {
    s_rs.ring_started_at = (uint32_t)time(NULL);
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
      int slot = s_rs.pending_slot;
      if (slot < 0) {
        time_t when = 0;
        slot = ac_next_alarm(s_alarms, s_count, now - 120, &when);
      }
      if (slot < 0) slot = 0;
      if (!s_ringing) {
        start_ring(slot, cookie == WC_DEADLINE);
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

  sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, time(NULL));

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

  app_event_loop();

  as_save_alarms(s_alarms, s_count);
  as_save_runstate(&s_rs);
  if (s_list_window) window_destroy(s_list_window);
  if (s_ring_window) window_destroy(s_ring_window);
  window_destroy(s_main_window);
  return 0;
}
