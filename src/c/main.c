// SPDX-License-Identifier: GPL-3.0-only
#include <pebble.h>
#include <string.h>
#include "alarm_calc.h"
#include "alarm_store.h"
#include "debug_dump.h"
#include "escalation.h"
#include "health_read.h"
#include "night_text.h"
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

// The main menu's rows are CONDITIONAL now (the ongoing-alarm row exists only
// while something is ongoing), so a fixed index per row would drift the moment
// a row appears. One table, built in one place, is the only thing that knows
// the layout: get_num_rows, draw_row and select_click all read it, so an index
// can never mean two different rows.
typedef enum {
  MAIN_ROW_ALARMS = 0,
  MAIN_ROW_ONGOING,
  MAIN_ROW_LAST_NIGHT,
  MAIN_ROW_TEST,
  MAIN_ROW_DUMP,
} MainRowKind;

#define MAIN_ROW_MAX 5
static MainRowKind s_main_row_kinds[MAIN_ROW_MAX];

// NOTE -- NO WINDOW ANIMATIONS ANYWHERE IN THIS APP.
//
// Every window_stack_push/pop below passes `animated = false`. Reported from the
// wrist 2026-08-01: every menu transition showed "joku outo välähdys
// (jonkinlainen rikkinäinen animaatio)" -- a flash, on all of them, not just the
// newest window. The slide is worth nothing here (this UI is a two-level menu
// glanced at for seconds) and it is the only thing capable of producing that
// artifact, so it goes rather than being chased. The ring and waiting screens in
// particular must appear INSTANTLY: they are the ones that matter at 07:20.
//
// Do not "restore" the animation for polish -- it was removed on evidence from
// real hardware, which the emulator cannot reproduce.
static void close_to_watchface(void) {
  // Land on the watchface rather than the launcher: the default
  // APP_EXIT_NOT_SPECIFIED returns to wherever the app was launched from.
  exit_reason_set(APP_EXIT_ACTION_PERFORMED_SUCCESSFULLY);
  window_stack_pop_all(false);
}

// A WAKEUP LAUNCH THAT PUTS NOTHING ON SCREEN MUST NOT LEAVE THE APP SITTING ON
// THE MAIN MENU.
//
// WC_DST fires at 03:00 EVERY night purely to re-arm the wakeups after a
// possible clock shift, and its handler is a bare sc_rearm(). The launch it
// rides in on used to be ended by the idle auto-exit, which was removed on
// 2026-08-01 -- and nothing replaced it. Reported from the wrist the very next
// morning: at 07:20 the watch was still showing this app's menu, subtitled
// "next alarm in 4 h" -- a value computed at 03:00 (07:50 minus 03:00) and never
// redrawn, since the main menu has no minute tick. So: no watchface for four and
// a half hours, an app running all night for nothing, and a stale number that
// read as a scheduling bug.
//
// Every other wakeup that legitimately ends up showing nothing needs the same
// treatment -- WC_WINDOW with no alarm left to arm, a WC_DEADLINE whose slot the
// phone deleted -- so the condition is "no ring and no waiting window on the
// stack". A window declined because a snooze is pending is deliberately NOT one
// of these any more (2026-08-05): main()'s launch-time check pushes the snooze
// screen in exactly that case, so by the time this guard runs the waiting
// window IS on the stack and the condition below correctly does not arm --
// the app stays up for the rest of the snooze, on purpose. Read from the
// WINDOW STACK, never from s_ringing: this app has already produced one defect
// from testing that proxy instead of what is actually on screen (over_cap clears
// s_ringing while leaving the "Alarm missed" screen up).
//
// The delay exists for the launch config handshake -- request_config() asks the
// phone to resend its saved config, and that reply is this app's nightly resync.
// Measured over the CloudPebble relay it lands ~1.4 s after launch, so 10 s is
// generous headroom while still being a bounded stay. It is deliberately NOT
// cancelled by a button press: a wakeup launch is unattended by construction,
// and a cancel path is the idle timer growing back.
#define WAKEUP_LAUNCH_EXIT_MS 10000
static void wakeup_launch_exit_cb(void *data) {
  APP_LOG(APP_LOG_LEVEL_INFO,
          "wakeup launch put nothing on screen -- returning to the watchface");
  close_to_watchface();
}

// A tentative definition, so reload_and_rearm (above the ring section) can read
// the live "an alarm is ringing right now" flag. Its one real definition is
// further down with the rest of the ring state; only one of two tentative
// definitions may carry an initializer and neither does, so this is legal C.
static bool s_ringing;

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

// The slot whose snooze is in flight, or -1. Bounded against s_count, because
// pending_slot can outlive the alarm it names -- the phone can delete a slot
// while a snooze is pending.
static int snoozed_slot(void) {
  if (!ac_snooze_pending(s_rs.snooze_count, s_rs.ring_started_at, time(NULL))) {
    return -1;
  }
  int slot = s_rs.pending_slot;
  return (slot >= 0 && slot < s_count) ? slot : -1;
}

// pending_slot as the CYCLE CLASSIFIER must see it: an index that no longer names
// an alarm is no owner at all, which is what makes ac_cycle_state report
// AC_CYCLE_ORPHAN (and the launch-time repair then ends the cycle).
//
// Same bound, and the same reason, as snoozed_slot just above -- the phone can
// delete slots while a cycle is live. ac_cycle_state cannot apply it itself: it
// takes plain integers so it stays pure and host-testable, so it knows nothing
// about s_count and only tests `pending_slot < 0`. Every OTHER reader in this app
// bounds the field (snoozed_slot, ac_dispatch_wakeup's cycle_live, the
// WC_WINDOW/WC_REENTRY handler's own check); the two ac_cycle_state call sites did
// not, and deleting the WHOLE alarm set while a window was open then left a cycle
// that nothing could clear: the menu advertised "rings by 08:20" for an alarm that
// no longer existed, the launch repair called it AC_CYCLE_WINDOW rather than
// AC_CYCLE_ORPHAN and left it standing, and sc_rearm re-armed the rolling
// re-entry (it is armed on window_started_at != 0 alone) so the watch woke the app
// every SC_REENTRY_GAP_S for ever.
static int8_t cycle_owner_slot(void) {
  int slot = s_rs.pending_slot;
  return (slot >= 0 && slot < s_count) ? (int8_t)slot : (int8_t)-1;
}

static void refresh_list(void) {
  if (s_list_menu) {
    menu_layer_reload_data(s_list_menu);
  }
  if (s_main_menu) {
    menu_layer_reload_data(s_main_menu);
  }
}

// The two menu screens are written in RELATIVE time ("next in 51 min", "in 23 h",
// "skip, in 1 d"), and a MenuLayer only redraws when something asks it to -- so
// left alone those numbers are as old as the screen. That is not theoretical: on
// 2026-08-02 the app sat on its main menu from 03:00 (see the wakeup-launch exit
// above) and the "next alarm in 4 h" the user read at 07:20 had been computed
// four hours earlier. It read as a scheduling defect. The exit fix bounds an
// unattended screen to 10 s, but a screen the user opens themselves can sit there
// as long as they like, so the numbers now redraw themselves.
//
// Subscribed PER SCREEN (.appear/.disappear) rather than once for the app: the
// tick service holds exactly one handler, and the ring and waiting screens
// install their own. Arming on appear and dropping it on disappear means whoever
// is on top owns the tick, in both directions, without any window having to know
// about the others. Both of those screens subscribe AFTER their own
// window_stack_push, i.e. after this disappear has run, so the handover is
// ordered correctly in the one direction that could clobber a live alarm screen.
static void menu_minute_tick(struct tm *t, TimeUnits units) {
  refresh_list();
}

static void menu_win_appear(Window *w) {
  tick_timer_service_subscribe(MINUTE_UNIT, menu_minute_tick);
  refresh_list();   // minutes may have passed while another window was on top
}

static void menu_win_disappear(Window *w) {
  tick_timer_service_unsubscribe();
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

static void inbox_received(DictionaryIterator *iter, void *context) {
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
  GET_INT(PreAlarmMin, pre_alarm_min);
  GET_BOOL(EscRampVib, esc_ramp_vib);
  // DebugFeatures changes main_rows()'s row count (the Diagnostics row), same
  // as any other field here that reload_and_rearm() already reacts to -- no
  // separate reload needed. reload_and_rearm() -> refresh_list() calls
  // menu_layer_reload_data(s_main_menu) whenever it exists, and GET_BOOL sets
  // changed=true unconditionally when the tuple is present, so a live config
  // message flips the row on/off immediately, without relaunching the app.
  GET_BOOL(DebugFeatures, debug_features);
  if (changed) {
    as_save_config(&s_cfg);   // esc_clamp runs inside as_save_config
    APP_LOG(APP_LOG_LEVEL_INFO, "CFG updated: smart=%d win=%d sens=%d prof=%d",
            (int)s_cfg.smart_enabled, s_cfg.smart_window_min, s_cfg.sensitivity,
            s_cfg.wake_profile);
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
  if ((int)ci->row == snoozed_slot()) {
    // A snoozed alarm's own row must say so. Its occurrence is already stamped
    // served, so the ordinary path below would show TOMORROW's time while the
    // alarm is minutes away from ringing again. The weekday letters are dropped
    // for this one row: "MTWTF--  snoozed, in 8 min" clips on a 144 px board.
    fmt_delta((time_t)s_rs.ring_started_at, time(NULL), delta, sizeof(delta));
    snprintf(sub, sizeof(sub), "snoozed, in %s", delta);
  } else {
    if (!a->enabled) {
      snprintf(state, sizeof(state), "off");
    } else if (a->skip_next) {
      snprintf(state, sizeof(state), "skip, in %s", delta);
    } else {
      snprintf(state, sizeof(state), "in %s", delta);
    }
    snprintf(sub, sizeof(sub), "%s  %s", days, state);
  }
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
// The rendered strings, built ONCE when the submenu opens rather than in
// draw_row. Each label costs an occurrence computation -- ac_next_occurrence
// walks up to 16 candidate days through localtime/mktime -- and a MenuLayer
// redraws every row on every frame of the push animation, so computing them per
// draw put that work on the frame budget and made the transition visibly stutter.
static char s_act_labels[AC_MAX_ACTIONS][28];
static char s_act_title[28];

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
  draw_header_text(gctx, cell, s_act_title);
}

// Draw only -- every string it needs was built by open_alarm_actions.
static void act_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  GRect b = layer_get_bounds(cell);
  graphics_context_set_text_color(gctx, menu_cell_layer_is_highlighted(cell)
                                            ? GColorWhite : GColorBlack);
  const char *label = ((int)ci->row < s_act_count) ? s_act_labels[ci->row] : "Back";
  graphics_draw_text(gctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_24),
      GRect(6, 1, b.size.w - 12, b.size.h - 2),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void act_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  int slot = act_resolve();
  if (slot < 0 || (int)ci->row >= s_act_count) {
    // The alarm was reconfigured from the phone while this was open. Do nothing
    // rather than act on whatever slid into that index.
    APP_LOG(APP_LOG_LEVEL_WARNING, "action submenu: alarm gone, ignoring");
    window_stack_pop(false);
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
  window_stack_pop(false);   // back to the list, which now shows the new state
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

  char days[12];
  fmt_weekdays(s_act_mask, days, sizeof(days));
  snprintf(s_act_title, sizeof(s_act_title), "%02u:%02u  %s",
           (unsigned)(s_act_minute / 60) % 100u,
           (unsigned)(s_act_minute % 60) % 100u, days);
  for (int i = 0; i < s_act_count; i++) {
    char occ[16];
    switch (s_act_actions[i]) {
      case AC_ACTION_SKIP_NEXT:
        fmt_occurrence(act_occurrence(row, false), occ, sizeof(occ));
        snprintf(s_act_labels[i], sizeof(s_act_labels[i]), "Skip %s", occ);
        break;
      case AC_ACTION_RING_NEXT:
        fmt_occurrence(act_occurrence(row, true), occ, sizeof(occ));
        snprintf(s_act_labels[i], sizeof(s_act_labels[i]), "Ring %s", occ);
        break;
      case AC_ACTION_TURN_OFF:
        snprintf(s_act_labels[i], sizeof(s_act_labels[i]), "Turn off");
        break;
      case AC_ACTION_TURN_ON:
        snprintf(s_act_labels[i], sizeof(s_act_labels[i]), "Turn on");
        break;
    }
  }
  if (!s_act_window) {
    s_act_window = window_create();
    // Its own idle handlers: pushing this window makes the LIST disappear, which
    // cancels the idle timer by this app's convention, so without them the app
    // could sit open on this screen indefinitely.
    window_set_window_handlers(s_act_window, (WindowHandlers){
      .load = act_window_load, .unload = act_window_unload,
    });
  }
  window_stack_push(s_act_window, false);   // never animated -- see NOTE at the top
}

// SELECT opens the action submenu. There is deliberately no second gesture: long
// SELECT used to set skip-next and nothing on screen ever said so.
static void list_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
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
      // Keeps every row's relative time honest while the list is on screen --
      // see menu_minute_tick.
      .appear = menu_win_appear, .disappear = menu_win_disappear,
    });
  }
  window_stack_push(s_list_window, false);
}

// --- The "Last night" summary: a calibration tool, not a dismissible post-ring
// screen. Reachable only from the main menu, so there is no config toggle to
// disable it and no half-asleep screen the user learns to ignore. ---

static Window      *s_night_window;
static ScrollLayer *s_night_scroll;
static TextLayer   *s_night_head_text;
static TextLayer   *s_night_text;
static char         s_night_head[128];
static char         s_night_buf[1024];   // grown: the explanation block is new

static void night_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  // static, not a local: NIGHT_HISTORY(7) * sizeof(NightSummary)(32) = 224
  // bytes against the ~2 KB app stack -- squarely the "couple hundred bytes"
  // class that produced App fault! PC:0 LR:0 on hardware in the sibling apps.
  // night_window_load runs from a single window-load callback (single-threaded
  // event loop), so a non-reentrant buffer is safe, same as as_push_night's.
  static NightSummary ns[NIGHT_HISTORY];
  int n = as_load_nights(ns, NIGHT_HISTORY);
  nt_build(ns, n, s_night_head, sizeof(s_night_head),
           s_night_buf, sizeof(s_night_buf));

  s_night_scroll = scroll_layer_create(b);
  scroll_layer_set_click_config_onto_window(s_night_scroll, w);
  // The default shadow dithers over the last visible line whenever there is
  // more to scroll to (a zoomed screenshot showed it degrade "P75 06:33" into a
  // speckled mess) -- this screen's own text is the only affordance it needs.
  scroll_layer_set_shadow_hidden(s_night_scroll, true);

  const int inner_w = b.size.w - 8;
  int y = 0;

  // The glance, large. nt_build puts the label and the value on separate lines
  // so nothing breaks mid-value at 24 pt. An empty head is legal (nothing
  // recorded, or the night could not be judged) and must take no space.
  if (s_night_head[0] != '\0') {
    s_night_head_text = text_layer_create(GRect(4, y, inner_w, 400));
    text_layer_set_font(s_night_head_text,
                        fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_overflow_mode(s_night_head_text, GTextOverflowModeWordWrap);
    text_layer_set_text(s_night_head_text, s_night_head);
    GSize hu = text_layer_get_content_size(s_night_head_text);
    text_layer_set_size(s_night_head_text, GSize(inner_w, hu.h + 4));
    scroll_layer_add_child(s_night_scroll, text_layer_get_layer(s_night_head_text));
    y += hu.h + 10;
  }

  s_night_text = text_layer_create(GRect(4, y, inner_w, 2000));
  text_layer_set_font(s_night_text, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_night_text, GTextOverflowModeWordWrap);
  text_layer_set_text(s_night_text, s_night_buf);
  GSize bu = text_layer_get_content_size(s_night_text);
  text_layer_set_size(s_night_text, GSize(inner_w, bu.h + 8));
  scroll_layer_add_child(s_night_scroll, text_layer_get_layer(s_night_text));

  scroll_layer_set_content_size(s_night_scroll, GSize(b.size.w, y + bu.h + 16));
  layer_add_child(root, scroll_layer_get_layer(s_night_scroll));
}

static void night_window_unload(Window *w) {
  if (s_night_head_text) {
    text_layer_destroy(s_night_head_text);
    s_night_head_text = NULL;
  }
  text_layer_destroy(s_night_text);
  s_night_text = NULL;
  scroll_layer_destroy(s_night_scroll);
  s_night_scroll = NULL;
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
    });
  }
  window_stack_push(s_night_window, false);
}

// Forward declaration: main_select (below) opens the pending screen, but its
// real definition and the PENDING_WAITING/PENDING_SNOOZED mode constants live
// further down with the rest of the pending-screen state (near main.c:1860),
// which is physically AFTER main_select. Redeclaring an identical
// prototype/macro pair here is legal C (same idiom this file already uses for
// s_wait_window) and costs nothing.
#define PENDING_WAITING 0
#define PENDING_SNOOZED 1
#define PENDING_PREALARM 2
static void open_pending_window(int mode);

// The live cycle, as the menu sees it. One call, so every row and caption in
// this file agrees with the pending screen and with the launch-time repair.
static AcCycleState menu_cycle_state(void) {
  return ac_cycle_state(cycle_owner_slot(), s_rs.window_started_at,
                        s_rs.ring_started_at, s_rs.snooze_count, time(NULL));
}

// Fills s_main_row_kinds in display order and returns how many rows there are.
static int main_rows(void) {
  int n = 0;
  s_main_row_kinds[n++] = MAIN_ROW_ALARMS;
  if (menu_cycle_state() != AC_CYCLE_NONE) {
    // Directly after Alarms: the alarm summary stays the first thing on the
    // screen, and this row APPEARING is itself the signal that something is
    // ongoing -- which is why there is no permanent "none" row.
    s_main_row_kinds[n++] = MAIN_ROW_ONGOING;
  }
  s_main_row_kinds[n++] = MAIN_ROW_LAST_NIGHT;
#if SA_DEV_MENU
  s_main_row_kinds[n++] = MAIN_ROW_TEST;
#endif
#if SA_DEBUG_DUMP
  // RUNTIME-gated, unlike the Test alarm above: this is how an ordinary user
  // produces a bug report after the public release, so it has to exist in a
  // release build -- and a compile flag that nobody enforces is exactly what
  // made "remember SA_DEV_MENU 0" a release hazard (backlog 13). Hidden unless
  // the phone's Debugging toggle is on, so a default install never shows it.
  //
  // Test alarm deliberately stays compile-gated: it occupies a real alarm slot,
  // and backlog 12 (a watch-created alarm is invisible and undeletable from the
  // phone) would become a user-facing defect the moment real users can reach it.
  if (s_cfg.debug_features) {
    s_main_row_kinds[n++] = MAIN_ROW_DUMP;
  }
#endif
  return n;
}

// The row kind at a visible index, or MAIN_ROW_ALARMS for an out-of-range index
// (MenuLayer can ask about a row that has just disappeared under a reload).
static MainRowKind main_row_kind(uint16_t row) {
  int n = main_rows();
  return (row < (uint16_t)n) ? s_main_row_kinds[row] : MAIN_ROW_ALARMS;
}

static uint16_t main_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return (uint16_t)main_rows();
}

static void main_draw_row(GContext *gctx, const Layer *cell, MenuIndex *ci, void *ctx) {
  switch (main_row_kind(ci->row)) {
    case MAIN_ROW_ALARMS: {
      // sub[32], not smaller: "snoozed, in " is 12 bytes and fmt_delta can fill
      // all 15 usable bytes of delta[16], so 28 is the bound gcc computes and 24
      // is genuinely one short -- a tighter buffer is a real truncation rather
      // than a warning to silence (same reasoning as list_draw_row's state/sub).
      char sub[32];
      time_t when = 0;
      time_t nw = time(NULL);
      int slot = ac_next_alarm_unserved(
          s_alarms, s_count, nw, (int)s_rs.served_slot, (time_t)s_rs.served_at,
          (int32_t)(nw - sc_ring_deadline(&s_cfg, nw)), &when);
      int snz = snoozed_slot();
      if (snz >= 0) {
        // Ahead of "next in ...": the alarm minutes away matters more than the
        // one tomorrow, and this row is the whole main menu's status line.
        char delta[16];
        fmt_delta((time_t)s_rs.ring_started_at, nw, delta, sizeof(delta));
        snprintf(sub, sizeof(sub), "snoozed, in %s", delta);
      } else if (slot < 0) {
        snprintf(sub, sizeof(sub), "none set");
      } else {
        char delta[16];
        fmt_delta(when, nw, delta, sizeof(delta));
        snprintf(sub, sizeof(sub), "next in %s", delta);
      }
      menu_cell_basic_draw(gctx, cell, "Alarms", sub, NULL);
      break;
    }
    case MAIN_ROW_ONGOING: {
      // sub[32] for the same reason as the Alarms row: "snooze N, in " plus a
      // full fmt_delta is the longest shape, and a tighter buffer is a real
      // truncation rather than a warning to silence.
      char sub[32];
      time_t nw = time(NULL);
      switch (menu_cycle_state()) {
        case AC_CYCLE_SNOOZE: {
          char delta[16];
          fmt_delta((time_t)s_rs.ring_started_at, nw, delta, sizeof(delta));
          snprintf(sub, sizeof(sub), "snooze %d, in %s", s_rs.snooze_count, delta);
          break;
        }
        case AC_CYCLE_RINGING:
          snprintf(sub, sizeof(sub), "alarm in progress");
          break;
        case AC_CYCLE_WINDOW: {
          time_t dl = (time_t)s_rs.deadline_at;
          struct tm *dtm = localtime(&dl);
          snprintf(sub, sizeof(sub), "open, rings by %02d:%02d",
                   dtm->tm_hour % 100, dtm->tm_min % 100);
          break;
        }
        case AC_CYCLE_ORPHAN:
          // ASCII only: Gothic has no glyph for an em dash or a multiplication
          // sign, and renders one as a tofu box.
          snprintf(sub, sizeof(sub), "stale - press to clear");
          break;
        default:
          snprintf(sub, sizeof(sub), "none");
          break;
      }
      menu_cell_basic_draw(gctx, cell, "Ongoing alarm", sub, NULL);
      break;
    }
    case MAIN_ROW_LAST_NIGHT:
      menu_cell_basic_draw(gctx, cell, "Last night", "sleep + trigger", NULL);
      break;
    case MAIN_ROW_TEST:
      menu_cell_basic_draw(gctx, cell, "Test alarm", "rings in 2 min", NULL);
      break;
    case MAIN_ROW_DUMP:
      // The subtitle says only what the row is FOR, not what it costs: dbg_dump
      // reads up to 640 minutes of health history and holds the event loop for
      // 1.5-4 s, which is the "buttons do not respond when I open the app"
      // report that got it removed from running at launch (see debug_dump.h).
      // That warning is deliberately NOT repeated here -- the phone's Clay
      // config page already describes the wait next to the Debugging toggle,
      // and a user who has just turned that toggle on has already read it.
      menu_cell_basic_draw(gctx, cell, "Diagnostics", "for a bug report", NULL);
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

// Defined next to s_night further down -- the dump borrows that buffer, so the
// wrapper lives where the buffer does and only its name is needed up here.
static void dump_last_night(void);

static void main_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  switch (main_row_kind(ci->row)) {
    case MAIN_ROW_ALARMS:     open_alarm_list(); break;
    // NO new cancel path: the pending screen already ends the cycle on DOWN 2x
    // (wait_cancel_now stamps served_slot/served_at so the occurrence cannot
    // ring again, clears the missed marker, re-arms and exits), and it carries
    // the permanent legend that says so. This row is the ROUTE to it that the
    // app was missing -- leaving that screen with BACK 2x deliberately does not
    // cancel, so before this there was no way back to it at all.
    case MAIN_ROW_ONGOING:
      open_pending_window(menu_cycle_state() == AC_CYCLE_SNOOZE
                              ? PENDING_SNOOZED : PENDING_WAITING);
      break;
    case MAIN_ROW_LAST_NIGHT: open_last_night(); break;
    // A terminal action, so it leaves for the watchface -- which doubles as the
    // only confirmation the row has ever given. Safe to exit immediately:
    // add_test_alarm ends in reload_and_rearm, which persists the alarms and
    // schedules the wakeup synchronously before returning. The alarm-list toggles
    // deliberately do NOT do this (the user may want to set two in a row, and idle
    // auto-exit already gets them out).
    case MAIN_ROW_TEST:       add_test_alarm(); close_to_watchface(); break;
    // Stays on the menu, unlike the Test alarm: the dump paces itself over
    // app_timer ticks for several seconds and needs the app alive to finish.
    case MAIN_ROW_DUMP:      dump_last_night(); break;
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

// --- The pending screen's two modes -------------------------------------------
//
// ONE window serves both "the smart window is open, waiting for light sleep" and
// "a snooze is in flight" (2026-08-05 spec). They want the same layout (big
// clock, two-line caption, a permanent key legend) and the same contract -- DOWN
// 2x cancels this alarm, BACK 2x goes to the watchface and leaves it armed, UP
// and SELECT inert -- so the difference is the caption, not a second window.
// aplite has ~2.1 KB of app heap free; a duplicate Window plus four TextLayers
// that can never be on-stack at the same time as the original is not worth it
// there. The window itself lives in the smart-window section below. A third
// mode, PENDING_PREALARM, shares the same window and the same button contract
// too -- it is waiting for an alarm that has not reached its smart window yet.
#define PENDING_WAITING 0
#define PENDING_SNOOZED 1
#define PENDING_PREALARM 2
static void open_pending_window(int mode);
// A tentative definition, so start_ring (~700 lines above the smart-window
// section's own statics) can check it too, to hand the pending screen back
// once the ring it was covering for resumes. Its one real definition is
// further down with the rest of the pending-screen state; only one of two
// tentative definitions may carry an initializer and neither does, so this is
// legal C (same idiom as s_ringing above).
static Window *s_wait_window;

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
  light_enable_interaction();

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

static bool     s_ringing;
static AppTimer *s_hint_timer;

// How many presses of the bottom button stop the alarm. Fixed at two since the
// 2026-08-01 settings cleanup: a single press must never end an alarm (a
// half-asleep hand finds one button by feel), and beyond that "two, three, or
// hold it down" is a choice nobody needs to make. Dropping the choice also
// removed the hold path, whose progress bar was the only drawing on the ring
// screen that was not a TextLayer.
#define STOP_PRESSES   2
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

  // THE SNOOZE OWNS THE SCREEN from here (2026-08-05). It used to exit to the
  // watchface, which left nothing anywhere saying an alarm was pending and no
  // way to cancel it short of waiting for it to ring again.
  //
  // Push BEFORE removing the ring window: the other order empties the window
  // stack for an instant, and an empty stack exits the app -- to the watchface,
  // mid-snooze, which is precisely the behaviour being removed.
  open_pending_window(PENDING_SNOOZED);
  window_stack_remove(s_ring_window, false);
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

static void show_press_again_hint(const char *verb) {
  // Guarded for the same reason as update_ring_text.
  if (!s_ring_hint || !s_ring_sub) {
    return;
  }
  static char hint[28];
  // Plain ASCII "x", not the U+00D7 multiplication sign: GOTHIC_18_BOLD has no
  // glyph for it, so it rendered as a tofu box ("2<box> = Stop") on the actual
  // ring screen -- confirmed by pixel-zooming an emulator screenshot. This is
  // the one instruction a half-asleep user actually reads, so it must render.
  //
  // STOP_PRESSES is still formatted in rather than spelled out in the literal,
  // so both buttons keep saying the number they actually require.
  snprintf(hint, sizeof(hint), "Press %dx to %s", STOP_PRESSES, verb);
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
  if (click_number_of_clicks_counted(rec) >= STOP_PRESSES) {
    ring_stop_now();
  } else {
    // Without this the button is bound only to a multi-click and a single press
    // does nothing at all, which feels broken and makes the user press again too
    // slowly for the 400 ms window.
    show_press_again_hint("stop");
  }
}

// TWO presses, exactly like Stop (reported from the wrist 2026-08-05). One
// button found by feel in the dark must never change what the alarm does on
// the first press -- and a snooze IS a change: it silences a ringing alarm for
// ten minutes. The first press only says what the second would do.
static void ring_up_multi(ClickRecognizerRef rec, void *ctx) {
  if (click_number_of_clicks_counted(rec) >= STOP_PRESSES) {
    ring_snooze_now();
  } else {
    show_press_again_hint("snooze");
  }
}

// BACK and SELECT are bound EXPLICITLY to a no-op. An unbound BACK pops the
// window by default, which would silently dismiss the alarm — the opposite of
// what an alarm clock must do.
static void ring_noop(ClickRecognizerRef rec, void *ctx) {}

static void ring_click_config(void *ctx) {
  window_multi_click_subscribe(BUTTON_ID_UP, 1, STOP_PRESSES, 400, false,
                               ring_up_multi);
  window_multi_click_subscribe(BUTTON_ID_DOWN, 1, STOP_PRESSES, 400, false,
                               ring_down_multi);
  window_single_click_subscribe(BUTTON_ID_BACK, ring_noop);
  window_single_click_subscribe(BUTTON_ID_SELECT, ring_noop);
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

  // "Stop" is constant -- at either label
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

  update_ring_text();
}

static void ring_window_unload(Window *w) {
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
  //
  // A night the algorithm COULD NOT JUDGE counts as unavailable too, even though
  // the health read itself succeeded: with insufficient_data there is no
  // baseline, no trigger level and no alt-sensitivity answer, so passing the read
  // through would render "Baseline 0 / Level 0 / Woke you at 07:50" and label the
  // night `smart` in the history -- an evaluation that never happened, on the one
  // screen the sensitivity setting is tuned from. Recording it as unavailable is
  // the honest description and needs no new persisted field. NOTE the wrong fix
  // here is passing from_deadline = true: that would stamp served_at = now while
  // ac_is_served compares against this occurrence's deadline, so the occurrence
  // would come back unserved and RING A SECOND TIME.
  if (s_rs.snooze_count == 0) {
    const bool judged = s_last_read.available && !s_last_eval.insufficient_data;
    record_night(&s_last_eval, judged ? &s_last_read : NULL, from_deadline);
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
  // Guarded like the s_wait_window removal just below: burst_cb's over_cap
  // branch deliberately LEAVES its "Alarm missed" s_ring_window on the stack
  // (see burst_cb) and does not end the cycle, so that window can still be
  // sitting there, unreached by any button, when a LATER cycle event (another
  // alarm's WC_WINDOW, or the same alarm's occurrence the next day) calls
  // start_ring again. Without this guard that would push the very same
  // Window* a second time onto a stack it is already on -- pushing an
  // already-created window that is already on the stack is what this avoids.
  if (!window_stack_contains_window(s_ring_window)) {
    window_stack_push(s_ring_window, false);
  }
  // The pending screen has done its job -- the smart window's wait, or the
  // snooze this ring is resuming from. Take it off the stack now that the ring
  // is on top, rather than leaving it buried underneath for the rest of the
  // cycle. After the push, so the stack is never empty; before the tick
  // subscribe, so its .disappear cannot unsubscribe the ring's own tick.
  if (s_wait_window && window_stack_contains_window(s_wait_window)) {
    window_stack_remove(s_wait_window, false);
  }
  tick_timer_service_subscribe(MINUTE_UNIT, ring_minute_tick);

  s_ringing = true;
  burst_cb(NULL);   // first burst immediately
}

// --- The smart window: minute-history reads, the waiting screen, the 1-min poll. ---

static Window    *s_wait_window;
static int        s_pending_mode;   // PENDING_WAITING / PENDING_SNOOZED
// THE PRE-ALARM SCREEN CARRIES ITS OWN SLOT, because it deliberately does NOT
// begin a cycle. runstate_begin_cycle sets window_started_at, which every
// consumer reads as "the smart window is open": poll_cb would start reading
// minute history and evaluating sleep an hour early, sc_rearm would arm the
// rolling re-entry, and ac_cycle_state would be asked to validate a cycle that
// does not exist. So RunState is untouched and these three fields are the
// whole state of this screen -- which also means it does not survive the app
// being killed, and nothing tries to revive it. The smart window and the ring
// bring the app back exactly as they always did.
//
// TWO instants, not one, because they differ under SEMANTICS_RING_FROM (and
// under AWAKE_BY when escalation's lead moves the ring start): the alarm time
// itself (s_pre_alarm_at) is what ac_prealarm_start anchors on and what the
// caption must NAME, while the RING deadline (s_pre_deadline) is what
// wait_cancel_now stamps into served_at, because ac_is_served compares against
// the ring deadline after subtracting lead_s. Conflating them would either
// anchor the wakeup on the wrong instant or let a cancelled alarm ring anyway
// -- they coincide only under RING_LATEST, where the caption collapses to one
// line on purpose (see wait_window_update).
static int8_t s_pre_slot = -1;      // -1 when no pre-alarm screen is live
static time_t s_pre_deadline;       // the RING instant of that occurrence
static time_t s_pre_alarm_at;       // the ALARM instant itself (may differ)
static TextLayer *s_wait_keys;        // the two-line "how to get out" legend
static AppTimer  *s_wait_hint_timer;
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

static void dump_last_night(void) {
  dbg_dump(s_alarms, s_count, &s_cfg, &s_rs, s_night, S_NIGHT_LEN);
}

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
  if (s_pending_mode == PENDING_PREALARM) {
    // No cycle, so the alarm time comes from this screen's own record rather
    // than from RunState.deadline_at (which is 0 until a window opens).
    //
    // localtime returns a pointer into ONE shared static buffer, so the two
    // reads below are sequenced deliberately: copy the fields out of the
    // first result before calling localtime again for the second, rather than
    // holding two struct tm* at once (the second call would silently
    // overwrite the first's storage).
    struct tm atm = *localtime(&s_pre_alarm_at);
    int ah = atm.tm_hour, am = atm.tm_min;
    if (s_pre_deadline != s_pre_alarm_at) {
      // SEMANTICS_RING_FROM (and AWAKE_BY, when escalation's lead moves the
      // ring start) can open this screen when the ALARM time is still up to
      // window_min minutes away, so naming only s_pre_deadline would tell the
      // user "it rings at X" when it can ring up to window_min earlier. Name
      // both, rather than let the screen assert a time the alarm is not.
      struct tm dtm2 = *localtime(&s_pre_deadline);
      snprintf(sub, sizeof(sub), "Alarm %02d:%02d\nRings by %02d:%02d",
               ah, am, dtm2.tm_hour, dtm2.tm_min);
    } else {
      // RING_LATEST: the two instants coincide, so naming the same time twice
      // would be noise rather than information -- keep the original one-line
      // second row.
      snprintf(sub, sizeof(sub), "Alarm %02d:%02d\nWaiting for alarm", ah, am);
    }
  } else if (s_pending_mode == PENDING_SNOOZED) {
    // ring_started_at holds the snooze EXPIRY while one is in flight. An
    // absolute time, matching "Alarm 07:50" on the other mode -- the running
    // clock right above it supplies the delta.
    time_t until = (time_t)s_rs.ring_started_at;
    struct tm *utm = localtime(&until);
    snprintf(sub, sizeof(sub), "Snooze %d\nRings again %02d:%02d",
             s_rs.snooze_count, utm->tm_hour, utm->tm_min);
  } else if (s_rs.smart_unavailable) {
    snprintf(sub, sizeof(sub), "Alarm %02d:%02d\nSmart alarm unavailable",
             dtm->tm_hour, dtm->tm_min);
  } else if (menu_cycle_state() == AC_CYCLE_RINGING) {
    // Force-quitting a ringing alarm (long BACK) leaves the ring cycle live
    // with no window and no snooze, and the keep-alive wakeup resumes it within
    // SC_REENTRY_GAP_S. Reached from the new Ongoing row, this screen would
    // otherwise claim to be "waiting for light sleep" for an alarm that is
    // mid-ring -- the one caption a half-awake user must be able to trust.
    snprintf(sub, sizeof(sub), "Alarm %02d:%02d\nAlarm in progress",
             dtm->tm_hour, dtm->tm_min);
  } else {
    snprintf(sub, sizeof(sub), "Alarm %02d:%02d\nWaiting for light sleep",
             dtm->tm_hour, dtm->tm_min);
  }
  text_layer_set_text(s_wait_sub, sub);
}

// The two-line legend at the foot of the waiting screen. It is drawn ALWAYS, not
// only after a press: the user was trapped on this screen on 2026-08-03 ("alarm
// waiting screenistä ei pääse pois ollenkaan"), and a way out nobody can find is
// the same defect the alarm list's hidden long-press already cost this app once.
#define WAIT_KEYS_TEXT "2x Back: watchface\n2x Down: cancel alarm"

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

  // Small, at the very bottom, so it does not compete with the clock and the
  // alarm time at 3 a.m. -- but present.
  s_wait_keys = text_layer_create(GRect(2, b.size.h - 36, b.size.w - 4, 34));
  text_layer_set_font(s_wait_keys, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_wait_keys, GTextAlignmentCenter);
  night_text_layer(s_wait_keys);
  text_layer_set_text(s_wait_keys, WAIT_KEYS_TEXT);
  layer_add_child(root, text_layer_get_layer(s_wait_keys));

  wait_window_update();
}

static void wait_window_unload(Window *w) {
  if (s_wait_hint_timer) {
    app_timer_cancel(s_wait_hint_timer);
    s_wait_hint_timer = NULL;
  }
  text_layer_destroy(s_wait_keys);
  s_wait_keys = NULL;
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

static void wait_hint_hide_cb(void *data) {
  s_wait_hint_timer = NULL;
  if (s_wait_keys) {
    text_layer_set_text(s_wait_keys, WAIT_KEYS_TEXT);
  }
}

// A single press is never inert: it says what the second press would do. Same
// reasoning as the ring screen's "Press 2x to stop" -- without it the button
// feels broken and the user presses again too slowly for the 400 ms window.
static void wait_show_hint(const char *msg) {
  if (!s_wait_keys) {
    return;
  }
  text_layer_set_text(s_wait_keys, msg);
  if (s_wait_hint_timer) {
    app_timer_cancel(s_wait_hint_timer);
  }
  s_wait_hint_timer = app_timer_register(HINT_SHOW_MS, wait_hint_hide_cb, NULL);
}

// CANCEL THE ALARM FROM THE WAITING SCREEN -- everything Stop does on the ring
// screen, minus the ring that never happened. Marking the occurrence SERVED is
// the part that makes "cancel" mean it: without that stamp the very next re-arm
// (this one, the 03:00 clock check, a phone config save) would pick today's
// occurrence straight back up and the window would reopen minutes later. Read
// deadline_at BEFORE runstate_end_cycle clears it.
// Correct unchanged in PENDING_PREALARM, where there is no cycle to end:
// runstate_end_cycle on an already-empty cycle is a no-op, and the served
// stamp is the same pair open_smart_window would have written -- the RING
// deadline, which is what ac_is_served compares against after subtracting
// lead_s. So cancelling from this screen and cancelling from the waiting
// screen are indistinguishable to every later re-arm.
static void wait_cancel_now(int slot, time_t deadline) {
  APP_LOG(APP_LOG_LEVEL_INFO, "%s cancel slot=%d deadline=%lu",
          s_pending_mode == PENDING_PREALARM ? "PREALARM" :
          s_pending_mode == PENDING_SNOOZED ? "SNOOZE" : "WAIT",
          slot, (unsigned long)deadline);
  if (slot >= 0 && slot < MAX_ALARMS) {
    s_rs.missed[slot] = false;
    if (s_alarms[slot].weekday_mask == 0) {
      s_alarms[slot].enabled = false;    // a one-time alarm is spent either way
    }
    if (s_alarms[slot].skip_next) {
      s_alarms[slot].skip_next = false;  // the skip has been consumed
    }
    if (deadline != 0) {
      s_rs.served_slot = (int8_t)slot;
      s_rs.served_at = (uint32_t)deadline;
    }
  }
  runstate_end_cycle();
  reload_and_rearm();
  close_to_watchface();
}

static void wait_down_multi(ClickRecognizerRef rec, void *ctx) {
  if (click_number_of_clicks_counted(rec) >= STOP_PRESSES) {
    if (s_pending_mode == PENDING_PREALARM) {
      int slot = s_pre_slot;
      time_t deadline = s_pre_deadline;
      s_pre_slot = -1;
      s_pre_deadline = 0;
      s_pre_alarm_at = 0;
      wait_cancel_now(slot, deadline);
    } else {
      wait_cancel_now(s_rs.pending_slot, (time_t)s_rs.deadline_at);
    }
  } else {
    wait_show_hint("Press 2x to cancel the alarm");
  }
}

// LEAVING IS NOT CANCELLING. This only pops back to the watchface; the smart
// window stays open and its rolling re-entry wakeup brings the app -- and this
// screen -- back within SC_REENTRY_GAP_S. That return is deliberate and was the
// user's own call: "toki toisella napilla voisi olla niin että pääsee
// kellotaululle ja se sit kuitenkin käynnistyy uudelleen itsestään". The only
// cost is that monitoring runs at re-entry granularity (3 min) instead of the
// 1-minute poll while the app is dead, which is why this is NOT the button that
// gets you out for good -- that is DOWN.
static void wait_back_multi(ClickRecognizerRef rec, void *ctx) {
  if (click_number_of_clicks_counted(rec) >= STOP_PRESSES) {
    if (s_pending_mode == PENDING_PREALARM) {
      // NOTHING IS BEING MONITORED HERE, so unlike the smart window's screen
      // this one does not come back: no re-entry wakeup was ever armed for it
      // (see the WC_PREALARM handler), and BACK 2x therefore means "leave me
      // alone until the alarm is actually near". The alternative -- reviving it
      // every SC_REENTRY_GAP_S for up to 90 minutes -- would be the app shoving
      // itself in front of the user roughly 30 times for no reason. The alarm
      // itself is untouched: its WC_DEADLINE and any WC_WINDOW are still armed.
      s_pre_slot = -1;
      s_pre_deadline = 0;
      s_pre_alarm_at = 0;
      APP_LOG(APP_LOG_LEVEL_INFO, "PREALARM -> watchface (alarm stays armed)");
    } else {
      APP_LOG(APP_LOG_LEVEL_INFO, "WAIT -> watchface (window stays open)");
    }
    close_to_watchface();
  } else {
    wait_show_hint("Press 2x for the watchface");
  }
}

static void wait_click_config(void *ctx) {
  // BOTH ways out need TWO presses, by the user's decision (2026-08-03): one
  // press must never end -- or even interrupt -- an alarm, because a half-asleep
  // hand finds one button by feel. That is the same rule the ring screen's stop
  // gesture already follows, and DOWN 2x deliberately means the same thing on
  // both screens: this alarm is done with.
  window_multi_click_subscribe(BUTTON_ID_DOWN, 1, STOP_PRESSES, 400, false,
                               wait_down_multi);
  window_multi_click_subscribe(BUTTON_ID_BACK, 1, STOP_PRESSES, 400, false,
                               wait_back_multi);
  // Explicitly inert rather than unbound: an unbound BACK pops the window by
  // default, and UP is the snooze button one screen later.
  window_single_click_subscribe(BUTTON_ID_UP, wait_noop);
  window_single_click_subscribe(BUTTON_ID_SELECT, wait_noop);
}

static void wait_minute_tick(struct tm *t, TimeUnits units) {
  wait_window_update();
}

// The tick is owned by whoever is on top, in both directions -- the same
// arrangement the two menus use (menu_win_appear/disappear). It used to be
// subscribed by open_smart_window, which the snooze mode does not go through.
static void wait_win_appear(Window *w) {
  tick_timer_service_subscribe(MINUTE_UNIT, wait_minute_tick);
  wait_window_update();   // minutes may have passed under another window
}

static void wait_win_disappear(Window *w) {
  tick_timer_service_unsubscribe();
}

// The ONE place that knows how the pending window is built. Re-captions in
// place when it is already up, which is what makes a mode change (waiting ->
// snoozed) free of any stack manipulation.
static void open_pending_window(int mode) {
  s_pending_mode = mode;
  if (mode != PENDING_PREALARM) {
    // The smart window opening, or a snooze starting, takes the screen over --
    // the pre-alarm record must not outlive the mode that owns it, or a later
    // cancel could stamp an occurrence this screen is no longer about.
    s_pre_slot = -1;
    s_pre_deadline = 0;
    s_pre_alarm_at = 0;
  }
  if (!s_wait_window) {
    s_wait_window = window_create();
    window_set_window_handlers(s_wait_window, (WindowHandlers){
      .load = wait_window_load, .unload = wait_window_unload,
      .appear = wait_win_appear, .disappear = wait_win_disappear,
    });
    window_set_click_config_provider(s_wait_window, wait_click_config);
  }
  if (!window_stack_contains_window(s_wait_window)) {
    window_stack_push(s_wait_window, false);
  } else {
    wait_window_update();
  }
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
  // PRE-EXISTING HOLE, closed here because this function has three ring paths and
  // all of them used to walk into it: `pending_slot` was handed to start_ring
  // unbounded, so a cycle whose alarms were deleted mid-window indexed s_alarms
  // out of range and rang a phantom -- the same class as the orphaned cycle
  // batch 5 fixed in the WAKEUP path (`ac_window_wakeup` guards it; poll_cb never
  // did). cycle_owner_slot() is the file's one definition of "does this cycle
  // still own a real alarm". Stopping the poll without rescheduling is
  // deliberate: the cycle is orphaned, and clearing it belongs to the launch
  // repair and ac_cycle_state's AC_CYCLE_ORPHAN, not to a timer callback.
  const int8_t slot = cycle_owner_slot();
  if (slot < 0) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "SMART poll: cycle has no owning alarm (pending_slot=%d of %d) -- stopping",
            (int)s_rs.pending_slot, s_count);
    return;
  }
  time_t now = time(NULL);
  if (s_rs.deadline_at != 0 && now >= (time_t)s_rs.deadline_at) {
    APP_LOG(APP_LOG_LEVEL_INFO, "SMART deadline reached");
    start_ring(slot, true);
    return;
  }
  // Zero-initialised, not just declared: smart_should_ring returns false without
  // writing *out on the health-unavailable path, and the check below reads it.
  SleepEvalResult r = {0};
  if (smart_should_ring(&r)) {
    APP_LOG(APP_LOG_LEVEL_INFO, "SMART firing early at acc=%lu", (unsigned long)r.acc);
    start_ring(slot, false);
    return;
  }

  // CANNOT JUDGE THIS NIGHT AT ALL, under SEMANTICS_RING_FROM: ring at the set
  // time rather than sitting out the window -- but never BEFORE that set time.
  //
  // In that mode the set time is the EARLIEST allowed ring and the hard deadline
  // is at the far end of the window, so standing down costs the user the whole
  // window -- the alarm rings up to `smart_window_min` LATE for a night it never
  // evaluated. Measured on the recorded night of 2026-08-05 (replayed in
  // tests/test_sleep_sessions.c): a night waking at 07:20 left the population
  // below SE_MIN_USABLE for every minute of a 07:50-08:20 window, so a 07:50
  // alarm would have rung at 08:20. Ringing at the window's start is never late
  // and never earlier than what the user asked for, which is the whole promise
  // of this mode; the smart alarm simply degrades to a plain alarm for that
  // night. The other two modes need no special case: their deadline is already
  // the moment the ring must START -- the set time for RING_LATEST, and the set
  // time minus the ramp for AWAKE_BY -- so standing down there is not late.
  //
  // "Ring now" is only "ring at the set time" while the live cycle's geometry
  // matches the mode in force, and it need not: a cycle looks IDENTICAL in all
  // three modes (`deadline_at == window_started_at + smart_window_min * 60`
  // everywhere) -- only `time_semantics` says where the set time sits inside it.
  // A cycle opened as RING_LATEST puts the set time at the window's END, so
  // reading the config alone would ring 29 minutes EARLY. That is reachable
  // without touching the phone during the window: switch the mode while the app
  // is not running, and the launch config handshake (`request_config`, whose
  // reply lands after `ac_window_wakeup` has already opened the window from the
  // persisted config) updates `s_cfg` mid-cycle -- `reload_and_rearm` re-arms
  // wakeups but deliberately does not rebuild the cycle.
  //
  // So the guard is the promise itself, checked against the occurrence THE CYCLE
  // IS ABOUT rather than against the config: never ring before the set time.
  // Probed from `window_started_at - 1`, not from `now`: under RING_FROM `now` is
  // already past the set time inside a live window, so "the next occurrence after
  // now" resolves to TOMORROW (the trap that cost backlog item 21 a review
  // round). When the probe cannot resolve an occurrence, the ordinary deadline
  // path stays in charge.
  if ((!s_last_read.available || s_last_read.window_start < 0 || r.insufficient_data)
      && s_cfg.time_semantics == SEMANTICS_RING_FROM) {
    time_t set_time = 0;
    if (s_rs.window_started_at != 0) {
      Alarm probe = s_alarms[slot];
      probe.enabled = true;
      probe.skip_next = false;
      set_time = ac_next_occurrence(&probe, (time_t)s_rs.window_started_at - 1);
    }
    if (set_time != 0 && now + AC_SERVED_TOLERANCE_S >= set_time) {
      APP_LOG(APP_LOG_LEVEL_INFO,
              "SMART cannot judge (avail=%d insuf=%d) -- ringing at the set time",
              (int)s_last_read.available, (int)r.insufficient_data);
      start_ring(slot, false);
      return;
    }
    APP_LOG(APP_LOG_LEVEL_INFO,
            "SMART cannot judge, but the set time (%lu) is still ahead -- waiting",
            (unsigned long)set_time);
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
  if (ac_snooze_pending(s_rs.snooze_count, s_rs.ring_started_at, time(NULL))) {
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

  open_pending_window(PENDING_WAITING);

  // Keep the rolling re-entry wakeup alive: if anything kills this app during the
  // window, the next re-entry relaunches it and monitoring resumes intact, because
  // the baseline comes from minute history rather than from our RAM.
  sc_arm_reentry(time(NULL));

  if (s_poll_timer) {
    app_timer_cancel(s_poll_timer);
  }
  poll_cb(NULL);   // evaluate immediately
}

// A CYCLE WAS JUST ENDED AND NOTHING IS BEING OPENED IN ITS PLACE: don't leave
// the waiting screen stranded on a dead cycle.
//
// With window_started_at cleared, poll_cb's next tick returns early for ever (its
// guard checks that field first), so the 1-minute poll is dead weight; and the
// waiting screen's BACK is deliberately a no-op (it must not be possible to
// cancel a smart window by brushing a button in your sleep), so nothing would
// bring the user back to the watchface. Worse, the screen keeps DESCRIBING the
// dead cycle: deadline_at is now 0, which the caption renders through
// localtime(0) as "Alarm 02:00 / Waiting for light sleep".
//
// Only ever called with s_ringing false (the ring owns the screen otherwise), so
// closing to the watchface here cannot interrupt an alarm.
//
// One helper, two callers: the AC_WAKE_NONE branch this was extracted from, and
// the WC_WINDOW/WC_REENTRY executor -- which no longer decides for itself whether
// a screen has been stranded (it used to, at five separate returns, and the one it
// missed is how a screen got stranded in the first place). That is now
// AcWindowDecision.abandon_screen, derived once in ac_window_wakeup.
static void abandon_waiting_screen(void) {
  // The pre-alarm screen has NO CYCLE BY DESIGN, so "the cycle is dead" says
  // nothing about it. Without this guard the next WC_WINDOW/WC_REENTRY that
  // found nothing live would close a screen that is doing exactly its job.
  if (s_pending_mode == PENDING_PREALARM) {
    return;
  }
  if (s_poll_timer) {
    app_timer_cancel(s_poll_timer);
    s_poll_timer = NULL;
  }
  if (s_wait_window && window_stack_contains_window(s_wait_window)) {
    close_to_watchface();
  }
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
        //
        // The accepted gap (user decision 2, 2026-08-05): if a SECOND alarm's
        // deadline arrives while a first alarm is still ringing, this branch
        // consumes it and that second alarm never rings at all -- it is not
        // queued anywhere. Accepted because the user is already being woken,
        // and serving both would need a queue (more than one served_slot/
        // served_at pair), i.e. a persist-format change.
        sc_schedule(now + SC_REENTRY_GAP_S, WC_SNOOZE);
        break;
      }
      // WHICH ALARM IS THIS WAKEUP FOR? Not "which cycle is in progress" --
      // pending_slot answers that, and reading it as the answer to this question
      // was backlog item 20: sc_rearm arms the next unserved alarm's own deadline
      // at priority 1 regardless of which alarm is mid-cycle, so a 07:05 alarm's
      // deadline landing inside a snoozed 07:00 alarm's cycle re-rang the 07:00
      // one, restarted its ramp, destroyed the snooze, and lost the 07:05 alarm
      // silently until the next day. ac_dispatch_wakeup re-derives the owner from
      // the clock, the alarms and RunState, and is host-tested for every case.
      AcWakeDecision wd = ac_dispatch_wakeup(
          s_alarms, s_count, now,
          (int)s_rs.pending_slot, s_rs.ring_started_at, s_rs.snooze_count,
          (int)s_rs.served_slot, (time_t)s_rs.served_at,
          (int32_t)(now - sc_ring_deadline(&s_cfg, now)));
      APP_LOG(APP_LOG_LEVEL_INFO, "WAKEUP dispatch cookie=%d -> action=%d slot=%d "
              "(pending=%d ring=%lu snooze=%d)",
              (int)cookie, (int)wd.action, wd.slot, (int)s_rs.pending_slot,
              (unsigned long)s_rs.ring_started_at, s_rs.snooze_count);

      if (wd.action == AC_WAKE_RING_DEADLINE) {
        start_ring(wd.slot, true);
      } else if (wd.action == AC_WAKE_RESUME) {
        start_ring(wd.slot, false);
      } else if (wd.action == AC_WAKE_KEEP) {
        // A live cycle with nothing due -- a pending snooze is the reachable
        // case. Do NOT end the cycle here: that is precisely what erased the
        // snooze. Re-arm so the chain stays alive and leave everything else.
        sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, now, s_ringing);
      } else {
        // Nothing to ring and no live cycle, but the wakeup was still consumed --
        // without this, the wakeup chain would silently end here and no alarm
        // would ever fire again.
        //
        // Ends the cycle through its one owner (runstate_end_cycle) rather than
        // picking a subset of the five fields by hand: window_started_at was the
        // subset miss found in Task 11's review, and deadline_at was the subset
        // miss the whole-branch review found (Critical 1) -- a stale deadline_at
        // left here made the NEXT night's WC_WINDOW ring at window start, up to
        // 60 minutes early.
        runstate_end_cycle();
        as_save_runstate(&s_rs);
        // If the waiting screen happens to be up for this now-invalid window,
        // don't leave it stranded (see abandon_waiting_screen). s_ringing is
        // guaranteed false here -- checked at the top of this case.
        abandon_waiting_screen();
        sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, now, s_ringing);
      }
      break;
    }
    case WC_WINDOW:
    case WC_REENTRY: {
      if (s_ringing) {
        break;
      }
      // THE WHOLE DECISION IS PURE (ac_window_wakeup), and this is a thin
      // executor: build the inputs, act on the result. Not a style preference --
      // six defects in two rounds this week lived in the branch tree that used to
      // stand here (a phantom 14:22 alarm, a lost day's alarm, a ring for a
      // deleted alarm, a hijacked window, a 180 s wake loop, an immediate ring
      // after a mid-window edit), and main.c has no host-test harness at all, so
      // each was found on the wrist or in a log. The assertions written alongside
      // those fixes passed identically against the buggy code, because they could
      // only reach the arithmetic the handler happened to call. The decision now
      // has host tests of its own -- see tests/test_alarm_calc.c's
      // ac_window_wakeup block, one case per defect.
      //
      // s_ringing stays HERE, above: it is live UI state that RunState cannot
      // express (a first, un-snoozed ring is indistinguishable in RunState from a
      // ring that has ended), and it means "do nothing at all".
      AcWindowDecision wd = ac_window_wakeup(
          s_alarms, s_count, now,
          s_cfg.time_semantics, sc_window_active(&s_cfg), s_cfg.smart_window_min,
          sc_full_dev_s(&s_cfg),
          s_rs.pending_slot, s_rs.window_started_at, s_rs.ring_started_at,
          s_rs.snooze_count, s_rs.deadline_at,
          (int)s_rs.served_slot, (time_t)s_rs.served_at);
      // The log line the last two rounds of bugs were actually found in. It now
      // reports the DECISION plus the state it was made from, so every branch is
      // still distinguishable after the fact: action=1 is a ring from `deadline`,
      // action=2 an open/continue of [window, deadline], and action=0 (rearm only)
      // is disambiguated by `reason` (AcWindowReason in alarm_calc.h) rather than
      // collapsing every one of its six causes -- no slot, already served, no
      // occurrence to restart from, a discarded cycle's basis behind `now`, the
      // smart window off, or its window not yet open -- into the same line.
      // `basis` is the instant the decision was actually derived from (0 when
      // there is none), which a log could not otherwise show at all.
      APP_LOG(APP_LOG_LEVEL_INFO,
              "WINDOW wakeup action=%d slot=%d now=%lu window=%lu deadline=%lu "
              "reason=%d basis=%lu end_cycle=%d abandon=%d (stored slot=%d "
              "window=%lu deadline=%lu ring=%lu snooze=%d)",
              (int)wd.action, wd.slot, (unsigned long)now,
              (unsigned long)wd.window_start, (unsigned long)wd.deadline,
              (int)wd.reason, (unsigned long)wd.basis,
              (int)wd.end_cycle, (int)wd.abandon_screen,
              (int)s_rs.pending_slot, (unsigned long)s_rs.window_started_at,
              (unsigned long)s_rs.deadline_at,
              (unsigned long)s_rs.ring_started_at, s_rs.snooze_count);
      if (wd.end_cycle) {
        // Through its one owner (runstate_end_cycle, which logs the cycle it
        // ended) rather than by picking a subset of the five fields by hand.
        runstate_end_cycle();
        as_save_runstate(&s_rs);
      }
      if (wd.abandon_screen) {
        // s_ringing is guaranteed false here -- checked at the top of this case.
        abandon_waiting_screen();
      }
      if (wd.action == AC_WIN_RING_NOW) {
        // start_ring takes the waiting screen off the stack itself, which is why
        // ac_window_wakeup never asks for abandon_screen on this path.
        start_ring(wd.slot, true);
        break;
      }
      if (wd.action == AC_WIN_OPEN) {
        // The one path where a discard must NOT close the waiting screen: the
        // same window is being re-opened under the new config, and
        // open_smart_window re-pushes and re-captions the screen itself.
        open_smart_window(wd.slot, wd.window_start, wd.deadline);
        break;
      }
      sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, now, s_ringing);
      break;
    }
    case WC_PREALARM: {
      // Put the pending screen up for the alarm we are counting down to. This
      // begins NO cycle, arms NO re-entry and re-arms nothing: a fired wakeup
      // is consumed one-shot and every other wakeup this alarm needs
      // (WC_DEADLINE, and WC_WINDOW if the smart window is on) is still
      // scheduled from the last sc_rearm.
      time_t now2 = time(NULL);
      // Read from the WINDOW STACK, never from s_ringing, for the "ring" half
      // of this test -- this file's own rule at main.c:83-85, and this is the
      // SECOND time the same proxy has bitten this app. burst_cb's over_cap
      // branch clears s_ringing while deliberately leaving the "Alarm missed"
      // screen up on s_ring_window (see its comment), so testing s_ringing
      // here would let a WC_PREALARM for the NEXT alarm cover that notice --
      // and DOWN 2x on the pre-alarm screen would then cancel a different
      // alarm than the one the user is looking at.
      if ((s_ring_window && window_stack_contains_window(s_ring_window))
          || ac_snooze_pending(s_rs.snooze_count, s_rs.ring_started_at, now2)
          || s_rs.window_started_at != 0) {
        // Something with a stronger claim already owns the screen: a ring (or
        // an unacknowledged missed-alarm notice), a snooze, or an open smart
        // window. Each of those is ABOUT this alarm or a nearer one, and each
        // already offers the same DOWN 2x cancel.
        APP_LOG(APP_LOG_LEVEL_INFO, "PREALARM declined: cycle/ring/snooze live");
        break;
      }
      time_t when = 0;
      int slot = sc_next_unserved(s_alarms, s_count, &s_cfg, &s_rs, now2, &when);
      if (slot < 0) {
        APP_LOG(APP_LOG_LEVEL_INFO, "PREALARM: no unserved alarm ahead");
        break;
      }
      s_pre_slot = (int8_t)slot;
      s_pre_alarm_at = when;
      s_pre_deadline = sc_ring_deadline(&s_cfg, when);
      APP_LOG(APP_LOG_LEVEL_INFO, "PREALARM screen for slot=%d alarm=%lu ring=%lu",
              slot, (unsigned long)s_pre_alarm_at, (unsigned long)s_pre_deadline);
      open_pending_window(PENDING_PREALARM);
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

  // An ORPHANED CYCLE: the alarm that owned the live cycle is gone, so there is
  // no owning slot (pending_slot is -1, or -- via cycle_owner_slot -- an index
  // past the end of the alarm array) while window_started_at still says a smart
  // window is open. Two things produce it. The prune above: it is exactly what
  // the built-in Test alarm leaves behind (the test alarm opens a window, gets
  // auto-disabled, and is pruned on the next launch -- taking its own slot
  // reference with it). And the phone deleting alarms while a window is open,
  // which shrinks the array under a pending_slot that still points into it -- in
  // the limit deleting them ALL, leaving s_count == 0 with pending_slot == 0.
  //
  // Left standing, the orphan is not inert. sc_rearm keeps re-arming the rolling
  // WC_REENTRY (it is armed on window_started_at != 0 alone) so the app is woken
  // every SC_REENTRY_GAP_S for the rest of time, and the WC_WINDOW/WC_REENTRY
  // handler trusts the STORED deadline whenever a window is live -- a deadline
  // belonging to the deleted alarm. Once that instant passes, the next re-entry
  // reads `now >= ring` and rings a full escalating alarm for an occurrence the
  // user never set. Observed on the real watch 2026-08-05: a 13:52 test alarm
  // left window=13:52/deadline=14:22 with pending_slot=-1, which would have rung
  // at ~14:22 that afternoon.
  //
  // Ending the cycle here is the whole fix: it runs BEFORE the launch sc_rearm
  // below, so no re-entry is placed and the stale deadline can never be read.
  if (ac_cycle_state(cycle_owner_slot(), s_rs.window_started_at,
                     s_rs.ring_started_at, s_rs.snooze_count,
                     time(NULL)) == AC_CYCLE_ORPHAN) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "orphaned cycle at launch (window=%lu deadline=%lu, no owning alarm)"
            " -- ending it",
            (unsigned long)s_rs.window_started_at,
            (unsigned long)s_rs.deadline_at);
    runstate_end_cycle();
    as_save_runstate(&s_rs);
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
    // Keeps the "next in ..." subtitle honest while the menu is on screen --
    // see menu_minute_tick.
    .appear = menu_win_appear, .disappear = menu_win_disappear,
  });
  window_stack_push(s_main_window, false);

  if (launched_by_wakeup) {
    handle_wakeup_cookie(launch_cookie);
  }

  // A SNOOZE IN FLIGHT OWNS THE SCREEN, however the app got launched -- by the
  // user reopening it, or by a wakeup that had nothing of its own to show. This
  // is what makes "cancel the alarm at any time" true after a long-BACK kill,
  // which is the only way to leave the snooze screen without deciding anything.
  //
  // AFTER handle_wakeup_cookie, and conditional on an empty screen: a snooze
  // whose expiry just fired has already become a ring, and that ring must win.
  if (!(s_ring_window && window_stack_contains_window(s_ring_window))
      && !(s_wait_window && window_stack_contains_window(s_wait_window))
      && ac_snooze_pending(s_rs.snooze_count, s_rs.ring_started_at, time(NULL))) {
    APP_LOG(APP_LOG_LEVEL_INFO, "launch during a pending snooze -- showing it");
    open_pending_window(PENDING_SNOOZED);
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

  // See WAKEUP_LAUNCH_EXIT_MS. Decided here, synchronously after
  // handle_wakeup_cookie has had its one chance to push a screen, so the test is
  // over settled state rather than over a race.
  if (launched_by_wakeup
      && !(s_ring_window && window_stack_contains_window(s_ring_window))
      && !(s_wait_window && window_stack_contains_window(s_wait_window))) {
    app_timer_register(WAKEUP_LAUNCH_EXIT_MS, wakeup_launch_exit_cb, NULL);
  }

  // The dump is no longer run at launch: measured on the watch it held the event
  // loop for 1.5-4 s right after start-up, which read as the app being stuck.
  // It is a menu action now (MAIN_ROW_DUMP), so the data is still one press away
  // and costs a shipped app nothing when the row is off.

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
