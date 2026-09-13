cmake_minimum_required(VERSION 3.19)

# 核心仅收集宿主无关模块，Filter 桥接由 iGameCore 自行编译
set(DATACODEC_CORE_MODULES API Common Codec Localization Log Platform Runtime Storage Validation Workflow)
set(DATACODEC_CORE_SOURCES)
foreach(module IN LISTS DATACODEC_CORE_MODULES)
    file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.cpp")
    list(APPEND DATACODEC_CORE_SOURCES ${module_sources})
endforeach()

include(Cmake/CheckDataCodecDependencies.cmake)
datacodec_check_dependencies("${CMAKE_CURRENT_SOURCE_DIR}")

add_library(DataCodecCore STATIC ${DATACODEC_CORE_SOURCES})
target_compile_features(DataCodecCore PUBLIC cxx_std_20)
set_target_properties(DataCodecCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(DataCodecCore PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
    "$<INSTALL_INTERFACE:include/Filters>")

# 压缩与序列化依赖属于核心，网格、渲染与 UI 依赖留给宿主
foreach(dependency IN ITEMS libzstd_static cereal libpressio)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR "DataCodecCore requires dependency target ${dependency}")
    endif()
    target_link_libraries(DataCodecCore PUBLIC "$<BUILD_INTERFACE:${dependency}>")
endforeach()
find_package(Threads REQUIRED)
target_link_libraries(DataCodecCore PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(DataCodecCore PRIVATE pdh psapi)
endif()
if(EMSCRIPTEN)
    datacodec_configure_wasm_core(DataCodecCore)
endif()

if(IGAME_DATACODEC_BUILD_TESTS)
    add_executable(DataCodecCoreTests Test/Runner/DataCodecCoreTests.cpp)
    target_link_libraries(DataCodecCoreTests PRIVATE DataCodecCore)
    add_test(NAME DataCodec.CoreBoundary COMMAND DataCodecCoreTests)
    set_tests_properties(DataCodec.CoreBoundary PROPERTIES TIMEOUT 120)
endif()
