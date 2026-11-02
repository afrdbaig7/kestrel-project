import type { ConnStatus } from '../useGatewaySocket';

const SPEEDS = [0.25, 0.5, 1, 2, 4, 8];

interface Props {
  status: ConnStatus;
  playing: boolean;
  index: number;
  count: number;
  speed: number;
  onPlayPause: () => void;
  onStep: () => void;
  onSeek: (index: number) => void;
  onSpeed: (v: number) => void;
}

/**
 * Playback transport mirroring demo.html: play/pause, step, speed, seek slider
 * bound to the count advertised by hello, and an index/count readout. Play and
 * step are disabled once the stream has ended until the user seeks (which
 * re-arms the stream).
 */
export function PlaybackControls({
  status,
  playing,
  index,
  count,
  speed,
  onPlayPause,
  onStep,
  onSeek,
  onSpeed,
}: Props) {
  const offline = status === 'connecting' || status === 'disconnected';
  const ended = status === 'ended';
  const max = Math.max(0, count - 1);
  const clampedIndex = Math.min(index, max);
  return (
    <div className="controls" data-testid="controls">
      <button
        className="primary"
        data-testid="play-btn"
        disabled={offline || ended}
        onClick={onPlayPause}
        aria-pressed={playing}
      >
        {playing ? '❚❚ pause' : '▶ play'}
      </button>
      <button data-testid="step-btn" disabled={offline || ended} onClick={onStep}>
        step ▸
      </button>
      <select
        data-testid="speed-select"
        aria-label="playback speed"
        value={speed}
        disabled={offline}
        onChange={(e) => onSpeed(Number(e.target.value))}
      >
        {SPEEDS.map((v) => (
          <option key={v} value={v}>
            {v}x
          </option>
        ))}
      </select>
      <input
        type="range"
        data-testid="seek-slider"
        aria-label="seek to snapshot"
        min={0}
        max={max}
        step={1}
        value={clampedIndex}
        disabled={offline || count === 0}
        onChange={(e) => onSeek(Number(e.target.value))}
      />
      <span
        className="tag"
        data-testid="poslabel"
        style={{ minWidth: 130, textAlign: 'right' }}
        aria-label={`snapshot ${index} of ${count}`}
      >
        {index} / {count}
      </span>
    </div>
  );
}
