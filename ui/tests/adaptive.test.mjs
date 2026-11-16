import test from 'node:test';
import assert from 'node:assert/strict';
import {quotePrices,randomSource} from '../bridge/adaptive.mjs';
import {encodeCommand} from '../bridge/protocol.mjs';
test('quotes follow external book changes and inventory rather than a fixed script',()=>{
  const empty={bids:[],asks:[]};
  const book={bids:[{price:'20000'}],asks:[{price:'20100'}]};
  assert.notDeepEqual(quotePrices(empty,10000n,5n),quotePrices(book,10000n,5n));
  assert.ok(BigInt(quotePrices(book,10000n,5n,200n).buy)<BigInt(quotePrices(book,10000n,5n,-200n).buy));
  assert.ok(BigInt(quotePrices(empty,1n,5n).buy)>0n);
});
test('seeded decisions are reproducible, distinct seeds differ',()=>{
  const a=randomSource(42),b=randomSource(42),c=randomSource(43);
  const first=Array.from({length:30},a);
  assert.deepEqual(first,Array.from({length:30},b));assert.notDeepEqual(first,Array.from({length:30},c));
});
test('order translation preserves exact integers and rejects identity spoofing and lossy input',()=>{
  const input={type:'submit',action:'new',orderId:'18446744073709551615',side:'buy',orderType:'limit',price:'10000',quantity:'10'};
  assert.equal(encodeCommand(input,5).payload.readBigUInt64BE(),18446744073709551615n);
  assert.equal(encodeCommand({...input,orderId:'0'},5).payload.readBigUInt64BE(),0n);
  for(const invalid of [{...input,participant:6},{...input,quantity:10},{...input,orderId:'18446744073709551616'},{...input,price:'1.5'},{...input,side:'invalid'}])assert.throws(()=>encodeCommand(invalid,5));
});
