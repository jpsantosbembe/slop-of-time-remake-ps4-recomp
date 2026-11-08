# ocarina-ps4 / Fase 0B: bateria Vulkan. O mesmo código compila para Linux (validação) e para o PS4 (bundle orbis-sdk).
#   Linux: cmake -S bench -B build/bench-linux -G Ninja
#   PS4:   cmake -S bench -B build/bench-ps4 -G Ninja -DCMAKE_TOOLCHAIN_FILE=<bundle>/toolchain/orbis-sdk.cmake -DORBIS_KIT_DIR=<orbis-porting-kit>
cmake_minimum_required(VERSION 3.16)
project(ocarina_bench C)

find_program(GLSLANG NAMES glslangValidator glslang REQUIRED)
set(SHADERS geo.vert forward.frag gbuf.frag fullscreen.vert light.frag fog.comp bloom.comp taa.comp)
set(SPV_HEADERS)
foreach(s ${SHADERS})
  string(REPLACE "." "_" stem "${s}")
  set(out "${CMAKE_CURRENT_BINARY_DIR}/${stem}.h")
  add_custom_command(OUTPUT "${out}"
    COMMAND "${GLSLANG}" -V --target-env vulkan1.2 "${CMAKE_CURRENT_LIST_DIR}/shaders/${s}" --vn "${stem}" -o "${out}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/shaders/${s}"
    COMMENT "glslang: ${s}")
  list(APPEND SPV_HEADERS "${out}")
endforeach()

add_executable(bench bench_vk.c ${SPV_HEADERS})
target_include_directories(bench PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
# -DBENCH_NUM_SE=4: liga a chave ORBIS_NUM_SE do mesa-ps4 (o Pro tem 4 motores de shader; o driver fixa 2)
if(BENCH_NUM_SE)
  target_compile_definitions(bench PRIVATE BENCH_NUM_SE=${BENCH_NUM_SE} BENCH_NUM_SE_STR="${BENCH_NUM_SE}")
endif()

if(PS4)
  if(NOT ORBIS_KIT_DIR)
    message(FATAL_ERROR "PS4: passe -DORBIS_KIT_DIR=<checkout do orbis-porting-kit> (o vkloader vem dele)")
  endif()
  add_subdirectory("${ORBIS_KIT_DIR}/vkloader" ps4-vkloader)
  target_link_libraries(bench PRIVATE ps4-vkloader m)
  orbis_compat_verify()
  if(COMMAND ps4_create_eboot)
    ps4_create_eboot(bench)
  endif()
else()
  find_package(Vulkan REQUIRED)
  target_compile_options(bench PRIVATE -O2 -Wall -Wno-unused-function)
  target_link_libraries(bench PRIVATE Vulkan::Vulkan m)
endif()
