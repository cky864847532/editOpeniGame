const test = require('node:test');
const assert = require('node:assert/strict');
const { createCodecResourceControls, createCodecHostMessages } = require('./codec_resources.js');

function fixture(maximumComputeThreads = 16, configureFailure = null) {
    const decode = { disabled: false };
    const configurations = [];
    const statuses = [];
    let busy = false;
    const controls = createCodecResourceControls({
        document: { getElementById(id) { assert.equal(id, 'btnDecodeIgc'); return decode; } },
        defaults: { maximumComputeThreads },
        configure: value => { if (configureFailure) { throw configureFailure; } configurations.push(value); },
        messages: { submit: (...args) => statuses.push(args) },
        isBusy: () => busy,
    });
    return { controls, decode, configurations, statuses, setBusy(value) { busy = value; } };
}

test('default to unlimited memory and threads independently of device core count', () => {
    for (const threads of [1, 6, 16, 32]) {
        const { controls, configurations } = fixture(threads);
        controls.apply();
        assert.deepEqual(configurations, [{ mode: 2, threadMode: 2, threads: 0, bytes: '' }]);
    }
});

test('file selection and busy state control the decode button without resource inputs', () => {
    const { controls, decode, setBusy } = fixture();
    assert.equal(decode.disabled, true);
    assert.equal(controls.selectedFile(), null);
    const file = { name: 'sample.igc' };
    controls.selectFile(file);
    assert.equal(controls.selectedFile(), file);
    assert.equal(decode.disabled, false);
    setBusy(true);
    controls.refresh();
    assert.equal(decode.disabled, true);
    setBusy(false);
    controls.refresh();
    assert.equal(decode.disabled, false);
    controls.selectFile(null);
    assert.equal(decode.disabled, true);
});

test('unlimited mode does not require a device thread count', () => {
    for (const count of [undefined, 0, -1, 1.5, NaN]) {
        const statuses = [];
        const configurations = [];
        const controls = createCodecResourceControls({
            document: { getElementById: () => ({}) },
            defaults: { maximumComputeThreads: count },
            configure(value) { configurations.push(value); },
            isBusy: () => false,
            messages: { submit: (...args) => statuses.push(args) },
        });
        controls.apply();
        assert.deepEqual(configurations, [{ mode: 2, threadMode: 2, threads: 0, bytes: '' }]);
        assert.deepEqual(statuses, []);
    }
});

test('configuration rejection uses the host message and preserves the original failure', () => {
    const failure = new Error('invalid memory limit');
    const { controls, statuses, configurations } = fixture(16, failure);
    assert.throws(() => controls.apply(), error => error === failure);
    assert.deepEqual(statuses, [['ResourceConfigurationFailed', {}, 'error', 'invalid memory limit']]);
    assert.deepEqual(configurations, []);
});

test('host messages preserve language, severity and diagnostic detail with one-pass interpolation', () => {
    for (const language of ['en', 'zh-CN']) {
        const statuses = [];
        const messages = createCodecHostMessages({ language, templates: {
            sample: language === 'en' ? 'Threads: {threads}; {name}' : '线程：{threads}；{name}',
        } }, status => statuses.push(status));
        const status = messages.submit('sample', { threads: 16, name: '{threads}' }, 'warning', 'offset=42');
        assert.equal(status.text, language === 'en' ? 'Threads: 16; {threads}' : '线程：16；{threads}');
        assert.equal(status.language, language);
        assert.equal(status.severity, 'warning');
        assert.equal(status.messageId, 'sample');
        assert.equal(status.technicalDetail, 'offset=42');
        assert.deepEqual(statuses, [status]);
    }
});
