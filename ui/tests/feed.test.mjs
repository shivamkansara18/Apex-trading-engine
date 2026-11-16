import test from 'node:test';
import assert from 'node:assert/strict';
import {advance, emptyFeed} from '../src/feed.ts';
const book = (sequence, anchor = false) => ({type: 'book', sequence, command: sequence, anchor, bids: [], asks: []});
test('gaps suspend updates, request one snapshot, then anchor restores feed', () => {
  let state = advance(emptyFeed(), book('10', true), 1000).feed;
  state = advance(state, book('11'), 1001).feed;
  const gap = advance(state, book('13'), 1002);
  assert.equal(gap.snapshot, true); assert.equal(gap.feed.gaps, 1);
  assert.equal(gap.feed.book.sequence, '11'); assert.equal(gap.feed.recovering, true);
  const pending = advance(gap.feed, book('14'), 1003);
  assert.equal(pending.snapshot, false); assert.equal(pending.feed.gaps, 1);
  state = advance(pending.feed, book('14', true), 1004).feed;
  assert.equal(state.recovering, false); assert.equal(state.book.sequence, '14');
  assert.equal(advance(state, book('15'), 1005).snapshot, false);
});
test('64-bit sequences remain exact above JavaScript integer precision', () => {
  let state = advance(emptyFeed(), book('18446744073709551612', true), 0).feed;
  assert.equal(advance(state, book('18446744073709551613'), 1).snapshot, false);
  assert.equal(advance(state, book('18446744073709551614'), 2).snapshot, true);
});
test('unanchored and duplicate feeds trigger recovery; heartbeat preserves quiet-market state', () => {
  assert.equal(advance(emptyFeed(), book('1'), 100).snapshot, true);
  const state = advance(emptyFeed(), book('0', true), 200).feed;
  assert.equal(advance(state, book('0'), 201).snapshot, true);
  const heartbeat = advance(state, {type: 'heartbeat'}, 20000).feed;
  assert.equal(heartbeat.lastMarket, 200); assert.equal(heartbeat.lastSignal, 20000);
});
test('rate buckets and trade retention stay bounded', () => {
  let state = advance(emptyFeed(), book('0', true), 0).feed;
  for (let i = 1; i <= 100; ++i) state = advance(state, {type: 'trade', sequence: String(i), id: String(i)}, i * 1000).feed;
  assert.equal(state.trades.length, 50); assert.equal(state.buckets.length, 5);
});

test('a feed cannot mix symbols or routing versions even when sequence numbers coincide',async()=>{
  const {advanceForSymbol}=await import('../src/feed.ts');
  const alpha={...book('1',true),schemaVersion:2,symbol:'ALPHA'};
  const feed=advanceForSymbol(emptyFeed(),alpha,'ALPHA',1).feed;
  assert.equal(feed.sequence,'1');
  assert.throws(()=>advanceForSymbol(feed,{...alpha,symbol:'BETA'},'ALPHA',2));
  assert.throws(()=>advanceForSymbol(feed,{...alpha,schemaVersion:1},'ALPHA',2));
  assert.equal(advanceForSymbol(emptyFeed(),{...alpha,symbol:'BETA'},'BETA',3).feed.sequence,'1');
});
