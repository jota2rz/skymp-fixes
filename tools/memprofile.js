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
    HITCH_THRESHOLD_MS + 'ms\n';
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

  function resetFrameAccum() {
    frameCount = 0; frameSumMs = 0; frameMinMs = 0; frameMaxMs = 0;
    frameHitches = 0;
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
        frameHitches + '\n';

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
      var now = Date.now();
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
