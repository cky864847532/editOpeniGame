// 参数只作用于下次请求，容量检查复用 DataCodec 的解码分析接口
function createCodecResourceControls({ document, defaults, log, configure, analyze, isBusy }) {
    const mode = document.getElementById('codecResourceMode');
    const memory = document.getElementById('codecMemoryMiB');
    const memoryRow = document.getElementById('codecMemoryRow');
    const threads = document.getElementById('codecComputeThreads');
    const decode = document.getElementById('btnDecodeIgc');
    const MiB = 1048576n;
    const maximumBytes = (1n << 64n) - 1n;
    let file = null;
    let cache = null;
    let generation = 0;
    let checking = false;
    let rejected = false;
    let configValid = true;
    let lastMessage = '';

    threads.max = String(defaults.maximumComputeThreads);
    threads.title = `0 表示设备默认；当前最多 ${defaults.maximumComputeThreads} 个计算线程`;
    memory.placeholder = `设备默认 ${BigInt(defaults.fixedMemoryBytes) / MiB} MiB`;

    function report(text) {
        if (text !== lastMessage) { log(`[内存检查] ${text}`); lastMessage = text; }
    }
    function markInvalid(invalid) {
        memory.style.color = invalid ? '#ff6868' : '';
        memory.setAttribute('aria-invalid', String(invalid));
    }
    function refresh() {
        const fixed = mode.value === '0';
        memory.disabled = !fixed;
        memoryRow.hidden = !fixed;
        decode.disabled = !file || checking || rejected || !configValid || isBusy();
    }
    function readParams() {
        if (mode.value !== '0' && mode.value !== '2') { throw new Error('请选择有效的内存模式'); }
        const count = Number(threads.value);
        const validThreads = Number.isInteger(count) && count >= 0 && count <= defaults.maximumComputeThreads;
        threads.setAttribute('aria-invalid', String(!validThreads));
        if (!validThreads) { throw new Error(`计算线程上限应为 0 至 ${defaults.maximumComputeThreads} 的整数，0 表示设备默认`); }
        const text = mode.value === '0' ? memory.value.trim() : '';
        if (text && (!/^\d+$/.test(text) || BigInt(text) > maximumBytes / MiB)) {
            markInvalid(true);
            throw new Error('内存上限应为有效的非负整数 MiB；留空使用设备默认值');
        }
        if (text && defaults.maximumMemoryBytes && BigInt(text) * MiB > BigInt(defaults.maximumMemoryBytes)) {
            markInvalid(true);
            throw new Error(`固定内存上限不能超过当前 WASM 的 ${BigInt(defaults.maximumMemoryBytes) / MiB} MiB 容量`);
        }
        return { mode: Number(mode.value), threads: count, bytes: text ? (BigInt(text) * MiB).toString() : '' };
    }
    async function check(enableReuseCache, force = false) {
        const serial = ++generation;
        rejected = false;
        markInvalid(false);
        let params;
        try { params = readParams(); configValid = true; }
        catch (error) { configValid = false; report(error.message); refresh(); return false; }
        if (params.mode === 2) { report('内存不设上限；实际分配由运行环境决定'); refresh(); return true; }
        if (!file) { report('尚未检查容量：请先选择待解压 IGC 文件'); refresh(); return true; }
        const selected = file;
        const key = Boolean(enableReuseCache);
        if (force || !cache || cache.file !== selected || cache.key !== key) {
            if (checking || isBusy()) { report('当前任务完成后可重新检查容量'); refresh(); return false; }
            checking = true;
            refresh();
            report('正在检查所选文件的首次解码容量…');
            try {
                const result = await analyze(selected, key);
                if (result.success) { cache = { file: selected, key, minimum: BigInt(result.minimumBytes) }; }
                else { throw new Error(result.detail || '未取得容量分析结果'); }
            } catch (error) {
                if (serial === generation) { report(`容量检查未完成：${error.message}`); }
                return false;
            } finally { checking = false; refresh(); }
        }
        // 文件和参数变化后重新使用最新参数，过期结果不能标红新的输入
        if (serial !== generation) { return check(enableReuseCache); }
        const limit = BigInt(params.bytes || defaults.fixedMemoryBytes);
        const minimum = cache.minimum;
        const roundedMiB = (minimum + MiB - 1n) / MiB;
        rejected = limit < minimum;
        markInvalid(rejected);
        report(rejected
            ? `内存上限不足：至少设置 ${roundedMiB} MiB（${minimum} 字节），或选择内存不设上限。修正后点击“解压所选 IGC”`
            : `当前额度未低于首次解码门槛 ${roundedMiB} MiB（${minimum} 字节）；按需属性、宿主输出和运行环境的内存另行申请`);
        refresh();
        return !rejected;
    }
    function apply() {
        const params = readParams();
        if (rejected) { throw new Error('请先修正内存上限或选择内存不设上限'); }
        configure(params);
    }
    function invalidate() {
        ++generation;
        rejected = false;
        configValid = true;
        markInvalid(false);
        refresh();
    }
    refresh();
    return {
        check, apply, refresh, invalidate,
        selectFile(nextFile) { file = nextFile; cache = null; invalidate(); },
        selectedFile() { return file; },
    };
}

if (typeof module !== 'undefined' && module.exports) { module.exports = { createCodecResourceControls }; }
