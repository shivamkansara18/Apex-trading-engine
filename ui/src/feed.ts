export type Level = {price: string; quantity: string; orders: number};
export type Book = {type: 'book'; sequence: string; command: string; anchor: boolean; bids: Level[]; asks: Level[]};
export type Trade = {type: 'trade'; sequence: string; event: string; command: string; id: string; price: string; quantity: string; buyOrder: string; sellOrder: string; restingOrder: string; buyer: number; seller: number};
export type Message = Book | Trade | {type: 'ready' | 'heartbeat' | 'subscribed'} | {type: 'error'; message: string};
export type Feed = {
  sequence: string | null; book: Book | null; trades: (Trade & {time: number})[];
  recovering: boolean; gaps: number; lastSignal: number; lastMarket: number;
  buckets: {second: number; count: number}[];
};
export function emptyFeed(): Feed {
  return {sequence: null, book: null, trades: [], recovering: false, gaps: 0, lastSignal: 0, lastMarket: 0, buckets: []};
}
export function advance(previous: Feed, message: Message, now: number): {feed: Feed; snapshot: boolean} {
  const feed = {...previous, lastSignal: now};
  if (message.type !== 'book' && message.type !== 'trade') return {feed, snapshot: false};
  const anchor = message.type === 'book' && message.anchor;
  if (!anchor && (feed.recovering || feed.sequence === null || BigInt(message.sequence) !== BigInt(feed.sequence) + 1n)) {
    const snapshot = !feed.recovering;
    return {feed: {...feed, recovering: true, gaps: feed.gaps + (snapshot ? 1 : 0)}, snapshot};
  }
  const second = Math.floor(now / 1000);
  const buckets = feed.buckets.filter(b => b.second > second - 5).map(b => ({...b}));
  if (!anchor) {
    if (buckets.at(-1)?.second === second) buckets[buckets.length - 1].count++;
    else buckets.push({second, count: 1});
  }
  return {feed: {...feed, sequence: message.sequence, recovering: false, buckets, lastMarket: now,
    book: message.type === 'book' ? message : feed.book,
    trades: message.type === 'trade' ? [{...message, time: now}, ...feed.trades].slice(0, 50) : feed.trades}, snapshot: false};
}

export function advanceForSymbol(previous:Feed,message:Message & {schemaVersion:number;symbol:string},symbol:string,now:number) {
  if(message.schemaVersion!==2||message.symbol!==symbol)throw new Error('Unexpected market route');
  return advance(previous,message,now);
}
