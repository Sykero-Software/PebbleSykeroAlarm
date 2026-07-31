// SPDX-License-Identifier: GPL-3.0-only
#include <pebble.h>
#include "alarm_store.h"

static Window *s_main_window;
static TextLayer *s_hello;

static void main_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_hello = text_layer_create(GRect(0, b.size.h / 2 - 20, b.size.w, 40));
  text_layer_set_text(s_hello, "Smart Alarm");
  text_layer_set_text_alignment(s_hello, GTextAlignmentCenter);
  text_layer_set_font(s_hello, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  layer_add_child(root, text_layer_get_layer(s_hello));
}

static void main_window_unload(Window *w) {
  text_layer_destroy(s_hello);
}

int main(void) {
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers){
    .load = main_window_load, .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);
  app_event_loop();
  window_destroy(s_main_window);
  return 0;
}
