// SPDX-License-Identifier: GPL-3.0-only
#include <pebble.h>
#include <string.h>
#include "alarm_calc.h"
#include "alarm_store.h"
#include "debug_dump.h"
#include "escalation.h"
#include "health_read.h"
#include "scheduler.h"
#include "sleep_eval.h"

static Alarm    s_alarms[MAX_ALARMS];
static int      s_count;
// The AlarmSet string that produced s_alarms, persisted across launches. An
// inbound AlarmSet identical to this one is a no-op, which is what keeps the
// every-launch config resend from reverting the watch's own on/off + skip toggles.
static char     s_alarmset_str[ALARMSET_STR_MAX];
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

// --- Idle auto-exit: matches the other four Sykerö watchapps. Armed only on
// the main menu and the alarm list (their .appear/.disappear below); never
// while a ring-family window is on screen, and never while the phone's config
// page is open. "Ring-family window on screen" is checked directly against the
// window stack (see idle_reset), NOT inferred from s_ringing/window_started_at
// alone -- burst_cb's over_cap branch clears s_ringing while deliberately
// leaving the ring window up (the intentionally non-dismissible "Alarm missed"
// screen), so a flag-only guard has a gap there.
//
// s_ringing/s_ring_window/s_wait_window's one real (still-tentative-at-file-
// scope) definitions are further down in the ring/smart-window sections -- this
// is the same forward-reference pattern the file already uses for
// s_last_eval/s_last_read (see the comment there): only one of the two
// tentative definitions may carry an initializer, and neither does, so this is
// legal C and not a redefinition.
static bool    s_ringing;
static Window *s_ring_window;
static Window *s_wait_window;

static AppTimer *s_idle_timer;
static bool      s_cfg_open;    // the phone's config page is open

static void idle_fire(void *data) {
  s_idle_timer = NULL;
  APP_LOG(APP_LOG_LEVEL_INFO, "IDLE exit");
  close_to_watchface();
}

static void idle_cancel(void) {
  if (s_idle_timer) {
    app_timer_cancel(s_idle_timer);
    s_idle_timer = NULL;
  }
}

static void idle_reset(void) {
  idle_cancel();
  // s_ringing/window_started_at are a cheap fast path (true for most callers,
  // since idle_reset is only ever called from the two safe windows or from an
  // inbox message) but are NOT sufficient on their own: over_cap clears
  // s_ringing while leaving the ring window up, and window_started_at is
  // already 0 throughout any ring (start_ring zeroes it on entry). The
  // window_stack_contains_window checks are what make "never while a ring or
  // smart-wait screen is showing" true in EVERY on-screen state, including the
  // post-over_cap "Alarm missed" screen and any future one -- reachable here via
  // the CfgOpen-close and IdleExitSec-changed paths in inbox_received, which run
  // regardless of what is on top of the window stack. And never while the
  // phone's config page is open -- that would make the config page close
  // itself under the user.
  if (s_cfg.idle_exit_sec == 0 || s_cfg_open
      || s_ringing || s_rs.window_started_at != 0
      || (s_ring_window && window_stack_contains_window(s_ring_window))
      || (s_wait_window && window_stack_contains_window(s_wait_window))) {
    return;
  }
  s_idle_timer = app_timer_register((uint32_t)s_cfg.idle_exit_sec * 1000,
                                    idle_fire, NULL);
}

// Window .appear/.disappear wrappers: armed only on the main menu and the
// alarm list (Step 2). Deliberately NOT added to the ring window or the smart
// waiting window -- pushing either of those already makes this window
// disappear, which cancels the timer for free (the same pattern the sibling
// apps use), and the ring/waiting screens must never time out on their own.
static void idle_win_appear(Window *w) { idle_reset(); }
static void idle_win_disappear(Window *w) { idle_cancel(); }

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

// --- The night palette -------------------------------------------------------
//
// The two screens a half-asleep person reads -- the smart window's waiting screen
// and the ring screen -- are drawn WHITE ON BLACK. They are looked at in the dark
// with the backlight on, where a full-white screen is a torch to the face; the
// menus keep the normal light look because they are read awake.
//
// Deliberately not configurable. A toggle would need an appended message key, a
// Config field and a CONFIG_VERSION bump (a trailing bool lands in existing
// padding, so sizeof does not change and the version is the only thing that would
// discard a stale blob) -- more machinery than the feature, and the option can be
// added later at exactly the same cost if anyone wants the light version back.
#define NIGHT_BG GColorBlack
#define NIGHT_FG GColorWhite

// A text layer on a night screen: transparent, so it inherits the window's black,
// and light text. Every layer on those two windows goes through this, so none can
// be forgotten into invisible black-on-black.
static void night_text_layer(TextLayer *tl) {
  text_layer_set_background_color(tl, GColorClear);
  text_layer_set_text_color(tl, NIGHT_FG);
}

// A MenuLayer section header. Not a row: it cannot be selected and costs no
// navigation. Two lines' worth of height so the phone hint wraps on a 144 px
// screen instead of being truncated.
#define HINT_HEADER_H 34

static int16_t hint_header_height(MenuLayer *ml, uint16_t section, void *ctx) {
  return HINT_HEADER_H;
}

static void draw_header_text(GContext *gctx, const Layer *cell, const char *text) {
  GRect b = layer_get_bounds(cell);
  graphics_context_set_text_color(gctx, GColorBlack);
  graphics_draw_text(gctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(4, -1, b.size.w - 8, b.size.h + 2),
      GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
}

// Where alarm times come from. Said on BOTH menus, always: the app used to say it
// only in the alarm list and only while there were no alarms, so the one sentence
// that explains the phone's role vanished the moment the app became useful.
static void draw_phone_hint_header(GContext *gctx, const Layer *cell,
                                   uint16_t section, void *ctx) {
  draw_header_text(gctx, cell, "Set times in the phone app");
}

// "13 h", "45 min", "2 d" -- BARE, with no "in " prefix, because the row composes
// three different sentences from it ("in 23 h", "skip, in 1 d", "off") and a
// baked-in prefix cannot be composed. Hand-rolled formatting; no float maths.
static void fmt_delta(time_t when, time_t now, char *out, size_t n) {
  if (when == 0) {
    if (n > 0) out[0] = '\0';
    return;
  }
  long d = (long)(when - now);
  if (d < 0) d = 0;
  if (d < 60 * 60) {
    snprintf(out, n, "%ld min", d / 60);
  } else if (d < 24 * 60 * 60) {
    snprintf(out, n, "%ld h", d / 3600);
  } else {
    snprintf(out, n, "%ld d", d / 86400);
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
  sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, time(NULL), s_ringing);
  refresh_list();
}

// --- The RunState alarm CYCLE: the single owner of the five fields that
// describe "the alarm we are currently dealing with".
//
// pending_slot, window_started_at, deadline_at, ring_started_at and snooze_count
// are one unit: they are meaningful only together, for the span from a smart
// window opening (or a deadline ringing) until the ring is dismissed. Four call
// sites used to set or clear four DIFFERENT subsets of them by hand, and that
// asymmetry -- not a single missing line -- is what produced the whole-branch
// review's Critical 1: deadline_at was written by two paths (start_ring and
// open_smart_window) and cleared by NONE, so after the very first ring it held
// yesterday's ring instant forever. The next night's WC_WINDOW then read a
// deadline ~24 h in the past, concluded the deadline had passed, and rang
// immediately AT WINDOW START -- up to 60 minutes early, with the smart window
// never opening again, and record_night reporting "smart alarm unavailable".
// The README's own "Test alarm in 2 min" validation step was enough to poison
// the state before the user's first real night.
//
// So: every mutation of the cycle goes through this pair. Nothing else may
// assign these five fields, except ring_snooze_now's deliberate move of
// ring_started_at/snooze_count WITHIN a live cycle (a snooze continues the
// cycle, it does not begin or end one).
static void runstate_end_cycle(void) {
  APP_LOG(APP_LOG_LEVEL_INFO,
          "CYCLE end (was slot=%d window=%lu deadline=%lu ring=%lu snooze=%d)",
          (int)s_rs.pending_slot, (unsigned long)s_rs.window_started_at,
          (unsigned long)s_rs.deadline_at, (unsigned long)s_rs.ring_started_at,
          s_rs.snooze_count);
  s_rs.pending_slot = -1;
  s_rs.window_started_at = 0;
  s_rs.deadline_at = 0;
  s_rs.ring_started_at = 0;
  s_rs.snooze_count = 0;
}

// Begin (or continue) the cycle for `slot`. `window_start` is 0 when no smart
// window is open (a ring has none). `deadline` is the hard alarm instant for
// this cycle. `fresh` distinguishes a brand-new cycle -- whose ring/snooze
// bookkeeping must start from zero -- from a continuation of the same cycle (a
// snooze expiry, or a mid-ring keep-alive relaunch), which must keep the
// original ring start so the escalation ramp resumes where it left off instead
// of quietly restarting at its gentlest stage.
static void runstate_begin_cycle(int slot, time_t window_start, time_t deadline,
                                 bool fresh) {
  s_rs.pending_slot = (int8_t)slot;
  s_rs.window_started_at = (uint32_t)window_start;
  s_rs.deadline_at = (uint32_t)deadline;
  if (fresh) {
    s_rs.ring_started_at = 0;
    s_rs.snooze_count = 0;
  }
  APP_LOG(APP_LOG_LEVEL_INFO,
          "CYCLE begin slot=%d window=%lu deadline=%lu fresh=%d",
          slot, (unsigned long)window_start, (unsigned long)deadline, (int)fresh);
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

// Clay's default auto-send can deliver a `select` (IdleExitSec is one) as a
// CString tuple rather than an int -- a plain tt->value->int32 read (as
// GET_INT does) would misread its bytes as garbage in that case. Type-tolerant,
// matching the sibling apps' idle_read_seconds(Tuple*) exactly: a hand-rolled
// digit loop, never atoi/strtol/strtod (not exported by the Core firmware --
// they link and run on host/emulator but hard-fault on real hardware).
static int idle_read_seconds(Tuple *t) {
  if (!t) { return -1; }
  if (t->type == TUPLE_CSTRING) {
    int v = 0;
    const char *p = t->value->cstring;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); }
    return v;
  }
  return (int)t->value->int32;
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  // Handled before every other key (Task 13): the phone's config-page open/close
  // signal must gate the idle timer regardless of what else is in this message.
  Tuple *co = dict_find(iter, MESSAGE_KEY_CfgOpen);
  if (co) {
    s_cfg_open = co->value->int32 != 0;
    APP_LOG(APP_LOG_LEVEL_INFO, "CfgOpen=%d", (int)s_cfg_open);
    if (s_cfg_open) {
      idle_cancel();
    } else {
      idle_reset();
    }
  }

  Tuple *t = dict_find(iter, MESSAGE_KEY_AlarmSet);
  if (t) {
    // A CString tuple is not guaranteed NUL-terminated at the buffer end; copy
    // into a bounded buffer and terminate it ourselves.
    static char buf[ALARMSET_STR_MAX];   // static: 160 B off a ~2 KB app stack
    size_t n = t->length < sizeof(buf) ? t->length : sizeof(buf) - 1;
    memcpy(buf, t->value->cstring, n);
    buf[n] = '\0';
    // Only act when the string actually differs from the one that produced the
    // alarms we already hold. The phone now resends its saved config on EVERY
    // launch (the config handshake), and dst_check launches the app around 03:00
    // every night, so an unconditional re-parse would revert the watch's own
    // SELECT on/off toggle, long-SELECT skip, and ring_stop_now's auto-disable of
    // a fired one-time alarm -- every night, silently, making the watch ring
    // after the user had turned the alarm off. See ac_apply_set_if_changed.
    if (!ac_apply_set_if_changed(buf, s_alarmset_str, s_alarms, &s_count, MAX_ALARMS)) {
      APP_LOG(APP_LOG_LEVEL_INFO,
              "CFG AlarmSet unchanged ('%s') -- no-op, keeping the watch's own "
              "on/off and skip flags", buf);
    } else {
      APP_LOG(APP_LOG_LEVEL_INFO, "CFG AlarmSet='%s' -> %d alarms", buf, s_count);
      // Record what was applied, so the next resend of the same string is a no-op.
      // snprintf, not strcpy: bounded, and both buffers are ALARMSET_STR_MAX.
      snprintf(s_alarmset_str, sizeof(s_alarmset_str), "%s", buf);
      as_save_alarmset_str(s_alarmset_str);
      reload_and_rearm();
    }
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
  GET_BOOL(EscRampVib, esc_ramp_vib);
  {
    Tuple *iet = dict_find(iter, MESSAGE_KEY_IdleExitSec);
    int isec = idle_read_seconds(iet);
    if (isec >= 0) {
      s_cfg.idle_exit_sec = (uint8_t)isec;
      changed = true;
      idle_reset();   // apply the new duration immediately, same as the sibling apps
    }
  }
  if (changed) {
    as_save_config(&s_cfg);   // esc_clamp runs inside as_save_config
    APP_LOG(APP_LOG_LEVEL_INFO, "CFG updated: smart=%d win=%d sens=%d prof=%d gest=%d",
            (int)s_cfg.smart_enabled, s_cfg.smart_window_min, s_cfg.sensitivity,
            s_cfg.wake_profile, s_cfg.stop_gesture);
    reload_and_rearm();
  }
}

// Outbox result handlers. These MUST be registered before anything is sent: on
// hardware the phone ACKs an outbound message and the SDK invokes the result
// callback, and a NULL callback jumps to a null address and faults. (The
// emulator never hits this -- there is no phone to ACK.)
static void outbox_sent(DictionaryIterator *iter, void *context) {}
static void outbox_failed(DictionaryIterator *iter, AppMessageResult r, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "outbox failed: %d", (int)r);
}

// Ask the phone for the config it last saved.
//
// THE ALARM TIMES LIVE ONLY ON THE PHONE and there is no on-watch editing by
// design, so config delivery has to be reliable -- and `webviewclosed` ->
// sendAppMessage, the only path there was, lands ONLY if this watchapp happened
// to be running at the moment the user hit Save. A watchapp normally is not.
// The user set 06:45, saw no error, and the watch went on ringing at the seeded
// demo 07:00: silent, and indistinguishable from working (the whole-branch
// review's Critical 2). So on every launch we ask, and the phone replies with
// the dict it persisted on its last Save (src/ts/config_sync.ts) -- exactly the
// handshake PebbleCountdownTimer already uses. The phone stays SILENT when
// nothing was ever saved, so the watch's own persisted state is never clobbered
// by an empty reply.
static void request_config(void) {
  DictionaryIterator *out;
  AppMessageResult r = app_message_outbox_begin(&out);
  if (r != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "CfgRequest: outbox_begin failed: %d", (int)r);
    return;
  }
  dict_write_uint8(out, MESSAGE_KEY_CfgRequest, 1);
  app_message_outbox_send();
  APP_LOG(APP_LOG_LEVEL_INFO, "CfgRequest sent");
}

static uint16_t list_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_count > 0 ? (uint16_t)s_count : 1;
}

static int16_t list_cell_height(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  return 44;   // two lines: time + weekday/next
}

// The next occurrence the SCHEDULER will actually arm -- i.e. skipping one that
// has already rung. The display and the scheduler must agree: an early smart
// wake leaves its own alarm time in the future, so a row reading "in 30 min" for
// a ring that is no longer going to happen is precisely how the double-ring
// defect looked from the wrist. Reuses the same pure, host-tested picker sc_rearm
// uses, over a one-element array so `slot` is 0 for the row being asked about.
static time_t next_armed_occurrence(int row) {
  if (row < 0 || row >= s_count) {
    return 0;
  }
  time_t now = time(NULL);
  int served = (s_rs.served_slot == (int8_t)row) ? 0 : -1;
  int32_t lead_s = (int32_t)(now - sc_ring_deadline(&s_cfg, now));
  time_t when = 0;
  ac_next_alarm_unserved(&s_alarms[row], 1, now, served, (time_t)s_rs.served_at,
                         lead_s, &when);
  return when;
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

  // Right-hand markers: missed, smart. NOT skip -- that is a STATE, and it is
  // spelled out in the subtitle now. A cryptic three-letter corner word is how the
  // skip feature managed to be invisible in the first place.
  char marks[12];
  marks[0] = '\0';
  snprintf(marks, sizeof(marks), "%s%s",
           s_rs.missed[ci->row] ? "missed " : "",
           (s_cfg.smart_enabled && PBL_IF_HEALTH_ELSE(1, 0)) ? "~" : "");
  graphics_draw_text(gctx, marks, fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GRect(4, 0, b.size.w - 8, 22), GTextOverflowModeFill, GTextAlignmentRight, NULL);

  // state[32]/sub[48], not smaller: "skip, in " plus a full-width delta is 25
  // bytes and gcc bounds it at that, so a tighter buffer is a real truncation
  // rather than a warning to silence.
  char days[12], delta[16], state[32], sub[48];
  fmt_weekdays(a->weekday_mask, days, sizeof(days));
  fmt_delta(next_armed_occurrence(ci->row), time(NULL), delta, sizeof(delta));
  // The state, in words, in ONE place. The relative time already accounts for a
  // pending skip (ac_next_occurrence honours skip_next), so the row used to be
  // correct and unexplained: the number jumped a day and only a corner marker
  // said why.
  if (!a->enabled) {
    snprintf(state, sizeof(state), "off");
  } else if (a->skip_next) {
    snprintf(state, sizeof(state), "skip, in %s", delta);
  } else {
    snprintf(state, sizeof(state), "in %s", delta);
  }
  snprintf(sub, sizeof(sub), "%s  %s", days, state);
  graphics_draw_text(gctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GRect(4, 20, b.size.w - 8, 22), GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

// --- The per-alarm action submenu ---------------------------------------------
//
// Opened by SELECT on an alarm row. It exists because two bare gestures (SELECT
// = on/off, long SELECT = skip next) were undiscoverable: nothing on screen
// mentioned either, and the first user pressed SELECT wanting the skip and
// switched the alarm off instead. Long SELECT is gone; this is the only route,
// and every row is written out.
static Window    *s_act_window;
static MenuLayer *s_act_menu;
// The alarm this submenu acts on, held by IDENTITY rather than row index: a phone
// config can arrive while the submenu is open and rebuild s_alarms
// (ac_apply_set_if_changed), after which the index refers to a different alarm.
// Same rule, same reason, as PebbleTuyaControl's find_light_by_id.
static uint16_t s_act_minute;
static uint8_t  s_act_mask;
static AcAction s_act_actions[AC_MAX_ACTIONS];
static int      s_act_count;

// The slot currently holding the alarm this submenu opened on, or -1 if it is
// gone. Never cached: resolving late is the entire point.
static int act_resolve(void) {
  for (int i = 0; i < s_count; i++) {
    if (s_alarms[i].minute_of_day == s_act_minute
        && s_alarms[i].weekday_mask == s_act_mask) {
      return i;
    }
  }
  return -1;
}

// "Mon 07:50" -- the occurrence NAMED, so "next" never has to be worked out.
// Three-letter ASCII weekday: Gothic has no typographic characters, and a
// localised name would need a font this app cannot rely on.
static void fmt_occurrence(time_t t, char *out, size_t n) {
  static const char *k_wday[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  if (t == 0) {
    if (n > 0) out[0] = '\0';
    return;
  }
  struct tm tm = *localtime(&t);
  int wd = (tm.tm_wday >= 0 && tm.tm_wday < 7) ? tm.tm_wday : 0;
  snprintf(out, n, "%s %02d:%02d", k_wday[wd], tm.tm_hour % 100, tm.tm_min % 100);
}

// The occurrence a skip would apply to, or that an undo would restore. `undo`
// asks with skip_next cleared, which is the occurrence currently being skipped.
// Both go through the same picker the list and the scheduler use, so the label
// can never name a ring that will not happen.
static time_t act_occurrence(int slot, bool undo) {
  if (slot < 0 || slot >= s_count) {
    return 0;
  }
  if (!undo) {
    return next_armed_occurrence(slot);
  }
  Alarm probe = s_alarms[slot];
  probe.skip_next = false;
  time_t now = time(NULL);
  int served = (s_rs.served_slot == (int8_t)slot) ? 0 : -1;
  time_t when = 0;
  ac_next_alarm_unserved(&probe, 1, now, served, (time_t)s_rs.served_at,
                         (int32_t)(now - sc_ring_deadline(&s_cfg, now)), &when);
  return when;
}

static uint16_t act_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return (uint16_t)(s_act_count > 0 ? s_act_count : 1);
}

static int16_t act_cell_height(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  return 36;
}

// Titled with the alarm it acts on, so the submenu can never be about "whichever
// row that was".
static void act_draw_header(GContext *gctx, const Layer *cell, uint16_t section,
                            void *ctx) {
  int slot = act_resolve();
  char title[28];
  if (slot < 0) {
    snprintf(title, sizeof(title), "Alarm gone");
  } else {
    char days[12];
    fmt_weekdays(s_alarms[slot].weekday_mask, days, sizeof(days));
    snprintf(title, sizeof(title), "%02u:%02u  %s",
             (unsigned)(s_act_minute / 60) % 100u,
             (unsigned)(s_act_minute % 60) % 100u, days);
  }
  draw_header_text(gctx, cell, title);
}

static void act_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  GRect b = layer_get_bounds(cell);
  graphics_context_set_text_color(gctx, menu_cell_layer_is_highlighted(cell)
                                            ? GColorWhite : GColorBlack);
  char label[28];
  int slot = act_resolve();
  if (slot < 0 || (int)ci->row >= s_act_count) {
    snprintf(label, sizeof(label), "Back");
  } else {
    char occ[16];
    switch (s_act_actions[ci->row]) {
      case AC_ACTION_SKIP_NEXT:
        fmt_occurrence(act_occurrence(slot, false), occ, sizeof(occ));
        snprintf(label, sizeof(label), "Skip %s", occ);
        break;
      case AC_ACTION_RING_NEXT:
        fmt_occurrence(act_occurrence(slot, true), occ, sizeof(occ));
        snprintf(label, sizeof(label), "Ring %s", occ);
        break;
      case AC_ACTION_TURN_OFF: snprintf(label, sizeof(label), "Turn off"); break;
      case AC_ACTION_TURN_ON:  snprintf(label, sizeof(label), "Turn on");  break;
    }
  }
  graphics_draw_text(gctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_24),
      GRect(6, 1, b.size.w - 12, b.size.h - 2),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void act_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  idle_reset();
  int slot = act_resolve();
  if (slot < 0 || (int)ci->row >= s_act_count) {
    // The alarm was reconfigured from the phone while this was open. Do nothing
    // rather than act on whatever slid into that index.
    APP_LOG(APP_LOG_LEVEL_WARNING, "action submenu: alarm gone, ignoring");
    window_stack_pop(true);
    return;
  }
  Alarm *a = &s_alarms[slot];
  switch (s_act_actions[ci->row]) {
    case AC_ACTION_SKIP_NEXT: a->skip_next = true;  break;
    case AC_ACTION_RING_NEXT: a->skip_next = false; break;
    case AC_ACTION_TURN_OFF:
      a->enabled = false;
      a->skip_next = false;   // an alarm that is off has nothing to skip
      break;
    case AC_ACTION_TURN_ON:   a->enabled = true;    break;
  }
  reload_and_rearm();
  window_stack_pop(true);   // back to the list, which now shows the new state
}

static void act_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_act_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_act_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = act_num_rows,
    .get_cell_height = act_cell_height,
    .get_header_height = hint_header_height,
    .draw_header = act_draw_header,
    .draw_row = act_draw_row,
    .select_click = act_select,
  });
  menu_layer_set_click_config_onto_window(s_act_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_act_menu));
}

static void act_window_unload(Window *w) {
  menu_layer_destroy(s_act_menu);
  s_act_menu = NULL;
}

static void open_alarm_actions(int row) {
  if (row < 0 || row >= s_count) {
    return;
  }
  s_act_minute = s_alarms[row].minute_of_day;
  s_act_mask = s_alarms[row].weekday_mask;
  s_act_count = ac_row_actions(&s_alarms[row], s_act_actions, AC_MAX_ACTIONS);
  if (!s_act_window) {
    s_act_window = window_create();
    // Its own idle handlers: pushing this window makes the LIST disappear, which
    // cancels the idle timer by this app's convention, so without them the app
    // could sit open on this screen indefinitely.
    window_set_window_handlers(s_act_window, (WindowHandlers){
      .load = act_window_load, .unload = act_window_unload,
      .appear = idle_win_appear, .disappear = idle_win_disappear,
    });
  }
  window_stack_push(s_act_window, true);
}

// SELECT opens the action submenu. There is deliberately no second gesture: long
// SELECT used to set skip-next and nothing on screen ever said so.
static void list_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  idle_reset();
  if (s_count == 0) return;
  open_alarm_actions(ci->row);
}

static void list_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_list_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_list_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = list_num_rows,
    .get_cell_height = list_cell_height,
    .get_header_height = hint_header_height,
    .draw_header = draw_phone_hint_header,
    .draw_row = list_draw_row,
    .select_click = list_select,
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
      .appear = idle_win_appear, .disappear = idle_win_disappear,
    });
  }
  window_stack_push(s_list_window, true);
}

// --- The "Last night" summary: a calibration tool, not a dismissible post-ring
// screen. Reachable only from the main menu, so there is no config toggle to
// disable it and no half-asleep screen the user learns to ignore. ---

static Window      *s_night_window;
static ScrollLayer *s_night_scroll;
static TextLayer   *s_night_text;
static char         s_night_buf[640];

static void fmt_hhmm(uint16_t minute_of_day, char *out, size_t n) {
  if (minute_of_day == NIGHT_NO_FIRE) {
    snprintf(out, n, "--:--");
  } else {
    snprintf(out, n, "%02d:%02d", minute_of_day / 60, minute_of_day % 60);
  }
}

// Appends a formatted chunk to s_night_buf at `off`, then clamps `off` so it
// can never run past sizeof(s_night_buf) even if NIGHT_HISTORY grows enough
// to make the worst case exceed the buffer -- today's worst case (~350 B
// against 640) can't reach it, but the bare `off += snprintf(...)`
// accumulation this replaces would otherwise silently walk s_night_buf + off
// out of bounds if it ever did. vsnprintf is blocked on this SDK
// (pebble_warn_unsupported_functions.h), so this stays a macro around
// snprintf rather than a va_list helper.
#define NIGHT_APPEND(...) do { \
    off += (size_t)snprintf(s_night_buf + off, sizeof(s_night_buf) - off, __VA_ARGS__); \
    if (off > sizeof(s_night_buf)) { off = sizeof(s_night_buf); } \
  } while (0)

static void build_night_text(void) {
  // static, not a local: NIGHT_HISTORY(7) * sizeof(NightSummary)(32) = 224
  // bytes against the ~2 KB app stack -- squarely the "couple hundred bytes"
  // class that produced App fault! PC:0 LR:0 on hardware in the sibling apps.
  // build_night_text runs from a single window-load callback (single-threaded
  // event loop), so a non-reentrant buffer is safe, same as as_push_night's.
  static NightSummary ns[NIGHT_HISTORY];
  int n = as_load_nights(ns, NIGHT_HISTORY);
  size_t off = 0;
  s_night_buf[0] = '\0';
  if (n == 0) {
    snprintf(s_night_buf, sizeof(s_night_buf),
             "No nights recorded yet.\n\nThe summary appears after the first "
             "alarm with the smart alarm on.");
    return;
  }
  const NightSummary *t = &ns[0];
  char onset[8], fired[8];
  fmt_hhmm(t->onset_min, onset, sizeof(onset));
  fmt_hhmm(t->fired_min, fired, sizeof(fired));

  if (t->smart_unavailable) {
    NIGHT_APPEND(
        "Last night\n\nSmart alarm was unavailable.\nCheck that activity "
        "tracking is on in the watch settings.\n");
  } else {
    // fired_min is ALWAYS the real ring instant (never NIGHT_NO_FIRE); which
    // label applies is carried separately in fired_by_deadline, so this never
    // renders as a label with a blank time.
    NIGHT_APPEND(
        "Last night\n\nAsleep from %s\nBaseline %u\nLevel %u\n%s %s\n",
        onset, t->baseline, t->trigger_level,
        t->fired_by_deadline ? "Deadline at" : "Woke you at", fired);
    NIGHT_APPEND("\nAt other sensitivities:\n");
    for (int k = 0; k < NIGHT_ALT_COUNT; k++) {
      char at[8];
      fmt_hhmm(t->alt_fired_min[k], at, sizeof(at));
      NIGHT_APPEND("  P%-2u  %s%s\n", t->alt_percentile[k], at,
          t->alt_percentile[k] == t->percentile ? "  <- in use" : "");
    }
  }
  if (n > 1) {
    NIGHT_APPEND("\nEarlier:\n");
    for (int i = 1; i < n; i++) {
      char at[8];
      fmt_hhmm(ns[i].fired_min, at, sizeof(at));
      NIGHT_APPEND("  %s  %s\n", at,
          ns[i].smart_unavailable ? "unavailable"
        : ns[i].fired_by_deadline ? "deadline" : "smart");
    }
  }
}

static void night_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  build_night_text();

  s_night_scroll = scroll_layer_create(b);
  scroll_layer_set_click_config_onto_window(s_night_scroll, w);
  // The default shadow dithers over the last visible line whenever there is
  // more content to scroll to (confirmed by a zoomed screenshot: it degraded
  // "P75 06:33" into a speckled mess) -- this screen's own text is the only
  // scroll affordance it needs.
  scroll_layer_set_shadow_hidden(s_night_scroll, true);

  GRect tb = GRect(4, 0, b.size.w - 8, 2000);
  s_night_text = text_layer_create(tb);
  text_layer_set_font(s_night_text, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_night_text, GTextOverflowModeWordWrap);
  text_layer_set_text(s_night_text, s_night_buf);
  GSize used = text_layer_get_content_size(s_night_text);
  text_layer_set_size(s_night_text, GSize(b.size.w - 8, used.h + 8));
  scroll_layer_set_content_size(s_night_scroll, GSize(b.size.w, used.h + 16));
  scroll_layer_add_child(s_night_scroll, text_layer_get_layer(s_night_text));
  layer_add_child(root, scroll_layer_get_layer(s_night_scroll));
}

static void night_window_unload(Window *w) {
  text_layer_destroy(s_night_text);
  scroll_layer_destroy(s_night_scroll);
}

static void open_last_night(void) {
  if (!s_night_window) {
    s_night_window = window_create();
    window_set_window_handlers(s_night_window, (WindowHandlers){
      .load = night_window_load, .unload = night_window_unload,
      // Scope addition beyond the brief (authorised by the controller, Task 13
      // review): this is a read-only screen exactly like the alarm list, so a
      // user who opens it and walks away must be returned to the watchface too
      // -- the brief only named the two windows it happened to introduce, not
      // every safe one that should get this.
      .appear = idle_win_appear, .disappear = idle_win_disappear,
    });
  }
  window_stack_push(s_night_window, true);
}

static uint16_t main_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return MAIN_ROW_COUNT;
}

static void main_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  switch (ci->row) {
    case MAIN_ROW_ALARMS: {
      char sub[24];
      time_t when = 0;
      time_t nw = time(NULL);
      int slot = ac_next_alarm_unserved(
          s_alarms, s_count, nw, (int)s_rs.served_slot, (time_t)s_rs.served_at,
          (int32_t)(nw - sc_ring_deadline(&s_cfg, nw)), &when);
      if (slot < 0) {
        snprintf(sub, sizeof(sub), "none set");
      } else {
        char delta[16];
        fmt_delta(when, time(NULL), delta, sizeof(delta));
        snprintf(sub, sizeof(sub), "next in %s", delta);
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
  idle_reset();
  switch (ci->row) {
    case MAIN_ROW_ALARMS:     open_alarm_list(); break;
    case MAIN_ROW_LAST_NIGHT: open_last_night(); break;
    // A terminal action, so it leaves for the watchface -- which doubles as the
    // only confirmation the row has ever given. Safe to exit immediately:
    // add_test_alarm ends in reload_and_rearm, which persists the alarms and
    // schedules the wakeup synchronously before returning. The alarm-list toggles
    // deliberately do NOT do this (the user may want to set two in a row, and idle
    // auto-exit already gets them out).
    case MAIN_ROW_TEST:       add_test_alarm(); close_to_watchface(); break;
    default: break;
  }
}

static void main_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_main_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_main_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = main_num_rows,
    .get_header_height = hint_header_height,
    .draw_header = draw_phone_hint_header,
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

// Play one escalation burst: `pulses` vibration pulses of `vib_ms`, an
// interaction-length backlight, and a short tone at `volume`.
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

  // Ask the system for its normal interaction backlight, rather than forcing the
  // light on for the burst's own length. Reported from the wrist 2026-07-31: a
  // burst-length hold (500 ms floor) reads as a strobe, not as the watch lighting
  // up. light_enable_interaction() holds it for the user's configured backlight
  // timeout and goes through the system's backlight logic, so the ambient-light
  // and backlight settings apply -- which light_enable(true) bypasses by forcing.
  // This deliberately reverses the earlier "seconds not minutes of backlight"
  // trade: over a long ring the light is now on much of the time. That is the
  // accepted cost of a light that behaves like every other light on the watch.
  if (s_cfg.light_pulse) {
    light_enable_interaction();
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

// Tentative declarations: start_ring (below) reads these, but their one real
// definition (with the comment explaining them) lives later in the file, in
// the smart-window section Task 11 built -- a plain static declaration is
// legal to repeat at file scope in C as long as only one carries an
// initializer, so this does not conflict with that later definition.
static SleepEvalResult s_last_eval;
static HistoryRead s_last_read;

// Forward declaration: start_ring calls this, but its definition (below) needs
// s_last_eval/s_last_read above -- placed here so record_night's real body,
// defined later once the smart-window helpers it also needs exist, can be
// called from start_ring regardless.
static void record_night(const SleepEvalResult *r, const HistoryRead *hr,
                         bool fired_by_deadline);

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
    // A ONE-TIME alarm has now had its one time, whether or not anyone dismissed
    // it. Only ring_stop_now used to switch it off, so an alarm that ran to the
    // cap stayed armed and rang again the next day -- found on the watch
    // 2026-08-01, where a 00:00 test alarm from the previous night was still
    // enabled and due to ring again. The `missed` marker is deliberately left
    // set: the user should still see that it went unanswered.
    if (s_rs.pending_slot >= 0 && s_rs.pending_slot < s_count
        && s_alarms[s_rs.pending_slot].weekday_mask == 0) {
      s_alarms[s_rs.pending_slot].enabled = false;
    }
    vibes_cancel();
#ifdef PBL_SPEAKER
    speaker_stop();
#endif
    // No backlight teardown: the interaction backlight is the system's to time out,
    // and forcing it off here would darken the "Alarm missed" screen the user is
    // meant to read.
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

  APP_LOG(APP_LOG_LEVEL_INFO, "RING t=%lu gap=%d vib=%d x%d vol=%d",
          (unsigned long)el, s.gap_s, s.vib_ms, s.pulses, s.volume);
  play_burst(&s);
  schedule_next_burst(s.gap_s);
}

static void stop_ring_output(void) {
  s_ringing = false;
  if (s_burst_timer) { app_timer_cancel(s_burst_timer); s_burst_timer = NULL; }
  if (s_hold_timer)  { app_timer_cancel(s_hold_timer);  s_hold_timer = NULL; }
  // Without this, pressing DOWN once (showing the hint) then UP within
  // HINT_SHOW_MS leaves hint_hide_cb pending against a TextLayer that
  // ring_window_unload may since have destroyed.
  if (s_hint_timer)  { app_timer_cancel(s_hint_timer);  s_hint_timer = NULL; }
  vibes_cancel();
#ifdef PBL_SPEAKER
  speaker_stop();
#endif
  // Nothing to undo for the backlight: it is the system's interaction light now,
  // and it times out on its own.
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
  // The cycle is over: clear all five cycle fields together (see
  // runstate_end_cycle). Clearing deadline_at here is what stops yesterday's
  // ring instant from making tonight's WC_WINDOW ring at window start.
  runstate_end_cycle();
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
  bool armed = sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, time(NULL), s_ringing);
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
  // Guarded for the same reason as update_ring_text (this task's review):
  // stop_ring_output always cancels this timer before any path pops the ring
  // window today, so it is not reachable stale in practice, but the ring
  // window is no longer the only one this app can pop without exiting, so
  // this stays consistent with that convention rather than relying on it.
  if (!s_ringing || !s_ring_progress) {
    return;
  }
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
  // NIGHT_FG, not black: this bar sits on the ring screen, which is black. It was
  // the one piece of drawing that does not go through a TextLayer, so it is also
  // the one that would silently vanish.
  graphics_context_set_fill_color(gctx, NIGHT_FG);
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
  window_set_background_color(w, NIGHT_BG);

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
  // 55/100, not 6/10: at 6/10 sub_h works out to 23 px for a GOTHIC_24_BOLD line
  // box, one row short, so "(muted)" (permanent on aplite/basalt/diorite -- no
  // PBL_SPEAKER, so sound_available() is always false there) lost the bottom
  // curl of its parentheses. Verified by re-shooting a zoomed diorite screenshot.
  int time_h  = mid_h * 55 / 100;

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
    time_h  = mid_h * 55 / 100;
    time_font = ring_time_font(b.size.w, time_h, &time_sz);
  }
  int sub_h = mid_h - time_h;

  s_ring_up = text_layer_create(GRect(0, up_y, b.size.w - 6, btn_h));
  text_layer_set_font(s_ring_up, btn_font);
  text_layer_set_text_alignment(s_ring_up, GTextAlignmentRight);
  text_layer_set_text(s_ring_up, "Snooze");
  night_text_layer(s_ring_up);
  layer_add_child(root, text_layer_get_layer(s_ring_up));

  // "Stop" is constant regardless of s_cfg.stop_gesture -- at either label
  // size, "2x = Stop" would not fit as well as "Stop" alone, and which button
  // stops the alarm is all this label needs to say; the hint (multi-tap) or
  // the progress bar (long-press) teaches the gesture itself.
  s_ring_down = text_layer_create(GRect(0, down_y, b.size.w - 6, btn_h));
  text_layer_set_font(s_ring_down, btn_font);
  text_layer_set_text_alignment(s_ring_down, GTextAlignmentRight);
  text_layer_set_text(s_ring_down, "Stop");
  night_text_layer(s_ring_down);
  layer_add_child(root, text_layer_get_layer(s_ring_down));

  // Time + subtitle share the band left between the two button labels, split
  // proportionally (60/40) -- verified by screenshot on both boards rather
  // than assumed (see the task report).
  s_ring_time = text_layer_create(GRect(0, mid_top, b.size.w, time_h));
  text_layer_set_font(s_ring_time, time_font);
  text_layer_set_text_alignment(s_ring_time, GTextAlignmentCenter);
  night_text_layer(s_ring_time);
  layer_add_child(root, text_layer_get_layer(s_ring_time));

  s_ring_sub = text_layer_create(GRect(0, mid_top + time_h, b.size.w, sub_h));
  text_layer_set_font(s_ring_sub, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_ring_sub, GTextAlignmentCenter);
  night_text_layer(s_ring_sub);
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
  night_text_layer(s_ring_hint);
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
  time_t now = time(NULL);
  // A fresh deadline always means a fresh ring (`fresh` below), which resets the
  // ring/snooze bookkeeping unconditionally rather than probing
  // snooze_count/ring_started_at for "does this look fresh" -- that check is
  // exactly what let a day-old snooze_count and ring_started_at survive an
  // over_cap miss and poison every later alarm (elapsed = 86400+ s -> immediate
  // over_cap, forever, silently).
  //
  // The cycle's hard alarm instant: "now" for a genuinely fresh deadline ring
  // (accurate to within sc_schedule's +/-2 min E_RANGE shift), or the deadline
  // this cycle already carries for a snooze/keep-alive continuation and for an
  // early smart wake -- where the window it came from set it and it must not be
  // overwritten with "now" (that would report an early wake as on-time).
  time_t deadline = from_deadline ? now : (time_t)s_rs.deadline_at;
  // A ring has no open smart window any more, hence window_start 0.
  runstate_begin_cycle(slot, 0, deadline, from_deadline);
  // Only stamp "now" when there is genuinely nothing to resume: a snooze
  // expiry (ring_started_at already holds it) or a mid-ring keep-alive
  // relaunch (ring_started_at already holds the original start) must NOT be
  // overwritten here, or resuming would restart the ramp at t=0 and quietly
  // downgrade a long-running alarm back to its gentlest stage.
  if (s_rs.ring_started_at == 0) {
    s_rs.ring_started_at = (uint32_t)now;
  }
  // THIS OCCURRENCE HAS NOW RUNG -- recorded here, at the ring, rather than at
  // any of the places a ring ENDS, because all three of those (Stop, the
  // missed-alarm cap, snooze-then-stop) must count as served and only this one
  // place is common to them. Nothing clears it: an early smart wake ends the
  // ring while its own alarm time is still in the future, so this has to outlive
  // the cycle or every later re-arm (Stop, a phone config save, the 03:00 DST
  // check, an ordinary launch) picks the same occurrence again -- which is
  // exactly how a 07:50 alarm rang at 07:20 and then again at 07:50.
  s_rs.served_slot = (int8_t)slot;
  s_rs.served_at = (uint32_t)deadline;
  as_save_runstate(&s_rs);

  // Record the night ONCE, on the first ring (never on a snooze resumption --
  // that would overwrite tonight's record with the same data read again, or
  // worse, with a stale re-evaluation). s_last_eval/s_last_read hold whatever
  // the last smart evaluation saw (Task 11); for a plain (non-smart) alarm
  // they are still the fresh-launch zero defaults, so s_last_read.available
  // is false and NULL is passed -- which is what makes ns.smart_unavailable
  // true for a plain alarm's summary.
  if (s_rs.snooze_count == 0) {
    record_night(&s_last_eval, s_last_read.available ? &s_last_read : NULL, from_deadline);
  }

  // Keep a wakeup live for the whole ring: if anything kills the app mid-ring
  // (another app's wakeup, a phone-initiated launch), the alarm comes back
  // instead of being lost.
  sc_schedule(now + SC_REENTRY_GAP_S, WC_SNOOZE);

  APP_LOG(APP_LOG_LEVEL_INFO,
          "RING start slot=%d deadline=%d sound=%d snooze=%d deadline_at=%lu",
          slot, (int)from_deadline, (int)sound_available(), s_rs.snooze_count,
          (unsigned long)s_rs.deadline_at);

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

// --- The smart window: minute-history reads, the waiting screen, the 1-min poll. ---

static Window    *s_wait_window;
static TextLayer *s_wait_time;
static TextLayer *s_wait_sub;
static AppTimer  *s_poll_timer;
// static, not a local: 720 * 4 B does not fit the ~2 KB app stack.
//
// ... but only where those 720 minutes can ever arrive. On a platform with no
// Health API hr_read_night has no implementation at all (health_read.c is one big
// PBL_IF_HEALTH_ELSE) and always reports "unavailable", and sc_window_start
// collapses the smart window onto the deadline -- so the full-night buffer would
// be 2.8 KB of permanently untouched static RAM. aplite has 24 KB of app RAM and
// already spends ~22 KB of it on the static footprint, so that 2.8 KB is the
// difference between working and not: without this, the ~900 bytes the config
// handshake added took aplite's free heap under 640 B and app_message_open began
// returning APP_MSG_OUT_OF_MEMORY -- no messaging at all, i.e. the alarms could
// not be configured from the phone. Verified on the aplite emulator both ways.
#if PBL_IF_HEALTH_ELSE(1, 0)
#define S_NIGHT_LEN SE_MAX_SAMPLES
#else
#define S_NIGHT_LEN 1
#endif
static SleepMinute s_night[S_NIGHT_LEN];

static void wait_window_update(void) {
  // Guarded for the same reason as update_ring_text: a timer callback (the
  // minute tick, or the poll's own reschedule) must not outlive the window
  // that owns these layers.
  if (!s_wait_time || !s_wait_sub) {
    return;
  }
  static char t[8];
  static char sub[64];
  clock_copy_time_string(t, sizeof(t));
  text_layer_set_text(s_wait_time, t);

  time_t deadline = (time_t)s_rs.deadline_at;
  struct tm *dtm = localtime(&deadline);
  if (s_rs.smart_unavailable) {
    snprintf(sub, sizeof(sub), "Alarm %02d:%02d\nSmart alarm unavailable",
             dtm->tm_hour, dtm->tm_min);
  } else {
    snprintf(sub, sizeof(sub), "Alarm %02d:%02d\nWaiting for light sleep",
             dtm->tm_hour, dtm->tm_min);
  }
  text_layer_set_text(s_wait_sub, sub);
}

static void wait_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  window_set_background_color(w, NIGHT_BG);

  s_wait_time = text_layer_create(GRect(0, b.size.h / 2 - 52, b.size.w, 44));
  text_layer_set_font(s_wait_time, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_wait_time, GTextAlignmentCenter);
  night_text_layer(s_wait_time);
  layer_add_child(root, text_layer_get_layer(s_wait_time));

  s_wait_sub = text_layer_create(GRect(4, b.size.h / 2, b.size.w - 8, 60));
  text_layer_set_font(s_wait_sub, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_wait_sub, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_wait_sub, GTextOverflowModeWordWrap);
  night_text_layer(s_wait_sub);
  layer_add_child(root, text_layer_get_layer(s_wait_sub));

  wait_window_update();
}

static void wait_window_unload(Window *w) {
  text_layer_destroy(s_wait_sub);
  text_layer_destroy(s_wait_time);
  // NULL both out (carried fix from Task 11's review, mirroring
  // ring_window_unload): this task is what makes it possible to pop the wait
  // window without exiting the app (the out-of-range branch in
  // handle_wakeup_cookie can now do so), so a timer racing an already-unloaded
  // window must not dereference a stale pointer.
  s_wait_sub = NULL;
  s_wait_time = NULL;
}

static void wait_noop(ClickRecognizerRef rec, void *ctx) {}

static void wait_click_config(void *ctx) {
  // BACK must not dismiss the window: doing so would abandon the smart window and
  // leave only the deadline. Same reasoning as on the ring screen.
  window_single_click_subscribe(BUTTON_ID_BACK, wait_noop);
}

static void wait_minute_tick(struct tm *t, TimeUnits units) {
  wait_window_update();
}

static void poll_cb(void *data);

// The most recent evaluation and history read. Task 12's record_night describes
// the night from these instead of re-reading history at ring time, so they are
// updated on EVERY evaluation — including the unavailable path, where
// s_last_read.available stays false and the summary reports "no data".
static SleepEvalResult s_last_eval;
static HistoryRead s_last_read;

// Evaluate the window once. Returns true when the alarm should ring now.
static bool smart_should_ring(SleepEvalResult *out) {
  uint8_t pct = 90, mins = 2;
  as_effective_sens(&s_cfg, &pct, &mins);
  SleepEvalCfg sc;
  se_default_cfg(&sc, pct, mins);

  // S_NIGHT_LEN, not SE_MAX_SAMPLES: the buffer is collapsed on platforms with no
  // Health API (see its definition), and the length passed must match the buffer.
  HistoryRead hr = hr_read_night(s_night, S_NIGHT_LEN,
                                 (time_t)s_rs.window_started_at);
  s_last_read = hr;
  if (!hr.available || hr.window_start < 0) {
    s_rs.smart_unavailable = true;
    as_save_runstate(&s_rs);
    APP_LOG(APP_LOG_LEVEL_WARNING, "SMART unavailable (count=%d)", hr.count);
    return false;
  }
  s_rs.smart_unavailable = false;

  SleepEvalResult r = se_evaluate(s_night, hr.count, hr.window_start,
                                  hr.is_restful, &sc);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "SMART n=%d win=%d base=%u level=%u acc=%lu restful=%d fire=%d",
          hr.count, hr.window_start, r.baseline, r.trigger_level,
          (unsigned long)r.acc, (int)hr.is_restful, (int)r.fire);
  s_last_eval = r;
  if (out) {
    *out = r;
  }
  return r.fire;
}

#define NIGHT_ALT_PCTS { 95, 90, 82, 75 }

static uint16_t prv_minute_of_day(time_t t) {
  struct tm *tm = localtime(&t);
  return (uint16_t)(tm->tm_hour * 60 + tm->tm_min);
}

// Forward declaration: record_night calls this, and gcc 14 makes an implicit
// function declaration a hard error, so it must be visible before the call.
static long prv_tz_offset(time_t t);

static void record_night(const SleepEvalResult *r, const HistoryRead *hr,
                         bool fired_by_deadline) {
  NightSummary ns;
  memset(&ns, 0, sizeof(ns));
  time_t now = time(NULL);
  ns.day_local = (uint32_t)((now + prv_tz_offset(now)) / 86400);
  ns.smart_unavailable = (hr == NULL || !hr->available) ? 1 : 0;
  ns.fired_by_deadline = fired_by_deadline ? 1 : 0;
  // The real instant the ring started, regardless of how it got there -- the
  // deadline-versus-early distinction is carried separately in
  // fired_by_deadline above, so this is never NIGHT_NO_FIRE.
  ns.fired_min = prv_minute_of_day(now);

  uint8_t pct = 90, mins = 2;
  as_effective_sens(&s_cfg, &pct, &mins);
  ns.percentile = pct;

  if (hr != NULL && hr->available) {
    ns.onset_min = prv_minute_of_day(hr->first_utc);
    ns.baseline = r ? r->baseline : 0;
    ns.trigger_level = r ? r->trigger_level : 0;
    ns.acc_at_fire = r ? r->acc : 0;

    // What the other sensitivities would have done to this same night.
    static const uint8_t k_alt[NIGHT_ALT_COUNT] = NIGHT_ALT_PCTS;
    for (int k = 0; k < NIGHT_ALT_COUNT; k++) {
      SleepEvalCfg c;
      se_default_cfg(&c, k_alt[k], mins);
      SleepEvalResult ar = se_evaluate(s_night, hr->count, hr->window_start,
                                       false /* ignore the veto for the what-if */, &c);
      ns.alt_percentile[k] = k_alt[k];
      ns.alt_fired_min[k] = ar.fire
          ? prv_minute_of_day(hr->first_utc + (time_t)ar.fired_index * SECONDS_PER_MINUTE)
          : NIGHT_NO_FIRE;
    }
  } else {
    ns.onset_min = NIGHT_NO_FIRE;
    for (int k = 0; k < NIGHT_ALT_COUNT; k++) {
      ns.alt_fired_min[k] = NIGHT_NO_FIRE;
    }
  }
  as_push_night(&ns);
  APP_LOG(APP_LOG_LEVEL_INFO, "NIGHT recorded base=%u level=%u fired=%d deadline=%d",
          ns.baseline, ns.trigger_level, ns.fired_min, (int)ns.fired_by_deadline);
}

// Converts UTC to the local day number without depending on timegm.
static long prv_tz_offset(time_t t) {
  struct tm lt = *localtime(&t);
  struct tm gt = *gmtime(&t);
  long dh = (lt.tm_hour - gt.tm_hour) * 3600L + (lt.tm_min - gt.tm_min) * 60L;
  int dd = lt.tm_mday - gt.tm_mday;
  if (dd == 1 || dd < -1) dh += 86400L;
  else if (dd == -1 || dd > 1) dh -= 86400L;
  return dh;
}

static void poll_cb(void *data) {
  s_poll_timer = NULL;
  if (s_rs.window_started_at == 0 || s_ringing) {
    return;
  }
  time_t now = time(NULL);
  if (s_rs.deadline_at != 0 && now >= (time_t)s_rs.deadline_at) {
    APP_LOG(APP_LOG_LEVEL_INFO, "SMART deadline reached");
    start_ring(s_rs.pending_slot, true);
    return;
  }
  SleepEvalResult r;
  if (smart_should_ring(&r)) {
    APP_LOG(APP_LOG_LEVEL_INFO, "SMART firing early at acc=%lu", (unsigned long)r.acc);
    start_ring(s_rs.pending_slot, false);
    return;
  }
  wait_window_update();
  s_poll_timer = app_timer_register(60 * 1000, poll_cb, NULL);
}

static void open_smart_window(int slot, time_t window_start, time_t deadline) {
  // A PENDING SNOOZE OWNS THE CYCLE -- never open a window over it (the
  // whole-branch review's Important 4). Beginning a new cycle here zeroes
  // ring_started_at and snooze_count, which together are the ONLY record of
  // that snooze, while its already-scheduled WC_SNOOZE wakeup stays live and
  // uncancelled. The expiry then lands in start_ring(pending_slot, false) with
  // ring_started_at == 0 -- and pending_slot is now the newly tracked alarm, so
  // the WRONG alarm rings, snooze_min after the snooze rather than at its own
  // time. Two alarms 30 minutes apart do it: snooze the 07:00, and the 07:30
  // alarm's window opens at 07:00, so the 07:30 alarm rings at 07:10.
  //
  // The condition is exactly sc_rearm's own definition of "a snooze wakeup is
  // pending" (ring_started_at holds the snooze EXPIRY while a snooze is in
  // flight -- see ring_snooze_now), so the two cannot disagree. sc_rearm keeps
  // the wakeup chain alive: it re-places this snooze at priority 2 plus every
  // alarm's own deadline, which is what the window branch would otherwise have
  // relied on this call to do.
  if (s_rs.snooze_count > 0 && s_rs.ring_started_at != 0
      && (time_t)s_rs.ring_started_at > time(NULL)) {
    APP_LOG(APP_LOG_LEVEL_INFO,
            "SMART window for slot=%d declined: snooze #%d pending until %lu",
            slot, s_rs.snooze_count, (unsigned long)s_rs.ring_started_at);
    sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, time(NULL), s_ringing);
    return;
  }

  // A window opening begins a fresh cycle: all five cycle fields, in one place
  // (see runstate_begin_cycle).
  runstate_begin_cycle(slot, window_start, deadline, true);
  as_save_runstate(&s_rs);

  APP_LOG(APP_LOG_LEVEL_INFO, "SMART window open slot=%d until %lu",
          slot, (unsigned long)deadline);

  if (!s_wait_window) {
    s_wait_window = window_create();
    window_set_window_handlers(s_wait_window, (WindowHandlers){
      .load = wait_window_load, .unload = wait_window_unload,
    });
    window_set_click_config_provider(s_wait_window, wait_click_config);
  }
  if (!window_stack_contains_window(s_wait_window)) {
    window_stack_push(s_wait_window, true);
  }
  tick_timer_service_subscribe(MINUTE_UNIT, wait_minute_tick);

  // Keep the rolling re-entry wakeup alive: if anything kills this app during the
  // window, the next re-entry relaunches it and monitoring resumes intact, because
  // the baseline comes from minute history rather than from our RAM.
  sc_arm_reentry(time(NULL));

  if (s_poll_timer) {
    app_timer_cancel(s_poll_timer);
  }
  poll_cb(NULL);   // evaluate immediately
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
        //
        // This ends the cycle, so it clears all five cycle fields through the
        // one owner (runstate_end_cycle) rather than picking a subset by hand:
        // window_started_at was the subset miss found in Task 11's review, and
        // deadline_at was the subset miss the whole-branch review found
        // (Critical 1) -- a stale deadline_at left here made the NEXT night's
        // WC_WINDOW ring at window start, up to 60 minutes early.
        runstate_end_cycle();
        as_save_runstate(&s_rs);
        // If the waiting screen happens to be up for this now-invalid window,
        // don't leave it stranded on screen: with window_started_at cleared,
        // poll_cb's own next tick would just return early forever (its guard
        // checks that field first) and the BACK button is deliberately a
        // no-op there, so nothing would ever bring the user back to the
        // watchface. s_ringing is guaranteed false here (checked at the top of
        // this case), so this is safe.
        if (s_poll_timer) {
          app_timer_cancel(s_poll_timer);
          s_poll_timer = NULL;
        }
        if (s_wait_window && window_stack_contains_window(s_wait_window)) {
          close_to_watchface();
        }
        sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, now, s_ringing);
      }
      break;
    }
    case WC_WINDOW:
    case WC_REENTRY: {
      if (s_ringing) {
        break;
      }
      time_t when = 0;
      int slot = s_rs.pending_slot;
      if (slot >= 0 && slot >= s_count) {
        // A stale pending_slot: the phone deleted/reconfigured alarms while
        // this window was open (the same class of bound check the
        // WC_DEADLINE/WC_SNOOZE case above already applies to its own
        // pending_slot read). Fall back to whatever is next now rather than
        // reading a slot that no longer represents the tracked alarm.
        slot = -1;
      }
      if (slot < 0) {
        slot = ac_next_alarm(s_alarms, s_count, now - 60, &when);
      } else {
        when = ac_next_occurrence(&s_alarms[slot], now - 60);
      }
      if (slot < 0) {
        sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, now, s_ringing);
        break;
      }
      // NEVER re-open a window for an occurrence that has already rung. sc_rearm
      // no longer arms one (it picks with ac_next_alarm_unserved), so this is
      // unreachable today; it is the second line of defence for the same reason
      // the deadline guard below is one -- a future path that forgets must not be
      // able to resurrect the double ring, which cost the user a 07:20 wake AND a
      // 07:50 one on the same morning.
      if (ac_is_served(when, slot, (int)s_rs.served_slot, (time_t)s_rs.served_at,
                       (int32_t)(now - sc_ring_deadline(&s_cfg, now)))) {
        APP_LOG(APP_LOG_LEVEL_WARNING,
                "WINDOW wakeup for slot %d ignored: occurrence %lu already rang",
                slot, (unsigned long)when);
        sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, now, s_ringing);
        break;
      }
      // Trust the STORED deadline only while the cycle it belongs to is
      // actually live -- window_started_at != 0 is what makes it live. Without
      // that conjunct a deadline_at left behind by a finished cycle (the
      // whole-branch review's Critical 1) reads as "the deadline passed ~24 h
      // ago" and rings instantly at window start, up to 60 minutes early, every
      // night from the second onwards. runstate_end_cycle now clears the field
      // at both sites that end a cycle; this guard is the second line of
      // defence, so a future path that forgets cannot resurrect the bug.
      // Deriving the deadline from `when` instead is always correct here -- it
      // is the same computation sc_rearm used to place this very wakeup.
      time_t ring = (s_rs.window_started_at != 0 && s_rs.deadline_at != 0)
                        ? (time_t)s_rs.deadline_at
                        : sc_ring_deadline(&s_cfg, when);
      APP_LOG(APP_LOG_LEVEL_INFO,
              "WINDOW wakeup slot=%d now=%lu ring=%lu (stored window=%lu deadline=%lu)",
              slot, (unsigned long)now, (unsigned long)ring,
              (unsigned long)s_rs.window_started_at, (unsigned long)s_rs.deadline_at);
      if (now >= ring) {
        start_ring(slot, true);
        break;
      }
      bool smart_on = s_cfg.smart_enabled && PBL_IF_HEALTH_ELSE(true, false);
      if (!smart_on) {
        sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, now, s_ringing);
        break;
      }
      time_t win = s_rs.window_started_at != 0 ? (time_t)s_rs.window_started_at
                                               : sc_window_start(&s_cfg, when);
      open_smart_window(slot, win, ring);
      break;
    }
    case WC_DST:
      sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, now, s_ringing);
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
  // Must be loaded before app_message_open/request_config below, since the phone's
  // reply can arrive as soon as the outbox flushes and this is what makes an
  // unchanged reply a no-op. Left "" on a fresh install (and on an install that
  // predates this key), so the phone's first AlarmSet always applies and can
  // replace the seeded demo set.
  as_load_alarmset_str(s_alarmset_str, sizeof(s_alarmset_str));

  // First-run seed only: a reasonable demo alarm set for a fresh install with
  // nothing stored yet. The phone overwrites this on its first config save.
  if (s_count == 0) {
    s_count = ac_parse_set("07:00|1111100;08:30|0000011;-06:15|1111111", s_alarms, MAX_ALARMS);
    as_save_alarms(s_alarms, s_count);
  }

  // Drop spent one-time alarms -- a one-time alarm switched off has no future, and
  // nothing could delete the row: the watch has no delete, and the phone cannot
  // remove a slot the watch's own Test alarm created without telling it (its next
  // config is byte-identical to the last applied one, so ac_apply_set_if_changed
  // rightly no-ops). Two such rows were stuck on the real watch (2026-08-01).
  //
  // AFTER the first-run seed above, never before: pruning an array down to empty
  // would otherwise look like a fresh install and re-seed the demo alarms. The
  // RunState slot references are remapped by the same call, because they are
  // indices into the array being compacted.
  {
    int before = s_count;
    s_count = ac_prune_spent_one_time(s_alarms, s_count, s_rs.missed,
                                      &s_rs.pending_slot, &s_rs.served_slot);
    if (s_count != before) {
      APP_LOG(APP_LOG_LEVEL_INFO, "pruned %d spent one-time alarm(s)",
              before - s_count);
      as_save_alarms(s_alarms, s_count);
      as_save_runstate(&s_rs);
    }
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

  if (!sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, time(NULL), s_ringing)) {
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
    .appear = idle_win_appear, .disappear = idle_win_disappear,
  });
  window_stack_push(s_main_window, true);

  if (launched_by_wakeup) {
    handle_wakeup_cookie(launch_cookie);
  }

  app_message_register_inbox_received(inbox_received);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  // Ask for the largest buffers the platform can actually afford, rather than a
  // hand-counted fixed size. The whole-branch review measured the worst-case
  // config dict at 414 B against the old fixed 512 B inbox -- it fit, with ~98 B
  // of headroom, and appending ONE key eats into that. An inbox overflow is not
  // a partial read: the ENTIRE message is dropped as a NACK the watch never
  // sees, alarm times included, which is the same silent-loss class as the
  // missing config handshake above.
  //
  // NOT an unconditional app_message_open(inbox_size_maximum(),
  // outbox_size_maximum()): those maxima are ~8 KB EACH and come out of the app
  // heap (the firmware logs exactly that), and aplite's 24 KB app RAM leaves
  // this app about 2.1 KB of heap in total -- the build's own memory report says
  // so. There, asking for the maxima returns APP_MSG_OUT_OF_MEMORY and the app
  // ends up with NO messaging at all: no config, no alarm times, nothing. That
  // would be a worse failure than the tight inbox this is fixing. So the request
  // is capped to what is free, keeping a reserve for the runtime allocations a
  // ring/menu still has to make, and it never drops below the 512 B this app
  // shipped with -- aplite therefore keeps exactly its current behaviour while
  // every roomier platform gets the system maximum.
  uint32_t in_size = app_message_inbox_size_maximum();
  uint32_t out_size = app_message_outbox_size_maximum();
  const uint32_t k_heap_reserve = 1536;
  uint32_t heap = (uint32_t)heap_bytes_free();
  if (in_size + out_size + k_heap_reserve > heap) {
    out_size = 128;   // the only thing this app ever sends is a 1-byte request
    uint32_t avail = heap > k_heap_reserve + out_size
                         ? heap - k_heap_reserve - out_size : 0;
    in_size = avail > 2048 ? 2048 : avail;
    if (in_size < 512) {
      in_size = 512;
    }
  }
  AppMessageResult amr = app_message_open(in_size, out_size);
  APP_LOG(amr == APP_MSG_OK ? APP_LOG_LEVEL_INFO : APP_LOG_LEVEL_ERROR,
          "app_message_open(in=%u out=%u) = %d (heap was %u)",
          (unsigned)in_size, (unsigned)out_size, (int)amr, (unsigned)heap);
  if (amr != APP_MSG_OK) {
    // heap_bytes_free() is a byte COUNT, not a promise that those bytes are
    // contiguous, so the large request above can still fail on fragmentation --
    // in a place where the old fixed 512/128 would have succeeded. Falling back to
    // exactly what this app shipped with means the sizing change can never be the
    // reason messaging is unavailable. Both outcomes are logged, because a silent
    // failure here means no config and no alarm times at all.
    amr = app_message_open(512, 128);
    APP_LOG(amr == APP_MSG_OK ? APP_LOG_LEVEL_WARNING : APP_LOG_LEVEL_ERROR,
            "app_message_open fallback(in=512 out=128) = %d", (int)amr);
  }
  // After open (the outbox does not exist before it) and after the handlers are
  // registered above.
  request_config();

  // TEMPORARY (2026-08-01): dump the past night to `pebble logs` so the
  // "rang 30 minutes early" report can be read off the watch's own recorded
  // data instead of guessed at. Compiled out by SA_DEBUG_DUMP = 0, and it
  // declines to run while a smart window or ring is live -- it borrows s_night.
  dbg_dump(s_alarms, s_count, &s_cfg, &s_rs, s_night, S_NIGHT_LEN);

  app_event_loop();

  as_save_alarms(s_alarms, s_count);
  as_save_runstate(&s_rs);
  if (s_list_window) window_destroy(s_list_window);
  if (s_ring_window) window_destroy(s_ring_window);
  if (s_night_window) window_destroy(s_night_window);
  if (s_act_window) window_destroy(s_act_window);
  window_destroy(s_main_window);
  return 0;
}
