import {execFileSync, spawnSync} from 'node:child_process';
import {cpSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';

const source = JSON.parse(readFileSync(new URL('../package.json', import.meta.url)));
// const packageNames = ['@curiositystack/a11', 'aeleven'];
const packageNames = ['@curiositystack/a11'];
const stagingRoot = mkdtempSync(join(tmpdir(), 'a11-npm-'));
const dryRun = process.argv.includes('--dry-run');

function publishedVersion(name) {
    const result = spawnSync('npm', ['view', `${name}@${source.version}`, 'version', '--json'], {
        encoding: 'utf8',
        stdio: ['ignore', 'pipe', 'ignore'],
    });
    return result.status === 0 ? JSON.parse(result.stdout) : null;
}

try {
    for (const name of packageNames) {
        if (publishedVersion(name) === source.version) {
            console.log(`${name}@${source.version} is already published; skipping.`);
            continue;
        }
        const directory = join(stagingRoot, name.replaceAll('/', '__'));
        cpSync(new URL('../dist', import.meta.url), join(directory, 'dist'), {recursive: true});
        const manifest = {
            name,
            version: source.version,
            description: source.description,
            type: source.type,
            sideEffects: source.sideEffects,
            main: source.main,
            types: source.types,
            exports: source.exports,
            files: source.files,
            engines: source.engines,
            license: source.license,
            dependencies: source.dependencies,
            publishConfig: {access: 'public'},
        };
        writeFileSync(join(directory, 'package.json'), `${JSON.stringify(manifest, null, 2)}\n`);
        const arguments_ = ['publish', directory, '--access', 'public', '--provenance'];
        if (dryRun) arguments_.push('--dry-run');
        execFileSync('npm', arguments_, {
            stdio: 'inherit',
        });
    }
} finally {
    rmSync(stagingRoot, {recursive: true, force: true});
}
