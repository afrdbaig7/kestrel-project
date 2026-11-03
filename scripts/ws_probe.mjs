// WS probe for kestrel-gateway: verifies hello/play/snapshot/seek/step/end
// semantics. Prints a JSON result line; exit 0 on pass. Node >=21 (global WebSocket).
const url = process.argv[2] ?? 'ws://127.0.0.1:8902/ws';
const ws = new WebSocket(url);
let snaps = 0, first = null, seekSeen = false, hello = null;
const t0 = Date.now();
const fail = (error) => { console.log(JSON.stringify({ ok: false, error, snaps })); process.exit(1); };
ws.onopen = () => ws.send(JSON.stringify({ type: 'play' }));
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.type === 'hello') {
    hello = m;
    if (m.mode !== 'playback' || m.count < 2) fail('bad hello: ' + JSON.stringify(m));
  } else if (m.type === 'snapshot') {
    snaps++;
    if (!first) { first = m; if (!m.data?.schema || m.data.schema !== 'kestrel.demo/1') fail('snapshot schema wrong'); }
    if (snaps === 3) ws.send(JSON.stringify({ type: 'seek', index: 50 }));
    if (snaps === 4) { if (m.i !== 50) fail('seek: expected i=50, got ' + m.i); seekSeen = true; ws.send(JSON.stringify({ type: 'pause' })); ws.send(JSON.stringify({ type: 'step' })); }
    if (snaps === 5) { if (m.i !== 51) fail('step: expected i=51, got ' + m.i); ws.send(JSON.stringify({ type: 'speed', value: 8 })); ws.send(JSON.stringify({ type: 'play' })); }
  } else if (m.type === 'end') {
    const ok = snaps >= 6 && seekSeen && !!hello;
    console.log(JSON.stringify({ ok, hello, snapshots: snaps, seekSeen, ms: Date.now() - t0 }));
    ws.close(); process.exit(ok ? 0 : 1);
  }
};
ws.onerror = () => fail('ws error');
setTimeout(() => fail('timeout'), 60000);
