cmake_minimum_required(VERSION 3.16)

# 在 ESP-IDF 里被当作组件编译（EXTRA_COMPONENT_DIRS 指过来）
if(COMMAND idf_component_register)
  idf_component_register(SRCS src/json_util.cc src/music_queue.cc src/mcp.cc src/state_machine.cc
                         INCLUDE_DIRS include)
  return()
endif()

project(xz_core CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XZ_SRC src/json_util.cc src/music_queue.cc src/mcp.cc src/state_machine.cc)

add_library(xz_core STATIC ${XZ_SRC})
target_include_directories(xz_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(EMSCRIPTEN)
  add_executable(xz_wasm src/wasm_api.cc src/json_util.cc src/music_queue.cc src/mcp.cc src/state_machine.cc)
  target_include_directories(xz_wasm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  set_target_properties(xz_wasm PROPERTIES OUTPUT_NAME "xz_core")
  target_link_options(xz_wasm PRIVATE
    "-sEXPORTED_FUNCTIONS=['_xz_tools_json','_xz_mcp_handle','_xz_music_state','_xz_music_set_index','_xz_queue_snapshot','_xz_device_event','_xz_device_reset']"
    "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','stringToUTF8','lengthBytesUTF8','ccall','cwrap']"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createXZCore"
    "-sENVIRONMENT=web,node"
    "--no-entry")
else()
  add_executable(xz_core_tests tests/tests.cc)
  target_link_libraries(xz_core_tests PRIVATE xz_core)
  add_executable(xz_dump_tools src/dump_tools.cc)
  target_link_libraries(xz_dump_tools PRIVATE xz_core)
  add_executable(xz_scenarios tests/scenarios_test.cc)
  target_link_libraries(xz_scenarios PRIVATE xz_core)
  enable_testing()
  add_test(NAME xz_core_tests COMMAND xz_core_tests)
  add_test(NAME xz_scenarios COMMAND xz_scenarios ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/device_scenarios.tsv)
endif()
