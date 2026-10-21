// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// From the checkout: node scripts/render_social_previews.mjs [frontend]
// The frontend defaults to ../a11x/frontend and supplies @fontsource fonts
// and Playwright. Install its dependencies and Chromium before rendering.

import assert from 'node:assert/strict';
import {existsSync, readFileSync, statSync} from 'node:fs';
import {createRequire} from 'node:module';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const frontend = resolve(process.argv[2] ?? resolve(root, '../a11x/frontend'));
const require = createRequire(resolve(frontend, 'package.json'));
const {chromium} = require('@playwright/test');
const fonts = [400, 600, 700].map(weight => ({
    family: 'Noto Sans',
    weight,
    file: require.resolve(
        `@fontsource/noto-sans/files/noto-sans-latin-${weight}-normal.woff`,
    ),
}));
fonts.push({
    family: 'JetBrains Mono',
    weight: 400,
    file: require.resolve(
        '@fontsource/jetbrains-mono/files/jetbrains-mono-latin-400-normal.woff',
    ),
});
const faces = fonts.map(font => `@font-face {
    font-family: '${font.family}';
    font-weight: ${font.weight};
    font-style: normal;
    src: url(data:font/woff;base64,${readFileSync(font.file).toString('base64')})
         format('woff');
}`).join('\n');
const css = `${faces}
    body { margin: 0; }
    svg { display: block; }
    svg text {
        font-synthesis: none;
        font-variant-ligatures: common-ligatures discretionary-ligatures contextual;
        font-feature-settings: 'liga' 1, 'dlig' 1, 'calt' 1;
    }
`;
const browser = await chromium.launch({headless: true});
try {
    for (const name of [
        'doc/docs/assets/a11-docs-social-card',
        'github-social-preview',
    ]) {
        const stem = resolve(root, name);
        if (!existsSync(`${stem}.svg`)) continue;
        const page = await browser.newPage({
            viewport: {width: 1200, height: 630},
            deviceScaleFactor: 2,
        });
        await page.setContent(
            `<html><head><style>${css}</style></head><body>` +
            `${readFileSync(`${stem}.svg`, 'utf8')}</body></html>`,
        );
        await page.evaluate(async fonts => {
            for (const font of fonts) {
                const loaded = await document.fonts.load(
                    `${font.weight} 62px "${font.family}"`,
                );
                if (loaded.length !== 1) {
                    throw new Error(`Missing font: ${font.family} ${font.weight}`);
                }
            }
            await document.fonts.ready;
        }, fonts);
        const cdp = await page.context().newCDPSession(page);
        await cdp.send('DOM.enable');
        await cdp.send('CSS.enable');
        const {root: document} = await cdp.send('DOM.getDocument');
        const {nodeId} = await cdp.send('DOM.querySelector', {
            nodeId: document.nodeId,
            selector: 'text[font-size="62"] > tspan:first-child',
        });
        const {fonts: rendered} = await cdp.send('CSS.getPlatformFontsForNode', {
            nodeId,
        });
        assert(rendered.length > 0, `${name}: hero text was not rendered`);
        for (const font of rendered) {
            assert(font.isCustomFont, `${name}: hero used a fallback font`);
            assert.equal(font.postScriptName, 'NotoSans-Bold');
        }
        await page.screenshot({path: `${stem}.png`});
        assert(statSync(`${stem}.png`).size < 1_000_000, 'Preview exceeds 1 MB');
        console.log(`${name}.png: 2400 × 1260, verified NotoSans-Bold`);
        await page.close();
    }
} finally {
    await browser.close();
}
