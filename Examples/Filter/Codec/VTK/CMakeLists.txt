if (NOT TARGET VTK::CommonCore)
    find_package(VTK 9 CONFIG REQUIRED COMPONENTS
            CommonCore
            CommonDataModel
            IOLegacy
            IOXML
    )
endif ()

set(IGAME_DATACODEC_VTK_MODULES
        VTK::CommonCore
        VTK::CommonDataModel
        VTK::IOLegacy
        VTK::IOXML
)

add_library(dataCodecVtkAdapter STATIC
        VtkDataCodecAdapters.cpp
        VtkDataCodecAdapters.h
        VtkUnstructuredGridIO.cpp
        VtkUnstructuredGridIO.h
)
target_include_directories(dataCodecVtkAdapter PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(dataCodecVtkAdapter PUBLIC
        DataCodecCore
        VTK::CommonCore
        VTK::CommonDataModel
        VTK::IOLegacy
        VTK::IOXML
)

vtk_module_autoinit(
        TARGETS dataCodecVtkAdapter
        MODULES ${IGAME_DATACODEC_VTK_MODULES}
)

if(IGAME_DATACODEC_BUILD_TESTS)
    add_executable(DataCodecVtkOwnershipTests TestVtkOwnedDecode.cpp)
    target_link_libraries(DataCodecVtkOwnershipTests PRIVATE dataCodecVtkAdapter)
    add_test(NAME DataCodec.VtkOwnership COMMAND DataCodecVtkOwnershipTests)
    set_tests_properties(DataCodec.VtkOwnership PROPERTIES TIMEOUT 120)
endif()
