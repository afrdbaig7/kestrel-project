import type { Level } from '../types';

export const fmt = (v: number, d = 0) =>
  Number(v).toLocaleString('en-US', { maximumFractionDigits: d });

interface SideProps {
  levels: Level[];
  side: 'bid' | 'ask';
}

/**
 * One side of the ladder. Depth bars are scaled against the largest size on
 * that side (same rule as demo.html). Best bid = first bid row; best ask =
 * last ask row (asks are rendered highest-first, so it sits next to the
 * spread).
 */
function Side({ levels, side }: SideProps) {
  const maxq = Math.max(1, ...levels.map((l) => l[1]));
  return (
    <>
      {levels.map(([px, qty], k) => {
        const best = side === 'bid' ? k === 0 : k === levels.length - 1;
        return (
          <div
            key={`${side}-${px}-${k}`}
            className={`row ${side}${best ? ' best' : ''}`}
            data-testid={`${side}-row`}
          >
            <span className="px">{fmt(px)}</span>
            <span className="qty" aria-hidden="true">
              <i style={{ width: `${((100 * qty) / maxq).toFixed(1)}%` }} />
            </span>
            <span className="qty">{fmt(qty)}</span>
          </div>
        );
      })}
    </>
  );
}

export function OrderBookLadder({ bids, asks }: { bids: Level[]; asks: Level[] }) {
  const asksReversed = [...asks].reverse();
  const spread =
    bids.length > 0 && asks.length > 0 ? asks[0][0] - bids[0][0] : null;
  return (
    <div className="body">
      {bids.length === 0 && asks.length === 0 ? (
        <div className="placeholder">waiting for gateway stream…</div>
      ) : (
        <>
          <div data-testid="asks">
            <Side levels={asksReversed} side="ask" />
          </div>
          <div className="spread" data-testid="spread">
            {spread !== null ? `spread ${fmt(spread)} ticks` : 'one-sided'}
          </div>
          <div data-testid="bids">
            <Side levels={bids} side="bid" />
          </div>
        </>
      )}
    </div>
  );
}
