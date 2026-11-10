#!/usr/bin/env node
// Comms (Zoom talkback) probe against the FAKE engine, through the node's
// control API only (JSON lines on corevideo-edge.exe stdin/stdout).
//
//   node scripts\probe-comms.mjs [--bin build\bin]
//
// Proves, end to end through node + fake engine + the shared comms core:
//   1. comms verbs are refused outside a meeting; `comms` is absent until the
//      engine reports and gone after leave
//   2. the bank forms (16 channels), every eligible guest lands on their own
//      channel, the web-client guest is reported (talkback:false) and refused
//   3. keying a slot: keyed + TX lamp, channelSends and framesSent rise;
//      releasing: sends stop
//   4. assign / unassign moves a person between channels
//   5. TONE moves the meter; LATCH (one slot and all), ALL CALL, gain, mic pick
//
// The fake proves the node/engine contract and the ported ladder, not Zoom:
// nothing here says a guest HEARD anything.

import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const args = {};
for (let i = 0; i < argv.length; i++) if (argv[i].startsWith('--')) args[argv[i].slice(2)] = argv[i + 1];
const bin = resolve(args.bin ?? join(root, 'build', 'bin'));
const exe = join(bin, 'corevideo-edge.exe');
if (!existsSync(exe)) {
  console.error(`missing ${exe}; build first`);
  process.exit(2);
}

let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${what}`);
  if (!ok) failures++;
  return ok;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const outDir = join(root, 'build', 'probe-comms');
mkdirSync(outDir, { recursive: true });
const configPath = join(outDir, 'probe-edge.json');
writeFileSync(configPath, JSON.stringify({ displayName: 'Edge Comms Probe', filePrefix: 'probe' }, null, 2));

const env = { ...process.env, COREVIDEO_FAKE_ENGINE_PARTICIPANTS: '4', COREVIDEO_FAKE_NO_CHURN: '1' };
delete env.COREVIDEO_FAKE_WEB_CLIENT_ID;
delete env.COREVIDEO_FAKE_COMMS_ROLE;
const child = spawn(exe, ['--fake', '--config', configPath], { env, stdio: ['pipe', 'pipe', 'pipe'] });
child.stderr.setEncoding('utf8');
child.stderr.on('data', () => {});

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
    let m;
    try { m = JSON.parse(line); } catch { continue; }
    if (m.type === 'state') state = m;
    else if (m.type === 'result' && m.id && pending.has(m.id)) {
      pending.get(m.id)(m);
      pending.delete(m.id);
    }
  }
});

function send(cmd, fields = {}) {
  const id = String(nextId++);
  return new Promise((res) => {
    pending.set(id, res);
    child.stdin.write(JSON.stringify({ cmd, id, ...fields }) + '\n');
  });
}

async function waitFor(pred, timeoutMs, what) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try { if (state && pred(state)) return check(true, what); } catch { /* not there yet */ }
    await sleep(100);
  }
  return check(false, `${what} (timed out after ${timeoutMs / 1000}s; phase=${state?.phase} comms=${JSON.stringify(state?.comms?.reason ?? null)})`);
}

const comms = () => state?.comms;
const chan = (slot) => comms().channels[slot];
const person = (id) => comms().roster.find((p) => p.participantId === id);

try {
  await waitFor((s) => s.phase === 'idle', 10000, 'node is up and idle');
  check(state.comms === undefined, '`comms` is absent before any engine reports');
  let r = await send('comms-key', { slot: 0, on: true });
  check(!r.ok && r.error === 'Join a meeting first.', `comms-key refused outside a meeting ("${r.error}")`);
  r = await send('comms-key', { slot: 0, on: false });
  check(r.ok, 'releasing a key is never refused');

  r = await send('join', { meeting: '1234567890' });
  check(r.ok, `join accepted${r.error ? ` (${r.error})` : ''}`);
  await waitFor((s) => s.phase === 'in-meeting', 15000, 'in the meeting');
  await waitFor((s) => s.comms && s.comms.available === true, 10000, 'comms available (host/co-host, talkback supported)');
  await waitFor((s) => s.comms.channels.length === 16 && s.comms.channels.every((c) => c.ready), 10000, 'all 16 channels formed');
  check(comms().lamps.link && comms().lamps.meetingMic && comms().lamps.channel, 'lamps: LINK, MTG MIC, CHANNEL on');

  // Fake roster: 101 mv1, 102 mv2, 103 Producer, 104 mv4 (the web client).
  await waitFor((s) => [101, 102, 103].every((id) => {
    const p = s.comms.roster.find((x) => x.participantId === id);
    return p && p.talkback && p.channels.length === 1 && s.comms.channels[p.channels[0]].listeners === 1;
  }), 15000, 'each eligible guest is confirmed on their own channel');
  const slots = [101, 102, 103].map((id) => person(id).channels[0]);
  check(new Set(slots).size === 3, `three distinct channels (${slots.join(', ')})`);
  check(chan(person(101).channels[0]).label === 'mv1', 'a one-person channel wears its person\'s name');
  const web = person(104);
  check(web && web.talkback === false && web.channels.length === 0, 'the web-client guest is reported with talkback:false and no channel');
  r = await send('comms-assign', { slot: 9, participantId: 104, on: true });
  check(!r.ok && /web client/.test(r.error), `assigning the web client is refused ("${r.error}")`);

  // Key mv1's channel.
  const s1 = person(101).channels[0];
  const before = { ...comms().stats };
  r = await send('comms-key', { slot: s1, on: true });
  check(r.ok, `comms-key slot ${s1} on`);
  await waitFor((s) => s.comms.channels[s1].keyed && s.comms.lamps.tx, 3000, 'cell keyed and TX lamp on');
  await sleep(1500);
  const keyed = { ...comms().stats };
  check(keyed.channelSends - before.channelSends >= 40, `channelSends rose while keyed (+${keyed.channelSends - before.channelSends} in ~1.5 s)`);
  check(keyed.framesSent > before.framesSent, `framesSent rose (+${keyed.framesSent - before.framesSent})`);
  check(keyed.sendErrors === 0, 'no send errors');
  check(comms().mic.peakDbfs !== null && comms().mic.peakDbfs > -30, `meter moves while keyed (${comms().mic.peakDbfs} dBFS)`);
  r = await send('comms-key', { slot: s1, on: false });
  await waitFor((s) => !s.comms.channels[s1].keyed && !s.comms.lamps.tx, 3000, 'released: not keyed, TX off');
  await sleep(500);
  const released = comms().stats.channelSends;
  await sleep(1500);
  check(comms().stats.channelSends - released <= 2, `sends stop after release (+${comms().stats.channelSends - released})`);

  // Assign / unassign: move mv1 to slot 10.
  const target = [...Array(16).keys()].find((s) => !slots.includes(s) && comms().channels[s].listeners === 0 && s >= 8);
  r = await send('comms-assign', { slot: target, participantId: 101, on: true });
  check(r.ok, `assign mv1 -> slot ${target}`);
  await waitFor((s) => s.comms.roster.find((p) => p.participantId === 101).channels.includes(target), 3000, 'assignment shows at once (pending)');
  await waitFor((s) => s.comms.channels[target].listeners === 1, 15000, `mv1 confirmed on slot ${target}`);
  r = await send('comms-assign', { slot: s1, participantId: 101, on: false });
  check(r.ok, `unassign mv1 from slot ${s1}`);
  await waitFor((s) => s.comms.channels[s1].listeners === 0 &&
    JSON.stringify(s.comms.roster.find((p) => p.participantId === 101).channels) === JSON.stringify([target]),
  15000, `mv1 removed from slot ${s1}, now only on ${target}`);

  // TONE.
  r = await send('comms-tone', { on: true });
  await waitFor((s) => s.comms.mic.tone === true && s.comms.mic.peakDbfs > -30, 3000, 'TONE on moves the meter');
  const seen = new Set();
  for (let i = 0; i < 8; i++) { seen.add(comms().mic.peakDbfs); await sleep(300); }
  check(seen.size > 1, `the meter moves while toned (${[...seen].join(', ')} dBFS)`);
  r = await send('comms-tone', { on: false });
  await waitFor((s) => s.comms.mic.tone === false && s.comms.mic.peakDbfs <= -59, 3000, 'TONE off: quiet room again');

  // LATCH one slot, LATCH ALL, ALL CALL.
  const latchedBefore = comms().stats.channelSends;
  r = await send('comms-latch', { slot: target, on: true });
  await waitFor((s) => s.comms.channels[target].latched && s.comms.channels[target].keyed, 3000, `slot ${target} latched and keyed`);
  await sleep(1000);
  check(comms().stats.channelSends > latchedBefore + 20, 'a latched channel sends');
  r = await send('comms-latch', { slot: target, on: false });
  await waitFor((s) => !s.comms.channels[target].keyed, 3000, 'unlatched');
  r = await send('comms-latch', { all: true, on: true });
  await waitFor((s) => s.comms.channels.every((c) => c.keyed && c.latched), 3000, 'LATCH ALL keys every channel');
  r = await send('comms-latch', { all: true, on: false });
  await waitFor((s) => s.comms.channels.every((c) => !c.keyed), 3000, 'LATCH ALL off releases every channel');
  r = await send('comms-key', { all: true, on: true });
  await waitFor((s) => s.comms.channels.every((c) => c.keyed && !c.latched), 3000, 'ALL CALL keys every channel');
  r = await send('comms-key', { all: true, on: false });
  await waitFor((s) => s.comms.channels.every((c) => !c.keyed), 3000, 'ALL CALL released');

  // Gain, sidetone, devices.
  r = await send('comms-gain', { db: 6 });
  await waitFor((s) => s.comms.mic.gainDb === 6, 3000, 'gain +6 dB reported');
  r = await send('comms-gain', { db: 21 });
  check(!r.ok, `gain 21 refused ("${r.error}")`);
  r = await send('comms-sidetone', { on: false });
  await waitFor((s) => s.comms.mic.sidetone === false, 3000, 'sidetone off reported');
  check(comms().mic.devices.length >= 2 && comms().mic.sidetoneDevices.length >= 2, 'device lists published');
  r = await send('comms-mic', { device: 'Headset' });
  await waitFor((s) => s.comms.mic.device === 'Fake Headset Mic', 3000, 'mic switched to "Fake Headset Mic"');
  r = await send('comms-mic', { device: 'No Such Mic' });
  await waitFor((s) => s.comms.log.some((l) => l.includes("no capture device matches 'No Such Mic'")) && s.comms.mic.peakDbfs === null,
    3000, 'a missing mic is said in the ops log and the meter is withdrawn');
  r = await send('comms-mic', { device: '' });
  await waitFor((s) => s.comms.mic.device === 'Fake Mic (USB Audio)' && s.comms.mic.peakDbfs !== null, 3000, 'back on the default mic');

  r = await send('leave');
  await waitFor((s) => s.phase === 'idle' && s.comms === undefined, 15000, '`comms` is gone after leave');
} catch (e) {
  check(false, `probe threw: ${e.stack ?? e}`);
} finally {
  child.stdin.write(JSON.stringify({ cmd: 'quit' }) + '\n');
  await sleep(1500);
  child.kill();
}

console.log(failures === 0 ? '\nCOMMS PROBE PASSED' : `\nCOMMS PROBE FAILED (${failures})`);
process.exit(failures === 0 ? 0 : 1);
