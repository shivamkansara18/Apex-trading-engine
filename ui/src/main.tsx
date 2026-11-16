import React, {useEffect, useRef, useState} from 'react';
import {createRoot} from 'react-dom/client';
import {advanceForSymbol, emptyFeed} from './feed';
import type {Feed, Level, Message} from './feed';
import './style.css';
import {Trading} from './Trading';

const integer = (value: string | bigint | undefined) => value === undefined ? '—' : BigInt(value).toLocaleString('en-US');
function Depth({levels, side}: {levels: Level[]; side: 'bid' | 'ask'}) {
  const max = levels.reduce((m, l) => BigInt(l.quantity) > m ? BigInt(l.quantity) : m, 1n);
  return <div className={`depth ${side}`}>
    <div className="table-head"><span>Price / ticks</span><span>Quantity</span><span>Orders</span></div>
    {levels.length ? levels.map(level => <div className="depth-row" key={level.price}>
      <i style={{width: `${Number(BigInt(level.quantity) * 10000n / max) / 100}%`}}/>
      <span>{integer(level.price)}</span><span>{integer(level.quantity)}</span><span>{level.orders}</span>
    </div>) : <div className="empty-depth">No resting {side === 'bid' ? 'bids' : 'asks'}</div>}
  </div>;
}
function DepthChart({bids, asks}: {bids: Level[]; asks: Level[]}) {
  const cumulative = (levels: Level[]) => { let total = 0n; return levels.map(l => total += BigInt(l.quantity)); };
  const buy = cumulative(bids), sell = cumulative(asks);
  const max = [...buy, ...sell].reduce((m, q) => q > m ? q : m, 1n);
  const points = (values: bigint[], side: number) => {
    let p = '300,145';
    values.forEach((q, i) => { const x = 300 + side * (i + 1) * 280 / Math.max(values.length, 1); const y = 145 - Number(q * 12000n / max) / 100; p += ` ${x},${y}`; });
    return p + ` ${300 + side * 280},145`;
  };
  return <svg className="depth-chart" viewBox="0 0 600 170" role="img" aria-label="Cumulative depth by price level; bids left and asks right">
    {[45, 95, 145].map(y => <line key={y} x1="20" x2="580" y1={y} y2={y} className="gridline"/>)}
    <line x1="300" x2="300" y1="20" y2="145" className="gridline"/>
    <polygon points={points(buy, -1)} className="buy-area"/><polygon points={points(sell, 1)} className="sell-area"/>
    <text x="20" y="165">BID DEPTH</text><text x="580" y="165" textAnchor="end">ASK DEPTH</text>
  </svg>;
}
function App() {
  const [theme, setTheme] = useState<'dark' | 'light'>(() => document.documentElement.dataset.theme === 'light' ? 'light' : 'dark');
  useEffect(() => {
    document.documentElement.dataset.theme = theme;
    document.querySelector('meta[name="theme-color"]')?.setAttribute('content', theme === 'light' ? '#f2eee4' : '#0b1015');
    try { localStorage.setItem('apex-theme', theme); } catch { /* Theme still works when storage is unavailable. */ }
  }, [theme]);
  const [symbol,setSymbol] = useState('APEX'), [markets,setMarkets] = useState<{symbol:string;available:boolean;gatewayPort:number}[]>([]);
  const activeSymbol=useRef('APEX');
  const [tradingPending,setTradingPending]=useState(false);
  const [configReady,setConfigReady] = useState(false);
  const [feed, setFeed] = useState<Feed>(emptyFeed);
  const current = useRef(feed), socket = useRef<WebSocket | null>(null);
  const [enabled, setEnabled] = useState(true), [status, setStatus] = useState('Connecting');
  const [error, setError] = useState(''), [now, setNow] = useState(Date.now()), [help, setHelp] = useState(false);
  useEffect(() => { const t = setInterval(() => setNow(Date.now()), 500); return () => clearInterval(t); }, []);
  useEffect(() => {
    let disposed=false, loaded=false;
    const load=async()=>{
      try {
        const response=await fetch('/api/markets',{signal:AbortSignal.timeout(4000)});
        if(!response.ok) throw new Error('Market configuration unavailable');
        const metadata=await response.json();
        if(metadata.schemaVersion!==2||!Array.isArray(metadata.markets)||!metadata.markets.some((m:{symbol:string})=>m.symbol===metadata.defaultSymbol)) throw new Error('Unsupported market routing configuration');
        if(metadata.capabilities?.privateOrders!==true)throw new Error('Restart the updated dashboard server to enable private-order synchronization');
        if(disposed) return;
        setMarkets(metadata.markets);
        if(!loaded){loaded=true;activeSymbol.current=metadata.defaultSymbol;setSymbol(metadata.defaultSymbol);setConfigReady(true);}
      } catch(error){if(!disposed) setError(error instanceof Error ? error.message : 'Market discovery failed');}
    };
    void load();const timer=setInterval(load,5000);
    return ()=>{disposed=true;clearInterval(timer);};
  },[]);
  useEffect(() => {
    if (!configReady) return;
    if (!enabled) { setStatus('Disconnected'); return; }
    let disposed = false, retry: ReturnType<typeof setTimeout>, attempts = 0;
    const connect = () => {
      if(disposed||activeSymbol.current!==symbol)return;
      setStatus('Connecting');
      const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/stream?symbol=${encodeURIComponent(symbol)}`);
      socket.current = ws;
      ws.onopen = () => { if(disposed||activeSymbol.current!==symbol)return; attempts = 0; current.current = emptyFeed(); setFeed(current.current); setError(''); setStatus('Awaiting snapshot'); };
      ws.onmessage = event => {
        if(disposed||activeSymbol.current!==symbol)return;
        try {
          const message = JSON.parse(event.data) as Message & {schemaVersion:number;symbol:string};
          if(message.schemaVersion!==2||message.symbol!==symbol)throw new Error('Unexpected market route');
          if (message.type === 'error') { setError(message.message); return; }
          const update = advanceForSymbol(current.current, message, symbol, Date.now());
          current.current = update.feed; setFeed(update.feed);
          if (update.snapshot) ws.send(JSON.stringify({type: 'snapshot',symbol}));
          if (update.feed.book) setStatus('Connected');
        } catch { setError('Invalid feed message'); ws.close(); }
      };
      ws.onclose = () => {
        if (disposed||activeSymbol.current!==symbol) return;
        setStatus('Reconnecting');
        retry = setTimeout(connect, Math.min(5000, 1000 * ++attempts));
      };
      ws.onerror = () => {if(!disposed&&activeSymbol.current===symbol)setError(`Market ${symbol} connection unavailable`);};
    };
    connect();
    return () => { disposed = true; clearTimeout(retry); socket.current?.close(); };
  }, [enabled,symbol,configReady]);
  const stale = status === 'Connected' && now - feed.lastSignal > 12000;
  const live = status === 'Connected' && !stale && !feed.recovering;
  const bids = feed.book?.bids ?? [], asks = feed.book?.asks ?? [];
  const bid = bids[0]?.price, ask = asks[0]?.price;
  const spread = bid !== undefined && ask !== undefined ? integer(BigInt(ask) - BigInt(bid)) : '—';
  const rate = feed.buckets.filter(b => b.second > Math.floor(now / 1000) - 5).reduce((sum, b) => sum + b.count, 0) / 5;
  const label = feed.recovering ? 'Recovering gap' : stale ? 'Stale feed' : status;
  const refresh = () => { if (socket.current?.readyState === WebSocket.OPEN) socket.current.send(JSON.stringify({type: 'snapshot',symbol})); };
  const nativePort=markets.find(m=>m.symbol===symbol)?.gatewayPort ?? 9000;
  const selectSymbol=(value:string)=>{activeSymbol.current=value;current.current=emptyFeed();setFeed(current.current);setStatus('Connecting');setError('');setSymbol(value);};
  return <div className="app-shell">
    <aside className="rail"><a className="brand" href="/" aria-label="Apex home">A<span>✦</span></a><div className="rail-item active" title="Exchange monitor">▥</div><span className="rail-bottom">AQ</span></aside>
    <div className="workspace">
      <header><div className="wordmark">APEX <span>QUANT</span></div><div className="header-right"><span className="environment">LOCAL SIMULATOR</span><span className="clock">{new Date(now).toLocaleTimeString('en-GB')}</span><button className="theme-toggle" aria-label="Light theme" aria-pressed={theme === 'light'} title={`Switch to ${theme === 'dark' ? 'light' : 'dark'} mode`} onClick={() => setTheme(theme === 'dark' ? 'light' : 'dark')}><svg viewBox="0 0 24 24" aria-hidden="true">{theme === 'dark' ? <><circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5"/></> : <path d="M20 15.5A8.5 8.5 0 0 1 8.5 4 8.5 8.5 0 1 0 20 15.5Z"/>}</svg>{theme === 'dark' ? 'Light mode' : 'Dark mode'}</button></div></header>
      <main>
        <div className="page-heading"><div><div className="eyebrow">EXCHANGE / LIVE MONITOR</div><h1>Inside the order book<span>.</span></h1><p>A live view of price, liquidity and every match.</p></div><button className="secondary" onClick={() => setHelp(!help)}>{help ? 'Close walkthrough' : 'Trading walkthrough ↗'}</button></div>
        <div className="session-bar"><div><span className={`status-dot ${live ? 'live' : 'waiting'}`}/><strong role="status">{label}</strong><span className="separator"/> <label className="market-selector">MARKET <select aria-label="Market symbol" value={symbol} disabled={!configReady||tradingPending} onChange={event=>selectSymbol(event.target.value)}>{markets.map(m=><option key={m.symbol} value={m.symbol}>{m.symbol}{m.available?'':' (offline)'}</option>)}</select></label><span className="observer-tag">OBSERVER 0</span></div><button className="text-button" onClick={() => setEnabled(!enabled)}>{enabled ? 'Disconnect' : 'Connect'} ↗</button></div>
        {error && <div className="notice" role="alert">{error}</div>}
        {feed.recovering && <div className="notice" role="alert">Sequence gap detected. Updates are paused while a fresh snapshot is requested. <button onClick={refresh}>Request again</button></div>}
        {stale && <div className="notice" role="alert">No gateway signal for 12 seconds. Displayed values may be stale.</div>}
        {help && <section className="walkthrough"><h2>Put the matching engine to work</h2><p>For adaptive simulated traders, run <code>npm --prefix ui run demo -- --symbol {symbol} --bridge-port {location.port || '80'}</code> in a second terminal. Agents quote from live depth, update inventory from fills and choose actions with a seeded random policy. Use the Trading desk below for manual orders. The bridge discovers the selected market’s native port.</p><p>For a scripted native-client example, use the printed TCP port for the selected market and run these commands from the project folder. The first two add bids; the third matches at the resting price. Orders survive client disconnects.</p><pre>{`./build/debug/apex_client --port ${nativePort} --participant 1 --command "BUY 1 100 10000"\n./build/debug/apex_client --port ${nativePort} --participant 2 --command "BUY 2 50 10000"\n./build/debug/apex_client --port ${nativePort} --participant 3 --command "SELL 3 120 9900"`}</pre><p>These commands use the selected market’s configured TCP port. Repeated live order IDs may reject.</p></section>}
        {live && feed.book?.command === '0' && <div className="market-empty-note"><span>The exchange is connected and empty. Use the Trading desk below or run adaptive agents to see live depth and trades; the feed rate returns to zero when the market is quiet.</span><button onClick={() => setHelp(true)}>Show trading commands ↗</button></div>}
        <section className="stats-grid" aria-label="Market summary">
          <div className="stat"><span>BEST BID <i className="green">↗</i></span><strong className="green">{integer(bid)}</strong><small>{bids[0] ? `${integer(bids[0].quantity)} units resting` : 'Waiting for bids'}</small></div>
          <div className="stat"><span>BEST ASK <i className="red">↘</i></span><strong className="red">{integer(ask)}</strong><small>{asks[0] ? `${integer(asks[0].quantity)} units resting` : 'Waiting for asks'}</small></div>
          <div className="stat"><span>SPREAD <i>↔</i></span><strong>{spread}<em> ticks</em></strong><small>Best ask minus best bid</small></div>
          <div className="stat"><span>FEED RATE <i>⌁</i></span><strong>{rate.toFixed(1)}<em> / sec</em></strong><small>Trade + book updates · 5s average</small></div>
        </section>
        <div className="content-grid"><section className="panel book-panel"><div className="panel-heading"><h2>Order book</h2><span>TOP {Math.max(bids.length, asks.length, 10)} LEVELS</span><button className="icon-button" aria-label="Request fresh snapshot" title="Request fresh snapshot" disabled={!enabled || status !== 'Connected'} onClick={refresh}>↻</button></div>
          <div className="book-labels"><span className="green">BIDS <small>BUY LIQUIDITY</small></span><span className="red">ASKS <small>SELL LIQUIDITY</small></span></div>
          <div className="book-halves"><Depth levels={bids} side="bid"/><Depth levels={asks} side="ask"/></div>
          <div className="book-footer"><span>Integer price ticks · aggregated quantity</span><span>{live ? '● Live' : '○ Awaiting live feed'}</span></div>
        </section>
        <section className="panel trades-panel"><div className="panel-heading"><h2>Recent trades</h2><span>LAST 50</span></div><div className="trade-head"><span>TIME</span><span>PRICE</span><span>QTY</span><span>TAKER</span></div><div className="trade-list">{feed.trades.length ? feed.trades.map(trade => <div className="trade-row" key={trade.id}><time>{new Date(trade.time).toLocaleTimeString('en-GB')}</time><span className={trade.restingOrder === trade.sellOrder ? 'green' : 'red'}>{integer(trade.price)}</span><span>{integer(trade.quantity)}</span><span className={`side-tag ${trade.restingOrder === trade.sellOrder ? 'buy' : 'sell'}`}>{trade.restingOrder === trade.sellOrder ? 'BUY' : 'SELL'}</span></div>) : <div className="empty-trades"><div>⇄</div><h3>Waiting for the first match</h3><p>Executed trades appear here as orders cross.</p></div>}</div><div className="book-footer">Time shown is bridge receipt time</div></section>
        <section className="panel chart-panel"><div className="panel-heading"><h2>Liquidity profile</h2><span>CUMULATIVE QUANTITY / LEVEL</span></div><DepthChart bids={bids} asks={asks}/></section>
        <section className="panel integrity-panel"><div className="panel-heading"><h2>Feed integrity</h2><span className={live ? 'green' : 'muted'}>{live ? 'HEALTHY' : 'WAITING'}</span></div><dl><div><dt>Feed sequence</dt><dd>{integer(feed.sequence ?? undefined)}</dd></div><div><dt>Engine command</dt><dd>{integer(feed.book?.command)}</dd></div><div><dt>Sequence gaps</dt><dd className={feed.gaps ? 'red' : ''}>{feed.gaps}</dd></div><div><dt>Last market update</dt><dd>{feed.lastMarket ? `${Math.max(0, Math.floor((now - feed.lastMarket) / 1000))}s ago` : '—'}</dd></div></dl><p>Gaps request a fresh snapshot. Heartbeats distinguish a quiet market from a stale connection.</p></section>
        </div>
        <p className="symbol-note">Switching markets closes the trading connection. Resting orders remain in their original book; risk limits and sequences are separate per market.</p>
        <Trading key={symbol} symbol={symbol} onPendingChange={setTradingPending}/>
        <footer><span><b>APEX</b> Deterministic exchange simulator</span><span>Single writer engine <i>•</i> Separate WebSocket bridge <i>•</i> Local session</span></footer>
      </main>
    </div>
  </div>;
}
createRoot(document.getElementById('root')!).render(<React.StrictMode><App/></React.StrictMode>);
