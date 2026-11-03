import { test, expect } from '@playwright/test';

// Connected-mode e2e against the REAL gateway + REAL recorded projection
// (web/demo-data/demo.jsonl, schema kestrel.demo/1). The gateway sends only
// hello on connect; snapshots flow after play/step commands.

test('console: loads, ladders real data, plays, seeks to end — zero console errors', async ({
  page,
}) => {
  const consoleErrors: string[] = [];
  page.on('console', (msg) => {
    if (msg.type() === 'error') consoleErrors.push(msg.text());
  });
  page.on('pageerror', (err) => consoleErrors.push(`pageerror: ${err.message}`));

  await page.goto('/');

  // hello arrives -> connected badge (text is exactly "Connected" on success)
  await expect(page.getByTestId('conn-status')).toHaveText('Connected', { timeout: 10_000 });
  await expect(page.getByTestId('poslabel')).toHaveText(/0 \/ \d+/);
  const countText = (await page.getByTestId('poslabel').textContent())!.split('/')[1].trim();
  const count = Number(countText);
  expect(count).toBeGreaterThanOrEqual(2); // real recording has 134 snapshots

  // page loaded with zero console errors
  expect(consoleErrors).toEqual([]);

  // step once -> a single real snapshot renders both sides of the ladder
  await page.getByTestId('step-btn').click();
  await expect(page.getByTestId('poslabel')).toHaveText(`0 / ${count}`);
  await expect(page.getByTestId('bid-row').first()).toBeVisible();
  await expect(page.getByTestId('ask-row').first()).toBeVisible();
  expect(await page.getByTestId('bid-row').count()).toBeGreaterThan(0);
  expect(await page.getByTestId('ask-row').count()).toBeGreaterThan(0);
  await expect(page.getByTestId('spread')).toContainText('spread');

  // play -> index readout advances and the trail path data changes within ~5s
  const trail = page.getByTestId('trail-line');
  await expect(trail).toBeAttached();
  const d0 = await trail.getAttribute('d');
  const labelBefore = (await page.getByTestId('poslabel').textContent())!;
  await page.getByTestId('play-btn').click();
  await expect(page.getByTestId('poslabel')).not.toHaveText(labelBefore, { timeout: 5_000 });
  await expect(trail).not.toHaveAttribute('d', d0 ?? '', { timeout: 5_000 });

  // seek to the end of the stream -> index display reaches the count-1 area
  const slider = page.getByTestId('seek-slider');
  await expect(slider).toHaveAttribute('max', String(count - 1));
  await slider.focus();
  await page.keyboard.press('End');
  await expect(page.getByTestId('poslabel')).toHaveText(`${count - 1} / ${count}`, {
    timeout: 5_000,
  });

  // the whole run produced zero console errors
  expect(consoleErrors).toEqual([]);
});
