import test from 'node:test';
import assert from 'node:assert/strict';
import {marketRoutes,resolveMarket,symbolName} from '../bridge/markets.mjs';
test('market registration is bounded and rejects ambiguous names or ports',()=>{
  assert.equal(marketRoutes(undefined,9000)[0].symbol,'APEX');
  for(const value of ['','aapl','ABC/DEF','X'.repeat(17),'1ABC'])assert.throws(()=>symbolName(value));
  for(const routes of [[],Array.from({length:9},(_,i)=>({symbol:'S'+i,gatewayPort:9000+i})),[{symbol:'A',gatewayPort:9000},{symbol:'A',gatewayPort:9001}], [{symbol:'A',gatewayPort:9000},{symbol:'B',gatewayPort:9000}],[{symbol:'A',gatewayPort:0}]]) assert.throws(()=>marketRoutes(routes));
});
test('legacy direct routing stays compatible while symbol routing rejects conflicting destinations',async()=>{
  assert.equal(await resolveMarket({}),9000);
  assert.equal(await resolveMarket({gatewayPort:12345}),12345);
  await assert.rejects(()=>resolveMarket({symbol:'ALPHA',gatewayPort:9000}),/not both/);
  await assert.rejects(()=>resolveMarket({symbol:'ALPHA',bridgePort:0}),/Invalid/);
});
