'use strict';

// 每个 WASM Module 持有独立文件表，页面只通过登记和释放接口管理 File 生命周期
// 每个输入描述保留一个引用，最后引用释放后移除登记，在途读取保持自己的 File 引用
function createDataCodecBrowserFiles({ onRead, onRelease } = {}) {
    const files = new Map();
    let nextId = 1;
    let lastError = '';
    let observerFailures = 0;
    const observe = (callback, ...args) => {
        if (typeof callback !== 'function') return;
        try { callback(...args); } catch (_) { ++observerFailures; }
    };
    return Object.freeze({
        register(file) {
            if (!file || typeof file.slice !== 'function' ||
                !Number.isSafeInteger(file.size) || file.size <= 0) {
                throw new Error('browser file requires a positive safe integer size and slice support');
            }
            if (nextId > 0xffffffff) throw new Error('browser file identifiers are exhausted');
            const id = nextId++;
            files.set(id, { file, references: 1 });
            lastError = '';
            return id;
        },
        async read(id, offset, count) {
            try {
                const file = files.get(id)?.file;
                if (!file) throw new Error(`browser file ${id} is unavailable`);
                if (!Number.isSafeInteger(offset) || offset < 0 ||
                    !Number.isSafeInteger(count) || count < 0 || count > 1048576 ||
                    offset > file.size || count > file.size - offset) {
                    throw new Error(`invalid browser file range offset=${offset} bytes=${count} size=${file.size}`);
                }
                const bytes = new Uint8Array(await file.slice(offset, offset + count).arrayBuffer());
                if (bytes.byteLength !== count) {
                    throw new Error(`browser file range returned ${bytes.byteLength} bytes, expected ${count}`);
                }
                observe(onRead, id, offset, count);
                lastError = '';
                return bytes;
            } catch (error) {
                lastError = error && error.message ? error.message : String(error);
                throw error;
            }
        },
        retain(id) {
            const entry = files.get(id);
            if (!entry || entry.references >= Number.MAX_SAFE_INTEGER) return false;
            ++entry.references;
            return true;
        },
        release(id) {
            const entry = files.get(id);
            if (entry && --entry.references === 0) { files.delete(id); observe(onRelease, id); }
        },
        recordError(detail) { lastError = String(detail); },
        lastError() { return lastError; },
        observerFailures() { return observerFailures; },
    });
}

if (typeof module !== 'undefined' && module.exports) {
    module.exports = { createDataCodecBrowserFiles };
}
