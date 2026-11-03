import { defineConfig, devices } from '@playwright/test';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '..', '..');

export default defineConfig({
  testDir: './tests',
  timeout: 60_000,
  expect: { timeout: 10_000 },
  fullyParallel: false,
  workers: 1,
  retries: 0,
  reporter: [['list']],
  use: {
    baseURL: 'http://127.0.0.1:8902',
    trace: 'retain-on-failure',
  },
  projects: [{ name: 'chromium', use: { ...devices['Desktop Chrome'] } }],
  webServer: {
    // The REAL gateway binary serving the BUILT console (web/console-dist).
    // Run `npm run build` before `npm test`.
    command:
      'services/gateway/target/release/kestrel-gateway --jsonl web/demo-data/demo.jsonl --static web/console-dist --port 8902',
    cwd: repoRoot,
    url: 'http://127.0.0.1:8902/api/health',
    reuseExistingServer: false,
    timeout: 30_000,
  },
});
