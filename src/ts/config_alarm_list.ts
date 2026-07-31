// SPDX-License-Identifier: GPL-3.0-only

/* Clay custom component "alarmList": the alarm list, one row per alarm, with an
   "Add alarm" button and a ✕ per row. Replaces the eight fixed Slot1..Slot8 blocks
   and their progressive-reveal logic (only the next empty slot was ever visible, so
   the page looked like it held two alarms rather than eight).

   The component's VALUE IS THE WIRE STRING itself -- "07:00|1111100;-08:30|0000011",
   exactly what the AlarmSet message key carries. That removes the whole
   slots-to-wire packing layer from the save path, and it means the value the page
   holds and the value the watch receives cannot drift apart.

   Clay serializes this object with toSource() and re-evals it inside the config
   webview, so EVERY function here MUST be self-contained: no module-scope helper
   (not even the packer in alarm_pack.ts), no import at runtime, no TS downlevel
   helper (__spreadArray / _this), no spread, no destructuring, no for...of.
   Native DOM and native array methods only. A call to a module-scope sibling throws
   ReferenceError in the webview, which blanks the config page.

   Note one useful property of reading the DOM in manipulator.get(): the saved value
   does not depend on a `change` event having fired. The Android WebView's native
   time picker is not trustworthy about that (it is why the old reveal logic could
   stall), and here it simply does not matter. */

function alarmListInitialize(this: any, _minified: any, _clayConfig: any): void {
  const self = this;
  const root: HTMLElement = self.$element[0];
  const MAX = 8;                    // MUST match SLOT_COUNT / MAX_ALARMS. Inlined:
                                    // an import would be undefined in the webview.
  const DAY_LABELS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];

  // Repeat patterns, Monday first. 'custom' has no fixed mask -- it reveals the
  // seven checkboxes instead, which is what keeps a row two lines tall rather than
  // nine.
  const PATTERNS: { code: string; label: string; mask: string }[] = [
    { code: 'daily',    label: 'Every day', mask: '1111111' },
    { code: 'weekdays', label: 'Mon-Fri',   mask: '1111100' },
    { code: 'weekend',  label: 'Sat-Sun',   mask: '0000011' },
    { code: 'once',     label: 'One-time',  mask: '0000000' },
    { code: 'custom',   label: 'Custom',    mask: '' },
  ];

  function isValidMask(m: any): boolean {
    if (typeof m !== 'string' || m.length !== 7) { return false; }
    for (let i = 0; i < 7; i++) {
      if (m.charAt(i) !== '0' && m.charAt(i) !== '1') { return false; }
    }
    return true;
  }

  // "HH:MM" -> normalized "HH:MM", or '' when it is not a time of day. Hand-rolled
  // rather than a shared helper, for the toSource reason above.
  function normTime(t: any): string {
    if (typeof t !== 'string' || t.length === 0) { return ''; }
    const colon = t.indexOf(':');
    if (colon < 1) { return ''; }
    const hs = t.substring(0, colon);
    const ms = t.substring(colon + 1, colon + 3);
    if (!/^\d{1,2}$/.test(hs) || !/^\d{2}$/.test(ms)) { return ''; }
    const h = parseInt(hs, 10);
    const m = parseInt(ms, 10);
    if (isNaN(h) || isNaN(m) || h < 0 || h > 23 || m < 0 || m > 59) { return ''; }
    return (h < 10 ? '0' + h : '' + h) + ':' + (m < 10 ? '0' + m : '' + m);
  }

  function patternCodeForMask(mask: string): string {
    for (let i = 0; i < PATTERNS.length; i++) {
      if (PATTERNS[i].mask !== '' && PATTERNS[i].mask === mask) {
        return PATTERNS[i].code;
      }
    }
    return 'custom';
  }

  function maskForPatternCode(code: string): string {
    for (let i = 0; i < PATTERNS.length; i++) {
      if (PATTERNS[i].code === code) { return PATTERNS[i].mask; }
    }
    return '';
  }

  // --- wire string <-> row objects {enabled, time, mask} ---

  function valueToRows(packed: any): any[] {
    const rows: any[] = [];
    if (typeof packed !== 'string' || packed.length === 0) { return rows; }
    const chunks = packed.split(';');
    for (let i = 0; i < chunks.length && rows.length < MAX; i++) {
      let c = chunks[i];
      if (typeof c !== 'string' || c.length === 0) { continue; }
      let enabled = true;
      if (c.charAt(0) === '-') { enabled = false; c = c.substring(1); }
      const bar = c.indexOf('|');
      if (bar < 0) { continue; }
      const time = normTime(c.substring(0, bar));
      const mask = c.substring(bar + 1);
      if (time === '' || !isValidMask(mask)) { continue; }
      rows.push({ enabled: enabled, time: time, mask: mask });
    }
    return rows;
  }

  function rowsToValue(rows: any[]): string {
    const out: string[] = [];
    for (let i = 0; i < rows.length && out.length < MAX; i++) {
      const r = rows[i];
      if (!r) { continue; }
      const time = normTime(r.time);
      // A row whose time was cleared cannot be expressed on the wire, so it is
      // dropped -- the same rule the old packer used. New rows are seeded with a
      // time, so this only happens if the user empties one deliberately.
      if (time === '') { continue; }
      const mask = isValidMask(r.mask) ? r.mask : '0000000';
      out.push((r.enabled ? '' : '-') + time + '|' + mask);
    }
    return out.join(';');
  }

  // --- rendering ---

  function daysHtml(mask: string): string {
    let html = '';
    for (let d = 0; d < 7; d++) {
      const on = mask.charAt(d) === '1';
      html += '<label class="al-day"><input type="checkbox" class="al-daybox"'
            + (on ? ' checked' : '') + '><span>' + DAY_LABELS[d] + '</span></label>';
    }
    return html;
  }

  function repeatHtml(code: string): string {
    let html = '';
    for (let i = 0; i < PATTERNS.length; i++) {
      const p = PATTERNS[i];
      html += '<option value="' + p.code + '"'
            + (p.code === code ? ' selected' : '') + '>' + p.label + '</option>';
    }
    return html;
  }

  function rowHtml(row: any): string {
    const time = normTime(row.time) || '07:00';
    const mask = isValidMask(row.mask) ? row.mask : '1111100';
    const code = patternCodeForMask(mask);
    const on = row.enabled !== false;
    return '<div class="al-row">'
      + '<div class="al-line">'
      + '<input type="time" class="al-time" value="' + time + '">'
      + '<button type="button" class="al-on' + (on ? '' : ' al-offstate')
        + '" title="Enable or disable this alarm">' + (on ? 'On' : 'Off') + '</button>'
      + '<button type="button" class="al-del" title="Delete this alarm">&#10005;</button>'
      + '</div>'
      + '<select class="al-rep">' + repeatHtml(code) + '</select>'
      + '<div class="al-days-wrap"' + (code === 'custom' ? '' : ' style="display:none"')
        + '>' + daysHtml(mask) + '</div>'
      + '</div>';
  }

  // Read the DOM back into row objects. The checkboxes are kept in sync with the
  // pattern even while hidden, so switching to Custom starts from what the pattern
  // had rather than from nothing.
  function readRows(): any[] {
    const rows: any[] = [];
    const els = root.querySelectorAll('.al-row');
    for (let i = 0; i < els.length; i++) {
      const el = els[i] as HTMLElement;
      const timeEl = el.querySelector('.al-time') as HTMLInputElement;
      const repEl = el.querySelector('.al-rep') as HTMLSelectElement;
      const onEl = el.querySelector('.al-on') as HTMLButtonElement;
      const code = repEl ? repEl.value : 'weekdays';
      let mask = maskForPatternCode(code);
      if (code === 'custom' || mask === '') {
        let bits = '';
        const boxes = el.querySelectorAll('.al-daybox');
        for (let d = 0; d < 7; d++) {
          const b = boxes[d] as HTMLInputElement;
          bits += (b && b.checked) ? '1' : '0';
        }
        mask = bits;
      }
      rows.push({
        enabled: !(onEl && onEl.classList.contains('al-offstate')),
        time: timeEl ? timeEl.value : '',
        mask: mask,
      });
    }
    return rows;
  }

  function renderRows(rows: any[]): void {
    const list = root.querySelector('.al-list') as HTMLElement;
    let html = '';
    for (let i = 0; i < rows.length && i < MAX; i++) { html += rowHtml(rows[i]); }
    list.innerHTML = html;
    updateChrome(rows.length);
  }

  function updateChrome(count: number): void {
    const add = root.querySelector('.al-add') as HTMLButtonElement;
    if (add) { add.style.display = (count >= MAX) ? 'none' : ''; }
    const empty = root.querySelector('.al-empty') as HTMLElement;
    if (empty) { empty.style.display = (count === 0) ? '' : 'none'; }
  }

  // Exposed for the manipulator, which runs after initialize.
  self._alGetValue = function (): string { return rowsToValue(readRows()); };
  self._alRebuild = function (packed: string): void { renderRows(valueToRows(packed)); };

  function rowOf(el: HTMLElement): HTMLElement | null {
    let n: HTMLElement | null = el;
    while (n && n !== root) {
      if (n.classList && n.classList.contains('al-row')) { return n; }
      n = n.parentNode as HTMLElement;
    }
    return null;
  }

  function indexOfRow(node: Node | null): number {
    const els = root.querySelectorAll('.al-row');
    for (let i = 0; i < els.length; i++) { if (els[i] === node) { return i; } }
    return -1;
  }

  root.addEventListener('click', function (ev: Event) {
    let target = ev.target as HTMLElement;
    if (!target) { return; }
    if (target.tagName !== 'BUTTON') {
      target = target.closest ? (target.closest('button') as HTMLElement) : (null as any);
    }
    if (!target) { return; }

    if (target.classList.contains('al-add')) {
      const rows = readRows();
      if (rows.length < MAX) {
        rows.push({ enabled: true, time: '07:00', mask: '1111100' });
        renderRows(rows);
        self.trigger('change');
      }
      return;
    }

    // Toggling enabled is a class flip on the button, not a re-render: re-rendering
    // would rebuild every <input type="time"> in the list and could drop a value the
    // user is part-way through entering.
    if (target.classList.contains('al-on')) {
      const nowOff = !target.classList.contains('al-offstate');
      if (nowOff) { target.classList.add('al-offstate'); } else { target.classList.remove('al-offstate'); }
      target.innerHTML = nowOff ? 'Off' : 'On';
      self.trigger('change');
      return;
    }

    if (target.classList.contains('al-del')) {
      const row = rowOf(target);
      const idx = row ? indexOfRow(row) : -1;
      if (idx === -1) { return; }
      const rows = readRows();
      rows.splice(idx, 1);
      renderRows(rows);
      self.trigger('change');
    }
  });

  root.addEventListener('change', function (ev: Event) {
    const t = ev.target as HTMLElement;
    if (!t) { return; }
    // Switching the repeat pattern shows or hides that row's weekday checkboxes,
    // and seeds them from the pattern being left behind. Done in place for the same
    // reason as the enabled toggle above.
    if (t.tagName === 'SELECT' && t.classList.contains('al-rep')) {
      const row = rowOf(t);
      if (row) {
        const code = (t as HTMLSelectElement).value;
        const wrap = row.querySelector('.al-days-wrap') as HTMLElement;
        if (code === 'custom') {
          if (wrap) { wrap.style.display = ''; }
        } else {
          const mask = maskForPatternCode(code);
          const boxes = row.querySelectorAll('.al-daybox');
          for (let d = 0; d < 7; d++) {
            const b = boxes[d] as HTMLInputElement;
            if (b) { b.checked = mask.charAt(d) === '1'; }
          }
          if (wrap) { wrap.style.display = 'none'; }
        }
      }
    }
    self.trigger('change');
  });

  // A time input on Android can report edits as `input` rather than `change`; both
  // are forwarded so Clay's dirty tracking sees them. The saved value never depends
  // on either -- manipulator.get() reads the DOM.
  root.addEventListener('input', function () { self.trigger('change'); });
}

const alarmListComponent = {
  name: 'alarmList',
  template:
    '<div class="al-root">'
    + '<div class="al-list"></div>'
    + '<div class="al-empty">No alarms. Add one below.</div>'
    + '<button type="button" class="al-add">+ Add alarm</button>'
    + '</div>',
  // Clay's base theme sets `button { min-width: 12rem; margin: 0 auto }`, so the row
  // buttons MUST override min-width -- otherwise each is forced to 12rem, overflows
  // the row and squeezes the time input to zero width. A raw <input>/<select> also
  // renders in the OS LIGHT theme inside the Core app's dark config page, hence the
  // explicit greys (Clay: body gray-2 #333, controls gray-7 #767676).
  style:
    '.al-row{display:flex;flex-direction:column;margin:0 0 10px 0;'
      + 'padding:0 0 8px 0;border-bottom:1px solid #444}'
    + '.al-line{display:flex;align-items:center}'
    + '.al-row .al-time{flex:1 1 auto;min-width:0;height:2.8rem;margin:0;'
      + 'background-color:#767676;color:#fff;border:none;border-radius:0.3rem;'
      + 'padding:0 0.5rem;font-size:1.1rem;color-scheme:dark}'
    + '.al-row .al-line button{flex:0 0 auto;min-width:0;height:2.8rem;'
      + 'margin:0 0 0 6px;padding:0}'
    + '.al-row .al-on{width:3.6rem}'
    + '.al-row .al-del{width:2.8rem}'
    + '.al-row .al-offstate{opacity:.5}'
    + '.al-row .al-rep{width:100%;min-width:0;height:2.6rem;margin:6px 0 0 0;'
      + 'background-color:#767676;color:#fff;border:none;border-radius:0.3rem;'
      + 'padding:0 0.5rem;color-scheme:dark}'
    + '.al-days-wrap{display:flex;flex-wrap:wrap;margin:6px 0 0 0}'
    + '.al-day{display:flex;align-items:center;margin:0 10px 4px 0;color:#fff}'
    + '.al-day input{margin:0 4px 0 0}'
    + '.al-empty{color:#aaa;margin:0 0 8px 0}'
    + '.al-add{min-width:0;margin:8px 0 4px 0}',
  manipulator: {
    get: function (this: any): string {
      return this._alGetValue ? this._alGetValue() : '';
    },
    set: function (this: any, value: any) {
      // Inlined normalization (toSource: no module-scope helper). Accepts the wire
      // string, or a {value:X} wrapper; anything else means "no alarms yet".
      let v: any = value;
      if (v && typeof v === 'object' && !Array.isArray(v) && v.value !== undefined) {
        v = v.value;
      }
      if (this._alRebuild) { this._alRebuild(typeof v === 'string' ? v : ''); }
      return this;
    },
  },
  defaults: { label: '' },
  initialize: alarmListInitialize,
};

export = alarmListComponent;
