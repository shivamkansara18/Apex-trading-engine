import {randomBytes} from 'node:crypto';
import {setTimeout as pause} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import {GatewaySession} from './session.mjs';
import {resolveMarket} from './markets.mjs';

export function randomSource(seed) {
  let state = seed >>> 0 || 1;
  return () => { state ^= state << 13; state ^= state >>> 17; state ^= state << 5; return (state >>> 0) / 4294967296; };
}
export function quotePrices(book, anchor, spread, inventory = 0n) {
  const bid = book.bids[0], ask = book.asks[0];
  const reference = bid && ask ? (BigInt(bid.price) + BigInt(ask.price)) / 2n
    : bid ? BigInt(bid.price) + spread : ask ? BigInt(ask.price) - spread : anchor;
  const skew = inventory > 100n ? -2n : inventory < -100n ? 2n : 0n;
  const center = reference + skew;
  return {buy: (center > spread ? center - spread : 1n).toString(), sell: (center + spread > 1n ? center + spread : 2n).toString()};
}
async function snapshot(session) {
  return new Promise((resolve, reject) => {
    let request;
    const timer = setTimeout(() => { cleanup(); reject(new Error('Book snapshot timed out')); }, 5000);
    const onMessage = value => { if (value.type === 'book' && value.request === request) { cleanup(); resolve(value); } };
    const onDisconnect = reason => { cleanup(); reject(new Error(reason)); };
    function cleanup() { clearTimeout(timer); session.off('message', onMessage); session.off('disconnect', onDisconnect); }
    session.on('message', onMessage); session.on('disconnect', onDisconnect);
    try { request = session.request(6); } catch (error) { cleanup(); reject(error); }
  });
}
export async function runAdaptive({gatewayPort = 9000, steps = 1000, interval = 250, seed = randomBytes(4).readUInt32BE(),
  initialPrice = 10000, spread = 5, maxQuantity = 50, signal, log = console.log} = {}) {
  for (const [name, value, min, max] of [['port',gatewayPort,1,65535],['steps',steps,1,100000],['interval',interval,0,5000],
    ['seed',seed,0,4294967295],['initial price',initialPrice,100,999999000],['spread',spread,1,1000],['quantity',maxQuantity,1,1000]]) {
    if (!Number.isInteger(value) || value < min || value > max) throw new Error(`Invalid ${name}`);
  }
  const rng = randomSource(seed), sessions = [], agents = [];
  const summary = {seed, submitted: 0, accepted: 0, rejected: 0, observedTrades: 0, stepsCompleted: 0, inventory: {}};
  let observer, nextId = BigInt('0x' + randomBytes(5).toString('hex')) * 1000000n;
  const id = () => (++nextId).toString();
  const quantity = () => String(1 + Math.floor(rng() * maxQuantity));
  async function execute(agent, input) {
    const result = await agent.session.execute({type: 'submit', ...input});
    ++summary.submitted; if (result.ack.accepted) ++summary.accepted; else ++summary.rejected;
    log(`P${agent.participant} ${input.action} ${input.side ?? ''} ${input.quantity ?? ''} @${input.price ?? 'market'} -> ${result.ack.accepted ? 'accepted' : result.reports.find(r => r.kind === 'rejected')?.reason}`);
    return result;
  }
  try {
    observer = new GatewaySession(gatewayPort, 0); sessions.push(observer); await observer.ready;
    observer.on('message', value => { if (value.type === 'trade') ++summary.observedTrades; }); observer.request(5);
    for (const participant of [9,10,11,12]) {
      const session = new GatewaySession(gatewayPort, participant); sessions.push(session); await session.ready;
      const agent = {participant, session, inventory: 0n, orders: new Map()}; agents.push(agent);
      session.on('message', value => {
        if (value.type !== 'report') return;
        if (value.order) {
          const order = value.order;
          if (value.kind === 'cancelled' || BigInt(order.remaining) === 0n) agent.orders.delete(order.id);
          else if (order.orderType === 0) agent.orders.set(order.id, order);
        }
        if (value.trade) agent.inventory += (value.trade.buyer === participant ? 1n : -1n) * BigInt(value.trade.quantity);
      });
    }
    log(`Adaptive simulation seed=${seed}; participants 9-12. Quotes react to live depth and inventory. Ctrl-C cancels owned quotes.`);
    for (let step = 0; step < steps && !signal?.aborted; ++step) {
      for (const agent of agents.slice(0,2)) {
        const book = await snapshot(observer);
        const prices = quotePrices(book, BigInt(initialPrice), BigInt(spread + Math.floor(rng() * 4)), agent.inventory);
        for (const side of ['buy','sell']) {
          const old = [...agent.orders.values()].find(order => order.side === (side === 'buy' ? 0 : 1));
          if (old && rng() > 0.4) continue;
          if (old) await execute(agent, {action: 'replace', orderId: old.id, newOrderId: id(), price: prices[side], quantity: quantity()});
          else await execute(agent, {action: 'new', orderId: id(), side, orderType: 'limit', price: prices[side], quantity: quantity()});
        }
      }
      for (const agent of agents.slice(2)) {
        if (rng() > 0.55) continue;
        const book = await snapshot(observer);
        const side = agent.inventory > 200n ? 'sell' : agent.inventory < -200n ? 'buy' : rng() < 0.5 ? 'buy' : 'sell';
        if (!(side === 'buy' ? book.asks.length : book.bids.length)) continue;
        await execute(agent, {action: 'new', orderId: id(), side, orderType: 'market', quantity: quantity()});
      }
      summary.stepsCompleted++;
      if (interval && !signal?.aborted) await pause(interval, undefined, {signal}).catch(error => { if (error.name !== 'AbortError') throw error; });
    }
  } finally {
    // Preserve human orders; clean up only orders tracked from these sessions.
    for (const agent of agents) {
      if (!agent.session.closed) for (const order of [...agent.orders.values()]) {
        try { await execute(agent, {action: 'cancel', orderId: order.id}); } catch { break; }
      }
      summary.inventory[agent.participant] = agent.inventory.toString();
    }
    for (const session of sessions) session.close();
  }
  log(JSON.stringify(summary)); return summary;
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const controller = new AbortController();
  for (const signal of ['SIGINT','SIGTERM']) process.once(signal, () => controller.abort());
  try {
    const args = process.argv.slice(2), options = {signal: controller.signal};
    if (args.length % 2) throw new Error('Options require values');
    for (let i = 0; i < args.length; i += 2) {
      const key = {'--gateway-port':'gatewayPort','--steps':'steps','--interval-ms':'interval','--seed':'seed','--price':'initialPrice','--spread':'spread','--max-quantity':'maxQuantity','--symbol':'symbol','--bridge-port':'bridgePort'}[args[i]];
      if (!key) throw new Error(`Unknown option ${args[i]}`); options[key] = key === 'symbol' ? args[i+1] : Number(args[i+1]);
    }
    options.gatewayPort = await resolveMarket(options);
    await runAdaptive(options);
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
