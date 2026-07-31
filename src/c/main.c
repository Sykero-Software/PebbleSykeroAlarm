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
  char marks[12];
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

  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    WakeupId id = 0;
    int32_t cookie = 0;
    wakeup_get_launch_event(&id, &cookie);
    APP_LOG(APP_LOG_LEVEL_INFO, "LAUNCHED BY WAKEUP cookie=%d", (int)cookie);
  } else {
    APP_LOG(APP_LOG_LEVEL_INFO, "LAUNCHED reason=%d", (int)launch_reason());
  }

  sc_rearm(s_alarms, s_count, &s_cfg, &s_rs, time(NULL));

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers){
    .load = main_window_load, .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);
  app_event_loop();

  as_save_alarms(s_alarms, s_count);
  as_save_runstate(&s_rs);
  if (s_list_window) window_destroy(s_list_window);
  window_destroy(s_main_window);
  return 0;
}
