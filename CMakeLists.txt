cmake_minimum_required(VERSION 3.28)
project(askmdb VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Enables FakeEngine through environment variables. Release builds set it to OFF.
option(ASKMDB_TEST_HOOKS "Fake engine driven by environment variables (ASKMDB_FAKE_*)" ON)

# TLS for libmongoc. AUTO: OpenSSL if its headers are found; otherwise no TLS, which also means
# no SCRAM (libmongoc needs a crypto library for it), so that build only talks to mongod without auth.
set(ASKMDB_TLS AUTO CACHE STRING "TLS with OpenSSL: AUTO, ON or OFF")
set_property(CACHE ASKMDB_TLS PROPERTY STRINGS AUTO ON OFF)

# Download URL of the default model. Empty = the one in src/fetch/model_info.hpp; a release
# build can override it here.
set(ASKMDB_MODEL_URL "" CACHE STRING "Download URL of the default model (empty = the one in model_info.hpp)")

# Strict warnings only on our own targets. Never via a global add_compile_options:
# llama.cpp and mongo-c-driver do not build with -Werror.
function(askmdb_warnings target)
  # -Wno-ignored-attributes: bson_t carries an alignment attribute that templates drop; harmless,
  # since askmdb only holds pointers to documents allocated by libbson.
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion
                         -Wno-ignored-attributes)
endfunction()

# --- TLS -------------------------------------------------------------------------------------
if(ASKMDB_TLS STREQUAL "ON")
  find_package(OpenSSL REQUIRED)
  set(ASKMDB_HAVE_TLS ON)
elseif(ASKMDB_TLS STREQUAL "AUTO")
  find_package(OpenSSL)
  set(ASKMDB_HAVE_TLS ${OPENSSL_FOUND})
elseif(ASKMDB_TLS STREQUAL "OFF")
  set(ASKMDB_HAVE_TLS OFF)
else()
  message(FATAL_ERROR "ASKMDB_TLS must be AUTO, ON or OFF")
endif()
if(ASKMDB_HAVE_TLS)
  message(STATUS "askmdb: TLS with OpenSSL ${OPENSSL_VERSION}")
  set(ASKMDB_MONGOC_SSL OPENSSL)
else()
  message(STATUS "askmdb: no OpenSSL found: building WITHOUT TLS and without SCRAM (mongod without auth only)")
  set(ASKMDB_MONGOC_SSL OFF)
endif()

# --- mongo-c-driver: static libbson + libmongoc, only what askmdb needs ------------------------
set(ENABLE_TESTS                  OFF CACHE BOOL   "" FORCE)
set(ENABLE_EXAMPLES               OFF CACHE BOOL   "" FORCE)
set(ENABLE_STATIC                 ON  CACHE STRING "" FORCE)
set(ENABLE_SHARED                 OFF CACHE BOOL   "" FORCE)
set(ENABLE_SNAPPY                 OFF CACHE STRING "" FORCE)
set(ENABLE_ZSTD                   OFF CACHE STRING "" FORCE)
set(ENABLE_ZLIB                   OFF CACHE STRING "" FORCE)
set(ENABLE_SASL                   OFF CACHE STRING "" FORCE)
set(ENABLE_CLIENT_SIDE_ENCRYPTION OFF CACHE STRING "" FORCE)
set(ENABLE_MONGODB_AWS_AUTH       OFF CACHE STRING "" FORCE)
set(ENABLE_SRV                    ON  CACHE STRING "" FORCE)
set(ENABLE_SSL ${ASKMDB_MONGOC_SSL}   CACHE STRING "" FORCE)
set(ENABLE_SHM_COUNTERS           OFF CACHE STRING "" FORCE)
set(ENABLE_MAN_PAGES              OFF CACHE BOOL   "" FORCE)
set(ENABLE_HTML_DOCS              OFF CACHE BOOL   "" FORCE)
set(ENABLE_UNINSTALL              OFF CACHE BOOL   "" FORCE)
set(BUILD_TESTING                 OFF CACHE BOOL   "" FORCE)
add_subdirectory(third_party/mongo-c-driver EXCLUDE_FROM_ALL SYSTEM)

# --- Vulkan loader shim ------------------------------------------------------------------------
# src/infer/vk_loader.cpp is its own library. With ASKMDB_VULKAN it stands in for libvulkan:
# ggml-vulkan does find_package(Vulkan COMPONENTS glslc REQUIRED) and links Vulkan::Vulkan, which
# is predefined here as this shim, so nothing links libvulkan (no DT_NEEDED) and the binary still
# starts without a Vulkan loader. FindVulkan then only needs the headers and glslc; Vulkan_LIBRARY
# names the shim target only to satisfy it. Build inputs: release/Dockerfile.
option(ASKMDB_VULKAN "Vulkan GPU backend, loaded at run time" OFF)
add_library(askmdb_vk STATIC src/infer/vk_loader.cpp)
target_include_directories(askmdb_vk PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(askmdb_vk PUBLIC ASKMDB_VULKAN=$<BOOL:${ASKMDB_VULKAN}>)
askmdb_warnings(askmdb_vk)
if(ASKMDB_VULKAN)
  find_path(ASKMDB_VULKAN_INCLUDE_DIR vulkan/vulkan_core.h HINTS ENV VULKAN_SDK PATH_SUFFIXES include REQUIRED)
  set(Vulkan_INCLUDE_DIR "${ASKMDB_VULKAN_INCLUDE_DIR}" CACHE PATH "Vulkan headers" FORCE)
  set(Vulkan_LIBRARY askmdb_vk CACHE STRING "Vulkan loader: the askmdb_vk shim target, never libvulkan" FORCE)
  target_include_directories(askmdb_vk SYSTEM PUBLIC "${ASKMDB_VULKAN_INCLUDE_DIR}")
  target_link_libraries(askmdb_vk PRIVATE ${CMAKE_DL_LIBS})
  add_library(Vulkan::Vulkan ALIAS askmdb_vk)
endif()

# --- llama.cpp: static; CPU, plus Vulkan with ASKMDB_VULKAN. No common => no cpp-httplib and no
# networking ------------------------------------------------------------------------------------
option(ASKMDB_NATIVE "Optimize for this CPU (development)" ON)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP      OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL        OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE   ${ASKMDB_NATIVE} CACHE BOOL "" FORCE)
set(GGML_OPENMP   OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN   ${ASKMDB_VULKAN} CACHE BOOL "" FORCE)
add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL SYSTEM)
# Unoptimised llama.cpp misses the time limit even in Debug.
foreach(t ggml-base ggml-cpu ggml llama ggml-vulkan)
  if(TARGET ${t})
    target_compile_options(${t} PRIVATE $<$<CONFIG:Debug>:-O2>)
  endif()
endforeach()

# --- askmdb ------------------------------------------------------------------------------------
# Library with all modules. A new file in src/<module>/ is picked up automatically,
# without touching this CMakeLists.txt. *_main.cpp files are separate programs.
set(ASKMDB_MODULES cli config exec fetch infer mongo schema verify)
set(ASKMDB_CORE_SOURCES)
foreach(module IN LISTS ASKMDB_MODULES)
  file(GLOB module_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/${module}/*.cpp")
  list(FILTER module_sources EXCLUDE REGEX "_main\\.cpp$")
  list(FILTER module_sources EXCLUDE REGEX "/infer/vk_loader\\.cpp$")  # askmdb_vk
  list(APPEND ASKMDB_CORE_SOURCES ${module_sources})
endforeach()

add_library(askmdb_core STATIC ${ASKMDB_CORE_SOURCES})
target_include_directories(askmdb_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
# askmdb_vk also brings ASKMDB_VULKAN=0/1 (PUBLIC) to askmdb_core and everything that links it.
target_link_libraries(askmdb_core PUBLIC askmdb_vk llama mongoc::static bson::static vendor::nlohmann)
if(ASKMDB_HAVE_TLS)
  # src/mongo initialises OpenSSL itself (without loading the system openssl.cnf).
  target_link_libraries(askmdb_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
target_compile_definitions(askmdb_core PUBLIC
  ASKMDB_VERSION="${PROJECT_VERSION}"
  ASKMDB_TEST_HOOKS=$<BOOL:${ASKMDB_TEST_HOOKS}>
  ASKMDB_HAVE_TLS=$<BOOL:${ASKMDB_HAVE_TLS}>)
if(NOT ASKMDB_MODEL_URL STREQUAL "")
  target_compile_definitions(askmdb_core PUBLIC ASKMDB_MODEL_URL="${ASKMDB_MODEL_URL}")
endif()
askmdb_warnings(askmdb_core)

add_executable(askmdb src/main.cpp)
target_link_libraries(askmdb PRIVATE askmdb_core)
askmdb_warnings(askmdb)

add_executable(askmdb_verify src/verify/askmdb_verify_main.cpp)
target_link_libraries(askmdb_verify PRIVATE askmdb_core)
askmdb_warnings(askmdb_verify)

# --- Tests -------------------------------------------------------------------------------------
# Suites (and ctest labels) that need something external:
#   mongo  the `mongo` service with the seed loaded (ASKMDB_E2E_URI)
#   tls    the `mongo-tls` service (ASKMDB_E2E_TLS_URI)
#   model  a GGUF (ASKMDB_TEST_MODEL)
# By default ctest runs everything else:  ctest -LE 'mongo|tls|model'
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/third_party/doctest/doctest")

enable_testing()

file(GLOB ASKMDB_UNIT_TESTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/*_test.cpp")
# The test that compares the written model contract with the code only builds where that
# document is part of the tree.
set(ASKMDB_CONTRACT_DOC_DIR "${CMAKE_SOURCE_DIR}/docs")
if(NOT EXISTS "${ASKMDB_CONTRACT_DOC_DIR}/contrato-modelo.md")
  list(FILTER ASKMDB_UNIT_TESTS EXCLUDE REGEX "/contrato_doc_test\\.cpp$")
endif()
add_executable(askmdb_tests tests/test_main.cpp ${ASKMDB_UNIT_TESTS})
target_link_libraries(askmdb_tests PRIVATE askmdb_core doctest)
target_compile_definitions(askmdb_tests PRIVATE
  ASKMDB_TESTS_DIR="${CMAKE_SOURCE_DIR}/tests"
  ASKMDB_VERIFY_CASES_DIR="${CMAKE_SOURCE_DIR}/tests/verify"
  ASKMDB_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
if(EXISTS "${ASKMDB_CONTRACT_DOC_DIR}/contrato-modelo.md")
  target_compile_definitions(askmdb_tests PRIVATE ASKMDB_DOCS_DIR="${ASKMDB_CONTRACT_DOC_DIR}")
endif()
askmdb_warnings(askmdb_tests)
add_test(NAME askmdb_tests COMMAND askmdb_tests -tse=mongo,tls,model)
foreach(label mongo tls model)
  add_test(NAME askmdb_tests_${label} COMMAND askmdb_tests -ts=${label})
  set_tests_properties(askmdb_tests_${label} PROPERTIES LABELS ${label})
endforeach()

file(GLOB ASKMDB_E2E_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/e2e/*.cpp")
add_executable(askmdb_e2e tests/test_main.cpp ${ASKMDB_E2E_SOURCES})
target_link_libraries(askmdb_e2e PRIVATE doctest util vendor::nlohmann)
target_compile_definitions(askmdb_e2e PRIVATE
  ASKMDB_BIN="$<TARGET_FILE:askmdb>"
  ASKMDB_VERIFY_BIN="$<TARGET_FILE:askmdb_verify>"
  ASKMDB_E2E_DIR="${CMAKE_SOURCE_DIR}/tests/e2e"
  ASKMDB_TESTS_DIR="${CMAKE_SOURCE_DIR}/tests")
add_dependencies(askmdb_e2e askmdb askmdb_verify)
askmdb_warnings(askmdb_e2e)
add_test(NAME askmdb_e2e COMMAND askmdb_e2e -tse=mongo,tls)
foreach(label mongo tls)
  add_test(NAME askmdb_e2e_${label} COMMAND askmdb_e2e -ts=${label})
  set_tests_properties(askmdb_e2e_${label} PROPERTIES LABELS ${label})
endforeach()

# Only aggregate, and only in the executor: no write or admin function of libmongoc in src/.
add_test(NAME mongoc_check COMMAND "${CMAKE_SOURCE_DIR}/tests/mongoc_check.sh" "${CMAKE_SOURCE_DIR}/src")
