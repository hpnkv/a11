/**
 * Two bundles and two copies.
 *
 * The extension runs in Node inside the extension host, and the webview runs in a
 * browser; they have different platforms, different formats and no shared globals,
 * so they are built separately rather than by one clever configuration.
 *
 * The copies are the generated grammars and the shared `index.html`. Both are owned
 * elsewhere -- the grammars by `a11 flow syntax`, the HTML by the shared webview
 * package -- and copied here at build time rather than duplicated in the
 * repository, so neither can drift from the thing it came from.
 */
import * as esbuild from 'esbuild';
import {copyFileSync, mkdirSync} from 'node:fs';

const dev = process.argv.includes('--dev');

const shared = {
  bundle: true,
  minify: !dev,
  sourcemap: dev ? 'inline' : false,
  logLevel: 'info',
};

await esbuild.build({
  ...shared,
  entryPoints: ['src/extension.ts'],
  outfile: 'dist/extension.js',
  platform: 'node',
  format: 'cjs',
  // Supplied by the host at run time, and not a thing to bundle a copy of.
  external: ['vscode'],
});

await esbuild.build({
  ...shared,
  entryPoints: ['webview/src/main.ts'],
  outfile: 'dist/app.js',
  platform: 'browser',
  format: 'iife',
  // The A11 client picks a WebSocket implementation at run time; in a webview that
  // is the browser's own, so the Node package must not be pulled in.
  external: ['ws'],
});

// The pure modules, as ESM, so `node --test` can exercise them with no editor
// in the way. They are where an off-by-one would live -- the patch placement,
// the fragment spans, the indent a continuation lands on -- and all three are
// text in, text out.
await esbuild.build({
  ...shared,
  entryPoints: ['src/tools/patch.ts', 'src/fragmentSpans.ts', 'src/flowIndent.ts'],
  outdir: 'dist/testable',
  outbase: 'src',
  platform: 'neutral',
  format: 'esm',
  minify: false,
  external: ['vscode'],
  // `.mjs`, because this package is CommonJS and Node would otherwise read a
  // `.js` in `dist/` as CJS and refuse the named exports.
  outExtension: {'.js': '.mjs'},
});

mkdirSync('dist', {recursive: true});
mkdirSync('syntaxes', {recursive: true});
copyFileSync('../webview/index.html', 'dist/index.html');
for (const name of ['a11flow.tmLanguage.json', 'a11flow-injection.tmLanguage.json']) {
  copyFileSync(`../editors/vscode/${name}`, `syntaxes/${name}`);
}
console.log('copied index.html and the generated grammars');
