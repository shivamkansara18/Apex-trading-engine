import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {setTimeout as pause} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';

// Submit real simulator commands through the existing native TCP client.
// This is opt-in activity, never fabricated browser data or external market data.
export async function runActivity({gatewayPort = 9000, rounds = 12, interval = 500,
  baseId = BigInt(Date.now()) * 1000n, buildDir = 'build/debug', log = console.log} = {}) {
  if (!Number.isInteger(gatewayPort) || gatewayPort < 1 || gatewayPort > 65535 ||
      !Number.isInteger(rounds) || rounds < 1 || rounds > 100 ||
      !Number.isInteger(interval) || interval < 0 || interval > 5000 ||
      baseId < 1n || baseId + BigInt(rounds * 4) > 18446744073709551615n) throw new Error('Invalid activity limits');
  const root = fileURLToPath(new URL('../../', import.meta.url));
  const client = path.resolve(root, buildDir, 'apex_client');
  let submitted = 0, previous;
  async function submit(participant, command) {
    const result = await promisify(execFile)(client, ['--port', String(gatewayPort), '--participant', String(participant), '--command', command], {timeout: 5000, maxBuffer: 65536});
    if (!/ACK command=\d+ accepted=1 /.test(result.stdout)) throw new Error(`Demo request rejected: ${command}\n${result.stdout}`);
    log(`participant ${participant}: ${command}`); ++submitted;
    if (interval) await pause(interval);
  }
  log(`Submitting simulated orders to 127.0.0.1:${gatewayPort}; base order ID ${baseId}.`);
  for (let round = 0; round < rounds; ++round) {
    if (previous) { await submit(2, `CANCEL ${previous.bid}`); await submit(4, `CANCEL ${previous.ask}`); }
    const id = baseId + BigInt(round * 4), price = 10000 + (round % 5) * 10;
    await submit(1, `BUY ${id} 100 ${price}`);
    await submit(2, `BUY ${id + 1n} 50 ${price}`);
    await submit(3, `SELL ${id + 2n} 120 ${price - 10}`);
    await submit(4, `SELL ${id + 3n} 80 ${price + 100}`);
    previous = {bid: id + 1n, ask: id + 3n};
  }
  log(`Activity complete: ${submitted} commands, ${rounds * 2} expected trades on a fresh gateway. Final bid/ask remain visible.`);
  return {submitted, rounds};
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const args = process.argv.slice(2), settings = {};
    if (args.length % 2) throw new Error('Options require values');
    for (let i = 0; i < args.length; i += 2) {
      const key = {'--gateway-port': 'gatewayPort', '--rounds': 'rounds', '--interval-ms': 'interval', '--base-id': 'baseId', '--build-dir': 'buildDir'}[args[i]];
      if (!key) throw new Error(`Unknown activity option ${args[i]}`);
      settings[key] = key === 'baseId' ? BigInt(args[i + 1]) : key === 'buildDir' ? args[i + 1] : Number(args[i + 1]);
    }
    await runActivity(settings);
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
