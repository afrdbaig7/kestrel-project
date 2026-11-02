// Wire types for the kestrel-gateway WS protocol (see services/gateway/src/main.rs).
// Everything the console renders arrives over this stream — no bundled data.

/** One book level as [price_ticks, quantity]. */
export type Level = [number, number];

export interface Order {
  id: number;
  side: string; // "B" | "S"
  px: number;
  qty: number;
  filled: number;
  state: string; // Live | PendingNew | ...
}

export interface Counts {
  orders: number;
  fills: number;
  declines: number;
}

/** One conflated snapshot, schema kestrel.demo/1. */
export interface Snapshot {
  schema?: string;
  ts_ns: number;
  event: number;
  mid: number;
  pos: number;
  cash: number;
  pnl: number;
  kill: boolean;
  counts: Counts;
  bids: Level[];
  asks: Level[];
  orders: Order[];
  force: boolean;
}

export interface HelloInfo {
  mode: 'playback' | 'follow';
  count: number;
  schema: string;
  source: string;
}

/** gateway -> console */
export type GatewayMessage =
  | { type: 'hello'; mode: 'playback' | 'follow'; count: number; schema: string; source: string }
  // i is null in --follow mode (live seam); always numeric in playback mode.
  | { type: 'snapshot'; i: number | null; data: Snapshot }
  | { type: 'end'; count: number }
  | { type: 'error'; message: string };

/** console -> gateway */
export type Command =
  | { type: 'play' }
  | { type: 'pause' }
  | { type: 'step' }
  | { type: 'seek'; index: number }
  | { type: 'speed'; value: number };
