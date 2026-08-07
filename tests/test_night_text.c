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

    // The sensitivity in use is marked, exactly once within the table (the
    // explanation below also contains a '*', in the legend sentence, so the
    // search is bounded to the table region rather than the whole body).
    const char *table_start = strstr(body, "At other sensitivities:");
    const char *table_end = strstr(body, "What this means");
    assert(table_start != NULL && table_end != NULL && table_end > table_start);
    const char *first = strstr(body, "Medium (90%)");
    assert(first != NULL && first < table_end);
    const char *star = strchr(first, '*');
    assert(star != NULL && star < table_end);
    const char *star2 = strchr(star + 1, '*');
    assert(star2 == NULL || star2 >= table_end);

    // A setting that had not rung by the time the alarm did reads as --:--.
    assert(strstr(body, "--:--") != NULL);
    // The old wording claimed such a setting "would not have rung early at
    // all" -- false on an early smart wake, since alt sensitivities are only
    // evaluated up to the moment the alarm actually rang (hr_read_night stops
    // at `now`), not over the whole window. A less eager setting could well
    // have fired later and still been early.
    assert(strstr(body, "would not have rung early at all") == NULL);
    assert(strstr(body, "had not rung by the time the alarm did") != NULL);

    // The explanation, and the inversion stated explicitly.
    assert(strstr(body, "What this means") != NULL);
    assert(strstr(body, "higher") != NULL || strstr(body, "HIGHER") != NULL);
    assert(strstr(body, "Low") != NULL);
    assert(strstr(body, "phone") != NULL);

    // The marker is legended, so "*" is not left unexplained.
    assert(strstr(body, "The * marks the sensitivity you use now.") != NULL);

    // This night's percentile (90) matches the "Medium" alt row, so no extra
    // "In use" line is needed -- the table's marker already says it.
    assert(strstr(body, "In use:") == NULL);

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
    // The explanation talks about a sensitivity list and a marker that were
    // never printed on this path -- it must not follow a table that isn't there.
    assert(strstr(body, "What this means") == NULL);
    assert(strstr(body, "In use:") == NULL);
  }

  // --- a Custom percentile matching none of the four named alternatives ----
  {
    NightSummary n = night_0806();
    n.percentile = 88;   // between High (82) and Medium (90); no table row is 88
    nt_build(&n, 1, head, sizeof(head), body, sizeof(body));
    // Every table row prints unmarked -- none of the four alternatives is 88 --
    // and none of the marked variants appear.
    assert(strstr(body, "Low (95%)  --:--\n") != NULL);
    assert(strstr(body, "Medium (90%)  07:51\n") != NULL);
    assert(strstr(body, "High (82%)  07:51\n") != NULL);
    assert(strstr(body, "Custom (75%)  07:51\n") != NULL);
    assert(strstr(body, "07:51 *") == NULL);
    // ...but the screen still states the setting it is helping the user tune,
    // by name and number, so the reader is never left without an answer.
    assert(strstr(body, "In use: Custom (88%)") != NULL);
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
