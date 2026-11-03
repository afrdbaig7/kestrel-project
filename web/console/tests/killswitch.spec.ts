import { test, expect } from '@playwright/test';

// Pins the kill-switch banner deterministically: the real recording has
// kill=false throughout, so this test MOCKS the gateway with
// page.routeWebSocket('**/ws') and pushes a valid kestrel.demo/1 snapshot
// carrying kill:true. The mock replies to nothing (the console's outbound
// commands are ignored); it just pushes hello + one snapshot.

const killSnapshot = {
  schema: 'kestrel.demo/1',
  ts_ns: 1_000_000,
  event: 42,
  mid: 100_000,
  pos: 5,
  cash: 1_000_010,
  pnl: 35,
  kill: true,
  counts: { orders: 3, fills: 1, declines: 0 },
  bids: [
    [99_999, 10],
    [99_998, 50],
    [99_997, 25],
  ],
  asks: [
    [100_001, 35],
    [100_002, 50],
    [100_003, 20],
  ],
  orders: [{ id: 1, side: 'B', px: 99_999, qty: 10, filled: 2, state: 'Live' }],
  force: false,
};

test('kill switch banner shows on a kill:true snapshot (mocked gateway WS)', async ({ page }) => {
  await page.routeWebSocket('**/ws', (ws) => {
    ws.send(
      JSON.stringify({
        type: 'hello',
        mode: 'playback',
        count: 2,
        schema: 'kestrel.demo/1',
        source: 'mock',
      }),
    );
    ws.send(JSON.stringify({ type: 'snapshot', i: 0, data: killSnapshot }));
  });

  await page.goto('/');

  const banner = page.getByTestId('kill-banner');
  await expect(banner).toBeVisible();
  await expect(banner).toContainText('KILL SWITCH ENGAGED');

  // sanity: the mocked stream really drives the UI (not canned state)
  await expect(page.getByTestId('conn-status')).toHaveText('Connected');
  await expect(page.getByTestId('poslabel')).toHaveText('0 / 2');
  await expect(page.getByTestId('bid-row').first()).toContainText('99,999');
  await expect(page.getByTestId('position-panel')).toContainText('+5 lots');
});
