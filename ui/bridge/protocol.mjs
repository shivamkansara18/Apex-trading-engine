// Version-1 TCP wire adapter. All 64-bit values cross JSON as decimal strings.
export const MAX_FRAME = 65536;
export function frame(type, sequence, payload = Buffer.alloc(0)) {
  const out = Buffer.alloc(16 + payload.length);
  if (out.length > MAX_FRAME) throw new Error('Frame exceeds limit');
  out.writeUInt32BE(out.length); out.writeUInt16BE(1, 4);
  out.writeUInt16BE(type, 6); out.writeBigUInt64BE(BigInt(sequence), 8);
  payload.copy(out, 16); return out;
}
export class Decoder {
  buffer = Buffer.alloc(0);
  append(chunk) {
    // Network chunks are bounded by Node; retain only one bounded partial frame.
    this.buffer = Buffer.concat([this.buffer, chunk]);
    const result = [];
    while (this.buffer.length >= 16) {
      const size = this.buffer.readUInt32BE();
      if (size < 16 || size > MAX_FRAME || this.buffer.readUInt16BE(4) !== 1) throw new Error('Invalid TCP frame');
      if (this.buffer.length < size) break;
      result.push({type: this.buffer.readUInt16BE(6), request: this.buffer.readBigUInt64BE(8).toString(), payload: this.buffer.subarray(16, size)});
      this.buffer = this.buffer.subarray(size);
    }
    if (this.buffer.length > MAX_FRAME) throw new Error('TCP buffer exceeds limit');
    return result;
  }
}
class Reader {
  offset = 0;
  constructor(bytes) { this.bytes = bytes; }
  get(size, fn) {
    if (this.offset + size > this.bytes.length) throw new Error('Truncated payload');
    const value = fn(this.offset); this.offset += size; return value;
  }
  u8() { return this.get(1, p => this.bytes.readUInt8(p)); }
  u32() { return this.get(4, p => this.bytes.readUInt32BE(p)); }
  u64() { return this.get(8, p => this.bytes.readBigUInt64BE(p).toString()); }
  price() { return this.get(8, p => this.bytes.readBigInt64BE(p).toString()); }
  end() { if (this.offset !== this.bytes.length) throw new Error('Trailing payload'); }
}
export function decode(message) {
  const r = new Reader(message.payload);
  let result;
  switch (message.type) {
    case 101: result = {type: 'ready', participant: r.u32()}; break;
    case 102: {
      const command = r.u64(), count = r.u32(), accepted = r.u8();
      if (!count || count > 100000 || accepted > 1) throw new Error('Invalid command ACK');
      result = {type: 'ack', command, count, accepted: Boolean(accepted)}; break;
    }
    case 103: result = report(r); break;
    case 104: result = {type: 'subscribed', sequence: r.u64()}; break;
    case 105: result = {type: 'heartbeat'}; break;
    case 106: {
      const command=r.u64(),participant=r.u32(),count=r.u32();
      if(!participant||count>1000)throw new Error('Invalid private snapshot bounds');
      const orders=[],ids=new Set();
      for(let i=0;i<count;i++){
        const order=readOrder(r);
        if(order.participant!==participant||order.orderType!==0||BigInt(order.price)<=0n||BigInt(order.remaining)===0n||
           BigInt(order.remaining)>BigInt(order.original)||![2,3].includes(order.status)||BigInt(order.priority)===0n||
           BigInt(order.priority)>BigInt(command)||ids.has(order.id))throw new Error('Invalid private open order');
        ids.add(order.id);orders.push(order);
      }
      result={type:'orders',command,participant,orders};break;
    }
    case 199: {
      const code = r.u32();
      if (message.payload.length > 260) throw new Error('Oversized error');
      return {type: 'error', code, message: message.payload.subarray(4).toString('utf8')};
    }
    case 202: {
      const sequence = r.u64(), command = r.u64();
      const levels = () => {
        const count = r.u32(); if (count > 64) throw new Error('Excess book depth');
        return Array.from({length: count}, () => ({price: r.price(), quantity: r.u64(), orders: r.u32()}));
      };
      result = {type: 'book', sequence, command, anchor: message.request !== '0', bids: levels(), asks: levels()}; break;
    }
    case 201: {
      const sequence = r.u64();
      if (r.u64() !== '1') throw new Error('Expected one trade');
      const event = r.u64(), command = r.u64();
      if (r.u8() !== 5) throw new Error('Expected trade event');
      result = {type: 'trade', sequence, event, command, id: r.u64(), buyOrder: r.u64(), sellOrder: r.u64(),
        buyer: r.u32(), seller: r.u32(), restingOrder: r.u64(), price: r.price(), quantity: r.u64()}; break;
    }
    default: throw new Error(`Unexpected gateway message ${message.type}`);
  }
  r.end(); return result;
}

const rejects = ['InvalidSide','InvalidOrderType','ZeroQuantity','InvalidPrice','DuplicateOrderId','UnknownOrder',
  'WrongParticipant','UnknownParticipant','QuantityLimit','NotionalLimit','OpenOrderLimit','PriceBand',
  'ArithmeticOverflow','InvalidReplacement','TradingDisabled'];
function readOrder(r) {
  const order={id:r.u64(),participant:r.u32(),side:r.u8(),orderType:r.u8(),price:r.price(),
    original:r.u64(),remaining:r.u64(),priority:r.u64(),status:r.u8()};
  if(order.side>1||order.orderType>1||order.status>6)throw new Error('Invalid order report');
  return order;
}
function report(r) {
  if (r.u64() !== '1') throw new Error('Expected one report');
  const event = r.u64(), command = r.u64(), tag = r.u8();
  const value = {type: 'report', event, command};
  if (tag >= 1 && tag <= 3) {
    const order = readOrder(r);
    const reason = tag === 3 ? ['ClientRequest','MarketRemainder','SelfTradePrevention','Replaced'][r.u8()] : undefined;
    if (tag === 3 && !reason) throw new Error('Invalid cancellation reason');
    return {...value, kind: ['accepted','updated','cancelled'][tag - 1], order, ...(reason ? {reason} : {})};
  }
  if (tag === 4) {
    const action = r.u8();
    let request;
    if (action === 1) request = {orderId: r.u64(), participant: r.u32(), side: r.u8(), orderType: r.u8(), price: r.price(), quantity: r.u64(), timestamp: r.u64()};
    else if (action === 2) request = {orderId: r.u64(), participant: r.u32()};
    else if (action === 3) request = {orderId: r.u64(), newOrderId: r.u64(), participant: r.u32(), price: r.price(), quantity: r.u64()};
    else throw new Error('Invalid rejected command');
    const reason = rejects[r.u8()]; if (!reason) throw new Error('Invalid rejection reason');
    return {...value, kind: 'rejected', reason, rejected: request};
  }
  if (tag === 5) return {...value, kind: 'trade', trade: {id: r.u64(), buyOrder: r.u64(), sellOrder: r.u64(),
    buyer: r.u32(), seller: r.u32(), restingOrder: r.u64(), price: r.price(), quantity: r.u64()}};
  throw new Error('Invalid execution report');
}
function unsigned(value, name, max = 18446744073709551615n) {
  if (typeof value !== 'string' || !/^(0|[1-9][0-9]{0,19})$/.test(value) || BigInt(value) > max) throw new Error(`${name} must be an unsigned integer string`);
  return BigInt(value);
}
export function encodeCommand(input, participant) {
  if (!input || input.type !== 'submit') throw new Error('Expected submit request');
  if ('participant' in input && input.participant !== participant) throw new Error('Session identity is immutable');
  const id = unsigned(input.orderId, 'Order ID');
  let payload, type;
  if (input.action === 'new') {
    if (!['buy','sell'].includes(input.side) || !['limit','market'].includes(input.orderType)) throw new Error('Invalid side or order type');
    type = 2; payload = Buffer.alloc(38);
    payload.writeBigUInt64BE(id); payload.writeUInt32BE(participant, 8);
    payload.writeUInt8(input.side === 'buy' ? 0 : 1, 12); payload.writeUInt8(input.orderType === 'limit' ? 0 : 1, 13);
    payload.writeBigInt64BE(input.orderType === 'market' ? 0n : unsigned(input.price, 'Price', 9223372036854775807n), 14);
    payload.writeBigUInt64BE(unsigned(input.quantity, 'Quantity'), 22);
  } else if (input.action === 'cancel') {
    type = 3; payload = Buffer.alloc(12); payload.writeBigUInt64BE(id); payload.writeUInt32BE(participant, 8);
  } else if (input.action === 'replace') {
    type = 4; payload = Buffer.alloc(36); payload.writeBigUInt64BE(id);
    payload.writeBigUInt64BE(unsigned(input.newOrderId, 'New order ID'), 8); payload.writeUInt32BE(participant, 16);
    payload.writeBigInt64BE(unsigned(input.price, 'Price', 9223372036854775807n), 20);
    payload.writeBigUInt64BE(unsigned(input.quantity, 'Quantity'), 28);
  } else throw new Error('Unknown trading action');
  return {type, payload};
}
