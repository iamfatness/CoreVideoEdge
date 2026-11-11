// Captures Comms state lines from the node + fake engine for the screenshot
// fixtures (docs/screenshots/fixtures/state-comms*.json):
//   node scripts\capture-comms-fixtures.mjs [--bin build\bin] [--out docs\screenshots\fixtures]
// "comms": 6 guests, the bank up, guest 2 keyed (held). "comms-not-host":
// COREVIDEO_FAKE_COMMS_ROLE=attendee, so talkback is refused.
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const args = {};
for (let i = 0; i < argv.length; i++) if (argv[i].startsWith('--')) args[argv[i].slice(2)] = argv[i + 1];
const exe = join(resolve(args.bin ?? join(root, 'build', 'bin')), 'corevideo-edge.exe');
const out = resolve(args.out ?? join(root, 'docs', 'screenshots', 'fixtures'));
mkdirSync(out, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function run(name, extraEnv, act) {
  const work = join(root, 'build', 'capture-comms');
  mkdirSync(work, { recursive: true });
  const config = join(work, 'edge.json');
  writeFileSync(config, JSON.stringify({ displayName: 'Edge', filePrefix: 'edge' }));
  const env = { ...process.env, COREVIDEO_FAKE_ENGINE_PARTICIPANTS: '7', COREVIDEO_FAKE_NO_CHURN: '1', ...extraEnv };
  const child = spawn(exe, ['--fake', '--config', config], { env, stdio: ['pipe', 'pipe', 'ignore'] });
  let state = null, line = null, buf = '', id = 1;
  const pending = new Map();
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', (c) => {
    buf += c;
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      const l = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (!l) continue;
      const m = JSON.parse(l);
      if (m.type === 'state') { state = m; line = l; }
      else if (m.type === 'result' && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    }
  });
  const send = (cmd, f = {}) => new Promise((r) => {
    const i = String(id++);
    pending.set(i, r);
    child.stdin.write(JSON.stringify({ cmd, id: i, ...f }) + '\n');
  });
  const until = async (pred, ms, what) => {
    const end = Date.now() + ms;
    while (Date.now() < end) { if (state && pred(state)) return; await sleep(100); }
    throw new Error(`${name}: timed out waiting for ${what}`);
  };
  try {
    await send('join', { meeting: '81234567890' });
    await until((s) => s.phase === 'in-meeting', 20000, 'join');
    await act(send, until);
    await sleep(600);
    writeFileSync(join(out, `state-${name}.json`), line + '\n');
    console.log(`wrote state-${name}.json`);
  } finally {
    child.stdin.write(JSON.stringify({ cmd: 'quit', id: 'q' }) + '\n');
    await sleep(500);
    child.kill();
  }
}

await run('comms', {}, async (send, until) => {
  await until((s) => s.comms?.available && s.comms.channels.filter((c) => c.label && c.listeners > 0).length >= 5, 30000, 'talent confirmed on channels');
  const second = (s) => s.comms.channels.filter((c) => c.label)[1].slot;
  let slot = 1;
  await until((s) => { slot = second(s); return true; }, 1000, 'slot');
  await send('comms-key', { slot, on: true });
  await until((s) => s.comms.channels.find((c) => c.slot === slot)?.keyed, 5000, 'keyed');
});

await run('comms-not-host', { COREVIDEO_FAKE_COMMS_ROLE: 'attendee' }, async (_send, until) => {
  await until((s) => s.comms && !s.comms.available, 15000, 'refusal');
});
