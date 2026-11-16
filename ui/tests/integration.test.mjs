import test from 'node:test';
import assert from 'node:assert/strict';
import net from 'node:net';
import http from 'node:http';
import {spawn, execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {createInterface} from 'node:readline';
import {once} from 'node:events';
import {mkdtemp} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {WebSocket} from 'ws';
import {startBridge} from '../bridge/server.mjs';
import {Decoder, frame} from '../bridge/protocol.mjs';
import {advance, emptyFeed} from '../src/feed.ts';
const project = fileURLToPath(new URL('../../', import.meta.url));
const build = path.resolve(project, process.env.APEX_BUILD_DIR || 'build/debug');
class Inbox {
  values = []; waiters = [];
  push(value) {
    const index = this.waiters.findIndex(w => w.predicate(value));
    if (index < 0) this.values.push(value);
    else { const waiter = this.waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); }
  }
  next(predicate) {
    const index = this.values.findIndex(predicate);
    if (index >= 0) return Promise.resolve(this.values.splice(index, 1)[0]);
    return new Promise((resolve, reject) => {
      const waiter = {predicate, resolve};
      waiter.timer = setTimeout(() => { this.waiters = this.waiters.filter(w => w !== waiter); reject(new Error('Message deadline exceeded')); }, 5000);
      this.waiters.push(waiter);
    });
  }
}
async function observe(port, symbol) {
  const inbox = new Inbox();
  const socket = new WebSocket(`ws://127.0.0.1:${port}/stream${symbol ? '?symbol='+encodeURIComponent(symbol) : ''}`, {origin: `http://127.0.0.1:${port}`});
  socket.on('message', bytes => inbox.push(JSON.parse(bytes)));
  await once(socket, 'open');
  return {socket, inbox};
}
async function trader(port, participant) {
  const socket = net.createConnection({host: '127.0.0.1', port});
  const inbox = new Inbox(), decoder = new Decoder();
  socket.on('data', chunk => decoder.append(chunk).forEach(value => inbox.push(value)));
  await once(socket, 'connect');
  const hello = Buffer.alloc(4); hello.writeUInt32BE(participant);
  socket.write(frame(1, 1, hello)); await inbox.next(m => m.type === 101);
  return {socket, async order(id, side, quantity, price) {
    const payload = Buffer.alloc(38); payload.writeBigUInt64BE(BigInt(id)); payload.writeUInt32BE(participant, 8);
    payload.writeUInt8(side, 12); payload.writeUInt8(0, 13); payload.writeBigInt64BE(BigInt(price), 14);
    payload.writeBigUInt64BE(BigInt(quantity), 22);
    socket.write(frame(2, 2, payload));
    const ack = await inbox.next(m => m.type === 102); assert.equal(ack.payload[12], 1);
  }};
}

test('real gateway -> WebSocket -> snapshot recovery -> reconnect -> sealed replay', {timeout: 20000}, async t => {
  const directory = await mkdtemp(path.join(tmpdir(), 'apex-ui-'));
  const journal = path.join(directory, 'session.apex');
  const gateway = spawn(path.join(build, 'apex_gateway'), ['--port', '0', '--journal', journal]);
  let errorText = ''; gateway.stderr.on('data', bytes => errorText += bytes);
  const lines = createInterface({input: gateway.stdout});
  const gatewayPort = await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error('Gateway startup deadline')), 5000);
    gateway.once('error', error => { clearTimeout(timeout); reject(error); });
    gateway.once('exit', () => { clearTimeout(timeout); reject(new Error(errorText)); });
    lines.on('line', line => { const match = line.match(/listening on 127\.0\.0\.1:(\d+)/); if (match) { clearTimeout(timeout); resolve(Number(match[1])); } });
  });
  t.after(() => { if (gateway.exitCode === null) gateway.kill('SIGINT'); });
  const bridge = await startBridge({port: 0, gatewayPort});
  t.after(() => bridge.close());
  const first = await observe(bridge.port); t.after(() => first.socket.terminate());
  const initial = await first.inbox.next(m => m.type === 'book');
  assert.equal(initial.anchor, true); assert.equal(initial.sequence, '0');
  let uiState = advance(emptyFeed(), initial, Date.now()).feed;
  const http = await fetch(`http://127.0.0.1:${bridge.port}/`);
  assert.equal(http.status, 200); assert.match(await http.text(), /Apex/);
  const traders = await Promise.all([1, 2, 3].map(id => trader(gatewayPort, id)));
  for (const client of traders) t.after(() => client.socket.destroy());
  await traders[0].order(1, 0, 100, 10000);
  const firstBook = await first.inbox.next(m => m.type === 'book' && m.command === '1');
  assert.deepEqual(firstBook.bids, [{price: '10000', quantity: '100', orders: 1}]);
  uiState = advance(uiState, firstBook, Date.now()).feed;
  await traders[1].order(2, 0, 50, 10000);
  await traders[2].order(3, 1, 120, 9900);
  const trade1 = await first.inbox.next(m => m.type === 'trade' && m.id === '1');
  const trade2 = await first.inbox.next(m => m.type === 'trade' && m.id === '2');
  assert.equal(trade1.price, '10000'); assert.equal(trade1.quantity, '100'); assert.equal(trade2.quantity, '20');
  const final = await first.inbox.next(m => m.type === 'book' && m.command === '3');
  assert.equal(final.sequence, '5'); assert.equal(final.bids[0].quantity, '30');
  // Deliberately skip intermediate publications at the UI boundary to simulate a gap.
  const gap = advance(uiState, final, Date.now());
  assert.equal(gap.snapshot, true); assert.equal(gap.feed.recovering, true);
  first.socket.send(JSON.stringify({type: 'snapshot'}));
  const anchor = await first.inbox.next(m => m.type === 'book' && m.anchor && m.command === '3');
  assert.equal(anchor.sequence, '5');
  uiState = advance(gap.feed, anchor, Date.now()).feed;
  assert.equal(uiState.recovering, false); assert.equal(uiState.book.bids[0].quantity, '30');
  // A second request inside the rate limit must be coalesced, never silently lost.
  first.socket.send(JSON.stringify({type: 'snapshot'}));
  assert.equal((await first.inbox.next(m => m.type === 'book' && m.anchor && m.command === '3')).sequence, '5');
  const invalidClose = once(first.socket, 'close');
  first.socket.send(JSON.stringify({type: 'order', quantity: 10}));
  assert.equal((await invalidClose)[0], 1008);
  const second = await observe(bridge.port); t.after(() => second.socket.terminate());
  const reconnected = await second.inbox.next(m => m.type === 'book');
  assert.equal(reconnected.anchor, true); assert.equal(reconnected.bids[0].quantity, '30');
  const exit = once(gateway, 'exit'); gateway.kill('SIGINT'); await exit;
  const replay = await promisify(execFile)(path.join(build, 'apex'), ['--replay', journal]);
  assert.match(replay.stdout, /commands=3/);
});

test('bridge rejects cross-origin upgrades and unexpected Host headers', {timeout: 10000}, async t => {
  const bridge = await startBridge({port: 0, gatewayPort: 1}); t.after(() => bridge.close());
  const evil = new WebSocket(`ws://127.0.0.1:${bridge.port}/stream`, {origin: 'https://example.com'});
  const rejected = new Promise(resolve => evil.on('unexpected-response', (_, response) => { resolve(response.statusCode); response.resume(); evil.terminate(); }));
  evil.on('error', () => {}); assert.equal(await rejected, 403);
  const status = await new Promise((resolve, reject) => {
    http.get({host: '127.0.0.1', port: bridge.port, path: '/', headers: {Host: 'attacker.example'}}, response => { response.resume(); resolve(response.statusCode); }).on('error', reject);
  });
  assert.equal(status, 403);
  const malformed = net.createConnection({host:'127.0.0.1',port:bridge.port});
  await once(malformed,'connect');
  const reply = new Promise(resolve => { let text=''; malformed.on('data', bytes=>text+=bytes); malformed.on('end',()=>resolve(text)); });
  malformed.write(`GET http://[ HTTP/1.1\r\nHost: 127.0.0.1:${bridge.port}\r\nOrigin: http://127.0.0.1:${bridge.port}\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n`);
  assert.match(await reply,/400 Bad Request/);
  assert.equal((await fetch(`http://127.0.0.1:${bridge.port}/`)).status,200);
});

test('one-command launcher serves the UI and stops its owned gateway', {timeout: 15000}, async t => {
  const launcher = spawn(process.execPath, [path.join(project, 'ui/bridge/run.mjs'), '--gateway-port', '0', '--port', '0', '--build-dir', build]);
  t.after(() => { if (launcher.exitCode === null) launcher.kill('SIGINT'); });
  let errors = '', gatewayPort;
  launcher.stderr.on('data', bytes => errors += bytes);
  const lines = createInterface({input: launcher.stdout});
  const url = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Launcher readiness deadline')), 5000);
    launcher.once('error', error => { clearTimeout(timer); reject(error); });
    launcher.once('exit', () => { clearTimeout(timer); reject(new Error(errors || 'Launcher exited before ready')); });
    lines.on('line', line => {
      const port = line.match(/listening on 127\.0\.0\.1:(\d+)/); if (port) gatewayPort = Number(port[1]);
      const match = line.match(/Apex dashboard: (http:\/\/127\.0\.0\.1:\d+)/);
      if (match) { clearTimeout(timer); resolve(match[1]); }
    });
  });
  assert.equal((await fetch(url)).status, 200);
  const themeScript = await fetch(`${url}/assets/theme.js`);
  assert.equal(themeScript.status, 200); assert.match(await themeScript.text(), /apex-theme/);
  const observer = await observe(Number(new URL(url).port));
  t.after(() => observer.socket.terminate());
  await observer.inbox.next(m => m.type === 'book' && m.anchor);
  const activity = await promisify(execFile)(process.execPath, [path.join(project, 'ui/bridge/activity.mjs'), '--gateway-port', String(gatewayPort), '--rounds', '2', '--interval-ms', '0', '--base-id', '10000', '--build-dir', build]);
  assert.match(activity.stdout, /10 commands, 4 expected trades/);
  const activeBook = await observer.inbox.next(m => m.type === 'book' && m.command === '10');
  assert.equal(activeBook.bids[0].quantity, '30'); assert.equal(activeBook.asks[0].quantity, '80');
  assert.equal(activeBook.bids[0].price, '10010');
  observer.socket.close(); await once(observer.socket, 'close');
  const exit = once(launcher, 'exit'); launcher.kill('SIGINT');
  assert.equal((await exit)[0], 0);
  const refused = net.createConnection({host: '127.0.0.1', port: gatewayPort});
  t.after(() => refused.destroy());
  const error = await once(refused, 'error'); assert.equal(error[0].code, 'ECONNREFUSED');
});

async function liveStack(t) {
  const directory = await mkdtemp(path.join(tmpdir(), 'apex-trading-'));
  const journal = path.join(directory, 'session.apex');
  const gateway = spawn(path.join(build, 'apex_gateway'), ['--port','0','--journal',journal]);
  const lines = createInterface({input: gateway.stdout});
  gateway.stderr.on('data', () => {});
  const gatewayPort = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Gateway readiness timeout')), 5000);
    gateway.once('error', error => { clearTimeout(timer); reject(error); });
    lines.on('line', line => { const match = line.match(/listening on 127\.0\.0\.1:(\d+)/); if (match) { clearTimeout(timer); resolve(Number(match[1])); } });
  });
  t.after(() => { if (gateway.exitCode === null) gateway.kill('SIGINT'); });
  const bridge = await startBridge({port:0,gatewayPort}); t.after(() => bridge.close());
  return {gateway, bridge, gatewayPort, journal};
}
async function browserTrader(t, port, participant, symbol) {
  const socket = new WebSocket(`ws://127.0.0.1:${port}/trade?participant=${participant}${symbol ? '&symbol='+encodeURIComponent(symbol) : ''}`, {origin:`http://127.0.0.1:${port}`});
  const inbox = new Inbox(); socket.on('message', bytes => inbox.push(JSON.parse(bytes)));
  t.after(() => socket.terminate()); await once(socket,'open');
  return {socket,inbox};
}

test('browser trading computes real partial fills, replaces, cancels and business rejections; feed is shared', {timeout:20000}, async t => {
  const {bridge,gateway,journal} = await liveStack(t);
  const a = await observe(bridge.port), b = await observe(bridge.port);
  t.after(() => a.socket.terminate()); t.after(() => b.socket.terminate());
  await a.inbox.next(m => m.type === 'book' && m.anchor); await b.inbox.next(m => m.type === 'book' && m.anchor);
  assert.equal(bridge.stats().observerConnections,1); assert.equal(bridge.stats().observers,2);
  const buyer = await browserTrader(t,bridge.port,5), seller = await browserTrader(t,bridge.port,6);
  await buyer.inbox.next(m => m.type === 'ready'); await seller.inbox.next(m => m.type === 'ready');
  async function submit(client, data) { client.socket.send(JSON.stringify({type:'submit',...data})); return client.inbox.next(m => m.type === 'complete'); }
  const id = '9007199254740993';
  assert.equal((await submit(buyer,{action:'new',orderId:id,side:'buy',orderType:'limit',price:'10500',quantity:'10'})).accepted,true);
  for (const client of [a,b]) assert.equal((await client.inbox.next(m => m.type === 'book' && m.command === '1')).bids[0].quantity,'10');
  assert.equal((await submit(seller,{action:'new',orderId:'2',side:'sell',orderType:'market',quantity:'4'})).accepted,true);
  const fill = await buyer.inbox.next(m => m.type === 'report' && m.kind === 'updated' && m.order.remaining === '6');
  assert.equal(fill.order.id,id); assert.equal(fill.request,'0');
  assert.equal((await submit(buyer,{action:'replace',orderId:id,newOrderId:'9007199254740994',price:'10400',quantity:'8'})).accepted,true);
  assert.equal((await submit(buyer,{action:'cancel',orderId:'9007199254740994'})).accepted,true);
  const rejection = await submit(buyer,{action:'new',orderId:'3',side:'buy',orderType:'limit',price:'10000',quantity:'0'});
  assert.equal(rejection.accepted,false); assert.equal(rejection.reason,'ZeroQuantity');
  buyer.socket.send(JSON.stringify({type:'submit',participant:6,action:'cancel',orderId:'3'}));
  assert.match((await buyer.inbox.next(m => m.type === 'inputError')).message,/identity/);
  const duplicate = await browserTrader(t,bridge.port,5);
  assert.match((await duplicate.inbox.next(m => m.type === 'error')).message,/already/);
  a.socket.close(); await once(a.socket,'close');
  assert.equal(bridge.stats().observerConnections,1);
  assert.equal((await submit(buyer,{action:'new',orderId:'4',side:'buy',orderType:'limit',price:'10000',quantity:'2'})).accepted,true);
  assert.equal((await b.inbox.next(m => m.type === 'book' && m.command === '6')).bids[0].quantity,'2');
  const exit = once(gateway,'exit'); gateway.kill('SIGINT'); await exit;
  assert.match((await promisify(execFile)(path.join(build,'apex'),['--replay',journal])).stdout,/commands=6/);
});

test('adaptive agents react to a human book, remain bounded, clean their own quotes and replay', {timeout:30000}, async t => {
  const {gateway,gatewayPort,journal} = await liveStack(t);
  const human = await trader(gatewayPort,1); t.after(() => human.socket.destroy());
  await human.order(42,0,100,20000);
  const {runAdaptive} = await import('../bridge/adaptive.mjs');
  const result = await runAdaptive({gatewayPort,steps:12,interval:0,seed:42,log:()=>{}});
  assert.equal(result.stepsCompleted,12); assert.ok(result.accepted>0); assert.ok(result.observedTrades>0);
  const {GatewaySession} = await import('../bridge/session.mjs');
  const observer = new GatewaySession(gatewayPort,0); t.after(() => observer.close()); await observer.ready;
  const book = new Promise(resolve => observer.on('message', value => { if (value.type === 'book') resolve(value); }));
  observer.request(6); const final = await book;
  assert.deepEqual(final.asks,[]); // All agent quotes were cancelled; human bid remains or was legitimately filled.
  assert.ok(final.bids.every(level => level.price === '20000'));
  const exit = once(gateway,'exit'); gateway.kill('SIGINT'); await exit;
  const replay = await promisify(execFile)(path.join(build,'apex'),['--replay',journal]);
  assert.match(replay.stdout,new RegExp(`commands=${result.submitted+1} `));
});

test('shared observer fanout remains one upstream and enforces browser capacity', {timeout:15000}, async t => {
  const {gatewayPort}=await liveStack(t);
  const bridge=await startBridge({port:0,gatewayPort,maxClients:4});t.after(()=>bridge.close());
  const clients=await Promise.all(Array.from({length:4},()=>observe(bridge.port)));
  for(const client of clients){t.after(()=>client.socket.terminate());await client.inbox.next(m=>m.type==='book'&&m.anchor);}
  assert.equal(bridge.stats().observerConnections,1);assert.equal(bridge.stats().observers,4);
  const excess=new WebSocket(`ws://127.0.0.1:${bridge.port}/stream`,{origin:`http://127.0.0.1:${bridge.port}`});
  excess.on('error',()=>{});
  const status=await new Promise(resolve=>excess.on('unexpected-response',(_,response)=>{resolve(response.statusCode);response.resume();excess.terminate();}));
  assert.equal(status,403);
  clients[0].socket.close();await once(clients[0].socket,'close');
  clients[1].socket.send(JSON.stringify({type:'snapshot'}));
  await clients[1].inbox.next(m=>m.type==='book'&&m.anchor);
  assert.equal(bridge.stats().observerConnections,1);
});

test('symbol routing isolates books, identities, order IDs, feed sequences and replay', {timeout:20000},async t=>{
  const alpha=await liveStack(t),beta=await liveStack(t);
  const bridge=await startBridge({port:0,markets:[{symbol:'ALPHA',gatewayPort:alpha.gatewayPort},{symbol:'BETA',gatewayPort:beta.gatewayPort}]});t.after(()=>bridge.close());
  const metadata=await(await fetch(`http://127.0.0.1:${bridge.port}/api/markets`)).json();
  assert.equal(metadata.schemaVersion,2);assert.equal(metadata.defaultSymbol,'ALPHA');
  const a=await observe(bridge.port,'ALPHA'),b=await observe(bridge.port,'BETA');
  for(const client of [a,b])t.after(()=>client.socket.terminate());
  assert.equal((await a.inbox.next(m=>m.type==='book')).symbol,'ALPHA');assert.equal((await b.inbox.next(m=>m.type==='book')).symbol,'BETA');
  const buyer=await browserTrader(t,bridge.port,5,'ALPHA'),seller=await browserTrader(t,bridge.port,5,'BETA');
  for(const client of [buyer,seller])await client.inbox.next(m=>m.type==='ready');
  async function submit(client,data){client.socket.send(JSON.stringify({type:'submit',...data}));return client.inbox.next(m=>m.type==='complete');}
  assert.equal((await submit(buyer,{symbol:'ALPHA',action:'new',orderId:'1',side:'buy',orderType:'limit',quantity:'10',price:'100'})).accepted,true);
  assert.equal((await submit(seller,{symbol:'BETA',action:'new',orderId:'1',side:'sell',orderType:'limit',quantity:'10',price:'50'})).accepted,true);
  const abook=await a.inbox.next(m=>m.type==='book'&&m.command==='1');
  const bbook=await b.inbox.next(m=>m.type==='book'&&m.command==='1');
  assert.equal(abook.sequence,'1');assert.deepEqual(abook.asks,[]);assert.equal(abook.bids[0].quantity,'10');
  assert.equal(bbook.sequence,'1');assert.deepEqual(bbook.bids,[]);assert.equal(bbook.asks[0].quantity,'10');
  buyer.socket.send(JSON.stringify({type:'submit',symbol:'BETA',action:'cancel',orderId:'1'}));
  assert.match((await buyer.inbox.next(m=>m.type==='inputError')).message,/symbol is immutable/);
  const taker=await browserTrader(t,bridge.port,6,'BETA');await taker.inbox.next(m=>m.type==='ready');
  await submit(taker,{symbol:'BETA',action:'new',orderId:'2',side:'buy',orderType:'market',quantity:'3'});
  const trade=await b.inbox.next(m=>m.type==='trade');assert.equal(trade.symbol,'BETA');assert.equal(trade.price,'50');
  assert.equal((await b.inbox.next(m=>m.type==='book'&&m.command==='2')).asks[0].quantity,'7');
  a.socket.send(JSON.stringify({type:'snapshot',symbol:'ALPHA'}));
  assert.equal((await a.inbox.next(m=>m.type==='book'&&m.anchor&&m.command==='1')).bids[0].quantity,'10');
  assert.equal(bridge.stats().observerConnections,2);
  const unknown=new WebSocket(`ws://127.0.0.1:${bridge.port}/stream?symbol=UNKNOWN`,{origin:`http://127.0.0.1:${bridge.port}`});unknown.on('error',()=>{});
  assert.equal(await new Promise(resolve=>unknown.on('unexpected-response',(_,res)=>{resolve(res.statusCode);res.resume();unknown.terminate();})),404);
  for(const [stack,count] of [[alpha,1],[beta,2]]){const exit=once(stack.gateway,'exit');stack.gateway.kill('SIGINT');await exit;assert.match((await promisify(execFile)(path.join(build,'apex'),['--replay',stack.journal])).stdout,new RegExp(`commands=${count} `));}
});

async function partitionLauncher(t,{httpPort=0}={}) {
  const directory=await mkdtemp(path.join(tmpdir(),'apex-partitions-'));const journalDirectory=path.join(directory,'journals');
  const launcher=spawn(process.execPath,[path.join(project,'ui/bridge/run.mjs'),'--symbols','ALPHA,BETA','--gateway-port','0','--port',String(httpPort),'--journal-dir',journalDirectory,'--build-dir',build]);
  t.after(()=>{if(launcher.exitCode===null)launcher.kill('SIGINT');});
  let output='',errors='';launcher.stdout.on('data',bytes=>output+=bytes);launcher.stderr.on('data',bytes=>errors+=bytes);
  return {launcher,journalDirectory,async ready(){return new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>{clearInterval(poll);reject(new Error(errors||'Partition readiness timeout'));},5000);
    const poll=setInterval(()=>{const match=output.match(/Apex dashboard: (http:\/\/127\.0\.0\.1:\d+)/);if(match){clearTimeout(timer);clearInterval(poll);resolve(match[1]);}},10);
    launcher.once('error',error=>{clearTimeout(timer);clearInterval(poll);reject(error);});
  });}};
}
test('partition launcher discovers named agents, isolates outages, seals journals and stops all children', {timeout:25000},async t=>{
  const stack=await partitionLauncher(t),url=await stack.ready();
  const {readFile}=await import('node:fs/promises');
  const metadata=await(await fetch(url+'/api/markets')).json();
  assert.notEqual(metadata.markets[0].gatewayPort,metadata.markets[1].gatewayPort);
  const {resolveMarket}=await import('../bridge/markets.mjs');
  assert.equal(await resolveMarket({symbol:'BETA',bridgePort:Number(new URL(url).port)}),metadata.markets[1].gatewayPort);
  const activity=await promisify(execFile)(process.execPath,[path.join(project,'ui/bridge/adaptive.mjs'),'--symbol','BETA','--bridge-port',new URL(url).port,'--steps','4','--interval-ms','0','--seed','42']);
  const summary=JSON.parse(activity.stdout.trim().split('\n').at(-1));assert.ok(summary.submitted>0);
  const view=await observe(Number(new URL(url).port),'ALPHA');t.after(()=>view.socket.terminate());
  assert.equal((await view.inbox.next(m=>m.type==='book')).command,'0');
  const session=JSON.parse(await readFile(path.join(stack.journalDirectory,'session.json'),'utf8'));
  process.kill(session.markets.find(m=>m.symbol==='BETA').pid,'SIGTERM');
  const deadline=Date.now()+5000;let offline;
  do{offline=await(await fetch(url+'/api/markets')).json();if(offline.markets.find(m=>m.symbol==='BETA').available===false)break;await new Promise(resolve=>setTimeout(resolve,10));}while(Date.now()<deadline);
  assert.equal(offline.markets.find(m=>m.symbol==='BETA').available,false);
  const trader=await browserTrader(t,Number(new URL(url).port),1,'ALPHA');await trader.inbox.next(m=>m.type==='ready');
  trader.socket.send(JSON.stringify({type:'submit',symbol:'ALPHA',action:'new',orderId:'1',side:'buy',orderType:'limit',price:'100',quantity:'1'}));
  assert.equal((await trader.inbox.next(m=>m.type==='complete')).accepted,true);
  const exit=once(stack.launcher,'exit');stack.launcher.kill('SIGINT');assert.equal((await exit)[0],0);
  for(const [symbol,count] of [['ALPHA',1],['BETA',summary.submitted]]){
    const replay=await promisify(execFile)(path.join(build,'apex'),['--replay',path.join(stack.journalDirectory,symbol+'.apex')]);assert.match(replay.stdout,new RegExp(`commands=${count} `));
  }
  const final=JSON.parse(await readFile(path.join(stack.journalDirectory,'session.json'),'utf8'));assert.ok(final.markets.every(m=>m.available===false));
});

test('partition startup failure cleans up owned gateways and preserves the occupied server', {timeout:15000},async t=>{
  const occupied=http.createServer((_,res)=>res.end('original'));await new Promise(resolve=>occupied.listen(0,'127.0.0.1',resolve));t.after(()=>new Promise(resolve=>occupied.close(resolve)));
  const stack=await partitionLauncher(t,{httpPort:occupied.address().port});
  const exit=await once(stack.launcher,'exit');assert.equal(exit[0],1);
  const {readFile}=await import('node:fs/promises');const session=JSON.parse(await readFile(path.join(stack.journalDirectory,'session.json'),'utf8'));
  assert.equal(session.markets.length,2);assert.ok(session.markets.every(m=>m.available===false));
  for(const market of session.markets){
    const socket=net.createConnection({host:'127.0.0.1',port:market.gatewayPort});t.after(()=>socket.destroy());
    assert.equal((await once(socket,'error'))[0].code,'ECONNREFUSED');
    assert.match((await promisify(execFile)(path.join(build,'apex'),['--replay',path.join(stack.journalDirectory,market.journal)])).stdout,/commands=0 /);
  }
  assert.equal(await(await fetch(`http://127.0.0.1:${occupied.address().port}`)).text(),'original');
});

test('reconnect restores private orders after offline fills and remains symbol/participant isolated', {timeout:20000},async t=>{
  const alpha=await liveStack(t),beta=await liveStack(t);
  const bridge=await startBridge({port:0,markets:[{symbol:'ALPHA',gatewayPort:alpha.gatewayPort},{symbol:'BETA',gatewayPort:beta.gatewayPort}]});t.after(()=>bridge.close());
  const original=await browserTrader(t,bridge.port,5,'ALPHA');
  assert.equal((await original.inbox.next(m=>m.type==='traderReady')).count,0);
  original.socket.send(JSON.stringify({type:'submit',symbol:'ALPHA',action:'new',orderId:'0',side:'buy',orderType:'limit',price:'100',quantity:'10'}));
  assert.equal((await original.inbox.next(m=>m.type==='complete')).accepted,true);
  original.socket.close();await once(original.socket,'close');
  const taker=await trader(alpha.gatewayPort,6);t.after(()=>taker.socket.destroy());await taker.order(43,1,4,100);
  const betaTrader=await browserTrader(t,bridge.port,5,'BETA');await betaTrader.inbox.next(m=>m.type==='traderReady');
  betaTrader.socket.send(JSON.stringify({type:'submit',symbol:'BETA',action:'new',orderId:'0',side:'buy',orderType:'limit',price:'200',quantity:'20'}));
  await betaTrader.inbox.next(m=>m.type==='complete');
  const restored=await browserTrader(t,bridge.port,5,'ALPHA');
  const snapshot=await restored.inbox.next(m=>m.type==='orders');
  assert.equal(snapshot.symbol,'ALPHA');assert.equal(snapshot.participant,5);assert.equal(snapshot.command,'2');
  assert.equal(snapshot.orders.length,1);assert.equal(snapshot.orders[0].id,'0');assert.equal(snapshot.orders[0].remaining,'6');assert.equal(snapshot.orders[0].price,'100');
  assert.equal((await restored.inbox.next(m=>m.type==='traderReady')).count,1);
  restored.socket.send(JSON.stringify({type:'submit',symbol:'ALPHA',action:'replace',orderId:'0',newOrderId:'44',price:'99',quantity:'8'}));
  assert.equal((await restored.inbox.next(m=>m.type==='complete')).accepted,true);
  restored.socket.send(JSON.stringify({type:'orders',symbol:'ALPHA'}));
  const refresh=await restored.inbox.next(m=>m.type==='orders'&&m.command==='3');assert.equal(refresh.orders[0].id,'44');assert.equal(refresh.orders[0].remaining,'8');
  await restored.inbox.next(m=>m.type==='traderReady'&&m.command==='3');
  const stranger=await browserTrader(t,bridge.port,7,'ALPHA');assert.deepEqual((await stranger.inbox.next(m=>m.type==='orders')).orders,[]);
  betaTrader.socket.send(JSON.stringify({type:'orders',symbol:'BETA'}));
  const b=await betaTrader.inbox.next(m=>m.type==='orders'&&m.command==='1');assert.equal(b.orders[0].remaining,'20');
  restored.socket.send(JSON.stringify({type:'submit',symbol:'ALPHA',action:'cancel',orderId:'44'}));await restored.inbox.next(m=>m.type==='complete');
  restored.socket.send(JSON.stringify({type:'orders',symbol:'ALPHA'}));assert.deepEqual((await restored.inbox.next(m=>m.type==='orders'&&m.command==='4')).orders,[]);
  const cli=await promisify(execFile)(path.join(build,'apex_client'),['--port',String(beta.gatewayPort),'--participant','8','--command','ORDERS']);assert.match(cli.stdout,/OPEN_ORDERS participant=8 command=1 count=0/);
  for(const [stack,count]of[[alpha,4],[beta,1]]){const exit=once(stack.gateway,'exit');stack.gateway.kill('SIGINT');await exit;assert.match((await promisify(execFile)(path.join(build,'apex'),['--replay',stack.journal])).stdout,new RegExp(`commands=${count} `));}
});
