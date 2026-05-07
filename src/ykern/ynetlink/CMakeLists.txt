# ykern_ynetlink — netlink + generic netlink transport
add_library(ykern_ynetlink STATIC
    ynetlink.c
    genl-namespace.c
    genl-family.c
    genl-operation.c
    genl-mcast-group.c
    genl-overlay.c
    genl-attribute.c
    rtnl.c
)

target_include_directories(ykern_ynetlink PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ykern_ynetlink PUBLIC ykern_ycore)

# libyaml — required for the per-family YAML overlay loader.
find_package(PkgConfig REQUIRED)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)
target_link_libraries(ykern_ynetlink PRIVATE PkgConfig::YAML)

# Tell genl-overlay.c where to look for metadata files at runtime when
# YKERN_METADATA_DIR is not set. Source-tree path is fine for development;
# packagers can override at configure time.
target_compile_definitions(ykern_ynetlink PRIVATE
    YKERN_METADATA_DIR="${CMAKE_SOURCE_DIR}/metadata"
)
