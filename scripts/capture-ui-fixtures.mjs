// Captures REAL node state lines (and copies of the node's live preview files)
// from corevideo-edge.exe --fake, for the operator app's headless screenshots
// (docs/screenshots). Everything here comes from the fake engine, labelled as
// such (engineKind "fake"); nothing is reachable from the shipped UI.
//
// usage: node scripts/capture-ui-fixtures.mjs <corevideo-edge.exe> <outDir> <recordDir> [main|move]
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';

const [exe, outDir, recordDir, scenario = 'main'] = process.argv.slice(2);
if (!exe || !outDir || !recordDir) {
  console.error('usage: node capture-ui-fixtures.mjs <corevideo-edge.exe> <outDir> <recordDir> [main|move]');
  process.exit(2);
}
fs.mkdirSync(outDir, { recursive: true });
fs.mkdirSync(recordDir, { recursive: true });

const env = { ...process.env, COREVIDEO_FAKE_ENGINE_PARTICIPANTS: '10' };
if (scenario === 'move') {
  env.COREVIDEO_FAKE_ROOM_MOVE_AT_MS = '6000';
  env.COREVIDEO_FAKE_ROOM_MOVE_MS = '12000';
}
const child = spawn(exe, ['--fake'], { env, stdio: ['pipe', 'pipe', 'pipe'] });
child.stderr.on('data', () => {});
let last = null;
let nextId = 1;
const waiting = new Map();
readline.createInterface({ input: child.stdout }).on('line', (line) => {
  let msg;
  try { msg = JSON.parse(line); } catch { return; }
  if (msg.type === 'state') last = line;
  else if (msg.type === 'result' && waiting.has(msg.id)) { waiting.get(msg.id)(msg); waiting.delete(msg.id); }
});
const send = (cmd, fields = {}) => new Promise((resolve) => {
  const id = String(nextId++);
  waiting.set(id, resolve);
  child.stdin.write(JSON.stringify({ cmd, id, ...fields }) + '\n');
});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const state = () => JSON.parse(last);
const save = async (name, copyPreview = false) => {
  await send('state');
  await sleep(300);
  fs.writeFileSync(path.join(outDir, `state-${name}.json`), last + '\n');
  const s = state();
  if (copyPreview && s.previewDir && fs.existsSync(s.previewDir)) {
    const dst = path.join(outDir, `preview-${name}`);
    fs.mkdirSync(dst, { recursive: true });
    for (const f of fs.readdirSync(s.previewDir)) fs.copyFileSync(path.join(s.previewDir, f), path.join(dst, f));
  }
  console.log(`saved ${name}: phase=${s.phase} subs=${(s.subscriptions || []).length} rec=${s.recording?.active} previewDir=${!!s.previewDir} moving=${!!s.movingRooms}`);
};
const until = async (pred, ms = 15000) => {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (last && pred(state())) return; await sleep(100); }
  throw new Error('timeout; last state: ' + last);
};
const check = (r, what) => { if (!r.ok) console.log(`${what} refused: ${r.error}`); };

try {
  await until(() => true);
  if (scenario === 'main') {
    await save('idle');
    check(await send('set-record-folder', { path: recordDir }), 'set-record-folder');
    check(await send('join', { meeting: 'https://zoom.us/j/81234567890?pwd=fake' }), 'join');
    await until((s) => s.phase === 'in-meeting' && s.participants);
    await sleep(1500);
    await save('joined-empty');
    const people = state().participants.filter((p) => !p.self);
    for (const p of people.slice(0, 5)) check(await send('subscribe', { participantId: p.id, on: true }), 'subscribe');
    await sleep(500);
    // Arm 3 of the 5 (the node may arm new subscriptions by default).
    for (const sub of state().subscriptions) check(await send('iso', { slot: sub.slot, on: sub.slot <= 3 }), 'iso');
    await sleep(3500);
    await save('in-meeting', true);
    check(await send('engine', { on: false }), 'engine off');
    await sleep(1500);
    await save('engine-off', true);
    check(await send('engine', { on: true }), 'engine on');
    await until((s) => s.rawMedia?.active === true);
    await sleep(3000);
    check(await send('set-record-format', { height: 1080, fps: 30 }), 'set-record-format');
    check(await send('record-start'), 'record-start');
    await sleep(8000);
    await save('recording', true);
    check(await send('record-stop'), 'record-stop');
    await sleep(1000);
    await save('saved', true);
    check(await send('leave'), 'leave');
    await until((s) => s.phase === 'idle');
    await save('left');
  } else if (scenario === 'feeds') {
    check(await send('join', { meeting: '81234567890' }), 'join');
    await until((s) => s.phase === 'in-meeting' && s.participants);
    await sleep(800);
    for (const p of state().participants.filter((p) => !p.self).slice(0, 3)) check(await send('subscribe', { participantId: p.id, on: true }), 'subscribe');
    check(await send('set-feeds', { mode: 'ndi' }), 'set-feeds ndi');
    await sleep(4000);
    await save('feeds-ndi', true);
    // Nothing listens on this port: the node reports the failure verbatim.
    check(await send('set-feeds', { mode: 'srt', srtHost: '127.0.0.1', srtBasePort: 19000, srtLatencyMs: 120 }), 'set-feeds srt');
    await sleep(5000);
    await save('feeds-srt', true);
    check(await send('set-feeds', { mode: 'off' }), 'set-feeds off');
  } else if (scenario === 'move') {
    check(await send('join', { meeting: '81234567890' }), 'join');
    await until((s) => s.phase === 'in-meeting' && s.participants);
    await sleep(800);
    for (const p of state().participants.filter((p) => !p.self).slice(0, 3)) check(await send('subscribe', { participantId: p.id, on: true }), 'subscribe');
    await until((s) => s.movingRooms === true, 20000);
    await sleep(200);
    await save('moving-rooms', true);
  }
} catch (e) {
  console.error(String(e));
  process.exitCode = 1;
} finally {
  child.stdin.write('{"cmd":"quit"}\n');
  child.stdin.end();
}
