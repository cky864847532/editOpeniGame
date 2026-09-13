include_guard(GLOBAL)

# 在第三方和核心目标创建前调用，统一所有翻译单元的线程与地址宽度 ABI
macro(datacodec_initialize_wasm memory_profile pthread_pool_size compute_workers)
    set(DATACODEC_WASM_MEMORY_PROFILE "${memory_profile}")
    set(DATACODEC_WASM_PTHREAD_POOL_SIZE "${pthread_pool_size}")
    set(DATACODEC_WASM_COMPUTE_WORKERS "${compute_workers}")
    foreach(value IN ITEMS DATACODEC_WASM_PTHREAD_POOL_SIZE DATACODEC_WASM_COMPUTE_WORKERS)
        if(NOT "${${value}}" MATCHES "^(0|[1-9][0-9]*)$")
            message(FATAL_ERROR "${value} must be a non-negative integer")
        endif()
    endforeach()
    if(NOT DATACODEC_WASM_MEMORY_PROFILE MATCHES "^memory-(4g|16g)$")
        message(FATAL_ERROR "Unknown DataCodec WASM memory profile: ${DATACODEC_WASM_MEMORY_PROFILE}")
    endif()
    # 一个宿主位置及一个请求 driver，与 ResourceComputeCapacity 的扣除规则一致
    set(DATACODEC_WASM_HOST_THREADS 1)
    set(DATACODEC_WASM_POOL_OVERHEAD 2)
    add_compile_options(-pthread "$<$<COMPILE_LANGUAGE:CXX>:-fexceptions>")
    add_link_options(-pthread -fexceptions)
    if(DATACODEC_WASM_MEMORY_PROFILE STREQUAL "memory-16g")
        add_compile_options(-sMEMORY64=1)
    endif()
endmacro()

function(datacodec_configure_wasm_core target)
    if(NOT DEFINED DATACODEC_WASM_MEMORY_PROFILE)
        message(FATAL_ERROR "Call datacodec_initialize_wasm before creating DataCodec targets")
    endif()
    target_compile_definitions(${target} PRIVATE
        DATACODEC_MAX_PARALLEL_WORKERS=${DATACODEC_WASM_COMPUTE_WORKERS}
        DATACODEC_RUNTIME_THREAD_LIMIT=${DATACODEC_WASM_PTHREAD_POOL_SIZE}
        DATACODEC_WASM_HOST_THREADS=${DATACODEC_WASM_HOST_THREADS}
        DATACODEC_WASM_POOL_OVERHEAD=${DATACODEC_WASM_POOL_OVERHEAD})
    target_compile_options(${target} PRIVATE
        "$<$<CONFIG:Release>:-flto>" "$<$<CONFIG:Release>:-msimd128>")
endfunction()

# 最终程序统一承担浏览器运行时配置，场景渲染与 embind 导出由宿主补充
function(datacodec_configure_wasm_executable target)
    set(pool_expression "${DATACODEC_WASM_PTHREAD_POOL_SIZE}")
    if(pool_expression STREQUAL "0")
        set(pool_expression "(navigator.hardwareConcurrency||1)+${DATACODEC_WASM_POOL_OVERHEAD}")
    endif()
    target_link_options(${target} PRIVATE
        "SHELL:-s WASM=1" "SHELL:-s ASYNCIFY=1"
        "SHELL:-s ASYNCIFY_STACK_SIZE=1048576"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=ccall"
        "SHELL:-s PTHREAD_POOL_SIZE=${pool_expression}"
        "SHELL:-s PTHREAD_POOL_SIZE_STRICT=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1" "SHELL:-s ABORTING_MALLOC=0"
        "SHELL:-s STACK_SIZE=1048576" "SHELL:-s MALLOC=dlmalloc"
        "SHELL:-s FORCE_FILESYSTEM=1"
        "$<$<CONFIG:Release>:-flto>" "$<$<CONFIG:Release>:-msimd128>")
    if(DATACODEC_WASM_MEMORY_PROFILE STREQUAL "memory-16g")
        target_link_options(${target} PRIVATE
            "SHELL:-s INITIAL_MEMORY=2147483648" "SHELL:-s MAXIMUM_MEMORY=17179869184"
            "SHELL:-s MEMORY64=1" "SHELL:-s WASMFS=1")
    else()
        target_link_options(${target} PRIVATE
            "SHELL:-s INITIAL_MEMORY=1073741824" "SHELL:-s MAXIMUM_MEMORY=4294967296")
    endif()
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../Platform/Wasm/datacodec_browser_files.js"
        "${CMAKE_CURRENT_BINARY_DIR}/datacodec_browser_files.js" COPYONLY)
endfunction()
