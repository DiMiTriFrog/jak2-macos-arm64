include(${CMAKE_CURRENT_LIST_DIR}/goalc/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/offline/CMakeLists.txt)

set(GOALC_TEST_AOT_FIXTURE ${CMAKE_CURRENT_LIST_DIR}/aot_fixture_fallback.cpp)
if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
  set(GOALC_TEST_AOT_FIXTURE ${CMAKE_CURRENT_LIST_DIR}/aot_fixture_arm64.S)
endif()

add_executable(goalc-test
        ${CMAKE_CURRENT_LIST_DIR}/emitter_util.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_main.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_reader.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_goos.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_listener_deci2.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_type_system.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_x86.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_avx.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_neon.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_arm64.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_arm64_integer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_arm64_blend_swizzle.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_arm64_shuffle.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_arm64_parity.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_relocation_model.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_object_generator_arm64.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_common_util.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_build_config.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_data_memory.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_aot_function.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_aot_data_template.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_aot_manifest.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_aot_assembly.cpp
        ${GOALC_TEST_AOT_FIXTURE}
        ${CMAKE_CURRENT_LIST_DIR}/test_pretty_print.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_math.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_zstd.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_zydis.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_AtomicOpBuilder.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_InstructionDecode.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_InstructionParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_DataParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_VuDisasm.cpp
        ${CMAKE_CURRENT_LIST_DIR}/common/formatter/test_formatter.cpp
        ${GOALC_TEST_FRAMEWORK_SOURCES}
        ${GOALC_TEST_CASES}
        )

target_link_libraries(goalc-test common runtime compiler gtest decomp Zydis libzstd_static tree-sitter)

add_executable(test_image_resize ${CMAKE_CURRENT_LIST_DIR}/common/test_image_resize.cpp)
target_link_libraries(test_image_resize common)

if(WIN32)
  target_link_libraries(goalc-test mman)
endif()

#gtest_discover_tests(goalc-test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

if(UNIX AND CMAKE_COMPILER_IS_GNUCXX AND CODE_COVERAGE)
  set(BUILD_ARGS "-w" "dupbuild=warn")
  include(CodeCoverage)
  append_coverage_compiler_flags()
  setup_target_for_coverage_lcov(NAME goalc-test_coverage
                                 EXECUTABLE goalc-test --gtest_color=yes
                                 DEPENDENCIES goalc-test
                                 EXCLUDE "third-party/*" "/usr/include/*")
endif()
