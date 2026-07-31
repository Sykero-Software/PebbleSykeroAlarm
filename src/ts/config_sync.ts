// SPDX-License-Identifier: GPL-3.0-only

// Reliable config delivery to the watch.
//
// The watch is a watchapp (not an always-running watchface), so the AppMessage
// sent on `webviewclosed` only lands if the app happened to be open at Save
// time -- which it usually is not. That matters more here than in the sibling
// apps: THE ALARM SET ITSELF LIVES ONLY ON THE PHONE, the watch ships a seeded
// demo set (07:00 Mon-Fri, 08:30 weekends), and there is no on-watch editing by
// design. A user who sets 06:45, taps Save with the app closed, and sees no
// error would go on being woken at 07:00 -- silent, and indistinguishable from
// working. So the watch asks for its config on launch (it sends CfgRequest) and
// the phone replies with the values it persisted on the last Save.
//
// PKJS localStorage works on this watch; the `data:`-URL localStorage ban
// applies only to the Clay config webview, not to PKJS.

// One key holding the whole last-saved dict as JSON. The dict is exactly what
// buildDict() produced, so the reply is byte-for-byte the message the watch
// would have received had it been running at Save time -- there is no second,
// hand-maintained per-key list to drift out of step with buildDict.
export const CFG_STORE_KEY = 'sykero-alarm-cfg';

// Remember the dict just sent, so it can be re-sent when the watch later asks.
export function saveDict(set: (k: string, v: string) => void,
                         dict: Record<string, any>): void {
  set(CFG_STORE_KEY, JSON.stringify(dict));
}

// The dict to reply to a CfgRequest with, or null when nothing usable was ever
// saved on this phone. NULL MEANS STAY SILENT: an empty or partial reply would
// clobber whatever the watch already has persisted (its own defaults, or a
// config saved before this phone was set up) with nothing, which is worse than
// not answering at all. `get` reads a stored string by key
// (e.g. window.localStorage.getItem), returning null if absent.
export function resendDict(get: (k: string) => string | null): Record<string, any> | null {
  const raw = get(CFG_STORE_KEY);
  if (raw === null || raw === undefined || raw === '') {
    return null;
  }
  let d: any;
  try {
    d = JSON.parse(raw);
  } catch (e) {
    // Corrupt/truncated store: silence beats sending garbage to an alarm clock.
    return null;
  }
  // AlarmSet is the one field whose absence would be actively harmful (the
  // watch would keep the demo alarms while every other setting changed), so it
  // is what makes a stored dict "usable" at all.
  if (!d || typeof d !== 'object' || typeof d.AlarmSet !== 'string') {
    return null;
  }
  return d as Record<string, any>;
}
