// SPDX-License-Identifier: GPL-3.0-only
#include "night_text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static NightSummary night_0806(void) {
  // The real night of 2026-08-05/06, as the watch recorded it.
  NightSummary n;
  memset(&n, 0, sizeof(n));
  n.day_local = 20671;
  n.onset_min = 23 * 60 + 50;
  n.baseline = 0;
  n.trigger_level = 354;
  n.fired_min = 7 * 60 + 51;
  n.fired_by_deadline = 0;
  n.smart_unavailable = 0;
  n.acc_at_fire = 800;
  n.percentile = 90;
  n.alt_percentile[0] = 95; n.alt_fired_min[0] = NIGHT_NO_FIRE;
  n.alt_percentile[1] = 90; n.alt_fired_min[1] = 7 * 60 + 51;
  n.alt_percentile[2] = 82; n.alt_fired_min[2] = 7 * 60 + 51;
  n.alt_percentile[3] = 75; n.alt_fired_min[3] = 7 * 60 + 51;
  return n;
}

int main(void) {
  char head[128], body[1024];

  // --- the setting names, including the one that has none -------------------
  assert(strcmp(nt_sens_name(95), "Low") == 0);
  assert(strcmp(nt_sens_name(90), "Medium") == 0);
  assert(strcmp(nt_sens_name(82), "High") == 0);
  assert(strcmp(nt_sens_name(75), "Custom") == 0);
  assert(strcmp(nt_sens_name(88), "Custom") == 0);

  // --- a recorded night ----------------------------------------------------
  {
    NightSummary n = night_0806();
    nt_build(&n, 1, head, sizeof(head), body, sizeof(body));

    // The glance: label and value on separate lines, so 24 pt cannot break a
    // value mid-number.
    assert(strstr(head, "Woke you at\n07:51") != NULL);
    assert(strstr(head, "Asleep from\n23:50") != NULL);
    assert(strstr(head, "Deadline") == NULL);

    // Plain language, not developer shorthand.
    assert(strstr(body, "Stir needed") != NULL);
    assert(strstr(body, "354") != NULL);
    assert(strstr(body, "Level ") == NULL);

    // Setting name AND percentile: the name makes the row actionable, the
    // number anchors the Custom slider.
    assert(strstr(body, "Low (95%)") != NULL);
    assert(strstr(body, "Medium (90%)") != NULL);
    assert(strstr(body, "High (82%)") != NULL);
    assert(strstr(body, "Custom (75%)") != NULL);
    assert(strstr(body, "P95") == NULL);
    assert(strstr(body, "Highest") == NULL);   // no invented setting name

    // The sensitivity in use is marked, exactly once.
    const char *first = strstr(body, "Medium (90%)");
    assert(first != NULL);
    const char *star = strchr(first, '*');
    assert(star != NULL);
    assert(strchr(star + 1, '*') == NULL);

    // A setting that would not have rung early reads as --:--.
    assert(strstr(body, "--:--") != NULL);

    // The explanation, and the inversion stated explicitly.
    assert(strstr(body, "What this means") != NULL);
    assert(strstr(body, "higher") != NULL || strstr(body, "HIGHER") != NULL);
    assert(strstr(body, "Low") != NULL);
    assert(strstr(body, "phone") != NULL);

    // ASCII only -- Gothic renders anything else as a tofu box.
    for (const char *p = body; *p; p++) { assert((unsigned char)*p < 0x80); }
    for (const char *p = head; *p; p++) { assert((unsigned char)*p < 0x80); }
  }

  // --- a deadline ring says so ---------------------------------------------
  {
    NightSummary n = night_0806();
    n.fired_by_deadline = 1;
    nt_build(&n, 1, head, sizeof(head), body, sizeof(body));
    assert(strstr(head, "Deadline at\n07:51") != NULL);
    assert(strstr(head, "Woke you at") == NULL);
  }

  // --- nothing recorded yet ------------------------------------------------
  {
    nt_build(NULL, 0, head, sizeof(head), body, sizeof(body));
    assert(head[0] == '\0');
    assert(strstr(body, "No nights recorded yet") != NULL);
  }

  // --- a night the algorithm could not judge -------------------------------
  {
    NightSummary n = night_0806();
    n.smart_unavailable = 1;
    nt_build(&n, 1, head, sizeof(head), body, sizeof(body));
    assert(strstr(body, "unavailable") != NULL);
    assert(strstr(body, "Custom (75%)") == NULL);   // no table to show
  }

  // --- earlier nights ------------------------------------------------------
  {
    NightSummary ns[3];
    ns[0] = night_0806();
    ns[1] = night_0806(); ns[1].day_local = 20670; ns[1].fired_min = 7 * 60 + 56;
    ns[2] = night_0806(); ns[2].day_local = 20669; ns[2].fired_by_deadline = 1;
    nt_build(ns, 3, head, sizeof(head), body, sizeof(body));
    assert(strstr(body, "Earlier") != NULL);
    assert(strstr(body, "07:56") != NULL);
    assert(strstr(body, "deadline") != NULL);
  }

  // --- buffers that are far too small must truncate, not overrun -----------
  {
    NightSummary n = night_0806();
    char tiny_head[8], tiny_body[12];
    nt_build(&n, 1, tiny_head, sizeof(tiny_head), tiny_body, sizeof(tiny_body));
    assert(tiny_head[sizeof(tiny_head) - 1] == '\0'
           || strlen(tiny_head) < sizeof(tiny_head));
    assert(strlen(tiny_body) < sizeof(tiny_body));
  }

  printf("test_night_text: all assertions passed\n");
  return 0;
}
