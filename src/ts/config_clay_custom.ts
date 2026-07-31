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

  clay.on(clay.EVENTS.AFTER_BUILD, function () {
    apply();
    for (let i = 1; i <= SLOTS; i++) {
      const time = clay.getItemById('slot' + i + '-time');
      if (time) {
        time.on('change', apply);
      }
    }
  });
}
