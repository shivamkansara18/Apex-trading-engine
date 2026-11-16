import http from 'node:http';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import {WebSocketServer, WebSocket} from 'ws';
import {GatewaySession} from './session.mjs';
import {marketRoutes} from './markets.mjs';

export async function startBridge({port = 8080, gatewayPort = 9000, maxClients = 32, markets} = {}) {
  const root = fileURLToPath(new URL('../dist/', import.meta.url));
  const clients = new Set();
  const contexts = new Map(marketRoutes(markets, gatewayPort).map(route => [route.symbol,
    {route, observers:new Set(), traders:new Set(), hub:null, snapshotTimer:undefined, lastSnapshot:0}]));
  const defaultSymbol = contexts.keys().next().value;
  function send(ws, value) {
    if (ws.readyState !== WebSocket.OPEN) return;
    const encoded = JSON.stringify({...value, schemaVersion:2, symbol:ws.marketSymbol, receivedAt: Date.now()});
    if (ws.bufferedAmount + Buffer.byteLength(encoded) > 1024 * 1024) { ws.terminate(); return; }
    ws.send(encoded);
  }
  function observerSession(context) {
    if (context.hub && !context.hub.closed) return context.hub;
    const session = new GatewaySession(context.route.gatewayPort, 0); context.hub = session;
    session.on('message', value => { for (const ws of context.observers) send(ws, value); });
    session.on('disconnect', reason => {
      if (context.hub === session) { context.hub = null; clearTimeout(context.snapshotTimer); context.snapshotTimer = undefined; }
      for (const ws of context.observers) { send(ws, {type:'error',message:reason}); ws.close(1011,'Gateway unavailable'); }
    });
    session.ready.then(() => session.request(5)).catch(() => {});
    return session;
  }
  function snapshot(context) {
    if (!context.hub?.identified || context.hub.closed || context.snapshotTimer) return;
    context.snapshotTimer = setTimeout(() => {
      context.snapshotTimer = undefined; context.lastSnapshot = Date.now();
      try { context.hub?.request(6); } catch (error) { context.hub?.close(error.message); }
    }, Math.max(0, 250 - (Date.now() - context.lastSnapshot)));
  }
  const wsServer = new WebSocketServer({noServer: true, maxPayload: 4096, perMessageDeflate: false});
  const server = http.createServer(async (request, response) => {
    try {
      if (!validHost(request.headers.host)) { response.writeHead(403).end(); return; }
      if (request.method !== 'GET') { response.writeHead(405).end(); return; }
      const name = new URL(request.url, 'http://localhost').pathname;
      if (name === '/api/markets') {
        response.writeHead(200, {'Content-Type':'application/json','Cache-Control':'no-store'});
        response.end(JSON.stringify({schemaVersion:2,capabilities:{privateOrders:true},defaultSymbol,markets:[...contexts.values()].map(({route}) =>
          ({symbol:route.symbol,gatewayPort:route.gatewayPort,available:route.available !== false}))})); return;
      }
      const relative = name === '/' ? 'index.html' : name.slice(1);
      const target = path.resolve(root, relative);
      if (!target.startsWith(root) || (!['index.html'].includes(relative) && !relative.startsWith('assets/'))) {
        response.writeHead(404).end(); return;
      }
      const body = await readFile(target);
      const mime = {'.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8'}[path.extname(target)] || 'application/octet-stream';
      response.writeHead(200, {'Content-Type': mime, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff',
        'Content-Security-Policy': "default-src 'self'; connect-src 'self'; style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'"});
      response.end(body);
    } catch { response.writeHead(404).end('Build the dashboard first: npm --prefix ui run build'); }
  });
  server.headersTimeout = 5000; server.requestTimeout = 5000;
  function validHost(host) {
    return ['127.0.0.1', 'localhost'].some(h => host === `${h}:${server.address()?.port}`);
  }
  server.on('upgrade', (request, socket, head) => {
    const origin = request.headers.origin;
    let url;
    try { url = new URL(request.url, 'http://localhost'); }
    catch { socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n'); return; }
    const participant = url.searchParams.get('participant');
    const context = contexts.get(url.searchParams.get('symbol') ?? defaultSymbol);
    const trading = url.pathname === '/trade' && /^(?:[1-9]|[12][0-9]|3[0-2])$/.test(participant ?? '');
    if ((!trading && url.pathname !== '/stream') || !validHost(request.headers.host) ||
        origin !== `http://${request.headers.host}` || clients.size >= maxClients) {
      socket.end('HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n'); return;
    }
    if (!context) { socket.end('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n'); return; }
    if (context.route.available === false) { socket.end('HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n'); return; }
    wsServer.handleUpgrade(request,socket,head,ws=>wsServer.emit('connection',ws,trading ? Number(participant) : 0,context));
  });
  wsServer.on('connection', (ws, participant, context) => {
    clients.add(ws); ws.marketSymbol = context.route.symbol;
    let session, recovery, alive = true;
    if (participant) {
      session = new GatewaySession(context.route.gatewayPort, participant); context.traders.add(session);
      session.on('message', value => send(ws, value));
      session.on('disconnect', reason => { send(ws, {type: 'error', message: reason}); ws.close(1011, 'Trading session ended'); });
      recovery=session.ready.then(()=>session.openOrders()).then(snapshot=>{
        send(ws,{type:'traderReady',participant,count:snapshot.orders.length,command:snapshot.command});return snapshot;
      });
      recovery.catch(()=>{});
    } else {
      const existing = context.hub && !context.hub.closed;
      context.observers.add(ws); session = observerSession(context);
      if (existing) session.ready.then(() => snapshot(context)).catch(() => {});
    }
    ws.on('error', () => ws.terminate());
    ws.on('pong', () => { alive = true; });
    let inFlight = false;
    ws.on('message', async (data, binary) => {
      try {
        if (binary) throw new Error('Expected JSON text');
        const message = JSON.parse(data.toString());
        if (message?.symbol !== undefined && message.symbol !== context.route.symbol) throw new Error('Session symbol is immutable');
        if (!participant) {
          if (message?.type !== 'snapshot') { ws.close(1008, 'Invalid observer request'); return; }
          snapshot(context); return;
        }
        if (inFlight) { send(ws, {type: 'inputError', message: 'Wait for the pending command to finish'}); return; }
        inFlight = true;
        try {
          await recovery;
          if(message?.type==='orders'){
            const snapshot=await session.openOrders();
            send(ws,{type:'traderReady',participant,count:snapshot.orders.length,command:snapshot.command});return;
          }
          const result = await session.execute(message);
          send(ws, {type: 'complete', accepted: result.ack.accepted, command: result.ack.command,
            reason: result.reports.find(r => r.kind === 'rejected')?.reason});
        } finally { inFlight = false; }
      } catch (error) {
        if (participant) send(ws, {type: 'inputError', message: error.message});
        else ws.close(1008, 'Invalid observer request');
      }
    });
    const timer = setInterval(() => { if (!alive) ws.terminate(); else { alive = false; ws.ping(); } }, 5000);
    ws.on('close', () => {
      clearInterval(timer); clients.delete(ws); context.observers.delete(ws);
      if (participant) { context.traders.delete(session); session.close(); }
    });
  });
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(port, '127.0.0.1', resolve); });
  return {port:server.address().port, stats:() => ({browsers:clients.size,
    observers:[...contexts.values()].reduce((n,c)=>n+c.observers.size,0),
    observerConnections:[...contexts.values()].filter(c=>c.hub&&!c.hub.closed).length,
    tradingConnections:[...contexts.values()].reduce((n,c)=>n+c.traders.size,0)}), async close() {
    for (const context of contexts.values()) {
      clearTimeout(context.snapshotTimer); context.hub?.close();
      for (const session of context.traders) session.close();
    }
    for (const ws of clients) ws.terminate();
    await new Promise(resolve => wsServer.close(resolve));
    await new Promise(resolve => server.close(resolve));
  }};
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2);
  function option(name, fallback) {
    const index = args.indexOf(name); const value = index < 0 ? fallback : Number(args[index + 1]);
    if (!Number.isInteger(value) || value < 0 || value > 65535) throw new Error(`Invalid ${name}`);
    return value;
  }
  const bridge = await startBridge({port: option('--port', 8080), gatewayPort: option('--gateway-port', 9000)});
  console.log(`Apex dashboard: http://127.0.0.1:${bridge.port}`);
  for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, async () => { await bridge.close(); });
}
