// SPDX-License-Identifier: GPL-3.0-only
#include "alarm_store.h"
#include <pebble.h>
#include <string.h>

// PK_ALARMS is the one persisted blob with no version byte: it is a bare array
// of Alarm, and the only validation as_load_alarms can do is that the stored
// length divides evenly by sizeof(Alarm). That check is silently unsound if
// sizeof(Alarm) ever changes -- grow Alarm from 6 to 8 bytes and an old
// 48-byte blob still divides evenly, so eight real alarms are read back as six
// garbage ones (wrong times, wrong days) with no error anywhere. A version
// prefix would change the persisted format for existing installs, so instead
// the assumption is pinned here: if Alarm's layout changes, THIS assert fails at
// compile time and whoever changed it must add a migration (bump the key, or
// prefix a version) rather than discovering it as garbage alarms on a watch.
_Static_assert(sizeof(Alarm) == 6,
               "PK_ALARMS is an unversioned Alarm array whose only validation is "
               "length % sizeof(Alarm) == 0 -- changing Alarm's size silently "
               "misreads existing blobs. Add a migration for PK_ALARMS.");

// Config and RunState are each persisted whole under one key, so both must stay
// inside PebbleOS's 256-byte per-key cap -- the same limit NightBlob already
// asserts against below. Config is the one that will actually grow: it embeds
// EscParams, so a new escalation field lands in this blob too. Over the cap,
// prv_write's write fails and every setting silently reverts on the next launch.
_Static_assert(sizeof(Config) <= 256,
               "Config exceeds the 256-byte persist limit for one key");
_Static_assert(sizeof(RunState) <= 256,
               "RunState exceeds the 256-byte persist limit for one key");

void as_load_alarms(Alarm *out, int *count) {
  memset(out, 0, sizeof(Alarm) * MAX_ALARMS);
  *count = 0;
  if (!persist_exists(PK_ALARMS)) {
    return;
  }
  int want = (int)(sizeof(Alarm) * MAX_ALARMS);
  int got = persist_read_data(PK_ALARMS, out, want);
  if (got <= 0 || (got % (int)sizeof(Alarm)) != 0) {
    memset(out, 0, sizeof(Alarm) * MAX_ALARMS);
    return;
  }
  *count = got / (int)sizeof(Alarm);
}

// persist_write_data returns the number of bytes written, or a negative status.
// A silent failure would mean the watch believes an alarm set or a pending snooze
// was saved when it was not — discovered only after a kill or a reboot, i.e. at
// the exact moment the alarm was supposed to fire. So every write is checked.
// There is no useful recovery at this layer; logging is what makes the failure
// visible in `pebble logs` instead of invisible.
static void prv_write(uint32_t key, const void *data, size_t size) {
  int w = persist_write_data(key, data, size);
  if (w != (int)size) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "persist write key=%u failed (%d, wanted %d)",
            (unsigned)key, w, (int)size);
  }
}

void as_save_alarms(const Alarm *alarms, int count) {
  if (count < 0) count = 0;
  if (count > MAX_ALARMS) count = MAX_ALARMS;
  prv_write(PK_ALARMS, alarms, sizeof(Alarm) * (size_t)count);
}

void as_load_config(Config *out) {
  memset(out, 0, sizeof(*out));
  bool ok = false;
  if (persist_exists(PK_CONFIG)
      && persist_read_data(PK_CONFIG, out, sizeof(*out)) == (int)sizeof(*out)
      && out->version == CONFIG_VERSION) {
    ok = true;
  }
  if (!ok) {
    // Defaults. These must agree with the Clay page's defaultValues; the C side
    // is what a fresh install uses before the phone has ever sent anything, and
    // what the emulator shows (the config page cannot open headless).
    memset(out, 0, sizeof(*out));
    out->version = CONFIG_VERSION;
    out->smart_enabled = true;
    out->smart_window_min = 30;
    out->time_semantics = SEMANTICS_RING_STARTS;
    out->sensitivity = SENS_MEDIUM;
    out->sens_percentile = 90;
    out->sens_minutes = 2;
    out->wake_profile = ESC_PROFILE_NORMAL;
    esc_profile(ESC_PROFILE_NORMAL, &out->esc);
    out->snooze_min = 10;
    out->snooze_max = 5;
    out->snooze_ramp_offset_s = 120;
    out->stop_gesture = STOP_TWO_TAP;
    out->light_pulse = true;
    out->dst_check = true;
    out->idle_exit_sec = 15;
  }
  esc_clamp(&out->esc);
}

void as_save_config(const Config *cfg) {
  Config c = *cfg;
  c.version = CONFIG_VERSION;
  esc_clamp(&c.esc);
  prv_write(PK_CONFIG, &c, sizeof(c));
}

void as_load_runstate(RunState *out) {
  memset(out, 0, sizeof(*out));
  if (persist_exists(PK_RUNSTATE)
      && persist_read_data(PK_RUNSTATE, out, sizeof(*out)) == (int)sizeof(*out)
      && out->version == RUNSTATE_VERSION) {
    return;
  }
  memset(out, 0, sizeof(*out));
  out->version = RUNSTATE_VERSION;
  out->pending_slot = -1;
}

void as_save_runstate(const RunState *rs) {
  RunState r = *rs;
  r.version = RUNSTATE_VERSION;
  prv_write(PK_RUNSTATE, &r, sizeof(r));
}

// The night ring buffer is stored newest-first so as_load_nights is a plain copy
// and pushing is a memmove. NIGHT_HISTORY * sizeof(NightSummary) must stay under
// the 256-byte per-key persist cap; a static assert enforces that at compile time.
typedef struct {
  uint8_t version;
  uint8_t count;
  NightSummary nights[NIGHT_HISTORY];
} NightBlob;

_Static_assert(sizeof(NightBlob) <= 256,
               "night history exceeds the 256-byte persist limit for one key");

static void prv_load_night_blob(NightBlob *b) {
  memset(b, 0, sizeof(*b));
  if (persist_exists(PK_NIGHTS)
      && persist_read_data(PK_NIGHTS, b, sizeof(*b)) == (int)sizeof(*b)
      && b->version == NIGHTS_VERSION) {
    if (b->count > NIGHT_HISTORY) {
      b->count = NIGHT_HISTORY;
    }
    return;
  }
  memset(b, 0, sizeof(*b));
  b->version = NIGHTS_VERSION;
}

void as_push_night(const NightSummary *n) {
  // static, not a local: NightBlob is 228 bytes against a ~2 KB app stack, and a
  // caller's own frame plus the stack used inside persist_* / memmove / APP_LOG
  // sits on top of it. An oversized local is how the sibling apps hit
  // "App fault! PC:0 LR:0" on hardware while the emulator looked fine. The event
  // loop is single-threaded, so a non-reentrant helper is safe here.
  static NightBlob b;
  prv_load_night_blob(&b);
  int keep = b.count < NIGHT_HISTORY ? b.count : NIGHT_HISTORY - 1;
  if (keep > 0) {
    memmove(&b.nights[1], &b.nights[0], sizeof(NightSummary) * (size_t)keep);
  }
  b.nights[0] = *n;
  if (b.count < NIGHT_HISTORY) {
    b.count++;
  }
  b.version = NIGHTS_VERSION;
  prv_write(PK_NIGHTS, &b, sizeof(b));
}

int as_load_nights(NightSummary *out, int max) {
  static NightBlob b;   // 228 bytes — see the note in as_push_night
  prv_load_night_blob(&b);
  int n = b.count < max ? b.count : max;
  for (int i = 0; i < n; i++) {
    out[i] = b.nights[i];
  }
  return n;
}

void as_effective_esc(const Config *cfg, EscParams *out) {
  if (cfg->wake_profile == ESC_PROFILE_CUSTOM) {
    *out = cfg->esc;
  } else {
    esc_profile(cfg->wake_profile, out);
  }
  esc_clamp(out);
}

void as_effective_sens(const Config *cfg, uint8_t *percentile, uint8_t *minutes) {
  uint8_t p, m = 2;
  switch (cfg->sensitivity) {
    case SENS_LOW:    p = 95; break;
    case SENS_HIGH:   p = 82; break;
    case SENS_CUSTOM:
      p = cfg->sens_percentile;
      m = cfg->sens_minutes;
      if (p < 70) p = 70;
      if (p > 99) p = 99;
      if (m < 1) m = 1;
      if (m > 5) m = 5;
      break;
    case SENS_MEDIUM:
    default:          p = 90; break;
  }
  if (percentile) *percentile = p;
  if (minutes) *minutes = m;
}
