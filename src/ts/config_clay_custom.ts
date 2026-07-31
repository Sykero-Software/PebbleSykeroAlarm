// SPDX-License-Identifier: GPL-3.0-only
//
// Clay serialises this function with toSource() and re-evals it isolated in the
// config webview. Anything it references that lives in module scope — including
// TypeScript downlevel helpers such as __spreadArray or __assign — is undefined
// there and blanks the page. So: no spread, no destructuring, no module-scope
// helper calls. Only `this`, locals, and native array methods.
export default function clayConfigCustom(this: any, minified: any): void {
  const clay = this;
  const SLOTS = 8;

  function apply() {
    let reveal = true;
    for (let i = 1; i <= SLOTS; i++) {
      const on = clay.getItemById('slot' + i + '-on');
      const time = clay.getItemById('slot' + i + '-time');
      const days = clay.getItemById('slot' + i + '-days');
      if (!on || !time || !days) {
        continue;
      }
      if (reveal) {
        on.show();
        time.show();
        days.show();
      } else {
        on.hide();
        time.hide();
        days.hide();
      }
      // The next slot is revealed only once this one has a time. Hidden items
      // still serialise, so saving is unaffected either way.
      const v = time.get();
      reveal = typeof v === 'string' && v.length > 0;
    }
  }

  function applyAdvanced() {
    // Custom sensitivity fields only when Sensitivity == Custom.
    const sens = clay.getItemById('smart-sens');
    const sensCustom = sens && String(sens.get()) === '3';
    const sensIds = ['sens-pct', 'sens-min'];
    for (let i = 0; i < sensIds.length; i++) {
      const it = clay.getItemById(sensIds[i]);
      if (it) { if (sensCustom) { it.show(); } else { it.hide(); } }
    }

    // Custom escalation fields only when Wake style == Custom.
    const prof = clay.getItemById('wake-profile');
    const profCustom = prof && String(prof.get()) === '3';
    const escIds = ['esc-lead', 'esc-min', 'esc-tighten', 'esc-vibstart',
                    'esc-vibmax', 'esc-pstart', 'esc-pmax', 'esc-sndafter',
                    'esc-sndramp', 'esc-volstart', 'esc-volmax', 'esc-cap'];
    for (let i = 0; i < escIds.length; i++) {
      const it = clay.getItemById(escIds[i]);
      if (it) { if (profCustom) { it.show(); } else { it.hide(); } }
    }

    // Four of those describe the vibration RAMP only: the gap it tightens to, the
    // time it tightens over, and the pulse length/count it starts from. With the
    // ramp off they control nothing, so hide them even in Custom. The remaining
    // three (esc-lead, esc-vibmax, esc-pmax) are the flat buzz itself.
    const rampItem = clay.getItemById('esc-ramp');
    const ramping = rampItem && rampItem.get() === true;
    const rampOnlyIds = ['esc-min', 'esc-tighten', 'esc-vibstart', 'esc-pstart'];
    for (let i = 0; i < rampOnlyIds.length; i++) {
      const it = clay.getItemById(rampOnlyIds[i]);
      if (it) { if (profCustom && ramping) { it.show(); } else { it.hide(); } }
    }

    // Smart-alarm detail only when the smart alarm is on.
    const smartOn = clay.getItemById('smart-on');
    const smart = smartOn && smartOn.get() === true;
    const smartIds = ['smart-window', 'smart-semantics', 'smart-sens'];
    for (let i = 0; i < smartIds.length; i++) {
      const it = clay.getItemById(smartIds[i]);
      if (it) { if (smart) { it.show(); } else { it.hide(); } }
    }
    if (!smart) {
      for (let i = 0; i < sensIds.length; i++) {
        const it = clay.getItemById(sensIds[i]);
        if (it) { it.hide(); }
      }
    }
  }

  clay.on(clay.EVENTS.AFTER_BUILD, function () {
    apply();
    for (let i = 1; i <= SLOTS; i++) {
      const time = clay.getItemById('slot' + i + '-time');
      if (time) {
        time.on('change', apply);
      }
    }

    applyAdvanced();
    const sensItem = clay.getItemById('smart-sens');
    if (sensItem) {
      sensItem.on('change', applyAdvanced);
    }
    const profItem = clay.getItemById('wake-profile');
    if (profItem) {
      profItem.on('change', applyAdvanced);
    }
    const rampToggle = clay.getItemById('esc-ramp');
    if (rampToggle) {
      rampToggle.on('change', applyAdvanced);
    }
    const smartOnItem = clay.getItemById('smart-on');
    if (smartOnItem) {
      smartOnItem.on('change', applyAdvanced);
    }
  });
}
