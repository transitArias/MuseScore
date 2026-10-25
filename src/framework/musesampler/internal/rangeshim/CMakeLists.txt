# Experimental (built with Claude): extends Muse Sounds instruments past their
# sampled range by routing out-of-range notes to a transposed second engine
# instance and pitch-shifting its output (Signalsmith Stretch, MIT).
# Built as a plain static library: no precompiled headers / unity build, since it
# uses its own copy of the MuseSampler API types.

add_library(muse_rangeshim STATIC rangeshim.cpp common.h msapi.h probe_core.h)
set_target_properties(muse_rangeshim PROPERTIES UNITY_BUILD OFF CXX_STANDARD 17)
target_include_directories(muse_rangeshim PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/thirdparty/stretch/include
    ${CMAKE_CURRENT_LIST_DIR}/thirdparty/linear/include)
if (WIN32)
    target_compile_definitions(muse_rangeshim PRIVATE MSR_INTREE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if (MSVC)
        target_compile_options(muse_rangeshim PRIVATE /utf-8 /EHsc)
    endif()
else()
    target_compile_definitions(muse_rangeshim PRIVATE MSR_STUB)
endif()
