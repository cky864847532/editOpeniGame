const test = require('node:test');
const assert = require('node:assert/strict');
const { createCodecResourceControls } = require('./codec_resources.js');

function fixture(analyze = async () => ({ success: true, minimumBytes: '1048577' })) {
    const elements = Object.fromEntries(['codecResourceMode', 'codecMemoryMiB', 'codecMemoryRow',
        'codecComputeThreads', 'btnDecodeIgc'].map(id => [id, {
            value: '', style: {}, attributes: {},
            setAttribute(key, value) { this.attributes[key] = value; },
        }]));
    elements.codecResourceMode.value = '0';
    elements.codecComputeThreads.value = '0';
    const messages = [];
    const configurations = [];
    const controls = createCodecResourceControls({
        document: { getElementById: id => elements[id] },
        defaults: { fixedMemoryBytes: '1048576', maximumComputeThreads: 6, maximumMemoryBytes: '17179869184' },
        log: message => messages.push(message),
        configure: value => configurations.push(value), analyze, isBusy: () => false,
    });
    return { controls, elements, messages, configurations };
}

test('default capacity, exact rounding, correction and unlimited', async () => {
    const { controls, elements, messages, configurations } = fixture();
    assert.equal(await controls.check(false), true);
    assert.match(messages.at(-1), /尚未检查/);
    controls.selectFile({ name: 'sample.igc' });
    assert.equal(await controls.check(false), false);
    assert.equal(elements.codecMemoryMiB.attributes['aria-invalid'], 'true');
    assert.match(messages.at(-1), /至少设置 2 MiB（1048577 字节）/);
    assert.equal(elements.btnDecodeIgc.disabled, true);
    elements.codecMemoryMiB.value = '2';
    controls.invalidate();
    assert.equal(await controls.check(false), true);
    assert.equal(elements.codecMemoryMiB.style.color, '');
    controls.apply();
    assert.deepEqual(configurations.at(-1), { mode: 0, threads: 0, bytes: '2097152' });
    elements.codecResourceMode.value = '2';
    elements.codecMemoryMiB.value = 'bad';
    assert.equal(await controls.check(false), true);
    controls.apply();
    assert.deepEqual(configurations.at(-1), { mode: 2, threads: 0, bytes: '' });
    assert.equal(elements.codecMemoryRow.hidden, true);
});

test('zero, uint64 overflow and the runtime thread ceiling', async () => {
    const { controls, elements, messages } = fixture();
    controls.selectFile({});
    for (const value of ['-1', '1.5', '17592186044416', '16385']) {
        elements.codecMemoryMiB.value = value;
        assert.equal(await controls.check(false), false);
        assert.equal(elements.codecMemoryMiB.attributes['aria-invalid'], 'true');
    }
    elements.codecMemoryMiB.value = '0';
    assert.equal(await controls.check(false), false);
    assert.match(messages.at(-1), /上限不足/);
    elements.codecComputeThreads.value = '7';
    assert.equal(await controls.check(false), false);
    assert.match(messages.at(-1), /0 至 6/);
});

test('file and topology mode invalidate the analysis cache', async () => {
    let calls = 0;
    const { controls, elements } = fixture(async () => { ++calls; return { success: true, minimumBytes: '1' }; });
    elements.codecMemoryMiB.value = '1';
    controls.selectFile({});
    await controls.check(false);
    await controls.check(false);
    assert.equal(calls, 1);
    await controls.check(true);
    controls.selectFile({});
    await controls.check(true);
    assert.equal(calls, 3);
});

test('late analysis cannot reject unlimited mode or a corrected limit', async () => {
    let release;
    const { controls, elements } = fixture(() => new Promise(resolve => { release = resolve; }));
    controls.selectFile({});
    const pending = controls.check(false);
    elements.codecResourceMode.value = '2';
    controls.invalidate();
    await controls.check(false);
    release({ success: true, minimumBytes: '999999999' });
    await pending;
    assert.equal(elements.codecMemoryMiB.attributes['aria-invalid'], 'false');
    assert.equal(elements.btnDecodeIgc.disabled, false);
});

test('analysis failure provides no invented minimum and can be retried', async () => {
    let success = false;
    const { controls, elements, messages } = fixture(async () => success
        ? { success: true, minimumBytes: '0' } : { success: false, detail: 'missing reference' });
    controls.selectFile({});
    assert.equal(await controls.check(false), false);
    assert.match(messages.at(-1), /missing reference/);
    assert.equal(elements.codecMemoryMiB.attributes['aria-invalid'], 'false');
    success = true;
    assert.equal(await controls.check(false), true);
});
