// 使用宿主目录提供的模板，正文与技术详情在交给展示层之前保持独立
function createCodecHostMessages(catalog, publish) {
    return {
        submit(messageId, args = {}, severity = 'info', technicalDetail = '') {
            const template = catalog.templates[messageId];
            if (typeof template !== 'string') { throw new Error(`Unknown codec host message: ${messageId}`); }
            const text = template.replace(/\{([^{}]+)\}/g, (placeholder, key) =>
                Object.prototype.hasOwnProperty.call(args, key) ? String(args[key]) : placeholder);
            const status = { language: catalog.language, messageId, severity, text, technicalDetail };
            publish(status);
            return status;
        },
    };
}

// 页面统一使用无内存上限和设备最大固定计算并发
function createCodecResourceControls({ document, defaults, configure, isBusy, messages }) {
    const decode = document.getElementById('btnDecodeIgc');
    const threads = defaults.maximumComputeThreads;
    if (!Number.isInteger(threads) || threads < 1) {
        messages.submit('DeviceComputeThreadsUnavailable', {}, 'error');
        throw new Error('invalid device compute thread capacity');
    }
    let file = null;
    function refresh() {
        decode.disabled = !file || isBusy();
    }
    refresh();
    return {
        apply() {
            try { configure({ mode: 2, threads, bytes: '' }); }
            catch (error) {
                messages.submit('ResourceConfigurationFailed', {}, 'error', error.message || String(error));
                throw error;
            }
        },
        refresh,
        selectFile(nextFile) { file = nextFile; refresh(); },
        selectedFile() { return file; },
    };
}

if (typeof module !== 'undefined' && module.exports) {
    module.exports = { createCodecResourceControls, createCodecHostMessages };
}
