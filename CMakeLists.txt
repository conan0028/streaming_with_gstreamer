cmake_minimum_required(VERSION 3.16)

project(StreamingGStreamer
        VERSION 1.0
        DESCRIPTION "CMP1103 - Streaming de audio RTP/UDP com GStreamer"
        LANGUAGES C)

# ==========================================
# Padrão da linguagem
# ==========================================
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Gera compile_commands.json (indexação/autocompletar no CLion e outros IDEs)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build padrão quando nenhum tipo é informado
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Tipo de build" FORCE)
endif()

# ==========================================
# Dependências: GStreamer 1.0 (via pkg-config)
# gstreamer-1.0 já traz glib-2.0/gobject-2.0 como dependências transitivas
# (inclui glib-unix.h, usado para tratar Ctrl+C).
# ==========================================
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.16)

message(STATUS "GStreamer encontrado: ${GST_VERSION}")

# ==========================================
# Função auxiliar para criar cada executável com a mesma configuração
# ==========================================
function(add_streaming_executable target source)
    add_executable(${target} ${source} streaming_config.h)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE PkgConfig::GST)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endfunction()

# ==========================================
# Executáveis
# ==========================================
add_streaming_executable(transmissor transmissor.c)
add_streaming_executable(receptor    receptor.c)