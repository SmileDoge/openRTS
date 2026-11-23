# Builds renderer/shaders into the program: run as
#   cmake -DSHADER_ROOT=<renderer/shaders> -DOUTPUT=<file.inc> -P embed_shaders.cmake
# Writes `embedded_shaders`, every .wgsl file under SHADER_ROOT by its path
# there without the extension ("model/interface"), each as a zero-terminated
# byte array (unsigned: the comments are UTF-8; no string literal's length limit), carriage returns left out
# so a checkout's line endings do not change the shader.

file(GLOB_RECURSE shader_files RELATIVE "${SHADER_ROOT}" "${SHADER_ROOT}/*.wgsl")
list(SORT shader_files)

set(arrays "")
set(table "")
set(index 0)
foreach(relative IN LISTS shader_files)
    file(READ "${SHADER_ROOT}/${relative}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REPLACE "0x0d," "" bytes "${bytes}")
    string(REGEX REPLACE "\\.wgsl$" "" name "${relative}")
    string(APPEND arrays "constexpr unsigned char shader_${index}[] = {${bytes}0x00};\n")
    string(APPEND table "    {\"${name}\", reinterpret_cast<const char*>(shader_${index})},\n")
    math(EXPR index "${index} + 1")
endforeach()

set(content "// Made by cmake/embed_shaders.cmake from renderer/shaders: do not edit.\n${arrays}const EmbeddedShader embedded_shaders[] = {\n${table}};\n")
# Rewritten only when it changes, so an unchanged shader set builds nothing.
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" old)
    if(old STREQUAL content)
        return()
    endif()
endif()
file(WRITE "${OUTPUT}" "${content}")
