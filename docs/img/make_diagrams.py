#!/usr/bin/env python3
"""Draw the diagrams used by docs/how-it-works.md and docs/how-it-works.fi.md.

Run from this directory:  python3 make_diagrams.py

Writes each diagram twice: name.svg (English) and name.fi.svg (Finnish). The
geometry is shared, so the two languages cannot drift apart in anything but
wording -- which is the point of generating them from one file.

No dependencies. Every number that describes the app's behaviour is taken from
the source (src/c/sleep_eval.c, escalation.c, scheduler.c) and every number in
the night chart is taken from night-2026-08-01.txt, which is real per-minute
data the watch recorded. Nothing here is illustrative-only; if the app changes,
these should be regenerated rather than touched up.
"""

import os

# --- palette -----------------------------------------------------------------
# Validated categorical slots 1-3 plus text/surface roles. Dark values are the
# same hues re-stepped for a dark surface, not an automatic inversion.
LIGHT = dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", grid="#dcdcd8",
             s1="#2a78d6", s2="#eb6834", s3="#1baf7a", band="#e8eff9")
DARK = dict(surface="#1a1a19", ink="#ffffff", ink2="#c3c2b7", grid="#3a3a37",
            s1="#3987e5", s2="#d95926", s3="#199e70", band="#22304180")

STYLE = """
  <style>
    .sfc{{fill:{L[surface]}}} .ink{{fill:{L[ink]}}} .ink2{{fill:{L[ink2]}}}
    .grid{{stroke:{L[grid]}}} .s1{{fill:{L[s1]}}} .s1s{{stroke:{L[s1]}}}
    .s2{{fill:{L[s2]}}} .s2s{{stroke:{L[s2]}}}
    .s3{{fill:{L[s3]}}} .s3s{{stroke:{L[s3]}}} .band{{fill:{L[band]}}}
    text{{font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif}}
    @media (prefers-color-scheme: dark) {{
      .sfc{{fill:{D[surface]}}} .ink{{fill:{D[ink]}}} .ink2{{fill:{D[ink2]}}}
      .grid{{stroke:{D[grid]}}} .s1{{fill:{D[s1]}}} .s1s{{stroke:{D[s1]}}}
      .s2{{fill:{D[s2]}}} .s2s{{stroke:{D[s2]}}}
      .s3{{fill:{D[s3]}}} .s3s{{stroke:{D[s3]}}} .band{{fill:{D[band]}}}
    }}
  </style>
""".format(L=LIGHT, D=DARK)

# --- wording -----------------------------------------------------------------
# Keys are shared; only the values differ. A missing key is a KeyError at draw
# time rather than a silently English label in a Finnish diagram.
EN = {
    "n_title": "One real night, as the watch recorded it",
    "n_sub": "Movement per minute (VMC), 2026-07-31 21:50 to 2026-08-01 08:30",
    "n_onset": "sleep session starts 04:05",
    "n_window": "window 07:20 → alarm 07:50",
    "n_level": "trigger level 587  =  the 90th percentile of this night's own minutes",
    "n_pop": "the ranking population: 175 minutes, median 0, 86 % of them exactly 0",
    "n_fire": "rings 07:22",
    "n_alt": "Movement through one night, with the trigger level and the moment "
             "the alarm rang",
    "n_foot": "Two still minutes, then one turn-over clears a threshold the night "
              "itself set. Nothing here is drawn to illustrate a point: the bars "
              "are the watch's own data.",

    "s_title": "What the alarm time means",
    "s_sub": "A 07:50 alarm with a 30-minute window. T is the time you set.",
    "s_t": "T = 07:50",
    "s_rows": [("The latest", "may ring earlier", "may ring anywhere in here",
                "Watches 07:20-07:50 and rings at the first good moment; "
                "07:50 at the latest."),
               ("The earliest", "may ring later", "may ring anywhere in here",
                "Never rings before 07:50, and rings by 08:20 whatever happens."),
               ("Fully awake by", "starts early", "the ramp builds",
                "Starts early enough that the alarm is at full strength at 07:50.")],
    "s_alt": "The three meanings of the alarm time",
    "s_foot": "Switching the smart alarm off makes all three ring at exactly T.",

    "e_title": "How the ringing escalates",
    "e_sub": "The Normal profile, on a watch with a speaker. Time from the first buzz.",
    "e_off": "ramp off (default)",
    "e_on": "ramp on",
    "e_panels": ["Vibration pulse length (ms)", "Gap between bursts (s)",
                 "Sound volume (identical either way)"],
    "e_sound": "sound joins",
    "e_cap": "cap: silence after 15 min",
    "e_ticks": ["0", "5 min", "10 min", "15 min"],
    "e_alt": "How vibration and sound escalate during a ring",
    "e_foot": "With the ramp off -- the default -- the vibration is at full strength "
              "and a constant 30 s apart from the first buzz. Only the sound ramps.",

    "p_title": "The app is asleep almost all night",
    "p_sub": "It owns no background thread. Everything below is the watch waking it "
             "up on a timer it set itself.",
    "p_arm": ["you set an alarm;", "the app arms its wakeups"],
    "p_dst": ["clock check: re-arms", "everything after a clock shift"],
    "p_win": ["window open: the app wakes every 3 minutes,",
              "looks at the movement history, and rings at the",
              "first good moment -- or at 07:50 regardless"],
    "p_alt": "When the alarm app actually runs during a night",
    "p_foot": ["Between those moments the app is not running, and that is why the "
               "movement data is READ BACK from the firmware's own history rather than",
               "collected live: the watch was recording all night whether the app was "
               "awake or not."],
}

FI = {
    "n_title": "Yksi oikea yö, sellaisena kuin kello sen tallensi",
    "n_sub": "Liikettä minuutissa (VMC), 31.7.2026 21:50 – 1.8.2026 08:30",
    "n_onset": "unijakso alkaa 04:05",
    "n_window": "ikkuna 07:20 → herätys 07:50",
    "n_level": "laukaisukynnys 587  =  tämän yön omien minuuttien 90. persentiili",
    "n_pop": "vertailujoukko: 175 minuuttia, mediaani 0, 86 % niistä tasan 0",
    "n_fire": "soi 07:22",
    "n_alt": "Liike yhden yön aikana, laukaisukynnys ja hetki jolloin herätys soi",
    "n_foot": "Kaksi hiljaista minuuttia, sitten yksi kääntyminen ylittää kynnyksen "
              "jonka yö itse asetti. Mitään ei ole piirretty havainnollistamaan "
              "väitettä: palkit ovat kellon omaa dataa.",

    "s_title": "Mitä herätysaika tarkoittaa",
    "s_sub": "Herätys 07:50 ja 30 minuutin ikkuna. T on asettamasi aika.",
    "s_t": "T = 07:50",
    "s_rows": [("Viimeisin hetki", "voi soida aiemmin", "voi soida missä tahansa tässä",
                "Tarkkailee 07:20–07:50 ja soittaa ensimmäisellä hyvällä hetkellä; "
                "viimeistään 07:50."),
               ("Aikaisin hetki", "voi soida myöhemmin", "voi soida missä tahansa tässä",
                "Ei soi koskaan ennen 07:50, ja soi viimeistään 08:20."),
               ("Silloin täysin hereillä", "alkaa aiemmin", "voimistuminen rakentuu",
                "Alkaa niin aikaisin, että herätys on täydessä voimassaan 07:50.")],
    "s_alt": "Herätysajan kolme merkitystä",
    "s_foot": "Älyherätyksen sammuttaminen saa kaikki kolme soimaan tasan hetkellä T.",

    "e_title": "Miten soitto voimistuu",
    "e_sub": "Normal-profiili kaiuttimellisessa kellossa. Aika ensimmäisestä värinästä.",
    "e_off": "ramppi pois (oletus)",
    "e_on": "ramppi päällä",
    "e_panels": ["Värinäsykäyksen pituus (ms)", "Tauko purskeiden välillä (s)",
                 "Äänenvoimakkuus (sama kummallakin)"],
    "e_sound": "ääni liittyy mukaan",
    "e_cap": "katto: hiljenee 15 min jälkeen",
    "e_ticks": ["0", "5 min", "10 min", "15 min"],
    "e_alt": "Miten värinä ja ääni voimistuvat soiton aikana",
    "e_foot": "Ramppi pois päältä – oletus – värinä on täydellä voimalla ja tasan 30 s "
              "välein ensimmäisestä purskeesta lähtien. Vain ääni voimistuu.",

    "p_title": "Sovellus nukkuu lähes koko yön",
    "p_sub": "Sillä ei ole taustasäiettä. Kaikki alla oleva on kello herättämässä sen "
             "ajastimella, jonka se itse asetti.",
    "p_arm": ["asetat herätyksen; sovellus", "virittää herätyksensä"],
    "p_dst": ["kellotarkistus: virittää kaiken", "uudelleen kellonsiirron jälkeen"],
    "p_win": ["ikkuna auki: sovellus herää 3 minuutin välein,",
              "katsoo liikehistoriaa ja soittaa ensimmäisellä",
              "hyvällä hetkellä – tai 07:50 joka tapauksessa"],
    "p_alt": "Milloin herätyssovellus todella käy yön aikana",
    "p_foot": ["Näiden hetkien välissä sovellus ei ole käynnissä, ja juuri siksi "
               "liikedata LUETAAN JÄLKIKÄTEEN laiteohjelmiston omasta historiasta",
               "eikä kerätä livenä: kello tallensi koko yön riippumatta siitä, oliko "
               "sovellus hereillä."],
}


def svg(w, h, body, title):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" '
            f'width="{w}" height="{h}" role="img" aria-label="{title}">'
            f"{STYLE}<rect class=\"sfc\" width=\"{w}\" height=\"{h}\"/>{body}</svg>\n")


def txt(x, y, s, cls="ink", size=13, anchor="start", weight="normal"):
    return (f'<text x="{x:.1f}" y="{y:.1f}" class="{cls}" font-size="{size}" '
            f'text-anchor="{anchor}" font-weight="{weight}">{s}</text>')


def line(x1, y1, x2, y2, cls="grid", w=1, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
            f'class="{cls}" stroke-width="{w}"{d}/>')


def rect(x, y, w, h, cls, rx=0):
    return (f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.2f}" height="{h:.2f}" '
            f'class="{cls}" rx="{rx}"/>')


HERE = os.path.dirname(os.path.abspath(__file__))


# --- 1. one real night -------------------------------------------------------
def night(S):
    vals = []
    for ln in open(os.path.join(HERE, "night-2026-08-01.txt")):
        if not ln.startswith("#"):
            vals += [int(v) for v in ln.split()]
    assert len(vals) == 640

    ONSET, WIN, RING, FIRE = 375, 570, 600, 572   # indices; 0 == 21:50
    SETTLE, LEVEL = 20, 587                        # settle_minutes, P90 of the night

    W, H = 900, 410
    L, R, T, B = 62, 878, 66, 310
    xs = lambda i: L + i * (R - L) / len(vals)
    top = 9600
    ys = lambda v: B - v * (B - T) / top

    b = [txt(L, 22, S["n_title"], size=15, weight="600"),
         txt(L, 38, S["n_sub"], cls="ink2", size=12)]

    # the alarm window, drawn behind everything
    b.append(rect(xs(WIN), T, xs(RING) - xs(WIN), B - T, "band"))

    for v in (0, 2000, 4000, 6000, 8000):
        b.append(line(L, ys(v), R, ys(v)))
        b.append(txt(L - 8, ys(v) + 4, f"{v:,}".replace(",", " "), cls="ink2",
                     size=11, anchor="end"))

    for i, lab in ((10, "22:00"), (130, "00:00"), (250, "02:00"), (370, "04:00"),
                   (490, "06:00"), (550, "07:00"), (610, "08:00")):
        b.append(line(xs(i), B, xs(i), B + 4))
        b.append(txt(xs(i), B + 18, lab, cls="ink2", size=11, anchor="middle"))

    bw = (R - L) / len(vals)
    for i, v in enumerate(vals):
        if v <= 0:
            continue
        b.append(rect(xs(i), ys(v), max(bw - 0.15, 0.8), B - ys(v), "s1"))

    # the ranking population and the level it produced
    b.append(line(L, B + 30, R, B + 30, "grid"))
    b.append(line(xs(ONSET + SETTLE), B + 26, xs(ONSET + SETTLE), B + 34, "s3s", 2))
    b.append(line(xs(WIN), B + 26, xs(WIN), B + 34, "s3s", 2))
    b.append(line(xs(ONSET + SETTLE), B + 30, xs(WIN), B + 30, "s3s", 2))
    b.append(txt((xs(ONSET + SETTLE) + xs(WIN)) / 2, B + 48, S["n_pop"],
                 cls="ink2", size=11, anchor="middle"))

    b.append(line(L, ys(LEVEL), R, ys(LEVEL), "s2s", 2, dash="6 4"))
    # On a backing plate: there is no stretch of this axis wide enough for the
    # label that is also free of bars, and a label read through the data is not a
    # label. Plate first, text on top.
    lab = S["n_level"]
    b.append(rect(xs(WIN) - 12 - len(lab) * 5.6, ys(LEVEL) - 21,
                  len(lab) * 5.6 + 6, 17, "sfc", rx=2))
    b.append(txt(xs(WIN) - 12, ys(LEVEL) - 8, lab, cls="ink2", size=11, anchor="end"))

    b.append(line(xs(ONSET), T, xs(ONSET), B, "grid", 1, dash="3 3"))
    b.append(txt(xs(ONSET) + 5, T - 8, S["n_onset"], cls="ink2", size=11))

    # One label for the band rather than two that crowd each other at the edge.
    b.append(txt(R, T - 8, S["n_window"], cls="ink2", size=11, anchor="end"))

    b.append(line(xs(FIRE), ys(vals[FIRE]) - 26, xs(FIRE), ys(vals[FIRE]) - 6, "s2s", 2))
    b.append(f'<circle cx="{xs(FIRE):.1f}" cy="{ys(vals[FIRE]) - 30:.1f}" r="5" class="s2"/>')
    b.append(txt(xs(FIRE) - 10, ys(vals[FIRE]) - 38, S["n_fire"], cls="ink2", size=12,
                 anchor="end", weight="600"))

    b.append(txt(L, H - 10, S["n_foot"], cls="ink2", size=11))
    return svg(W, H, "".join(b), S["n_alt"])


# --- 2. what the alarm time means --------------------------------------------
def semantics(S):
    W, H = 900, 330
    L, R = 150, 870
    b = [txt(24, 24, S["s_title"], size=15, weight="600"),
         txt(24, 42, S["s_sub"], cls="ink2", size=12)]
    spans = [(-30, 0), (0, 30), (-30, 0)]
    # minutes -40..+40 across the row
    xs = lambda m: L + (m + 40) * (R - L) / 80
    for i, (name, sub, inside, note) in enumerate(S["s_rows"]):
        a, z = spans[i]
        y = 92 + i * 72
        b.append(txt(24, y + 4, name, size=13, weight="600"))
        b.append(txt(24, y + 20, sub, cls="ink2", size=11))
        b.append(line(L, y, R, y, "grid"))
        b.append(rect(xs(a), y - 11, xs(z) - xs(a), 22, "band", rx=3))
        b.append(txt((xs(a) + xs(z)) / 2, y + 5, inside, cls="ink2", size=11,
                     anchor="middle"))
        b.append(line(xs(0), y - 20, xs(0), y + 20, "s2s", 2))
        # Under the row, never to its right: a right-hand column runs off the canvas.
        b.append(txt(L, y + 32, note, cls="ink2", size=11))
    b.append(txt(xs(0), 74, S["s_t"], cls="ink2", size=11, anchor="middle"))
    b.append(txt(24, H - 14, S["s_foot"], cls="ink2", size=11))
    return svg(W, H, "".join(b), S["s_alt"])


# --- 3. how the ringing escalates --------------------------------------------
def escalation(S):
    CAP, TIGHTEN, SOUND_AFTER, SOUND_RAMP = 900, 360, 300, 300

    def lerp(a, z, t, den):
        return a + (z - a) * min(t, den) / den

    W = 900
    L, R = 70, 700
    shapes = [(0, 840, "700 ms", lambda t: 700, lambda t: lerp(80, 700, t, TIGHTEN)),
              (0, 42, "30 s", lambda t: 30, lambda t: lerp(30, 5, t, TIGHTEN)),
              (0, 120, "100",
               lambda t: 0 if t < SOUND_AFTER else lerp(15, 100, t - SOUND_AFTER, SOUND_RAMP),
               lambda t: 0 if t < SOUND_AFTER else lerp(15, 100, t - SOUND_AFTER, SOUND_RAMP))]
    ph, gap = 84, 34
    H = 92 + len(shapes) * (ph + gap)
    b = [txt(24, 24, S["e_title"], size=15, weight="600"),
         txt(24, 42, S["e_sub"], cls="ink2", size=12)]
    # legend
    b.append(rect(560, 16, 22, 3, "s1", rx=1.5))
    b.append(txt(590, 23, S["e_off"], cls="ink2", size=11))
    b.append(rect(560, 34, 22, 3, "s2", rx=1.5))
    b.append(txt(590, 41, S["e_on"], cls="ink2", size=11))

    xs = lambda t: L + t * (R - L) / CAP
    for pi, (lo, hi, endlab, f_off, f_on) in enumerate(shapes):
        top = 78 + pi * (ph + gap)
        bot = top + ph
        ys = lambda v: bot - (v - lo) * (bot - top) / (hi - lo)
        b.append(txt(L, top - 8, S["e_panels"][pi], cls="ink2", size=12))
        b.append(line(L, bot, R, bot, "grid"))
        b.append(txt(L - 8, bot + 4, str(lo), cls="ink2", size=10, anchor="end"))
        # Direct end label instead of an axis maximum nothing reaches.
        b.append(txt(R + 8, ys(f_off(CAP)) + 4, endlab, cls="ink2", size=11))
        for cls, f in (("s2s", f_on), ("s1s", f_off)):
            pts = " ".join(f"{xs(t):.1f},{ys(f(t)):.1f}" for t in range(0, CAP + 1, 10))
            b.append(f'<polyline points="{pts}" fill="none" class="{cls}" '
                     f'stroke-width="2" stroke-linejoin="round"/>')
        if pi == len(shapes) - 1:
            for t, lab in zip((0, 300, 600, 900), S["e_ticks"]):
                b.append(line(xs(t), bot, xs(t), bot + 4))
                b.append(txt(xs(t), bot + 18, lab, cls="ink2", size=10, anchor="middle"))
        b.append(line(xs(SOUND_AFTER), top, xs(SOUND_AFTER), bot, "grid", 1, dash="3 3"))
    b.append(txt(xs(SOUND_AFTER) + 6, 70, S["e_sound"], cls="ink2", size=11))
    b.append(txt(R + 14, 70, S["e_cap"], cls="ink2", size=11))
    b.append(txt(24, H - 12, S["e_foot"], cls="ink2", size=11))
    return svg(W, H, "".join(b), S["e_alt"])


# --- 4. when the app is actually running -------------------------------------
def process(S):
    W, H = 900, 250
    L, R, Y = 70, 850, 130
    b = [txt(24, 24, S["p_title"], size=15, weight="600"),
         txt(24, 42, S["p_sub"], cls="ink2", size=12)]
    b.append(line(L, Y, R, Y, "grid", 2))
    # 22:00 .. 08:00 == 600 minutes
    xs = lambda m: L + m * (R - L) / 600
    # The window's two marks are 30 min apart on a 10-hour axis, so they get ONE
    # caption block between them rather than two that would sit on top of each
    # other -- which is exactly what the first draft did.
    marks = [(0, "22:00", S["p_arm"], "s1"), (300, "03:00", S["p_dst"], "s1"),
             (560, "07:20", [], "s3"), (590, "07:50", [], "s2")]
    b.append(rect(xs(560), Y - 8, xs(590) - xs(560), 16, "band", rx=3))
    for m, when, what, cls in marks:
        b.append(line(xs(m), Y - 16, xs(m), Y + 16, cls + "s", 2))
        b.append(f'<circle cx="{xs(m):.1f}" cy="{Y:.1f}" r="5" class="{cls}"/>')
        b.append(txt(xs(m), Y - 26, when, size=12, anchor="middle", weight="600"))
        for i, ln in enumerate(what):
            b.append(txt(xs(m), Y + 34 + i * 15, ln, cls="ink2", size=11, anchor="middle"))
    # ABOVE the axis and right-anchored: there is no room to the right of the last
    # mark, and below it would land on the 03:00 caption.
    for i, ln in enumerate(S["p_win"]):
        b.append(txt(R, Y - 76 + i * 15, ln, cls="ink2", size=11, anchor="end"))
    for i, ln in enumerate(S["p_foot"]):
        b.append(txt(24, H - 40 + i * 16, ln, cls="ink2", size=11))
    return svg(W, H, "".join(b), S["p_alt"])


for suffix, strings in (("", EN), (".fi", FI)):
    for name, fn in (("night", night), ("semantics", semantics),
                     ("escalation", escalation), ("process", process)):
        path = os.path.join(HERE, name + suffix + ".svg")
        open(path, "w").write(fn(strings))
        print("wrote", path)
