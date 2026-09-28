cmake_minimum_required(VERSION 3.20)
project(RVThereYetHeadTracking LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single source of truth for the mod version is pixi.toml. Parse it here and
# inject as RVTY_MOD_VERSION so the bootstrap log can print it - that string
# is what users paste into crash reports, so getting it from a real artifact
# (not a hand-maintained define) is what keeps reports accurate.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/pixi.toml" _pixi_toml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/pixi.toml")
if(_pixi_toml MATCHES "(^|\n)version[ \t]*=[ \t]*\"([^\"]+)\"")
    set(RVTY_MOD_VERSION "${CMAKE_MATCH_2}")
else()
    message(FATAL_ERROR "Could not parse version from pixi.toml")
endif()

# Short git SHA, captured at configure time. Lets two nightlies built off the
# same pixi.toml version be distinguished in crash reports. Appended "-dirty"
# if the working tree has uncommitted changes at configure time.
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE RVTY_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_sha_rc
    )
    if(NOT _git_sha_rc EQUAL 0)
        set(RVTY_GIT_SHA "unknown")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_dirty
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT _git_dirty STREQUAL "")
        set(RVTY_GIT_SHA "${RVTY_GIT_SHA}-dirty")
    endif()
else()
    set(RVTY_GIT_SHA "unknown")
endif()
message(STATUS "RVThereYetHeadTracking version: ${RVTY_MOD_VERSION} (${RVTY_GIT_SHA})")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MSVC)
    add_compile_options(/permissive- /Zc:__cplusplus /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

include(FetchContent)
# Pinned to the commit, not the tag: a tag can be repointed upstream, and the
# version this build claims in THIRD-PARTY-NOTICES.md has to be the version it
# actually compiles. 9fbd087 is release v1.3.3, dated 2017-01-07.
FetchContent_Declare(
    minhook_src
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        9fbd087432700d73fc571118d6a9697a36443d88
)
FetchContent_MakeAvailable(minhook_src)

add_library(minhook STATIC
    ${minhook_src_SOURCE_DIR}/src/hook.c
    ${minhook_src_SOURCE_DIR}/src/buffer.c
    ${minhook_src_SOURCE_DIR}/src/trampoline.c
    ${minhook_src_SOURCE_DIR}/src/hde/hde64.c
    ${minhook_src_SOURCE_DIR}/src/hde/hde32.c
)
target_include_directories(minhook PUBLIC ${minhook_src_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(minhook PRIVATE /wd4267 /wd4244 /wd4127 /wd4131)
endif()

set(CAMERAUNLOCK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CAMERAUNLOCK_BUILD_HOOKS ON CACHE BOOL "" FORCE)
set(CAMERAUNLOCK_BUILD_UNREAL ON CACHE BOOL "" FORCE)
add_subdirectory(cameraunlock-core/cpp)

set(MOD_NAME RVThereYetHeadTracking)

add_library(${MOD_NAME} SHARED
    src/RVThereYetHeadTracking/dllmain.cpp
    src/RVThereYetHeadTracking/headtracking_mod.cpp
    src/RVThereYetHeadTracking/reticle.cpp
    src/RVThereYetHeadTracking/lean_trace.cpp
    src/RVThereYetHeadTracking/builds/build_registry.cpp
    src/RVThereYetHeadTracking/builds/image_discovery.cpp
    src/RVThereYetHeadTracking/builds/steam_offsets.cpp
    src/RVThereYetHeadTracking/builds/gdk_offsets.cpp
    src/RVThereYetHeadTracking/config.cpp
    src/legacy_config/legacy_config.cpp
)

target_include_directories(${MOD_NAME} PRIVATE
    src/RVThereYetHeadTracking
    src
    ${minhook_src_SOURCE_DIR}/src/hde
)

target_compile_definitions(${MOD_NAME} PRIVATE
    RVTY_MOD_VERSION="${RVTY_MOD_VERSION}"
    RVTY_GIT_SHA="${RVTY_GIT_SHA}"
)

option(RVTY_DEV_HOTKEYS "Build the reticle/widget diagnostic hotkeys (developer aid, not for release)" OFF)
if(RVTY_DEV_HOTKEYS)
    target_compile_definitions(${MOD_NAME} PRIVATE RVTY_DEV_HOTKEYS=1)
    message(WARNING "RVTY_DEV_HOTKEYS is ON. This build registers F7/F8 to shift "
                    "the reticle scale by 0.1, Ctrl+Shift+J to toggle a fixed +300px "
                    "test nudge on the target widget, and Ctrl+Shift+U to dump live "
                    "widget names to the log. Do not ship it.")
endif()

if(MSVC)
    target_compile_options(${MOD_NAME} PRIVATE /W4 /WX /wd4324)
endif()

target_link_libraries(${MOD_NAME} PRIVATE
    cameraunlock
    cameraunlock_hooks
    cameraunlock_unreal
    minhook
    ws2_32
    psapi
)

set_target_properties(${MOD_NAME} PROPERTIES
    SUFFIX ".dll"
    OUTPUT_NAME "dxgi"
)

# Behaviour lock for the lean boundary, the config tests and the config
# differential test. Separate executables linking none of the hook machinery,
# so `pixi run test` runs anywhere the mod builds, with no game installed.
option(RVTY_BUILD_TESTS "Build the position behaviour tests and the config tests" ON)
if(RVTY_BUILD_TESTS)
    enable_testing()
    add_executable(rvty_lean_clamp_tests tests/lean_clamp_main.cpp cameraunlock-core/cpp/tests/lean_clamp_tests.cpp)
    target_include_directories(rvty_lean_clamp_tests PRIVATE cameraunlock-core/cpp/include)
    add_test(NAME rvty_lean_clamp_tests COMMAND rvty_lean_clamp_tests)

    add_executable(rvty_discovery_tests
        tests/discovery_tests.cpp
        src/RVThereYetHeadTracking/builds/image_discovery.cpp
        src/RVThereYetHeadTracking/builds/build_registry.cpp
        src/RVThereYetHeadTracking/builds/steam_offsets.cpp
        src/RVThereYetHeadTracking/builds/gdk_offsets.cpp
    )
    target_include_directories(rvty_discovery_tests PRIVATE
        src/RVThereYetHeadTracking ${minhook_src_SOURCE_DIR}/src/hde)
    target_link_libraries(rvty_discovery_tests PRIVATE minhook cameraunlock)
    if(MSVC)
        target_compile_options(rvty_discovery_tests PRIVATE /W4 /WX /EHsc)
    endif()
    add_test(NAME rvty_discovery_tests COMMAND rvty_discovery_tests)

    add_executable(rvty_position_tests tests/position_tests.cpp)
    target_include_directories(rvty_position_tests PRIVATE
        src/RVThereYetHeadTracking
        cameraunlock-core/cpp/include
    )
    add_test(NAME rvty_position_tests COMMAND rvty_position_tests)

    # The committed config against the table's fresh render, and the saves.
    # `rvty_config_tests --render-config <path>` is pixi run render-config.
    add_executable(rvty_config_tests
        tests/config_tests.cpp
        src/RVThereYetHeadTracking/config.cpp
        src/legacy_config/legacy_config.cpp
    )
    target_include_directories(rvty_config_tests PRIVATE
        src
        src/RVThereYetHeadTracking
        cameraunlock-core/cpp/include
    )
    target_compile_definitions(rvty_config_tests PRIVATE
        RVTY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(rvty_config_tests PRIVATE cameraunlock)
    if(MSVC)
        target_compile_options(rvty_config_tests PRIVATE /W4 /WX /EHsc)
    endif()
    add_test(NAME rvty_config_tests COMMAND rvty_config_tests)

    # The canonical config conversion's differential test: v0.3.0's reader and
    # startup code (the oracle, transcribed in tests/config_differential/oracle)
    # against the frozen reader in src/legacy_config/, and that against the
    # config owner's migration into CameraUnlock.ini. The oracle is compiled on
    # its own with the mod's and core's namespaces renamed, against byte copies
    # of the core headers v0.3.0's pin (3465659) had that core has changed since,
    # so it links beside today's code and nothing in it can resolve to a symbol of
    # the mod under test.
    set(RVTY_DIFFERENTIAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential)
    add_library(rvty_config_oracle STATIC
        ${RVTY_DIFFERENTIAL_DIR}/oracle/oracle_reader.cpp
        cameraunlock-core/cpp/src/config/ini_reader.cpp
        cameraunlock-core/cpp/src/logging/file_log.cpp
    )
    target_include_directories(rvty_config_oracle PRIVATE
        ${RVTY_DIFFERENTIAL_DIR}/oracle
        ${RVTY_DIFFERENTIAL_DIR}/oracle/core
        cameraunlock-core/cpp/include
    )
    target_compile_definitions(rvty_config_oracle PRIVATE
        RVThereYetHeadTracking=rvty_oracle_mod
        cameraunlock=rvty_oracle_core
    )

    add_executable(rvty_config_differential
        ${RVTY_DIFFERENTIAL_DIR}/differential_tests.cpp
        src/RVThereYetHeadTracking/config.cpp
        src/legacy_config/legacy_config.cpp
    )
    target_include_directories(rvty_config_differential PRIVATE
        src
        src/RVThereYetHeadTracking
        ${RVTY_DIFFERENTIAL_DIR}
        cameraunlock-core/cpp/include
    )
    target_compile_definitions(rvty_config_differential PRIVATE
        RVTY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(rvty_config_differential PRIVATE rvty_config_oracle cameraunlock)
    if(MSVC)
        target_compile_options(rvty_config_differential PRIVATE /W4 /WX /EHsc)
    endif()
    add_test(NAME rvty_config_differential COMMAND rvty_config_differential)
    # Core's canonical config lint over the committed file and every file the
    # differential test migrated, which it writes beside itself under migrated\.
    add_test(NAME rvty_config_lint COMMAND node
             ${RVTY_DIFFERENTIAL_DIR}/lint-migrated.mjs
             $<TARGET_FILE_DIR:rvty_config_differential>/migrated)
    set_tests_properties(rvty_config_lint PROPERTIES DEPENDS rvty_config_differential)
    set_tests_properties(rvty_config_differential rvty_config_lint PROPERTIES LABELS differential)

    # What the differential test compiles is pinned by hash, line endings aside:
    # the oracle's byte copies must stay v0.3.0's (git show
    # v0.3.0:src/RVThereYetHeadTracking/logging.h, and git -C cameraunlock-core
    # show 3465659:cpp/include/cameraunlock/<path> for core/), the core sources
    # both readers compile must stay the ones v0.3.0 was built with, and the
    # frozen reader is never edited.
    function(rvty_require_hash path expected)
        file(READ "${path}" text)
        string(REPLACE "\r" "" text "${text}")
        string(SHA256 actual "${text}")
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "${path} has changed (SHA-256 of its text ${actual}, pinned "
                                "${expected}). The config differential test compares against "
                                "exactly this file; see CMakeLists.txt.")
        endif()
    endfunction()
    set(RVTY_ORACLE_CORE ${RVTY_DIFFERENTIAL_DIR}/oracle/core/cameraunlock)
    set(RVTY_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cameraunlock-core/cpp)
    foreach(pinned
            "${RVTY_DIFFERENTIAL_DIR}/oracle/src/logging.h|90d47b8031a42e0811f4fbb4d30228a2cab387dc88047b6237080248ca3bc0e2"
            "${RVTY_ORACLE_CORE}/data/position_settings.h|ccd022e0f63bfe6cde243f2769b803bfa7d0ead6f77118874fa6c57961021a89"
            "${RVTY_ORACLE_CORE}/processing/position_processor.h|9a4857ba41117a058a87fd95784a29067d370f9fa118377059ee3359591ce92f"
            "${RVTY_ORACLE_CORE}/protocol/udp_receiver.h|fc7cc79943103264b9964bb77625f7ee3d848bcc634fbf1181cb8c54faca17db"
            "${RVTY_ORACLE_CORE}/protocol/socket_types.h|0dc7e23ae364dc12216604070a52f60811bb18e35f591d38b4ecdd0a655eb627"
            "${RVTY_ORACLE_CORE}/protocol/udp_socket.h|e3b07fba03de6942c4e6eec696dcd08f3931849994edb46fc297dc5ee479df34"
            "${RVTY_CORE_DIR}/include/cameraunlock/config/ini_reader.h|a7ffb44210ff59672fa97e8e5feaa2cb3e81938fcc0334a384c68bc371b3857a"
            "${RVTY_CORE_DIR}/src/config/ini_reader.cpp|e01515c2656aaf533bae4350dc45b702c3e3d4043935743dcc9bd5ea581fbe1c"
            "${RVTY_CORE_DIR}/include/cameraunlock/logging/file_log.h|43bdd2ef8554c78e5f440333463750c13b95110fe672b0b6e273244df9e7d169"
            "${RVTY_CORE_DIR}/src/logging/file_log.cpp|73c53c2baa06bbfebe8211f62678aa2b60cb95f604743d3686951ba56b87ea47"
            "${RVTY_CORE_DIR}/include/cameraunlock/math/smoothing_utils.h|fc2146f8c585e5f610c7234e302f59de4945679cfa28ff479ca47477ec073f22"
            "${RVTY_CORE_DIR}/include/cameraunlock/math/angle_utils.h|d7a905270933e3cb0c4c361d29d3fd701655498cbcd1875ea79d180468bdbe6a"
            "${RVTY_CORE_DIR}/include/cameraunlock/math/vec3.h|021c45902da44d12911a7cacd9b76fa28775b382583dca2e5e9df3888e242683"
            "${RVTY_CORE_DIR}/include/cameraunlock/math/quat4.h|e390c99092abd55f4296f573e0e537638c1f4f21cecfac34e0c0a50c44f7486e"
            "${RVTY_CORE_DIR}/include/cameraunlock/data/position_data.h|f97388fa6ec0ca02e0fa7c4eeb26579bcaecbb5d56f88d90be59a9e66689ba7d"
            "${RVTY_CORE_DIR}/include/cameraunlock/data/tracking_pose.h|7cdb1ad02461e4cf37d3f2e2f9eedde12a9ed3238a61f8906302596589d2f629"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/RVThereYetHeadTracking/logging.h|90d47b8031a42e0811f4fbb4d30228a2cab387dc88047b6237080248ca3bc0e2"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/legacy_config/legacy_config.h|4d50091893e710c0bf7c568e3863bdb7927f43b483d08332ba9134d9f5b1afbf"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/legacy_config/legacy_config.cpp|76fdf6980aa6d714480c2ea71c5400162f77f156852cc3da4426e00694e7db08")
        string(REPLACE "|" ";" pair "${pinned}")
        list(GET pair 0 pinned_path)
        list(GET pair 1 pinned_hash)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pinned_path}")
        rvty_require_hash("${pinned_path}" "${pinned_hash}")
    endforeach()
endif()
