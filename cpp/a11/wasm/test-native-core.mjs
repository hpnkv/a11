import createModule from './build/threaded/a11_wasm_native_test.mjs';

const module = await createModule({noInitialRun: true});
await module.callMain(process.argv.slice(2));
process.exitCode = module._a11_wasm_native_test_status();
