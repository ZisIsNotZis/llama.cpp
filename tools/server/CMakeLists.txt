include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# ncursesw for the interactive terminal dashboard (--tui)
find_package(PkgConfig)
pkg_check_modules(NCURSESW QUIET ncursesw)

# md4c (markdown parser, MIT) for the TUI styled-line rendering
find_library(MD4C_LIB NAMES md4c libmd4c)
if (NOT MD4C_LIB)
    find_library(MD4C_LIB NAMES libmd4c.so.0 PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /lib /lib/x86_64-linux-gnu)
endif()

# server-context containing the core server logic, used by llama-server and CLI

set(TARGET server-context)

add_library(${TARGET} STATIC
    server-chat.cpp
    server-chat.h
    server-task.cpp
    server-task.h
    server-queue.cpp
    server-queue.h
    server-common.cpp
    server-common.h
    server-context.cpp
    server-context.h
    server-stream.cpp
    server-stream.h
    server-tools.cpp
    server-tools.h
    server-mcp.cpp
    server-mcp.h
    server-schema.cpp
    server-schema.h
    server-dash.cpp
    server-dash.h
    nctui.cpp
    nctui.h
    markup.cpp
    markup.h
    printui.cpp
    printui.h
)

if (BUILD_SHARED_LIBS)
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(${TARGET} PRIVATE ../mtmd)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC llama-common mtmd ${CMAKE_THREAD_LIBS_INIT})
if (NCURSESW_FOUND)
    target_include_directories(${TARGET} PRIVATE ${NCURSESW_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PUBLIC ${NCURSESW_LIBRARIES})
endif()
if (MD4C_LIB)
    target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/md4c)
    target_link_libraries(${TARGET} PUBLIC ${MD4C_LIB})
endif()

# llama-server-impl: server logic, reusable by app

set(TARGET llama-server-impl)

add_library(${TARGET}
    server.cpp
    server-http.cpp
    server-http.h
    server-models.cpp
    server-models.h
)
set_target_properties(${TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET} PRIVATE ../mtmd ${CMAKE_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC server-context llama-ui cpp-httplib ${CMAKE_THREAD_LIBS_INIT})

if(LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} LIBRARY)
endif()

# llama-server executable

set(TARGET llama-server)

add_executable(${TARGET} main.cpp)
install(TARGETS ${TARGET} RUNTIME)

target_link_libraries(${TARGET} PRIVATE llama-server-impl)
target_compile_features(${TARGET} PRIVATE cxx_std_17)
