cmake_minimum_required(VERSION 3.15)
project(DBOPLEngine VERSION 1.0.0 LANGUAGES CXX)

# ===========================================================================
# コンパイラ設定
# ===========================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 共有ライブラリの出力先
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ===========================================================================
# サブモジュール確認
# extern/dbopl は git submodule (https://github.com/rofl0r/dbopl)
# クローン後に以下を実行してください:
#   git submodule update --init --recursive
# ===========================================================================
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/dbopl/dbopl.h")
    message(FATAL_ERROR
        "extern/dbopl が見つかりません。\n"
        "以下のコマンドでサブモジュールを初期化してください:\n"
        "  git submodule update --init --recursive"
    )
endif()

# ===========================================================================
# ソースファイル
# ===========================================================================
set(DBOPL_SOURCES
    extern/dbopl/dbopl.cpp
)

set(ENGINE_SOURCES
    src/DBOPLEngine.cpp
)

# ===========================================================================
# 共有ライブラリ (DLL / .so)
# ===========================================================================
add_library(DBOPLEngine SHARED
    ${ENGINE_SOURCES}
    ${DBOPL_SOURCES}
)

# FMENGINE_EXPORTS を定義してエクスポート属性を有効化
target_compile_definitions(DBOPLEngine PRIVATE FMENGINE_EXPORTS)

target_include_directories(DBOPLEngine PRIVATE
    include          # FmEngineApi.h
    extern/dbopl     # dbopl.h
)

# プラットフォーム別設定
if(WIN32)
    # Windows: シンボル可視性のデフォルト制御
    set_target_properties(DBOPLEngine PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS OFF
    )
elseif(UNIX)
    # Linux/macOS: シンボル可視性を hidden にして dllexport 相当を使う
    target_compile_options(DBOPLEngine PRIVATE -fvisibility=hidden)
endif()

# コンパイルオプション
if(MSVC)
    target_compile_options(DBOPLEngine PRIVATE
        /utf-8          # ソースファイルおよび実行ファイルの文字セットを UTF-8 に統一
        /W3             # 警告レベル3
        /wd4244         # dbopl: int→char 縮小変換警告を抑制
        /wd4267         # dbopl: size_t→int 縮小変換警告を抑制

        # /RTC1 (VS がデバッグ構成に自動付与) は /O2 と競合するため、
        # CMAKE_<LANG>_FLAGS_DEBUG から除去して競合を回避する。
        # 最適化は構成ごとに generator expression で制御する。
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:RelWithDebInfo>:/O2 /Zi>
        $<$<CONFIG:MinSizeRel>:/O1>
        # Debug 構成は CMake デフォルト (/Od /Zi) に任せ /O2 を付与しない
    )

    # デバッグ構成の /RTC1 を除去（/O2 との競合を防ぐ）
    # Release/RelWithDebInfo/MinSizeRel では不要だが念のため全構成で除去する
    foreach(flag_var
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_CXX_FLAGS_MINSIZEREL)
        string(REPLACE "/RTC1" "" ${flag_var} "${${flag_var}}")
    endforeach()
else()
    target_compile_options(DBOPLEngine PRIVATE -O2 -Wall -Wextra)
endif()

# スレッドライブラリをリンク (std::mutex)
find_package(Threads REQUIRED)
target_link_libraries(DBOPLEngine PRIVATE Threads::Threads)

# ===========================================================================
# テスト (任意)
# api_test は FMEngineTest と同じく DLL を実行時にロードして
# エクスポートされた関数だけを叩く。
# ctest -C Release で走らせる。
# ===========================================================================
option(DBOPL_BUILD_TESTS "Build the API test" ON)
if(DBOPL_BUILD_TESTS)
    enable_testing()
    add_executable(api_test tests/api_test.cpp)
    target_include_directories(api_test PRIVATE include)
    target_link_libraries(api_test PRIVATE ${CMAKE_DL_LIBS})
    if(MSVC)
        target_compile_options(api_test PRIVATE /W3 /utf-8)
    endif()
    add_dependencies(api_test DBOPLEngine)
    add_test(NAME api_test COMMAND api_test $<TARGET_FILE:DBOPLEngine>)
endif()

# ===========================================================================
# インストールルール
# ===========================================================================
install(TARGETS DBOPLEngine
    RUNTIME DESTINATION bin    # Windows DLL
    LIBRARY DESTINATION lib    # Linux .so
    ARCHIVE DESTINATION lib    # Import lib
)
install(FILES include/FmEngineApi.h DESTINATION include)
