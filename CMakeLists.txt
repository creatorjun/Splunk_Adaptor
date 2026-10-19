# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(resource-monitor VERSION 1.7.0 LANGUAGES CXX)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Installation prefix" FORCE)
endif()
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "resource-monitor targets Linux. Build in Oracle Linux 8/9 or another Linux development environment.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(MONITOR_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(MONITOR_FETCH_JSON "Download the pinned JSON dependency when not installed" ON)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <expected>\nint main() { std::expected<int, int> result{1}; return *result - 1; }" MONITOR_HAS_EXPECTED)
if(NOT MONITOR_HAS_EXPECTED)
    message(FATAL_ERROR "A C++23 compiler and standard library with std::expected are required. Use GCC Toolset 15 on Oracle Linux.")
endif()

find_package(nlohmann_json 3.12.0 EXACT QUIET)
if(NOT nlohmann_json_FOUND)
    if(NOT MONITOR_FETCH_JSON AND NOT FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON)
        message(FATAL_ERROR "Install nlohmann_json 3.12.0 or provide FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON for an offline build.")
    endif()
    include(FetchContent)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(monitor_options INTERFACE)
target_compile_options(monitor_options INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(MONITOR_SANITIZERS)
    target_compile_options(monitor_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(monitor_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(monitor_domain
    src/domain/CollectCountAnalysis.cpp
    src/domain/LogSilenceAnalysis.cpp
    src/domain/MetricCalculators.cpp
    src/domain/ThresholdPolicy.cpp
)
target_include_directories(monitor_domain PUBLIC src)
target_link_libraries(monitor_domain PRIVATE monitor_options)

add_library(monitor_application src/application/CollectAndStore.cpp src/application/SettingsController.cpp src/application/CollectCountMonitor.cpp src/application/JavaProcessMonitor.cpp src/application/LogSilenceMonitor.cpp)
target_link_libraries(monitor_application PUBLIC monitor_domain PRIVATE monitor_options)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MARIADB REQUIRED IMPORTED_TARGET libmariadb>=3.1)
add_library(monitor_infrastructure
    src/infrastructure/linux/ProcJavaProcessReader.cpp
    src/infrastructure/linux/LocalDayWindow.cpp
    src/infrastructure/database/CollectCountQuery.cpp
    src/infrastructure/database/LogSilenceQuery.cpp
    src/infrastructure/linux/ProcResourceReader.cpp
    src/infrastructure/linux/MountReader.cpp
    src/infrastructure/config/JsonSettingsReader.cpp
    src/infrastructure/config/JsonMySqlCredentialsReader.cpp
    src/infrastructure/database/MariaDbConnectionFactory.cpp
    src/infrastructure/database/MySqlConfiguration.cpp
    src/infrastructure/persistence/JsonSnapshotStore.cpp
)
target_link_libraries(monitor_infrastructure PUBLIC monitor_application PRIVATE nlohmann_json::nlohmann_json monitor_options Threads::Threads PkgConfig::MARIADB)

add_library(monitor_runtime src/runtime/DaemonRunner.cpp)
target_link_libraries(monitor_runtime PUBLIC monitor_application PRIVATE monitor_options)

add_executable(resource-monitor src/main.cpp)
target_link_libraries(resource-monitor PRIVATE monitor_runtime monitor_infrastructure monitor_options)

include(CTest)
if(BUILD_TESTING)
    foreach(test_name MetricCalculators CollectCountAnalysis LogSilenceAnalysis CollectCountMonitor LogSilenceMonitor JavaProcessMonitor ProcJavaProcessReader LocalDayWindow CollectCountQuery LogSilenceQuery CollectAndStore SettingsController LinuxCollectors JsonSnapshotStore JsonSettingsReader JsonMySqlCredentialsReader MySqlConfiguration)
        add_executable(${test_name}Test tests/${test_name}Test.cpp)
        target_include_directories(${test_name}Test PRIVATE tests)
        if(test_name STREQUAL "MetricCalculators" OR test_name STREQUAL "CollectCountAnalysis" OR test_name STREQUAL "LogSilenceAnalysis")
            target_link_libraries(${test_name}Test PRIVATE monitor_domain monitor_options)
        elseif(test_name STREQUAL "CollectAndStore" OR test_name STREQUAL "SettingsController" OR test_name STREQUAL "CollectCountMonitor" OR test_name STREQUAL "JavaProcessMonitor" OR test_name STREQUAL "LogSilenceMonitor")
            target_link_libraries(${test_name}Test PRIVATE monitor_application monitor_options)
        else()
            target_link_libraries(${test_name}Test PRIVATE monitor_infrastructure nlohmann_json::nlohmann_json monitor_options Threads::Threads)
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name}Test)
    endforeach()
    add_executable(DaemonRunnerTest tests/DaemonRunnerTest.cpp)
    target_include_directories(DaemonRunnerTest PRIVATE tests)
    target_link_libraries(DaemonRunnerTest PRIVATE monitor_runtime monitor_options Threads::Threads)
    add_test(NAME DaemonRunner COMMAND DaemonRunnerTest)
    set_tests_properties(DaemonRunner PROPERTIES TIMEOUT 20)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME Architecture COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/architecture_test.py ${CMAKE_SOURCE_DIR})
    add_test(NAME DaemonIntegration COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/daemon_integration_test.py $<TARGET_FILE:resource-monitor>)
    add_test(NAME Installation COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/installation_test.py ${CMAKE_COMMAND} ${CMAKE_BINARY_DIR})
    set_tests_properties(DaemonIntegration PROPERTIES TIMEOUT 90)
endif()

include(GNUInstallDirs)
install(TARGETS resource-monitor RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(FILES packaging/systemd/resource-monitor.service DESTINATION lib/systemd/system)
install(FILES config/settings.json DESTINATION share/resource-monitor)
install(FILES config/mysql.example.json DESTINATION share/resource-monitor)
install(FILES README.md DESTINATION share/doc/resource-monitor)
install(FILES packaging/THIRD_PARTY_NOTICES.txt DESTINATION share/doc/resource-monitor)
install(CODE "
    set(config_dir \"\$ENV{DESTDIR}/etc/resource-monitor\")
    set(secrets_dir \"\${config_dir}/secrets\")
    if(IS_SYMLINK \"\${secrets_dir}\" OR (EXISTS \"\${secrets_dir}\" AND NOT IS_DIRECTORY \"\${secrets_dir}\"))
        message(FATAL_ERROR \"Existing secrets path must be a directory and not a symbolic link\")
    endif()
    if(NOT EXISTS \"\${secrets_dir}\")
        file(MAKE_DIRECTORY \"\${secrets_dir}\")
        file(CHMOD \"\${secrets_dir}\" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
    endif()
    set(settings_file \"\${config_dir}/settings.json\")
    set(legacy_file \"\${config_dir}/thresholds.json\")
    if(EXISTS \"\${settings_file}\" OR IS_SYMLINK \"\${settings_file}\")
        execute_process(COMMAND /usr/bin/test -f \"\${settings_file}\" RESULT_VARIABLE settings_is_regular)
        if(IS_SYMLINK \"\${settings_file}\" OR NOT settings_is_regular EQUAL 0)
            message(FATAL_ERROR \"Existing settings.json must be a regular file and not a symbolic link\")
        endif()
    elseif(EXISTS \"\${legacy_file}\" OR IS_SYMLINK \"\${legacy_file}\")
        execute_process(COMMAND /usr/bin/test -f \"\${legacy_file}\" RESULT_VARIABLE legacy_is_regular)
        if(IS_SYMLINK \"\${legacy_file}\" OR NOT legacy_is_regular EQUAL 0)
            message(FATAL_ERROR \"Legacy thresholds.json must be a regular file and not a symbolic link\")
        endif()
        configure_file(\"\${legacy_file}\" \"\${settings_file}\" COPYONLY)
    else()
        file(INSTALL DESTINATION \"/etc/resource-monitor\" TYPE FILE FILES \"${CMAKE_SOURCE_DIR}/config/settings.json\")
    endif()
")
