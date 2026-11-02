import { Fragment, useMemo } from 'react';
import { useGatewaySocket } from './useGatewaySocket';
import { KillSwitchBanner } from './components/KillSwitchBanner';
import { OrderBookLadder } from './components/OrderBookLadder';
import { MidPriceTrail } from './components/MidPriceTrail';
import { OrdersTable } from './components/OrdersTable';
import { PositionPanel } from './components/PositionPanel';
import { PlaybackControls } from './components/PlaybackControls';
import { fmt } from './components/OrderBookLadder';

// Bundled benchmark metadata from demo.html (build/host measurements, not per-event data).
const BENCH = { p50: 387, p90: 456, p99: 528, p999: 627, label: 't2t minimal (no journal), n=4525' } as const;

/**
 * WS endpoint: same-origin /ws when served by the gateway (production build);
 * vite dev server defaults to the gateway on 127.0.0.1:8902; VITE_WS_URL
 * overrides both.
 */
function resolveWsUrl(): string {
  const override = import.meta.env.VITE_WS_URL;
  if (override) return override;
  if (import.meta.env.DEV) return 'ws://127.0.0.1:8902/ws';
  const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${proto}//${window.location.host}/ws`;
}

const STATUS_TEXT: Record<string, string> = {
  connecting: 'Connecting…',
  connected: 'Connected',
  disconnected: 'Disconnected',
  ended: 'Ended',
};

const STATUS_BADGE: Record<string, string> = {
  connecting: '',
  connected: 'ok',
  disconnected: 'err',
  ended: 'warn',
};

export default function App() {
  const url = useMemo(resolveWsUrl, []);
  const gw = useGatewaySocket(url);
  const snap = gw.snapshot;
  const kill = !!snap && (snap.kill === true || snap.force === true);

  return (
    <div className="app">
      <header>
        <h1>KESTREL</h1>
        <span className="tag">
          gateway console — {gw.hello?.mode ?? '…'} session · schema {gw.hello?.schema ?? '…'}
        </span>
        <span
          className={`badge ${STATUS_BADGE[gw.status] ?? ''}`}
          data-testid="conn-status"
          role="status"
          aria-live="polite"
        >
          <span className="dot" aria-hidden="true" />
          {STATUS_TEXT[gw.status] ?? gw.status}
        </span>
      </header>

      <KillSwitchBanner active={kill} />

      <main className="grid">
        <section className="panel">
          <h2>Order book — instrument 0</h2>
          <OrderBookLadder bids={snap?.bids ?? []} asks={snap?.asks ?? []} />
          <div className="note">
            Mirrored from the engine feed handler; depth bars scaled per side. Stale-quote markers =
            our resting orders.
          </div>
        </section>

        <div className="col">
          <section className="panel">
            <h2>Mid price trail</h2>
            <MidPriceTrail history={gw.history} index={gw.index} />
          </section>

          <section className="panel">
            <h2>Open orders (engine OMS projection)</h2>
            <OrdersTable orders={snap?.orders} />
          </section>
        </div>

        <div className="col">
          <PositionPanel snap={snap} />

          <section className="panel">
            <h2>Measured latency (bundled benchmark, not per-event)</h2>
            <div className="body">
              <div className="kv">
                <span>{BENCH.label}</span>
                <b></b>
              </div>
              {(['p50', 'p90', 'p99', 'p999'] as const).map((p) => (
                <Fragment key={p}>
                  <div className="kv">
                    <span>{p}</span>
                    <b>{fmt(BENCH[p])} ns</b>
                  </div>
                  <div className="latbar" aria-hidden="true">
                    <i style={{ width: `${Math.min(100, BENCH[p] / 1000)}%` }} />
                  </div>
                </Fragment>
              ))}
              <div className="kv">
                <span>journal on (p50)</span>
                <b>1.02 us</b>
              </div>
            </div>
            <div className="note">
              From <b>bench/results/full-1919</b> on this exact build &amp; host: software
              feed-to-order, order-producing events, minimal instrumentation. Per-event latency is
              intentionally NOT journaled (hot-path observer cost, ADR in docs/benchmarks/report.md).
            </div>
          </section>
        </div>
      </main>

      <PlaybackControls
        status={gw.status}
        playing={gw.playing}
        index={gw.index}
        count={gw.count}
        speed={gw.speed}
        onPlayPause={() => (gw.playing ? gw.pause() : gw.play())}
        onStep={gw.step}
        onSeek={gw.seek}
        onSpeed={gw.setSpeed}
      />
    </div>
  );
}
