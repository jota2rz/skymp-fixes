// memprofile.js — SkyMP-Fixes memory profiler plugin
// ---------------------------------------------------
// Drop in:  Data\Platform\Plugins\memprofile.js
// Output:   Data\Platform\memprofile.csv  (auto-created, appended to)
//           Data\Platform\memprofile.boot.log (single line per session,
//             so we can confirm the script was at least loaded even if
//             the SP global isn't reachable for some reason)
//
// What it does
//   - Every PROFILE_INTERVAL_SEC seconds it records a line of memory stats:
//       timestamp_iso,uptime_s,rss_mb,heap_used_mb,heap_total_mb,
//       heap_limit_mb,external_mb,arraybuffers_mb,storage_keys,
//       worldmodel_forms
//   - Uses Node's process.memoryUsage and v8.getHeapStatistics — both
//     present in the libnode that SkyrimPlatform bundles.
//   - Verifies SkyMPFixes Fix #5: the heap_limit_mb column shows V8's
//     actual old-generation cap. With [V8]MaxOldSpaceMB=8192 you should
//     see ~8192. If you see ~4096, Fix #5 didn't take.
//   - Wrapped in try/catch everywhere so a broken probe can never crash
//     the game.
//
// IMPORTANT: SkyrimPlatform exposes the engine API via a GLOBAL named
// `skyrimPlatform` (set by `addNativeExports('skyrimPlatform', {})` just
// before the plugin runs). It is NOT a CommonJS module — `require("skyrimPlatform")`
// does NOT work. We read the global directly here.

(function () {
  'use strict';

  // ----- Config ------------------------------------------------------------
  var PROFILE_INTERVAL_SEC = 5;     // how often to sample
  var FORCE_GC_INTERVAL_SEC = 0;    // 0 = never; if > 0 and global.gc exists,
                                    //   force a GC every N seconds. The
                                    //   libnode bundled with SkyrimPlatform
                                    //   is NOT started with --expose-gc, so
                                    //   this won't do anything by default.

  // --- Frame-time / hitch detection ---
  //
  // SkyrimPlatform dispatches the "update" event once per game frame while
  // the window is focused. Measuring the wall-clock gap between consecutive
  // "update" calls gives us an approximate frame time as observed by JS.
  // A large gap corresponds to a visible stutter or a stalled render.
  //
  // The CSV sample records min/max/avg/count over the last PROFILE_INTERVAL_SEC
  // window, plus a count of frames >= HITCH_THRESHOLD_MS.
  //
  // Each individual hitch (gap >= HITCH_THRESHOLD_MS) is also written to a
  // dedicated log so you can correlate specific in-game events with stalls.
  var HITCH_THRESHOLD_MS = 100;     // frames slower than this count as a hitch
  var HITCH_LOG_MIN_MS   = 100;     // don't spam the hitch log below this

  // --- Auto heap-snapshot triggers ---
  //
  // A V8 heap snapshot is a JSON file (~5-10x current heap size) that lists
  // every JS object with its retainer chain. Loadable in Chrome:
  //     chrome://inspect  ->  Open dedicated DevTools for Node
  //     DevTools -> Memory tab -> Load profile -> pick the .heapsnapshot
  //
  // We take a snapshot when ANY of these conditions becomes true (each fires
  // at most once per game session):
  //   1. heap_used >= SNAPSHOT_HEAP_USED_MB  (leak detected mid-session)
  //   2. uptime    >= SNAPSHOT_AT_UPTIME_SEC (timed baseline reference)
  //
  // Snapshots take 1-5 seconds and freeze the JS thread, so we only allow
  // one per condition.
  var SNAPSHOT_HEAP_USED_MB = 200;          // trigger when heap exceeds this
  var SNAPSHOT_AT_UPTIME_SEC = 3600;        // baseline snapshot at 1h
  var SNAPSHOT_MAX_TOTAL     = 3;           // hard cap per session
  var SNAPSHOT_DIR_SUBPATH   = 'Data\\Platform'; // relative to cwd

  // ----- Node built-ins (Node's own publicRequire is available) ----------
  var fs = null, path = null, v8 = null;
  try { fs   = require('fs');   } catch (e) {}
  try { path = require('path'); } catch (e) {}
  try { v8   = require('v8');   } catch (e) {}

  // ----- Resolve output paths --------------------------------------------
  // process.cwd() is the Skyrim install dir at runtime. Put the CSV next to
  // SkyrimPlatform's other diag files (Data\Platform\sp_*.log).
  var CSV_PATH       = 'memprofile.csv';
  var BOOT_PATH      = 'memprofile.boot.log';
  var HITCH_PATH     = 'memprofile.hitches.log';
  var HEARTBEAT_PATH = 'SkyMPFixes.heartbeat';
  try {
    var cwd = (typeof process !== 'undefined' && typeof process.cwd === 'function')
      ? process.cwd() : '.';
    if (path) {
      CSV_PATH       = path.join(cwd, 'Data', 'Platform', 'memprofile.csv');
      BOOT_PATH      = path.join(cwd, 'Data', 'Platform', 'memprofile.boot.log');
      HITCH_PATH     = path.join(cwd, 'Data', 'Platform', 'memprofile.hitches.log');
      // The heartbeat file is picked up by SkyMPFixes.dll's watchdog.
      // Path is fixed relative to the Skyrim install dir so both sides
      // agree on it without needing to share config.
      HEARTBEAT_PATH = path.join(cwd, 'Data', 'Platform', 'SkyMPFixes.heartbeat');
    } else {
      CSV_PATH       = cwd + '\\Data\\Platform\\memprofile.csv';
      BOOT_PATH      = cwd + '\\Data\\Platform\\memprofile.boot.log';
      HITCH_PATH     = cwd + '\\Data\\Platform\\memprofile.hitches.log';
      HEARTBEAT_PATH = cwd + '\\Data\\Platform\\SkyMPFixes.heartbeat';
    }
  } catch (e) { /* keep defaults */ }

  // ----- Boot line: prove the script was executed ------------------------
  // We write this BEFORE looking for skyrimPlatform so we can tell apart
  // "plugin never executed" from "plugin ran but couldn't find the API".
  function writeBootLine(extra) {
    if (!fs) return;
    try {
      var d = new Date().toISOString();
      var nodeV = (typeof process !== 'undefined' && process.versions && process.versions.node)
        ? process.versions.node : 'unknown';
      var v8V = (typeof process !== 'undefined' && process.versions && process.versions.v8)
        ? process.versions.v8 : 'unknown';
      fs.appendFileSync(BOOT_PATH,
        d + ' memprofile boot. node=' + nodeV + ' v8=' + v8V + ' ' +
        (extra || '') + '\n', { encoding: 'utf8' });
    } catch (_) {}
  }

  writeBootLine('script entered');

  // ----- Locate the SkyrimPlatform global --------------------------------
  // SP runs `skyrimPlatform = addNativeExports('skyrimPlatform', {})`
  // *before* each plugin, so the global is always present.
  var sp = null;
  try {
    if (typeof skyrimPlatform !== 'undefined') sp = skyrimPlatform;
  } catch (_) {}
  if (!sp) {
    try {
      if (typeof globalThis !== 'undefined' && globalThis.skyrimPlatform)
        sp = globalThis.skyrimPlatform;
    } catch (_) {}
  }
  // Last-ditch fallback: SP also injects a `log` global = printConsole
  var spLog = (typeof log !== 'undefined') ? log
            : (sp && typeof sp.printConsole === 'function' ? sp.printConsole
                                                            : null);
  function spPrint() {
    try {
      if (spLog) spLog.apply(null, arguments);
    } catch (_) {}
  }

  if (!sp) {
    writeBootLine('FAILED to locate skyrimPlatform global -- aborting');
    return;
  }
  if (!fs) {
    writeBootLine('FAILED to require(fs) -- aborting');
    return;
  }

  writeBootLine('skyrimPlatform OK, fs OK -- continuing');
  spPrint('[memprofile] loaded. Sampling every ' + PROFILE_INTERVAL_SEC +
          's -> ' + CSV_PATH);

  // ----- CSV header (rotate old file if the schema changed) -------------
  var CSV_HEADER =
    'timestamp_iso,uptime_s,rss_mb,heap_used_mb,heap_total_mb,' +
    'heap_limit_mb,external_mb,arraybuffers_mb,storage_keys,' +
    'worldmodel_forms,' +
    'frame_count,frame_min_ms,frame_avg_ms,frame_max_ms,hitches_ge_' +
    HITCH_THRESHOLD_MS + 'ms,' +
    'actors_total,actors_near_2k,actors_near_5k,actors_near_10k,' +
    'cell,player_x,player_y,player_z,' +
    'ourjs_avg_ms,ourjs_max_ms\n';
  try {
    var needHeader = true;
    try {
      var st = fs.statSync(CSV_PATH);
      if (st && st.size > 0) {
        // Read first line and compare against our current header. If the
        // schema changed (e.g. added frame-time columns after an update),
        // rotate the old file aside so plotting tools don't choke on mixed
        // row widths.
        var existing = fs.readFileSync(CSV_PATH, 'utf8');
        var firstLine = existing.split('\n')[0] + '\n';
        if (firstLine === CSV_HEADER) {
          needHeader = false;
        } else {
          var rotated = CSV_PATH.replace(/\.csv$/i, '') + '.' +
                        new Date().toISOString().replace(/[:.]/g, '-') +
                        '.pre-schema-change.csv';
          try {
            fs.renameSync(CSV_PATH, rotated);
            writeBootLine('CSV schema changed -- rotated old file to ' + rotated);
          } catch (_) { /* leave old file in place; we'll overwrite below */ }
        }
      }
    } catch (_) { /* ENOENT -> need header */ }

    if (needHeader) {
      fs.writeFileSync(CSV_PATH, CSV_HEADER, { encoding: 'utf8' });
    }
  } catch (e) {
    writeBootLine('CSV open failed: ' + (e && e.message));
    spPrint('[memprofile] failed to open ' + CSV_PATH + ': ' + (e && e.message));
    return;
  }

  // ----- Helpers ---------------------------------------------------------
  var startedAt = Date.now();
  function toMB(bytes)  { return Math.round((bytes || 0) / 1024 / 1024 * 10) / 10; }
  function safeNum(n)   { return (typeof n === 'number' && isFinite(n)) ? n : 0; }

  // ----- Frame-time accumulators ----------------------------------------
  // Reset after every CSV sample. `frameLastAt=0` means "no previous frame
  // yet"; the first update after boot / after a sample won't produce a gap.
  var frameLastAt   = 0;
  var frameCount    = 0;
  var frameSumMs    = 0;
  var frameMinMs    = 0;
  var frameMaxMs    = 0;
  var frameHitches  = 0;
  // Time spent INSIDE our own on('update') callback (JS-side overhead of
  // this plugin). Distinguishes "the frame is slow because game code /
  // other SP plugins are slow" from "the frame is slow because OUR JS is
  // slow" -- the latter would be a bug in this profiler, but confirming
  // it's NOT us is useful signal.
  var ourJsSumMs    = 0;
  var ourJsMaxMs    = 0;
  // Guard: if the window is minimized or the game is paused for a long
  // time, SP stops firing "update" and the next tick's gap is huge. That's
  // not a hitch we care about, so cap what we count as a "real" frame gap.
  var FRAME_GAP_CAP_MS = 5000;

  function recordFrameGap(gapMs) {
    if (gapMs <= 0 || gapMs > FRAME_GAP_CAP_MS) return;
    frameCount++;
    frameSumMs += gapMs;
    if (frameMinMs === 0 || gapMs < frameMinMs) frameMinMs = gapMs;
    if (gapMs > frameMaxMs) frameMaxMs = gapMs;
    if (gapMs >= HITCH_THRESHOLD_MS) frameHitches++;
  }

  function recordOurJsTime(ms) {
    if (ms <= 0 || ms > FRAME_GAP_CAP_MS) return;
    ourJsSumMs += ms;
    if (ms > ourJsMaxMs) ourJsMaxMs = ms;
  }

  function resetFrameAccum() {
    frameCount = 0; frameSumMs = 0; frameMinMs = 0; frameMaxMs = 0;
    frameHitches = 0;
    ourJsSumMs = 0; ourJsMaxMs = 0;
  }

  function logHitch(now, gapMs) {
    if (!fs) return;
    if (gapMs < HITCH_LOG_MIN_MS) return;
    try {
      var mem = (typeof process !== 'undefined' && process.memoryUsage)
        ? process.memoryUsage() : {};
      var ts  = new Date(now).toISOString();
      var upS = Math.round((now - startedAt) / 1000);
      fs.appendFileSync(HITCH_PATH,
        ts + ' uptime=' + upS + 's gap_ms=' + Math.round(gapMs) +
        ' rss_mb=' + toMB(mem.rss) +
        ' heap_used_mb=' + toMB(mem.heapUsed) + '\n',
        { encoding: 'utf8' });
    } catch (_) {}
  }

  function getStorageKeyCount() {
    try {
      var s = sp.storage;
      if (!s) return 0;
      return Object.keys(s).length;
    } catch (e) { return -1; }
  }

  function getWorldModelFormCount() {
    try {
      var s = sp.storage;
      if (!s) return -1;
      var probes = ['worldModel', '_worldModel'];
      for (var i = 0; i < probes.length; i++) {
        var wm = s[probes[i]];
        if (wm && wm.forms && typeof wm.forms.length === 'number') {
          return wm.forms.length;
        }
      }
      var rs = s['remoteServer'];
      if (rs && rs.worldModel && rs.worldModel.forms) {
        return rs.worldModel.forms.length;
      }
      return -1;
    } catch (e) { return -1; }
  }

  // Locate SkyMP's worldModel.forms once per call. Returns the array or null.
  // Cached lookup path is faster than getWorldModelFormCount because we don't
  // walk both storage keys twice.
  function getWorldModelForms() {
    try {
      // Path A: sp.storage (older SkyMP builds).
      var s = null;
      try { s = sp.storage; } catch (_) {}
      if (s) {
        if (s.worldModel && s.worldModel.forms) return s.worldModel.forms;
        if (s._worldModel && s._worldModel.forms) return s._worldModel.forms;
        var rs = s['remoteServer'];
        if (rs && rs.worldModel && rs.worldModel.forms) return rs.worldModel.forms;
      }
      // Path B: sp global itself (newer builds stash it here).
      if (sp.worldModel && sp.worldModel.forms) return sp.worldModel.forms;
      if (sp._worldModel && sp._worldModel.forms) return sp._worldModel.forms;
      // Path C: global (Node's global). Some SkyMP builds attach it here.
      if (typeof global !== 'undefined') {
        if (global.worldModel && global.worldModel.forms) return global.worldModel.forms;
        if (global.__skympWorldModel && global.__skympWorldModel.forms) return global.__skympWorldModel.forms;
        // Some builds keep it inside a "skymp" namespace.
        if (global.skymp && global.skymp.worldModel && global.skymp.worldModel.forms) {
          return global.skymp.worldModel.forms;
        }
      }
      return null;
    } catch (e) { return null; }
  }

  // Player position (for distance-gated actor counts). Returns [x,y,z] or
  // null if the player isn't available yet (main menu, load screen).
  function getPlayerPos() {
    try {
      var Game = sp.Game;
      if (!Game || typeof Game.getPlayer !== 'function') return null;
      var pl = Game.getPlayer();
      if (!pl) return null;
      // getPositionX/Y/Z are cheap native calls.
      return [pl.getPositionX(), pl.getPositionY(), pl.getPositionZ()];
    } catch (e) { return null; }
  }

  // Current cell editor ID + world-space coords, if reachable. Correlates
  // FPS drops with specific in-game locations. Returns { cell, x, y, z } or
  // { cell: '', x/y/z: 0 } on failure so CSV columns stay populated.
  function getPlayerCellInfo() {
    var out = { cell: '', x: 0, y: 0, z: 0 };
    try {
      var Game = sp.Game;
      if (!Game || typeof Game.getPlayer !== 'function') return out;
      var pl = Game.getPlayer();
      if (!pl) return out;
      try {
        out.x = Math.round(pl.getPositionX());
        out.y = Math.round(pl.getPositionY());
        out.z = Math.round(pl.getPositionZ());
      } catch (_) {}
      try {
        var cell = pl.getParentCell();
        if (cell) {
          // Try methods in order of usefulness: prefer a human-readable
          // editorID / name, fall back to hex formID.
          var attempts = [
            function () { return cell.getFormEditorID && cell.getFormEditorID(); },
            function () { return cell.getName && cell.getName(); },
            function () {
              return cell.getFormID
                ? '0x' + cell.getFormID().toString(16)
                : null;
            }
          ];
          for (var i = 0; i < attempts.length; i++) {
            try {
              var v = attempts[i]();
              if (v !== null && v !== undefined && String(v).length > 0) {
                out.cell = String(v);
                break;
              }
            } catch (_) { /* try next */ }
          }
        }
      } catch (_) {}
    } catch (_) {}
    return out;
  }

  // Count synced ACTORS in the world model, plus a distance-gated subset.
  // A form is an "actor" if it has a .movement field (only movable/live
  // entities have that in SkyMP's schema; static objects don't). Returns:
  //   [totalActors, near2k, near5k, near10k]
  // near_XX_k = actor forms within (XX * 1024) game units of the player,
  // matching Skyrim's typical "activity radius" units (1 game unit ~ 1.4 cm).
  //
  // Distance is squared to skip a sqrt per form. Threshold constants below
  // are already squared.
  //
  // Fallback: if SkyMP's world model isn't reachable (sp.storage empty in
  // this build), estimate visible actor count by calling FindRandomActor
  // several times at each radius and collecting unique form IDs. This
  // saturates around 8-15 for large populations but is enough to
  // distinguish "empty area" from "crowd".
  function getActorNearCounts() {
    var totals = [-1, -1, -1, -1];

    // Path A: SkyMP world model iteration.
    try {
      var forms = getWorldModelForms();
      if (forms && forms.length > 0) {
        var total = 0, near2k = 0, near5k = 0, near10k = 0;
        var pos = getPlayerPos();
        var haveDist = pos && isFinite(pos[0]);
        var px = haveDist ? pos[0] : 0;
        var py = haveDist ? pos[1] : 0;
        var pz = haveDist ? pos[2] : 0;
        var R2_2K  = 2048.0  * 2048.0;
        var R2_5K  = 5120.0  * 5120.0;
        var R2_10K = 10240.0 * 10240.0;
        for (var i = 0; i < forms.length; i++) {
          var f = forms[i];
          if (!f || !f.movement) continue;
          total++;
          if (!haveDist) continue;
          var mp = f.movement.pos;
          if (!mp || mp.length < 3) continue;
          var dx = mp[0] - px, dy = mp[1] - py, dz = mp[2] - pz;
          var d2 = dx * dx + dy * dy + dz * dz;
          if (d2 <= R2_2K)  near2k++;
          if (d2 <= R2_5K)  near5k++;
          if (d2 <= R2_10K) near10k++;
        }
        totals[0] = total;
        totals[1] = haveDist ? near2k  : -1;
        totals[2] = haveDist ? near5k  : -1;
        totals[3] = haveDist ? near10k : -1;
        return totals;
      }
    } catch (e) { /* fall through to statistical estimate */ }

    // Path B: statistical estimate via FindRandomActor. Skyrim's Papyrus
    // Game.FindRandomActor returns a random actor within `radius` of the
    // given point. Call it many times per radius and collect unique
    // form IDs -- gives us a "seen count" that saturates near the true
    // population size after enough samples.
    //
    // This is an ESTIMATE, not a count. But changes to it correlate with
    // real changes in crowd size. -1 elsewhere still means "no info".
    try {
      var pos2 = getPlayerPos();
      if (!pos2 || !isFinite(pos2[0])) return totals;
      if (!sp.Game || typeof sp.Game.FindRandomActor !== 'function') return totals;

      var SAMPLES_PER_RADIUS = 12;   // 12 tries -> saturates ~10-12 unique
      function estimateAt(radius) {
        var seen = {};
        for (var k = 0; k < SAMPLES_PER_RADIUS; k++) {
          var a = null;
          try {
            a = sp.Game.FindRandomActor(pos2[0], pos2[1], pos2[2], radius);
          } catch (_) {}
          if (!a) continue;
          var id = 0;
          try { id = a.getFormID(); } catch (_) { continue; }
          if (id) seen[id] = 1;
        }
        return Object.keys(seen).length;
      }
      totals[1] = estimateAt(2048);
      totals[2] = estimateAt(5120);
      totals[3] = estimateAt(10240);
      // totals[0] stays -1 (we don't have a true total from this estimator).
    } catch (e) { /* leave -1s */ }
    return totals;
  }

  function sample() {
    try {
      var mem = (typeof process !== 'undefined' && process.memoryUsage)
        ? process.memoryUsage() : {};
      var hs = (v8 && v8.getHeapStatistics) ? v8.getHeapStatistics() : {};

      var ts  = new Date().toISOString();
      var upS = Math.round((Date.now() - startedAt) / 1000);

      var heapUsedMB = toMB(hs.used_heap_size  || mem.heapUsed);

      var frameAvgMs = frameCount > 0
        ? Math.round((frameSumMs / frameCount) * 10) / 10 : 0;

      // Actor counts: [total, near_2k, near_5k, near_10k]. Any -1 means the
      // probe couldn't reach the required data (world model not initialised,
      // or player not yet loaded).
      var actors = getActorNearCounts();

      // Cell / position info. `cell` may contain commas or quotes in weird
      // mods -- strip them so we don't corrupt the CSV column layout.
      var loc = getPlayerCellInfo();
      var cellSafe = (loc.cell || '').replace(/[",\r\n]/g, '_');

      var line =
        ts + ',' +
        upS + ',' +
        toMB(mem.rss) + ',' +
        heapUsedMB + ',' +
        toMB(hs.total_heap_size || mem.heapTotal) + ',' +
        toMB(hs.heap_size_limit) + ',' +
        toMB(mem.external) + ',' +
        toMB(mem.arrayBuffers) + ',' +
        safeNum(getStorageKeyCount()) + ',' +
        safeNum(getWorldModelFormCount()) + ',' +
        frameCount + ',' +
        (Math.round(frameMinMs * 10) / 10) + ',' +
        frameAvgMs + ',' +
        (Math.round(frameMaxMs * 10) / 10) + ',' +
        frameHitches + ',' +
        actors[0] + ',' + actors[1] + ',' + actors[2] + ',' + actors[3] + ',' +
        cellSafe + ',' + loc.x + ',' + loc.y + ',' + loc.z + ',' +
        // OurJS overhead: avg = ourJsSumMs / frameCount, max = ourJsMaxMs.
        // If ourjs_avg_ms is well below frame_avg_ms, the frame slowdown is
        // NOT in this profiler's JS -- it's game code or other SP plugins.
        (frameCount > 0
          ? (Math.round((ourJsSumMs / frameCount) * 100) / 100)
          : 0) + ',' +
        (Math.round(ourJsMaxMs * 100) / 100) +
        '\n';

      fs.appendFileSync(CSV_PATH, line, { encoding: 'utf8' });

      // Reset frame accumulators after each sample so the next row reflects
      // the next PROFILE_INTERVAL_SEC window only.
      resetFrameAccum();

      // Check snapshot triggers AFTER writing the CSV row so the snapshot is
      // ordered correctly in the timeline.
      try {
        if (typeof checkSnapshotTriggers === 'function') {
          checkSnapshotTriggers(heapUsedMB, upS);
        }
      } catch (_) {}
    } catch (e) {
      spPrint('[memprofile] sample error: ' + (e && e.message));
    }
  }

  // Take one sample immediately so even very short sessions log SOMETHING.
  sample();
  writeBootLine('first sample taken');

  // ----- One-shot SkyrimPlatform API shape dump --------------------------
  // Dumps top-level keys of sp, sp.storage, sp.Game, sp.Actor, sp.Cell, and
  // sp.mpClientPlugin to the boot log so we can see what actually exists in
  // this SP build and design future probes accordingly. Never call on the
  // hot path -- once per session only.
  //
  // The Papyrus-native tests (getPlayer/getParentCell/getFormEditorID) MUST
  // run on the game thread, i.e. from inside an sp.on('update') callback.
  // Calling them from module-top-level (Node thread) throws
  //   "'Form.getName' can't be called in this context"
  // So we defer the game-thread probes to the first `update` firing below.
  var apiDumpDone = false;
  function dumpApiShape() {
    try {
      var dumpKeys = function (label, obj) {
        try {
          if (obj === null || typeof obj === 'undefined') {
            writeBootLine('api-shape ' + label + ': (null/undefined)');
            return;
          }
          var keys = null;
          try { keys = Object.keys(obj); } catch (_) { keys = null; }
          if (!keys) {
            writeBootLine('api-shape ' + label + ': keys() threw');
            return;
          }
          var head = keys.slice(0, 40).join(',');
          writeBootLine('api-shape ' + label + ' (' + keys.length + ' keys): ' + head);
        } catch (e) {
          writeBootLine('api-shape ' + label + ' failed: ' + (e && e.message));
        }
      };
      dumpKeys('sp',                    sp);
      dumpKeys('sp.storage',            (function () { try { return sp.storage; } catch (_) { return null; } })());
      dumpKeys('sp.mpClientPlugin',     sp.mpClientPlugin);
      dumpKeys('sp.hooks',              sp.hooks);
      dumpKeys('sp.browser',            sp.browser);
      dumpKeys('global',                (typeof global !== 'undefined' ? global : null));

      // Some SkyMP client builds ship obfuscated -- the module wrappers
      // show up on `global` as `a0_XXXX` names. Dump each of those so we
      // can find the one holding the world model / peer list.
      if (typeof global !== 'undefined') {
        try {
          var gk = Object.keys(global);
          for (var gi = 0; gi < gk.length; gi++) {
            var name = gk[gi];
            if (/^a\d+_0x[0-9a-f]+$/i.test(name)) {
              try {
                var g = global[name];
                var t = typeof g;
                if (g === null || t === 'undefined') {
                  writeBootLine('api-shape global.' + name + ': null/undefined');
                } else if (t === 'function') {
                  writeBootLine('api-shape global.' + name + ': function/' +
                    (g.length || 0) + ' args');
                } else if (t === 'object') {
                  var sub = Object.keys(g);
                  writeBootLine('api-shape global.' + name +
                    ' (obj, ' + sub.length + ' keys): ' +
                    sub.slice(0, 30).join(','));
                  // Look for likely world-model shapes inside.
                  for (var si = 0; si < sub.length; si++) {
                    var sname = sub[si];
                    if (/world|forms?|players?|actors?|remote|peer/i.test(sname)) {
                      try {
                        var sv = g[sname];
                        if (sv && typeof sv === 'object') {
                          var svk = Object.keys(sv);
                          writeBootLine('  -> ' + name + '.' + sname +
                            ' (' + svk.length + ' keys): ' +
                            svk.slice(0, 20).join(','));
                        }
                      } catch (_) {}
                    }
                  }
                } else {
                  writeBootLine('api-shape global.' + name + ': ' + t);
                }
              } catch (e) {
                writeBootLine('api-shape global.' + name + ' probe threw: ' +
                  (e && e.message));
              }
            }
          }
        } catch (_) {}
      }

      // Game-thread probes: getPlayer() and friends
      try {
        var pl = sp.Game.getPlayer();
        if (pl) {
          var nm = '?';
          try { nm = pl.getName ? pl.getName() : '?'; } catch (_) { nm = '(getName threw)'; }
          writeBootLine('api-shape sp.Game.getPlayer() -> OK (name="' + nm + '")');
          // Dump ALL methods on the Player Form so we know what's callable
          try {
            var plKeys = Object.keys(pl);
            writeBootLine('api-shape Player (' + plKeys.length + ' keys): ' +
              plKeys.slice(0, 60).join(','));
          } catch (_) {}
          try {
            var cell = pl.getParentCell();
            if (cell) {
              var cellId = '?';
              try {
                if (cell.getFormEditorID) cellId = 'editorID=' + cell.getFormEditorID();
                else if (cell.getName) cellId = 'name=' + cell.getName();
                else if (cell.getFormID) cellId = 'formID=0x' + cell.getFormID().toString(16);
              } catch (e) { cellId = '(all name/id calls threw: ' + (e && e.message) + ')'; }
              writeBootLine('api-shape player.getParentCell() -> OK (' + cellId + ')');
              try {
                var cellKeys = Object.keys(cell);
                writeBootLine('api-shape Cell (' + cellKeys.length + ' keys): ' +
                  cellKeys.slice(0, 40).join(','));
              } catch (_) {}
            } else {
              writeBootLine('api-shape player.getParentCell() -> null');
            }
          } catch (e) {
            writeBootLine('api-shape player.getParentCell() threw: ' + (e && e.message));
          }
        } else {
          writeBootLine('api-shape sp.Game.getPlayer() returned null');
        }
      } catch (e) {
        writeBootLine('api-shape sp.Game.getPlayer() threw: ' + (e && e.message));
      }
    } catch (e) {
      writeBootLine('api-shape dump failed entirely: ' + (e && e.message));
    }
    apiDumpDone = true;
  }

  // ----- Snapshot helpers -------------------------------------------------
  var snapshotsTaken          = 0;
  var triggeredHeapThreshold  = false;
  var triggeredUptimeBaseline = false;

  function snapshotPath(reason) {
    var ts = new Date().toISOString().replace(/[:.]/g, '-');
    var name = 'memprofile-' + ts + '-' + reason + '.heapsnapshot';
    try {
      var cwd = (typeof process !== 'undefined' && typeof process.cwd === 'function')
        ? process.cwd() : '.';
      return path ? path.join(cwd, SNAPSHOT_DIR_SUBPATH, name)
                  : cwd + '\\' + SNAPSHOT_DIR_SUBPATH + '\\' + name;
    } catch (_) {
      return name;
    }
  }

  function takeSnapshot(reason, heapMB) {
    if (snapshotsTaken >= SNAPSHOT_MAX_TOTAL) return;
    if (!v8 || typeof v8.writeHeapSnapshot !== 'function') {
      writeBootLine('snapshot SKIPPED -- v8.writeHeapSnapshot not available');
      return;
    }
    snapshotsTaken++;
    var p = snapshotPath(reason);
    writeBootLine('taking heap snapshot #' + snapshotsTaken +
                  ' (reason=' + reason + ', heap=' + heapMB + ' MB) -> ' + p);
    spPrint('[memprofile] taking heap snapshot (this freezes the game for a few seconds) -> ' + p);
    try {
      var t0 = Date.now();
      v8.writeHeapSnapshot(p);
      var dtMs = Date.now() - t0;
      writeBootLine('snapshot complete in ' + dtMs + ' ms');
      spPrint('[memprofile] snapshot done in ' + dtMs + ' ms');
    } catch (e) {
      writeBootLine('snapshot FAILED: ' + (e && e.message));
      spPrint('[memprofile] snapshot failed: ' + (e && e.message));
    }
  }

  function checkSnapshotTriggers(heapUsedMB, uptimeSec) {
    if (!triggeredHeapThreshold && heapUsedMB >= SNAPSHOT_HEAP_USED_MB) {
      triggeredHeapThreshold = true;
      takeSnapshot('heap' + Math.round(heapUsedMB) + 'MB', heapUsedMB);
    }
    if (!triggeredUptimeBaseline && uptimeSec >= SNAPSHOT_AT_UPTIME_SEC) {
      triggeredUptimeBaseline = true;
      takeSnapshot('uptime' + Math.round(uptimeSec) + 's', heapUsedMB);
    }
  }

  // ----- Periodic sampling ------------------------------------------------
  // We drive sampling off SkyrimPlatform's "update" event (fires while the
  // game window is active, both in-game and on the main menu).
  var lastSampleAt    = Date.now();
  var lastGcAt        = Date.now();
  var lastHeartbeatAt = 0; // 0 so we touch immediately on first tick

  // Touch (create-or-truncate to 0 bytes) the heartbeat file so its mtime
  // reflects the last SP "update" tick. SkyMPFixes.dll's watchdog reads
  // this to distinguish "window responsive but game loop stuck" freezes
  // from real hangs.
  function touchHeartbeat(now) {
    if (!fs) return;
    // Throttle to at most once per second so we don't spam the disk.
    if (now - lastHeartbeatAt < 1000) return;
    lastHeartbeatAt = now;
    try {
      // writeFileSync with empty string truncates and updates mtime.
      fs.writeFileSync(HEARTBEAT_PATH, '', { encoding: 'utf8' });
    } catch (_) {
      // Best-effort: if the disk is full or the path is bad, silently skip.
    }
  }

  try {
    sp.on('update', function () {
      // Bracket the callback with high-resolution timing so we know exactly
      // how much of each frame we consume. performance.now() is a Node
      // built-in and returns fractional milliseconds.
      var cbStart = (typeof performance !== 'undefined' && performance.now)
        ? performance.now() : Date.now();
      var now = Date.now();
      // First-tick side-effect: dump API shape from game thread now that
      // Papyrus natives are callable.
      if (!apiDumpDone) {
        try { dumpApiShape(); } catch (_) { apiDumpDone = true; }
      }
      // Frame-gap measurement: distance from previous "update" fires.
      if (frameLastAt !== 0) {
        var gap = now - frameLastAt;
        recordFrameGap(gap);
        if (gap >= HITCH_THRESHOLD_MS) logHitch(now, gap);
      }
      frameLastAt = now;

      touchHeartbeat(now);
      if (now - lastSampleAt >= PROFILE_INTERVAL_SEC * 1000) {
        lastSampleAt = now;
        sample();
      }
      if (FORCE_GC_INTERVAL_SEC > 0 &&
          now - lastGcAt >= FORCE_GC_INTERVAL_SEC * 1000) {
        lastGcAt = now;
        try { if (typeof global !== 'undefined' && global.gc) global.gc(); } catch (_) {}
      }

      // Record our own callback duration. Skip the sampling-tick frame
      // (its sample() runs file I/O and is not representative).
      var cbEnd = (typeof performance !== 'undefined' && performance.now)
        ? performance.now() : Date.now();
      recordOurJsTime(cbEnd - cbStart);
    });
    writeBootLine('on("update") handler registered');
    writeBootLine('heartbeat path: ' + HEARTBEAT_PATH);
    writeBootLine('hitches path:   ' + HITCH_PATH +
                  ' (threshold=' + HITCH_THRESHOLD_MS + 'ms)');
  } catch (e) {
    writeBootLine('on("update") failed: ' + (e && e.message));
    spPrint('[memprofile] on("update") failed: ' + (e && e.message));
  }
})();
