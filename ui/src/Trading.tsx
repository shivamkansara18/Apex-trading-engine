import {useEffect, useRef, useState} from 'react';
type Order = {id: string; side: number; price: string; remaining: string; participant: number};
function nextId() { const words = crypto.getRandomValues(new Uint32Array(2)); return ((BigInt(words[0]) << 32n | BigInt(words[1])) || 1n).toString(); }
export function Trading({symbol='APEX',onPendingChange}:{symbol?:string;onPendingChange?:(pending:boolean)=>void}) {
  const [participant, setParticipant] = useState('1'), [status, setStatus] = useState('Disconnected');
  const [action, setAction] = useState('new'), [side, setSide] = useState('buy'), [orderType, setOrderType] = useState('limit');
  const [orderId, setOrderId] = useState(nextId), [newOrderId, setNewOrderId] = useState(nextId);
  const [price, setPrice] = useState('10000'), [quantity, setQuantity] = useState('10');
  const [pending, setPending] = useState(false), [notice, setNotice] = useState('Connect a participant to submit simulated orders.');
  const [orders, setOrders] = useState<Record<string, Order>>({}), [events, setEvents] = useState<string[]>([]);
  const socket = useRef<WebSocket | null>(null), waiting = useRef(false), disposed=useRef(false);
  useEffect(() => {disposed.current=false;return () => {disposed.current=true;socket.current?.close();onPendingChange?.(false);};}, [onPendingChange]);
  function trackPending(value:boolean){if(disposed.current)return;waiting.current=value;setPending(value);onPendingChange?.(value);}
  function connect() {
    if (status !== 'Disconnected') { socket.current?.close(); return; }
    setStatus('Connecting'); setNotice('Opening an identified simulator trading session…');
    const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/trade?participant=${participant}&symbol=${encodeURIComponent(symbol)}`);
    socket.current = ws;
    ws.onmessage = event => {
      if(disposed.current||socket.current!==ws)return;
      const message = JSON.parse(event.data);
      if(message.schemaVersion!==2||message.symbol!==symbol){setNotice('Unexpected market route; session closed');ws.close();return;}
      if(message.type==='ready'){setStatus('Synchronizing');setOrders({});trackPending(true);setNotice('Restoring open orders from the engine…');}
      if(message.type==='orders'){
        const restored:Record<string,Order>={};for(const order of message.orders as Order[])restored[order.id]=order;
        setOrders(restored);
      }
      if(message.type==='traderReady'){setStatus('Connected');trackPending(false);setNotice(`Participant ${message.participant}: ${message.count} open orders synchronized at command ${message.command}. Live execution updates continue.`);}
      if (message.type === 'report') {
        const order = message.order as Order | undefined;
        if (order) setOrders(current => {
          const updated = {...current};
          if (message.kind === 'cancelled' || BigInt(order.remaining) === 0n) delete updated[order.id];
          else updated[order.id] = order;
          return updated;
        });
        const detail = order ? `order ${order.id} · remaining ${order.remaining}` : message.reason ?? (message.trade ? `${message.trade.quantity} @ ${message.trade.price}` : '');
        setEvents(current => [`#${message.command} ${message.kind} · ${detail}`, ...current].slice(0, 50));
      }
      if (message.type === 'complete' || message.type === 'inputError') {
        trackPending(false);
        setNotice(message.type === 'inputError' ? message.message : message.accepted ? `Command ${message.command} accepted by the engine.` : `Command ${message.command} rejected: ${message.reason}`);
        if (message.type === 'complete' && message.accepted) { setOrderId(nextId()); setNewOrderId(nextId()); }
      }
      if (message.type === 'error') setNotice(message.message);
    };
    ws.onerror = () => {if(!disposed.current&&socket.current===ws)setNotice('Trading connection unavailable. Check the gateway and participant session.');};
    ws.onclose = () => { if(disposed.current||socket.current!==ws)return;setStatus('Disconnected'); if (waiting.current) setNotice('Disconnected before completion. Outcome is unknown; do not automatically resubmit. Orders can remain live.'); trackPending(false); };
  }
  function refreshOrders(){if(status==='Connected'&&!waiting.current&&socket.current?.readyState===WebSocket.OPEN){trackPending(true);socket.current.send(JSON.stringify({type:'orders',symbol}));}}
  function submit(event: React.FormEvent) {
    event.preventDefault();
    if (socket.current?.readyState !== WebSocket.OPEN || status !== 'Connected' || waiting.current) return;
    const request = {type: 'submit', symbol, action, orderId, newOrderId, side, orderType, price, quantity};
    trackPending(true); setNotice('Waiting for the engine…'); socket.current.send(JSON.stringify(request));
  }
  return <section className="panel trading-panel">
    <div className="panel-heading"><h2>Trading desk · {symbol}</h2><span>LOCAL SIMULATED ORDERS</span></div>
    <div className="trading-body">
      <div className="trading-connect"><label>Participant <select value={participant} disabled={status !== 'Disconnected'} onChange={e => setParticipant(e.target.value)}>{Array.from({length: 16}, (_, i) => <option key={i+1}>{i+1}</option>)}</select></label><button className="secondary" onClick={connect}>{status === 'Disconnected' ? 'Connect trader' : 'Disconnect trader'}</button><span role="status">{status}</span><button className="secondary" onClick={refreshOrders} disabled={status!=='Connected'||pending}>Refresh my orders</button></div>
      <p className="trading-note">Use participants 1–8 for manual orders; adaptive agents use 9–12. One writable session per participant. Disconnecting preserves resting orders.</p>
      <form onSubmit={submit} className="order-form">
        <label>Action<select value={action} onChange={e => setAction(e.target.value)}><option value="new">New order</option><option value="cancel">Cancel order</option><option value="replace">Replace order</option></select></label>
        <label>Order ID<input inputMode="numeric" pattern="[0-9]+" required value={orderId} onChange={e => setOrderId(e.target.value)}/></label>
        {action === 'new' && <><label>Side<select value={side} onChange={e => setSide(e.target.value)}><option value="buy">Buy</option><option value="sell">Sell</option></select></label><label>Type<select value={orderType} onChange={e => setOrderType(e.target.value)}><option value="limit">Limit</option><option value="market">Market</option></select></label></>}
        {action === 'replace' && <label>New order ID<input inputMode="numeric" pattern="[0-9]+" required value={newOrderId} onChange={e => setNewOrderId(e.target.value)}/></label>}
        {action !== 'cancel' && <><label>Quantity<input inputMode="numeric" pattern="[0-9]+" required value={quantity} onChange={e => setQuantity(e.target.value)}/></label>{(action === 'replace' || orderType === 'limit') && <label>Price / ticks<input inputMode="numeric" pattern="[0-9]+" required value={price} onChange={e => setPrice(e.target.value)}/></label>}</>}
        <button className={`submit-order ${side}`} disabled={status !== 'Connected' || pending}>{pending ? 'Processing…' : action === 'new' ? `Submit ${side}` : `${action} order`}</button>
      </form>
      <p className="trading-notice" role="status">{notice}</p>
      {status==='Disconnected'&&<p className="trading-note">Disconnected: displayed orders are last known state. Reconnect to synchronize current quantities.</p>}
      <div className="trading-results"><div><h3>My open orders</h3>{Object.values(orders).length ? <div className="owned-orders">{Object.values(orders).map(order => <button key={order.id} disabled={status !== 'Connected'} title="Select this order to cancel or replace" onClick={() => {setOrderId(order.id);setAction('cancel');}}><span className={order.side === 0 ? 'green' : 'red'}>{order.side === 0 ? 'BUY' : 'SELL'} {order.remaining} @ {order.price}</span><small>{order.id}</small></button>)}</div> : <p className="trading-note">No open orders in this synchronized live view. Reconnect or refresh to check current state.</p>}</div><div><h3>Execution activity · last 50</h3><div className="execution-list">{events.length ? events.map((event,i) => <div key={i}>{event}</div>) : <p className="trading-note">ACKs, fills and rejections come directly from the matching engine.</p>}</div></div></div>
    </div>
  </section>;
}
