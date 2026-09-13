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

// 页面默认取消内存与计算线程额度，具体分配交给核心及运行时
function createCodecResourceControls({ document, configure, isBusy, messages }) {
    const decode = document.getElementById('btnDecodeIgc');
    let file = null;
    function refresh() {
        decode.disabled = !file || isBusy();
    }
    refresh();
    return {
        apply() {
            try { configure({ mode: 2, threadMode: 2, threads: 0, bytes: '' }); }
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
