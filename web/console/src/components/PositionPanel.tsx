import type { Snapshot } from '../types';
import { fmt } from './OrderBookLadder';

const CAP = 200; // max_position from the demo engine config

const posCls = (v: number) => (v > 0 ? 'pos-long' : v < 0 ? 'pos-short' : '');

/**
 * Position / PnL panel plus session counters (orders/fills/declines), both
 * projected live from the latest WS snapshot — "—" until the first one lands.
 */
export function PositionPanel({ snap }: { snap: Snapshot | null }) {
  const pos = snap?.pos ?? 0;
  const usage = Math.min(100, (Math.abs(pos) / CAP) * 100);
  const pnlCls = (snap?.pnl ?? 0) >= 0 ? 'pnl-pos' : 'pnl-neg';
  return (
    <>
      <section className="panel">
        <h2>Position / PnL (cold-path projection)</h2>
        <div className="body" data-testid="position-panel">
          <div className="kv">
            <span>position</span>
            <b className={snap ? posCls(pos) : undefined}>
              {snap ? `${pos > 0 ? '+' : ''}${fmt(pos)} lots` : '—'}
            </b>
          </div>
          <div className="kv">
            <span>risk usage (pos cap {CAP})</span>
            <b>{snap ? `${usage.toFixed(0)}%` : '—'}</b>
          </div>
          <div className="latbar" aria-hidden="true">
            <i style={{ width: `${usage.toFixed(1)}%` }} />
          </div>
          <div className="kv">
            <span>cash flow</span>
            <b>{snap ? fmt(snap.cash) : '—'}</b>
          </div>
          <div className="kv">
            <span>mark mid</span>
            <b>{snap ? fmt(snap.mid) : '—'}</b>
          </div>
          <div className="kv">
            <span>unrealized PnL</span>
            <b className={snap ? pnlCls : undefined}>{snap ? fmt(snap.pnl) : '—'}</b>
          </div>
        </div>
        <div className="note">
          PnL = signed cash flow + position × mark mid (average-cost approximation; demo
          semantics).
        </div>
      </section>

      <section className="panel">
        <h2>Session counters</h2>
        <div className="body" data-testid="counters">
          <div className="kv">
            <span>events replayed</span>
            <b>{snap ? fmt(snap.event) : '—'}</b>
          </div>
          <div className="kv">
            <span>orders submitted</span>
            <b>{snap ? fmt(snap.counts.orders) : '—'}</b>
          </div>
          <div className="kv">
            <span>fills</span>
            <b>{snap ? fmt(snap.counts.fills) : '—'}</b>
          </div>
          <div className="kv">
            <span>declines</span>
            <b>{snap ? fmt(snap.counts.declines) : '—'}</b>
          </div>
        </div>
      </section>
    </>
  );
}
