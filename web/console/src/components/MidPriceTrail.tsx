import { useEffect, useMemo, useRef, useState } from 'react';
import type { Snapshot } from '../types';
import { fmt } from './OrderBookLadder';

const H = 120; // svg height (demo canvas height)
const PAD = 4;
const MAX_PTS = 600; // cap rendered points (follow mode can run unbounded)

interface Pt {
  x: number;
  y: number;
  pos: number;
  mid: number;
}

interface Shade {
  d: string;
  sign: 'long' | 'short';
}

/**
 * Mid-price trail as an SVG polyline with position shading: where pos != 0 the
 * area under the curve is filled green (long) / red (short), mirroring
 * demo.html's canvas shading but in SVG. Price axis labels on the right.
 */
export function MidPriceTrail({
  history,
  index,
}: {
  history: Map<number, Snapshot>;
  index: number;
}) {
  const wrapRef = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(0);

  useEffect(() => {
    const el = wrapRef.current;
    if (!el) return;
    setWidth(el.clientWidth);
    const ro = new ResizeObserver((entries) => {
      for (const e of entries) setWidth(e.contentRect.width);
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const pts = useMemo<Pt[]>(() => {
    if (width <= 0) return [];
    const idxs = [...history.keys()].filter((i) => i <= index).sort((a, b) => a - b);
    const slice = idxs.slice(-MAX_PTS);
    const n = slice.length;
    if (n === 0) return [];
    const mids = slice
      .map((i) => history.get(i)!.mid)
      .filter((m) => m > 0);
    const denom = Math.max(1, n - 1);
    if (mids.length === 0) {
      return slice.map((_, k) => ({ x: (k / denom) * width, y: H / 2, pos: 0, mid: 0 }));
    }
    const lo = Math.min(...mids);
    const hi = Math.max(...mids);
    const y = (m: number) =>
      hi === lo ? H / 2 : H - PAD - ((m - lo) / (hi - lo)) * (H - 2 * PAD);
    return slice.map((i, k) => {
      const s = history.get(i)!;
      return { x: (k / denom) * width, y: y(s.mid), pos: s.pos, mid: s.mid };
    });
  }, [history, index, width]);

  const lineD = pts
    .map((p, k) => `${k === 0 ? 'M' : 'L'} ${p.x.toFixed(2)} ${p.y.toFixed(2)}`)
    .join(' ');

  // Contiguous runs where pos != 0 → area under the curve (green long / red short).
  const shades = useMemo<Shade[]>(() => {
    const out: Shade[] = [];
    let run: Pt[] = [];
    const flush = () => {
      if (run.length === 0) return;
      const first = run[0];
      const last = run[run.length - 1];
      const d = [
        `M ${first.x.toFixed(2)} ${H}`,
        ...run.map((p) => `L ${p.x.toFixed(2)} ${p.y.toFixed(2)}`),
        `L ${last.x.toFixed(2)} ${H}`,
        'Z',
      ].join(' ');
      out.push({ d, sign: first.pos > 0 ? 'long' : 'short' });
      run = [];
    };
    for (const p of pts) {
      if (p.pos !== 0) run.push(p);
      else flush();
    }
    flush();
    return out;
  }, [pts]);

  const axis = useMemo(() => {
    const mids = pts.map((p) => p.mid).filter((m) => m > 0);
    if (mids.length === 0) return null;
    const lo = Math.min(...mids);
    const hi = Math.max(...mids);
    return { lo, hi, mid: (lo + hi) / 2 };
  }, [pts]);

  return (
    <div className="body trail-wrap" ref={wrapRef}>
      {width > 0 && (
        <svg
          className="trail-svg"
          data-testid="trail-svg"
          width={width}
          height={H}
          role="img"
          aria-label="mid price trail"
        >
          {[1, 2, 3].map((k) => (
            <line
              key={k}
              x1={0}
              x2={width}
              y1={(H * k) / 4}
              y2={(H * k) / 4}
              stroke="#1d2937"
              strokeWidth={1}
            />
          ))}
          {shades.map((s, k) => (
            <path
              key={k}
              d={s.d}
              fill={s.sign === 'long' ? 'rgba(47,208,122,.16)' : 'rgba(255,92,108,.16)'}
            />
          ))}
          <path
            data-testid="trail-line"
            d={lineD}
            fill="none"
            stroke="#4cc2ff"
            strokeWidth={1.4}
          />
          {axis && (
            <>
              <text x={width - 2} y={11} textAnchor="end" fontSize={10} fill="#64798f">
                {fmt(axis.hi)}
              </text>
              <text x={width - 2} y={H / 2 - 2} textAnchor="end" fontSize={10} fill="#64798f">
                {fmt(axis.mid)}
              </text>
              <text x={width - 2} y={H - 2} textAnchor="end" fontSize={10} fill="#64798f">
                {fmt(axis.lo)}
              </text>
            </>
          )}
        </svg>
      )}
    </div>
  );
}
