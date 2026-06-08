// dump_checkpoint_table.js — periodically dump the original's live checkpoint
// threshold table (g_raceCheckpointTablePtr @ 0x4AED88 -> table) + reverse flag.
// Used by /fix to verify whether reverse vs forward checkpoint thresholds differ.
// Layout (per RE): table[0] = {count u16 | initial_time u16}; per-cp entries at
// table + 4 + cp*4 = {span_threshold u16 | time_bonus u16}. Image base 0x400000.
(function () {
  var REV   = ptr(0x4AAF54); // gReverseTrackDirection (int32)
  var TPTR  = ptr(0x4AED88); // g_raceCheckpointTablePtr
  var last  = "";
  function dump() {
    try {
      var rev = REV.readS32();
      var t   = TPTR.readPointer();
      if (t.isNull()) { return; }
      var count = t.readU16();
      var initt = t.add(2).readU16();
      if (count === 0 || count > 16) { return; }
      var ths = [], bon = [];
      for (var i = 0; i < count; i++) {
        ths.push(t.add(4 + i * 4).readU16());
        bon.push(t.add(6 + i * 4).readU16());
      }
      var line = "rev=" + rev + " count=" + count + " init=" + initt +
                 " thresholds=" + JSON.stringify(ths) + " bonuses=" + JSON.stringify(bon);
      if (line !== last) { console.log("[cpdump] " + line); last = line; }
    } catch (e) { /* table not ready yet */ }
  }
  setInterval(dump, 1000);
  console.log("[cpdump] installed (polling g_raceCheckpointTablePtr @0x4AED88 every 1s)");
})();
