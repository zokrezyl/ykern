# ykern_ysys — sysfs (/sys) transport.
add_library(ykern_ysys STATIC
    ysys.c
)

target_include_directories(ykern_ysys PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ykern_ysys PUBLIC ykern_ycore)
