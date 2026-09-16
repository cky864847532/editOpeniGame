cmake_minimum_required(VERSION 3.19)

project(iGameWeb LANGUAGES C CXX)

if (NOT EMSCRIPTEN)
    message(FATAL_ERROR "Examples/Wasm must be built with Emscripten")
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

add_executable(iGameWeb main_wasm.cpp
    Codec/iGameWasmDataCodecBridge.cpp
    Codec/iGameWasmDataCodecTiming.cpp
    Codec/iGameWasmDecodedModelRegistry.cpp)
target_include_directories(iGameWeb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(iGameWeb PUBLIC iGameCore)
datacodec_configure_wasm_executable(iGameWeb)

target_compile_options(iGameCore PRIVATE
    "$<$<CONFIG:Release>:-flto>"
    "$<$<CONFIG:Release>:-msimd128>"
)
target_compile_options(iGameWeb PRIVATE
    "$<$<CONFIG:Release>:-flto>"
    "$<$<CONFIG:Release>:-msimd128>"
)
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(IGAME_WASM_ASSERTIONS 0)
else()
    set(IGAME_WASM_ASSERTIONS 1)
endif()

set(IGAME_WASM_BUILD_ID "web-surface-prebuild-v2" CACHE STRING "Wasm web asset build identity")

if (IGAME_WASM_MEMORY_PROFILE STREQUAL "memory-4g")
    set(IGAME_WASM_OUTPUT_NAME "iGameWeb_32")
    set(IGAME_WASM_JS_NAME "iGameWeb_32.js")
    set(IGAME_WASM_WASM_NAME "iGameWeb_32.wasm")
elseif (IGAME_WASM_MEMORY_PROFILE STREQUAL "memory-16g")
    set(IGAME_WASM_OUTPUT_NAME "iGameWeb_64")
    set(IGAME_WASM_JS_NAME "iGameWeb_64.js")
    set(IGAME_WASM_WASM_NAME "iGameWeb_64.wasm")
else()
    message(FATAL_ERROR "Unknown IGAME_WASM_MEMORY_PROFILE: ${IGAME_WASM_MEMORY_PROFILE}")
endif ()

target_compile_definitions(iGameWeb PRIVATE
    "IGAME_WASM_BUILD_ID=\"${IGAME_WASM_BUILD_ID}\""
    "IGAME_WASM_MEMORY_PROFILE_NAME=\"${IGAME_WASM_MEMORY_PROFILE}\""
)

target_link_options(iGameWeb PRIVATE
    "SHELL:-s USE_GLFW=3"
    "SHELL:-s FULL_ES2=1"
    "SHELL:-lembind"
    "SHELL:-s ASSERTIONS=${IGAME_WASM_ASSERTIONS}"
    "SHELL:--embed-file ${CMAKE_SOURCE_DIR}/iGameCore/Rendering/Shaders/GLSL_Web@/Resources/Shaders"
    "SHELL:--embed-file ${CMAKE_SOURCE_DIR}/iGameCore/Rendering/Assests/Fonts/SourceHanSansCN-Normal.otf@/Resources/Assests/Fonts/SourceHanSansCN-Normal.otf"
)

set_target_properties(iGameWeb PROPERTIES
    OUTPUT_NAME "${IGAME_WASM_OUTPUT_NAME}"
)

if (IGAME_WASM_MEMORY_PROFILE STREQUAL "memory-16g")
    add_custom_command(TARGET iGameWeb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f
                $<TARGET_FILE_DIR:iGameWeb>/iGameWeb.js
        COMMAND ${CMAKE_COMMAND} -E rm -f
                $<TARGET_FILE_DIR:iGameWeb>/iGameWeb.wasm
    )
endif ()

set(IGAME_WASM_HTML_ASSETS
    index.html
    cache.js
    codec_resources.js
    serve_wasm.py
)

foreach (asset IN LISTS IGAME_WASM_HTML_ASSETS)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${asset}
                   ${CMAKE_CURRENT_BINARY_DIR}/${asset}
                   @ONLY)
endforeach ()
