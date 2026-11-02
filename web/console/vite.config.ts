import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// The built console is served by the Rust gateway (kestrel-gateway --static
// web/console-dist), so the output lands OUTSIDE this package, in web/.
export default defineConfig({
  plugins: [react()],
  build: {
    outDir: '../console-dist',
    emptyOutDir: true,
  },
  server: {
    port: 5173,
  },
});
