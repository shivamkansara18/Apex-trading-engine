import net from 'node:net';
import {EventEmitter} from 'node:events';
import {Decoder, frame, decode, encodeCommand} from './protocol.mjs';

// One persistent, identified TCP session; matching remains solely in C++.
export class GatewaySession extends EventEmitter {
  sequence = 0n; pending = new Map(); closed = false; identified = false;
  constructor(port, participant) {
    super(); this.participant = participant;
    this.socket = net.createConnection({host: '127.0.0.1', port}); this.socket.setNoDelay(true);
    this.ready = new Promise((resolve, reject) => { this.resolveReady = resolve; this.rejectReady = reject; });
    const decoder = new Decoder(); let lastReceive = Date.now();
    this.socket.on('connect', () => { const payload = Buffer.alloc(4); payload.writeUInt32BE(participant); this.request(1, payload); });
    this.socket.on('data', bytes => {
      lastReceive = Date.now();
      try {
        for (const packet of decoder.append(bytes)) {
          const value = {...decode(packet), request: packet.request};
          if (value.type === 'ready') {
            if (value.participant !== participant) throw new Error('Unexpected session identity');
            this.identified = true; this.resolveReady(this);
          }
          if (value.type === 'error') { this.close(value.message); return; }
          if(value.type==='orders'&&value.participant!==participant)throw new Error('Private snapshot identity mismatch');
          const pending = this.pending.get(packet.request);
          if(pending?.kind==='orders'){
            if(value.type!=='orders')throw new Error('Expected private order snapshot');
            clearTimeout(pending.timer);this.pending.delete(packet.request);pending.resolve(value);
          }else if (pending) {
            if (value.type === 'ack') pending.ack = value;
            if (value.type === 'report') pending.reports.push(value);
            if (pending.ack && pending.reports.length === pending.ack.count) {
              clearTimeout(pending.timer); this.pending.delete(packet.request);
              pending.resolve({ack: pending.ack, reports: pending.reports});
            }
          }
          this.emit('message', value);
        }
      } catch (error) { this.close(error.message); }
    });
    this.socket.on('error', error => this.close(error.message));
    this.socket.on('end', () => this.close(decoder.buffer.length ? 'Truncated gateway stream' : 'Gateway disconnected'));
    this.socket.on('close', () => this.close('Gateway disconnected'));
    this.timer = setInterval(() => {
      if (Date.now() - lastReceive >= 15000) this.close('Gateway feed timed out');
      else if (this.identified) { try { this.request(7); } catch (error) { this.close(error.message); } }
    }, 5000);
    this.startup = setTimeout(() => this.close('Gateway handshake timed out'), 5000);
    this.ready.then(() => clearTimeout(this.startup), () => clearTimeout(this.startup));
  }
  request(type, payload) {
    if (this.closed || this.socket.writableLength > 65536 || this.sequence === 18446744073709551615n) throw new Error('Gateway connection unavailable or overloaded');
    const sequence = ++this.sequence; this.socket.write(frame(type, sequence, payload)); return sequence.toString();
  }
  async openOrders() {
    await this.ready;
    if(!this.participant)throw new Error('Observers cannot request private orders');
    if(this.pending.size>=16)throw new Error('Too many outstanding commands');
    const sequence=this.request(8);
    return new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>this.close('Private order synchronization timed out'),5000);
      this.pending.set(sequence,{kind:'orders',resolve,reject,timer});
    });
  }
  async execute(input) {
    await this.ready;
    if (this.pending.size >= 16) throw new Error('Too many outstanding commands');
    const encoded = encodeCommand(input, this.participant);
    const sequence = this.request(encoded.type, encoded.payload);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => this.close('Command outcome unknown after timeout; do not automatically retry'), 5000);
      this.pending.set(sequence, {resolve, reject, timer, reports: []});
    });
  }
  close(reason = 'Session closed') {
    if (this.closed) return; this.closed = true;
    clearInterval(this.timer); clearTimeout(this.startup); this.socket.destroy();
    this.rejectReady(new Error(reason));
    for (const pending of this.pending.values()) { clearTimeout(pending.timer); pending.reject(new Error(reason)); }
    this.pending.clear(); this.emit('disconnect', reason);
  }
}
