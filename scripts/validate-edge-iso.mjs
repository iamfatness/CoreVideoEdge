#!/usr/bin/env node
// End-to-end check of a CoreVideo Edge node, driven ONLY through its control
// API (JSON lines on corevideo-edge.exe stdin/stdout) - no desktop, no UI.
//
//   node scripts/validate-edge-iso.mjs                       # fake meeting, 4 ISOs, 20 s
//   node scripts/validate-edge-iso.mjs --sources 6 --seconds 60
//   node scripts/validate-edge-iso.mjs --engine zoom --meeting "https://...zoom.us/j/<id>?pwd=..." \
//        --names "Alice,Bob" --seconds 30                     # real meeting (needs ZOOM_SDK build)
//
// Checks, and exits non-zero on any failure:
//   1. join; nothing is subscribed until asked (no auto-subscribe)
//   2. the subscription cap holds (default 6) and refuses the next one
//   3. every subscribed source is received at 1920x1080 (fake) / reports its real size (zoom)
//   4. record -> stop produces <prefix>-<stamp>/ISO-NN-<name>.mp4 + manifest.json
//   5. ffprobe: each ISO has h264 1920x1080 video AND aac 48 kHz stereo audio,
//      both tracks end together, all ISOs end together
//   6. no ISO arrival was lost; frame count matches the source rate
//   7. (fake) the synthetic clap lines up: A/V inside each ISO, and across ISOs

import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..');
const args = Object.fromEntries(
  process.argv.slice(2).reduce((pairs, value, i, all) => {
    if (value.startsWith('--')) pairs.push([value.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : 'true']);
    return pairs;
  }, []),
);
const bin = resolve(args.bin ?? join(root, 'build', 'bin'));
const engine = args.engine ?? 'fake';
const fake = engine === 'fake';
const wantSources = Number(args.sources ?? (fake ? 4 : 0));
const seconds = Number(args.seconds ?? 20);
const fps = Number(args.fps ?? 30);
const outDir = resolve(args.out ?? join(root, 'build', 'validate-out'));
const meeting = args.meeting ?? (fake ? 'https://zoom.us/j/1234567890?pwd=fake' : '');
const names = args.names ? args.names.split(',').map((s) => s.trim()).filter(Boolean) : [];

const failures = [];
const check = (ok, what) => {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${what}`);
  if (!ok) failures.push(what);
  return ok;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

if (!fake && !meeting) {
  console.error('--engine zoom needs --meeting "<full join link>"');
  process.exit(2);
}
mkdirSync(outDir, { recursive: true });
const exe = join(bin, 'corevideo-edge.exe');
if (!existsSync(exe)) {
  console.error(`missing ${exe}; build first (see README)`);
  process.exit(2);
}
const configPath = join(outDir, 'validate-edge.json');
writeFileSync(configPath, JSON.stringify({ displayName: 'CoreVideo Edge (validator)', filePrefix: 'validate' }, null, 2));

const env = { ...process.env };
if (fake) {
  env.COREVIDEO_FAKE_ENGINE_PARTICIPANTS = '9';
  env.COREVIDEO_FAKE_NO_CHURN = '1';
  env.COREVIDEO_FAKE_CLAP_MS = '2000';
  env.COREVIDEO_FAKE_ENGINE_FPS = String(fps);
}
const child = spawn(exe, [fake ? '--fake' : '--engine', ...(fake ? [] : [join(bin, 'corevideo-zoom-engine.exe')]), '--config', configPath], {
  env,
  stdio: ['pipe', 'pipe', 'pipe'],
});
const logLines = [];
child.stderr.setEncoding('utf8');
child.stderr.on('data', (d) => logLines.push(d));

let state = null;
const pending = new Map();
let nextId = 1;
let buffer = '';
child.stdout.setEncoding('utf8');
child.stdout.on('data', (chunk) => {
  buffer += chunk;
  let nl;
  while ((nl = buffer.indexOf('\n')) >= 0) {
    const line = buffer.slice(0, nl).trim();
    buffer = buffer.slice(nl + 1);
    if (!line) continue;
    let message;
    try { message = JSON.parse(line); } catch { console.error('unparseable stdout line:', line); continue; }
    if (message.type === 'state') state = message;
    else if (message.type === 'result' && message.id && pending.has(message.id)) {
      pending.get(message.id)(message);
      pending.delete(message.id);
    }
  }
});

function send(cmd, fields = {}) {
  const id = String(nextId++);
  return new Promise((resolveResult) => {
    pending.set(id, resolveResult);
    child.stdin.write(JSON.stringify({ cmd, id, ...fields }) + '\n');
  });
}

async function waitFor(predicate, timeoutMs, what) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (state && predicate(state)) return true;
    await sleep(100);
  }
  check(false, `${what} (timed out after ${timeoutMs / 1000}s; phase=${state?.phase} error=${state?.error ?? ''})`);
  return false;
}

function ffprobe(file) {
  const r = spawnSync('ffprobe', ['-v', 'error', '-show_streams', '-show_format', '-of', 'json', file], { encoding: 'utf8' });
  if (r.status !== 0) return null;
  return JSON.parse(r.stdout);
}

// Times (s) of the fake's full-scale audio bursts in an ISO.
function audioClaps(file) {
  const r = spawnSync('ffmpeg', ['-v', 'error', '-i', file, '-map', '0:a:0', '-ac', '1', '-ar', '48000', '-f', 's16le', '-'], {
    maxBuffer: 1 << 30,
  });
  if (r.status !== 0) return [];
  const samples = new Int16Array(r.stdout.buffer, r.stdout.byteOffset, Math.floor(r.stdout.length / 2));
  const times = [];
  let last = -1e9;
  for (let i = 0; i < samples.length; i++) {
    if (Math.abs(samples[i]) > 24000 && i - last > 4800) {
      times.push(i / 48000);
      last = i;
    }
  }
  return times;
}

// Times (s) of the fake's full-white video frames in an ISO.
function videoClaps(file) {
  const r = spawnSync('ffmpeg', ['-v', 'error', '-i', file, '-an', '-vf',
    'scale=192:108,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-', '-f', 'null', '-'], {
    encoding: 'utf8', maxBuffer: 1 << 28,
  });
  const times = [];
  let pts = null;
  for (const line of (r.stdout ?? '').split(/\r?\n/)) {
    const p = line.match(/pts_time:([0-9.]+)/);
    if (p) pts = Number(p[1]);
    const y = line.match(/YAVG=([0-9.]+)/);
    if (y && pts !== null && Number(y[1]) > 235) {
      if (!times.length || pts - times[times.length - 1] > 0.5) times.push(pts);
    }
  }
  return times;
}

async function main() {
  console.log(`corevideo-edge: ${exe} (${fake ? 'fake engine' : 'Zoom engine'})`);
  await waitFor((s) => s.phase === 'idle', 10000, 'node starts idle');

  const joined = await send('join', { meeting });
  check(joined.ok, `join accepted${joined.ok ? '' : ': ' + joined.error}`);
  if (!(await waitFor((s) => s.phase === 'in-meeting' && Array.isArray(s.participants) && s.participants.length > 0,
    fake ? 30000 : 120000, 'joined the meeting and received the roster'))) return;

  await sleep(2000);
  const others = state.participants.filter((p) => !p.self);
  check(state.participants.every((p) => p.slot === undefined && p.received === undefined),
    `nothing subscribed or received before the operator asked (${others.length} others in the meeting)`);
  const self = state.participants.find((p) => p.self);
  if (self) check(self.muted && !self.videoOn, `this node is in the meeting muted with its camera off (${self.name})`);
  else console.log(`INFO  roster has no self row (${fake ? 'the fake engine omits it' : 'unexpected'})`);

  let picks;
  if (names.length) picks = names.map((n) => others.find((p) => p.name.toLowerCase() === n.toLowerCase())).filter(Boolean);
  else picks = others.filter((p) => p.videoOn).slice(0, wantSources);
  check(picks.length > 0 && (names.length === 0 || picks.length === names.length), `found ${picks.length} participant(s) to subscribe`);

  // Cap: subscribe everyone available up to cap+1, expect exactly cap accepted.
  const cap = state.maxSubscriptions;
  const candidates = [...picks, ...others.filter((p) => !picks.includes(p))];
  const accepted = [];
  let refusal = null;
  for (const p of candidates.slice(0, cap + 1)) {
    const r = await send('subscribe', { participantId: p.id, on: true });
    if (r.ok) accepted.push(p);
    else refusal = r.error;
  }
  if (candidates.length > cap) {
    check(accepted.length === cap && refusal, `cap of ${cap} enforced (${accepted.length} accepted; refused: "${refusal}")`);
  } else {
    console.log(`INFO  only ${candidates.length} candidates; cap refusal not exercised`);
  }
  for (const p of accepted.filter((a) => !picks.includes(a))) {
    const r = await send('subscribe', { participantId: p.id, on: false });
    check(r.ok, `unsubscribed extra ${p.name}`);
  }

  const pickIds = new Set(picks.map((p) => p.id));
  await waitFor((s) => s.participants?.filter((p) => pickIds.has(p.id)).every((p) => p.received?.receiving), 20000,
    'every subscribed source is being received');
  for (const p of state.participants.filter((x) => pickIds.has(x.id))) {
    const r = p.received;
    if (fake) check(r && r.width === 1920 && r.height === 1080, `${p.name} received at ${r?.width}x${r?.height} (1080p requested)`);
    else console.log(`INFO  ${p.name} received at ${r?.width}x${r?.height}`);
  }
  check(state.participants.filter((p) => !pickIds.has(p.id)).every((p) => !p.received),
    'no unsubscribed participant is being received');

  const folder = await send('set-record-folder', { path: outDir });
  check(folder.ok, `record folder set to ${outDir}`);
  const start = await send('record-start');
  if (!check(start.ok, `record-start${start.ok ? '' : ': ' + start.error}`)) return;
  const takeStarted = Date.now();
  for (let t = 0; t < seconds; t += 5) {
    await sleep(Math.min(5, seconds - t) * 1000);
    const line = (state.recording?.streams ?? []).map((s) =>
      `${s.name}: ${s.videoFrames}f ${(s.audioSamples / 48000).toFixed(1)}s-audio lost=${s.videoLost}${s.warning ? ' WARN ' + s.warning : ''}`).join(' | ');
    console.log(`      t=${Math.min(seconds, t + 5)}s  ${line}`);
  }
  const stop = await send('record-stop');
  const takeSeconds = (Date.now() - takeStarted) / 1000;
  check(stop.ok, `record-stop${stop.ok ? '' : ': ' + stop.error}`);
  await waitFor((s) => s.recording && !s.recording.active && s.recording.streams.length === picks.length &&
    s.recording.streams.every((x) => x.state === 'finalized' || x.state === 'failed'), 30000, 'every ISO finalized');

  const rec = state.recording;
  const health = rec.streams;
  for (const s of health) {
    check(s.state === 'finalized' && !s.warning, `${s.name}: finalized (${s.encoder ?? 'encoder unknown'} encoder)${s.warning ? ' - ' + s.warning : ''}`);
    check(s.videoLost === 0, `${s.name}: no ISO arrival lost (lost=${s.videoLost})`);
    if (fake) {
      const expected = fps * takeSeconds;
      check(s.videoFrames >= expected * 0.9 && s.videoFrames <= expected * 1.05,
        `${s.name}: ${s.videoFrames} frames for ${takeSeconds.toFixed(1)} s at ${fps} fps (expected ~${Math.round(expected)})`);
      check(s.audioSamples > 0.9 * 48000 * takeSeconds, `${s.name}: ${(s.audioSamples / 48000).toFixed(2)} s of isolated audio`);
    }
  }

  await send('leave');
  await waitFor((s) => s.phase === 'idle', 15000, 'left the meeting');
  await send('quit');

  // ---- files ----
  const dir = rec.sessionDir;
  check(dir && existsSync(dir), `session folder ${dir}`);
  const files = readdirSync(dir).sort();
  const isoFiles = files.filter((f) => /^ISO-\d\d-.+\.mp4$/.test(f));
  check(isoFiles.length === picks.length, `${isoFiles.length} ISO file(s): ${isoFiles.join(', ')}`);
  check(!files.includes('Program.mp4'), 'no Program.mp4 (Edge has no program)');
  check(files.includes('manifest.json'), 'manifest.json present');
  const manifest = JSON.parse(readFileSync(join(dir, 'manifest.json'), 'utf8'));
  check(manifest.complete === true && manifest.isos.length === picks.length, `manifest complete with ${manifest.isos.length} ISO entries`);

  const durations = [];
  for (const f of isoFiles) {
    const path = join(dir, f);
    const probe = ffprobe(path);
    if (!check(probe, `${f}: ffprobe reads it`)) continue;
    const v = probe.streams.find((s) => s.codec_type === 'video');
    const a = probe.streams.find((s) => s.codec_type === 'audio');
    check(v && v.codec_name === 'h264' && v.width === 1920 && v.height === 1080, `${f}: video ${v?.codec_name} ${v?.width}x${v?.height}`);
    check(a && a.codec_name === 'aac' && Number(a.sample_rate) === 48000 && a.channels === 2,
      `${f}: audio ${a?.codec_name} ${a?.sample_rate} Hz x${a?.channels}`);
    const vd = Number(v?.duration), ad = Number(a?.duration), fd = Number(probe.format.duration);
    check(Math.abs(vd - ad) < 0.1, `${f}: video ${vd.toFixed(3)} s and audio ${ad.toFixed(3)} s end together`);
    check(Math.abs(fd - takeSeconds) < 1.5, `${f}: ${fd.toFixed(2)} s long for a ${takeSeconds.toFixed(1)} s take`);
    durations.push(fd);
  }
  if (durations.length > 1) {
    const spread = Math.max(...durations) - Math.min(...durations);
    check(spread < 0.2, `all ISOs end together (spread ${(spread * 1000).toFixed(0)} ms)`);
  }

  if (fake) {
    // Same clap instant in every ISO (shared epoch), and A/V inside each.
    const firstVideoClap = [];
    for (const f of isoFiles) {
      const path = join(dir, f);
      const v = videoClaps(path);
      const a = audioClaps(path);
      if (!check(v.length >= 2 && a.length >= 2, `${f}: found ${v.length} video and ${a.length} audio claps`)) continue;
      // Pair each video clap with an audio clap within 0.5 s (claps are 2 s
      // apart). A clap cut by the take's start or stop has no partner.
      const skews = [];
      for (const t of v) {
        const nearest = a.reduce((best, x) => (Math.abs(x - t) < Math.abs(best - t) ? x : best), a[0]);
        if (Math.abs(nearest - t) < 0.5) skews.push((nearest - t) * 1000);
      }
      const worst = skews.reduce((m, x) => (Math.abs(x) > Math.abs(m) ? x : m), 0);
      check(skews.length >= Math.min(v.length, a.length) - 1 && Math.abs(worst) <= 45,
        `${f}: audio vs video clap skew worst ${worst.toFixed(1)} ms over ${skews.length} paired claps (${v.length} video, ${a.length} audio)`);
      firstVideoClap.push(v[0]);
    }
    if (firstVideoClap.length > 1) {
      const spread = (Math.max(...firstVideoClap) - Math.min(...firstVideoClap)) * 1000;
      check(spread <= 40, `the same clap lands at the same time in every ISO (spread ${spread.toFixed(1)} ms)`);
    }
  }
}

try {
  await main();
} catch (error) {
  check(false, `validator crashed: ${error.stack ?? error}`);
} finally {
  child.stdin.end();
  await sleep(500);
  if (child.exitCode === null) child.kill();
  writeFileSync(join(outDir, 'corevideo-edge.log'), logLines.join(''));
  console.log(`\nlog: ${join(outDir, 'corevideo-edge.log')}`);
  console.log(failures.length ? `\n${failures.length} FAILURE(S)` : '\nALL CHECKS PASSED');
  process.exit(failures.length ? 1 : 0);
}
