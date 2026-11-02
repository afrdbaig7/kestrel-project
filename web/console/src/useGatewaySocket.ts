import { useCallback, useEffect, useRef, useState } from 'react';
import type { Command, GatewayMessage, HelloInfo, Snapshot } from './types';

/** Socket-level state. */
export type SocketStatus = 'connecting' | 'connected' | 'disconnected';
/**
 * Badge-level status: `ended` means the socket is healthy but the playback
 * stream reached its end (end message received).
 */
export type ConnStatus = SocketStatus | 'ended';

const MAX_HISTORY = 5000; // snapshots kept for the mid trail (playback counts are tiny)
const RECONNECT_BASE_MS = 400;
const RECONNECT_CAP_MS = 5000; // capped exponential backoff
const DEFAULT_SPEED = 4; // matches demo.html default

export interface GatewayApi {
  status: ConnStatus;
  hello: HelloInfo | null;
  /** total snapshot count advertised by hello */
  count: number;
  /** index of the latest snapshot (0-based; playback mode) */
  index: number;
  snapshot: Snapshot | null;
  /** snapshots seen so far, keyed by index — history for the mid trail */
  history: Map<number, Snapshot>;
  playing: boolean;
  speed: number;
  setSpeed: (v: number) => void;
  play: () => void;
  pause: () => void;
  step: () => void;
  seek: (index: number) => void;
}

function sanitize(s: Snapshot): Snapshot {
  // Never let a malformed frame crash the console.
  return {
    ...s,
    counts: s.counts ?? { orders: 0, fills: 0, declines: 0 },
    bids: Array.isArray(s.bids) ? s.bids : [],
    asks: Array.isArray(s.asks) ? s.asks : [],
    orders: Array.isArray(s.orders) ? s.orders : [],
  };
}

/**
 * Owns the single WebSocket connection to kestrel-gateway: hello/snapshot/end
 * handling, snapshot history, capped-backoff auto-reconnect, and the
 * play/pause/step/seek/speed command channel.
 */
export function useGatewaySocket(url: string): GatewayApi {
  const [sockStatus, setSockStatus] = useState<SocketStatus>('connecting');
  const [ended, setEnded] = useState(false);
  const [hello, setHello] = useState<HelloInfo | null>(null);
  const [count, setCount] = useState(0);
  const [index, setIndex] = useState(0);
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [history, setHistory] = useState<Map<number, Snapshot>>(() => new Map());
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeedState] = useState(DEFAULT_SPEED);

  const wsRef = useRef<WebSocket | null>(null);
  const attemptsRef = useRef(0);
  const endedRef = useRef(false);
  const speedRef = useRef(DEFAULT_SPEED);
  const followSeqRef = useRef(0);
  const reconnectRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [attempt, setAttempt] = useState(0);

  const send = useCallback((cmd: Command) => {
    const ws = wsRef.current;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(cmd));
    }
  }, []);

  const clearEnded = useCallback(() => {
    if (endedRef.current) {
      endedRef.current = false;
      setEnded(false);
    }
  }, []);

  const setSpeed = useCallback(
    (v: number) => {
      speedRef.current = v;
      setSpeedState(v);
      send({ type: 'speed', value: v });
    },
    [send],
  );

  const play = useCallback(() => {
    setPlaying(true);
    send({ type: 'play' });
  }, [send]);

  const pause = useCallback(() => {
    setPlaying(false);
    send({ type: 'pause' });
  }, [send]);

  const step = useCallback(() => {
    setPlaying(false);
    send({ type: 'pause' });
    send({ type: 'step' });
  }, [send]);

  const seek = useCallback(
    (target: number) => {
      // Seeking re-arms a finished stream and scrubs like demo.html (pauses first).
      clearEnded();
      setPlaying(false);
      setIndex(target);
      send({ type: 'pause' });
      send({ type: 'seek', index: target });
    },
    [send, clearEnded],
  );

  useEffect(() => {
    let disposed = false;
    setSockStatus('connecting');

    const scheduleReconnect = () => {
      if (disposed) return;
      setSockStatus('disconnected');
      setPlaying(false);
      const delay = Math.min(RECONNECT_CAP_MS, RECONNECT_BASE_MS * 2 ** attemptsRef.current);
      attemptsRef.current += 1;
      reconnectRef.current = setTimeout(() => {
        if (!disposed) setAttempt((a) => a + 1);
      }, delay);
    };

    let ws: WebSocket;
    try {
      ws = new WebSocket(url);
    } catch {
      scheduleReconnect();
      return () => {
        disposed = true;
      };
    }
    wsRef.current = ws;

    ws.onopen = () => {
      attemptsRef.current = 0; // reset backoff on a healthy connection
      setSockStatus('connected');
    };

    ws.onmessage = (ev: MessageEvent) => {
      let msg: GatewayMessage;
      try {
        msg = JSON.parse(String(ev.data)) as GatewayMessage;
      } catch {
        console.warn('kestrel console: dropping non-JSON frame');
        return;
      }
      switch (msg.type) {
        case 'hello': {
          setHello({ mode: msg.mode, count: msg.count, schema: msg.schema, source: msg.source });
          setCount(msg.count);
          setSockStatus('connected');
          if (endedRef.current) {
            // Fresh session after a reconnect past the end: reset the view.
            endedRef.current = false;
            setEnded(false);
            setIndex(0);
            setSnapshot(null);
            setHistory(new Map());
            followSeqRef.current = 0;
          }
          // Keep server pacing aligned with the UI speed selection.
          send({ type: 'speed', value: speedRef.current });
          break;
        }
        case 'snapshot': {
          // In --follow mode the gateway sends i:null; derive a sequence then.
          const i = typeof msg.i === 'number' ? msg.i : followSeqRef.current++;
          endedRef.current = false;
          setEnded(false);
          setIndex(i);
          const data = sanitize(msg.data);
          setSnapshot(data);
          setHistory((prev) => {
            const next = new Map(prev);
            next.set(i, data);
            if (next.size > MAX_HISTORY) {
              const oldest = next.keys().next();
              if (!oldest.done) next.delete(oldest.value);
            }
            return next;
          });
          break;
        }
        case 'end': {
          endedRef.current = true;
          setEnded(true);
          setPlaying(false);
          break;
        }
        case 'error': {
          console.warn('kestrel gateway:', msg.message);
          break;
        }
      }
    };

    ws.onclose = () => {
      if (wsRef.current === ws) wsRef.current = null;
      scheduleReconnect();
    };
    ws.onerror = () => {
      /* onclose always follows; backoff is scheduled there */
    };

    return () => {
      disposed = true;
      if (reconnectRef.current !== null) {
        clearTimeout(reconnectRef.current);
        reconnectRef.current = null;
      }
      ws.onopen = null;
      ws.onmessage = null;
      ws.onerror = null;
      ws.onclose = null;
      if (ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.OPEN) {
        ws.close();
      }
      if (wsRef.current === ws) wsRef.current = null;
    };
  }, [url, attempt, send, clearEnded]);

  const status: ConnStatus = ended && sockStatus === 'connected' ? 'ended' : sockStatus;

  return {
    status,
    hello,
    count,
    index,
    snapshot,
    history,
    playing,
    speed,
    setSpeed,
    play,
    pause,
    step,
    seek,
  };
}
