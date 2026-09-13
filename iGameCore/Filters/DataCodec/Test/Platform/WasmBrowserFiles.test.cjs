const test = require('node:test');
const assert = require('node:assert/strict');
const { createDataCodecBrowserFiles } = require('../../Platform/Wasm/datacodec_browser_files.js');

test('file registry preserves range data and owns release independently of the page', async () => {
    const reads = [];
    const released = [];
    const files = createDataCodecBrowserFiles({
        onRead: (...args) => reads.push(args),
        onRelease: id => released.push(id),
    });
    const id = files.register(new Blob([new Uint8Array([11, 22, 33, 44])]));
    assert.deepEqual(await files.read(id, 1, 2), new Uint8Array([22, 33]));
    assert.deepEqual(reads, [[id, 1, 2]]);
    files.release(id);
    files.release(id);
    assert.deepEqual(released, [id]);
    await assert.rejects(files.read(id, 0, 1), /unavailable/);
    assert.match(files.lastError(), /unavailable/);
});

test('range validation and short reads fail without invoking page observers', async () => {
    let observed = false;
    const files = createDataCodecBrowserFiles({ onRead: () => { observed = true; } });
    const id = files.register(new Blob([new Uint8Array([1, 2])]));
    for (const [offset, count] of [[-1, 1], [1, 2], [0, 1048577], [0.5, 1]]) {
        await assert.rejects(files.read(id, offset, count), /invalid browser file range/);
    }
    const short = files.register({ size: 2, slice: () => new Blob([new Uint8Array([1])]) });
    await assert.rejects(files.read(short, 0, 2), /expected 2/);
    assert.equal(observed, false);
});

test('observer failures and another Module cannot corrupt file ownership', async () => {
    const first = createDataCodecBrowserFiles({ onRead: () => { throw new Error('observer'); } });
    const second = createDataCodecBrowserFiles();
    const id = first.register(new Blob([new Uint8Array([7])]));
    await assert.rejects(second.read(id, 0, 1), /unavailable/);
    assert.deepEqual(await first.read(id, 0, 1), new Uint8Array([7]));
    assert.equal(first.observerFailures(), 1);
    assert.equal(first.lastError(), '');
});

test('an admitted read retains its File until completion after release', async () => {
    let finish;
    const files = createDataCodecBrowserFiles();
    const id = files.register({ size: 1, slice: () => ({
        arrayBuffer: () => new Promise(resolve => { finish = resolve; }),
    }) });
    const pending = files.read(id, 0, 1);
    files.release(id);
    finish(new Uint8Array([9]).buffer);
    assert.deepEqual(await pending, new Uint8Array([9]));
    await assert.rejects(files.read(id, 0, 1), /unavailable/);
});
