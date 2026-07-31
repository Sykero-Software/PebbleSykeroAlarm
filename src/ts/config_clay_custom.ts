// SPDX-License-Identifier: GPL-3.0-only
//
// Clay serialises this function with toSource() and re-evals it isolated in the
// config webview. Anything it references that lives in module scope — including
// TypeScript downlevel helpers such as __spreadArray or __assign — is undefined
// there and blanks the page. So: no spread, no destructuring, no module-scope
// helper calls. Only `this`, locals, and native array methods.
export default function clayConfigCustom(this: any, minified: any): void {
  const clay = this;

  // The eight-slot progressive reveal is gone: the alarmList component adds and
  // deletes its own rows, so there is nothing to show or hide per slot. It also
  // removes that logic's dependence on a `change` event firing from the Android
  // time picker, which was never verified and would have stalled the reveal.

  // A fixed Save bar, so an edit cannot be lost by leaving a long page without
  // reaching the button at the bottom -- which is exactly what happened on the
  // phone 2026-07-31 (an alarm looked like it had been deleted and had not been).
  function injectFloatingSaveStyle(): void {
    if (typeof document === 'undefined') { return; }
    if (document.getElementById('sa-floating-save')) { return; }
    const style = document.createElement('style');
    style.id = 'sa-floating-save';
    // The clearance goes on #main-form (the in-flow scrolling content), NOT body:
    // Clay sets html,body{height:100%} with border-box, so a body padding-bottom
    // sits INSIDE the fixed-height body box at the first screen's bottom and never
    // clears the fixed bar -- leaving the last setting permanently unreachable.
    // (TimeStyle shipped that bug once; 96px vs a bar of ~72px.)
    style.textContent =
      '.component-submit{position:fixed;bottom:0;left:0;right:0;margin:0;'
      + 'z-index:100;background:#262626;padding:8px 0;'
      + 'box-shadow:0 -2px 6px rgba(0,0,0,0.4);}'
      + '#main-form{padding-bottom:96px;}';
    document.head.appendChild(style);
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
    injectFloatingSaveStyle();
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
