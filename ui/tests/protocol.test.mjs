import test from 'node:test';
import assert from 'node:assert/strict';
import {Decoder, decode, frame} from '../bridge/protocol.mjs';
test('TCP fragmentation and coalescing preserve bounded frames and 64-bit sequences', () => {
  const bytes = frame(105, 18446744073709551615n);
  for (let split = 0; split <= bytes.length; ++split) {
    const decoder = new Decoder();
    const messages = [...decoder.append(bytes.subarray(0, split)), ...decoder.append(bytes.subarray(split))];
    assert.equal(messages.length, 1); assert.equal(messages[0].request, '18446744073709551615');
    assert.deepEqual(decode(messages[0]), {type: 'heartbeat'});
  }
  assert.equal(new Decoder().append(Buffer.concat([bytes, bytes])).length, 2);
});
test('malformed headers and payloads fail closed', () => {
  for (const size of [0, 15, 65537]) {
    const bytes = frame(105, 1); bytes.writeUInt32BE(size);
    assert.throws(() => new Decoder().append(bytes));
  }
  const version = frame(105, 1); version.writeUInt16BE(2, 4);
  assert.throws(() => new Decoder().append(version));
  assert.throws(() => decode({type: 105, payload: Buffer.from([1])}));
  assert.throws(() => decode({type: 202, payload: Buffer.alloc(0)}));
});
test('book codec uses interleaved side counts and exact large quantities', () => {
  const payload = Buffer.alloc(64);
  payload.writeBigUInt64BE(9n); payload.writeBigUInt64BE(8n, 8);
  payload.writeUInt32BE(1, 16); payload.writeBigInt64BE(10000n, 20);
  payload.writeBigUInt64BE(18446744073709551615n, 28); payload.writeUInt32BE(2, 36);
  payload.writeUInt32BE(1, 40); payload.writeBigInt64BE(10100n, 44);
  payload.writeBigUInt64BE(42n, 52); payload.writeUInt32BE(1, 60);
  const value = decode({type: 202, request: '2', payload});
  assert.equal(value.anchor, true); assert.equal(value.bids[0].quantity, '18446744073709551615');
  assert.equal(value.asks[0].price, '10100');
  for (let i = 0; i < payload.length; ++i) assert.throws(() => decode({type: 202, request: '2', payload: payload.subarray(0, i)}));
});

test('private snapshot decoding preserves exact IDs and rejects malformed or foreign orders',()=>{
  const payload=Buffer.alloc(63);payload.writeBigUInt64BE(3n);payload.writeUInt32BE(7,8);payload.writeUInt32BE(1,12);
  payload.writeBigUInt64BE(18446744073709551615n,16);payload.writeUInt32BE(7,24);
  payload.writeBigInt64BE(100n,30);payload.writeBigUInt64BE(10n,38);payload.writeBigUInt64BE(5n,46);payload.writeBigUInt64BE(1n,54);payload[62]=3;
  const result=decode({type:106,payload});assert.equal(result.participant,7);assert.equal(result.orders[0].id,'18446744073709551615');assert.equal(result.orders[0].remaining,'5');
  for(let i=0;i<payload.length;i++)assert.throws(()=>decode({type:106,payload:payload.subarray(0,i)}));
  const foreign=Buffer.from(payload);foreign.writeUInt32BE(8,24);assert.throws(()=>decode({type:106,payload:foreign}));
  const duplicate=Buffer.concat([payload,payload.subarray(16)]);duplicate.writeUInt32BE(2,12);assert.throws(()=>decode({type:106,payload:duplicate}));
  const terminal=Buffer.from(payload);terminal[62]=4;assert.throws(()=>decode({type:106,payload:terminal}));
});
