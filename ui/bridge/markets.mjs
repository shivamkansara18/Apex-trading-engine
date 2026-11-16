export function symbolName(value) {
  if (typeof value !== 'string' || !/^[A-Z][A-Z0-9._-]{0,15}$/.test(value)) throw new Error('Symbols must be 1-16 uppercase letters, digits, dots, underscores or hyphens, starting with a letter');
  return value;
}
export function marketRoutes(markets, gatewayPort = 9000) {
  const routes = markets ?? [{symbol:'APEX',gatewayPort}];
  if (!Array.isArray(routes) || routes.length < 1 || routes.length > 8) throw new Error('Configure 1-8 symbol partitions');
  const names = new Set(), ports = new Set();
  for (const market of routes) {
    symbolName(market.symbol);
    if (!Number.isInteger(market.gatewayPort) || market.gatewayPort < 1 || market.gatewayPort > 65535) throw new Error('Invalid symbol gateway port');
    if (names.has(market.symbol) || ports.has(market.gatewayPort)) throw new Error('Symbol names and gateway ports must be unique');
    names.add(market.symbol); ports.add(market.gatewayPort);
  }
  return routes;
}
export async function resolveMarket({symbol, bridgePort = 8080, gatewayPort}) {
  if (!symbol) return gatewayPort ?? 9000;
  symbolName(symbol);
  if (gatewayPort !== undefined) throw new Error('Use --symbol or --gateway-port, not both');
  if (!Number.isInteger(bridgePort) || bridgePort < 1 || bridgePort > 65535) throw new Error('Invalid bridge port');
  const response = await fetch(`http://127.0.0.1:${bridgePort}/api/markets`, {signal:AbortSignal.timeout(5000)});
  if (!response.ok) throw new Error('Cannot discover configured markets');
  const metadata = await response.json();
  if (metadata.schemaVersion !== 2) throw new Error('Unsupported market routing version');
  const market = metadata.markets?.find(m => m.symbol === symbol);
  if (!market || market.available === false) throw new Error(`Market ${symbol} is unknown or unavailable`);
  return marketRoutes([market])[0].gatewayPort;
}
