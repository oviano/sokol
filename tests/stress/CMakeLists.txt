# ----------------------------------------------------------------------------------------------------
# PROJECT
# ----------------------------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)
project(stress-test)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ----------------------------------------------------------------------------------------------------
# D3D12 STRESS TEST EXECUTABLE
# ----------------------------------------------------------------------------------------------------

add_executable(
    stress-test-d3d12
        "stress_test.cpp"
)

target_compile_definitions(
    stress-test-d3d12
        PRIVATE
            SOKOL_D3D12
            $<$<CONFIG:Debug>:_DEBUG>
)

if(MSVC)
    target_compile_options(
        stress-test-d3d12
            PRIVATE
                /W4
                /WX-
                /D_CRT_SECURE_NO_WARNINGS
                $<$<CONFIG:Release>:/Zi>
    )
    target_link_options(
        stress-test-d3d12
            PRIVATE
                /SUBSYSTEM:WINDOWS
                /ENTRY:mainCRTStartup
                $<$<CONFIG:Release>:/DEBUG>
    )
endif()

target_link_libraries(
    stress-test-d3d12
        PRIVATE
            d3d12
            dxgi
            d3dcompiler
)

# ----------------------------------------------------------------------------------------------------
# D3D11 STRESS TEST EXECUTABLE
# ----------------------------------------------------------------------------------------------------

add_executable(
    stress-test-d3d11
        "stress_test.cpp"
)

target_compile_definitions(
    stress-test-d3d11
        PRIVATE
            SOKOL_D3D11
            $<$<CONFIG:Debug>:_DEBUG>
)

if(MSVC)
    target_compile_options(
        stress-test-d3d11
            PRIVATE
                /W4
                /WX-
                /D_CRT_SECURE_NO_WARNINGS
                $<$<CONFIG:Release>:/Zi>
    )
    target_link_options(
        stress-test-d3d11
            PRIVATE
                /SUBSYSTEM:WINDOWS
                /ENTRY:mainCRTStartup
                $<$<CONFIG:Release>:/DEBUG>
    )
endif()

target_link_libraries(
    stress-test-d3d11
        PRIVATE
            d3d11
            dxgi
            d3dcompiler
)
