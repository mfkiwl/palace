# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

#
# Build libCEED
#

# Force build order
set(LIBCEED_DEPENDENCIES)
if(PALACE_BUILD_EXTERNAL_DEPS)
  if(PALACE_WITH_LIBXSMM)
    list(APPEND LIBCEED_DEPENDENCIES libxsmm)
  endif()
  if(PALACE_WITH_MAGMA)
    list(APPEND LIBCEED_DEPENDENCIES magma)
  endif()
endif()

# Note on recommended flags for libCEED (from Makefile, Spack):
#   OPT: -O3 -g -march=native -ffp-contract=fast [-fopenmp-simd/-qopenmp-simd]
include(CheckCCompilerFlag)
set(LIBCEED_C_FLAGS "${CMAKE_C_FLAGS}")
if(CMAKE_C_COMPILER_ID MATCHES "Intel|IntelLLVM")
  set(OMP_SIMD_FLAG -qopenmp-simd)
else()
  set(OMP_SIMD_FLAG -fopenmp-simd)
endif()
check_c_compiler_flag(${OMP_SIMD_FLAG} SUPPORTS_OMP_SIMD)
if(SUPPORTS_OMP_SIMD)
  set(LIBCEED_C_FLAGS "${LIBCEED_C_FLAGS} ${OMP_SIMD_FLAG}")
endif()

# Silence some CUDA/HIP include file warnings
if(PALACE_WITH_CUDA)
  set(LIBCEED_C_FLAGS "${LIBCEED_C_FLAGS} -isystem ${CUDA_DIR}/include")
endif()
if(PALACE_WITH_HIP)
  set(LIBCEED_C_FLAGS "${LIBCEED_C_FLAGS} -isystem ${ROCM_DIR}/include")
endif()

# Build libCEED (always as a shared library)
set(LIBCEED_OPTIONS
  "prefix=${CMAKE_INSTALL_PREFIX}"
  "CC=${CMAKE_C_COMPILER}"
  "OPT=${LIBCEED_C_FLAGS}"
  "STATIC="
  "VERBOSE=1"
)

# Configure OpenMP
if(PALACE_WITH_OPENMP)
  list(APPEND LIBCEED_OPTIONS
    "OPENMP=1"
  )
endif()

# Configure libCEED backends
if(PALACE_WITH_LIBXSMM)
  if(PALACE_BUILD_EXTERNAL_DEPS)
    list(APPEND LIBCEED_OPTIONS
      "XSMM_DIR=${CMAKE_INSTALL_PREFIX}"
    )
  else()
    list(APPEND LIBCEED_OPTIONS
      "XSMM_DIR=${LIBXSMM_DIR}"
    )
  endif()
  # LIBXSMM can require linkage with BLAS for fallback
  list(APPEND LIBCEED_OPTIONS
    "BLAS_LIB="
  )
  # if(NOT "${BLAS_LAPACK_LIBRARIES}" STREQUAL "")
  #   string(REPLACE "$<SEMICOLON>" " " LIBCEED_BLAS_LAPACK_LIBRARIES "${BLAS_LAPACK_LIBRARIES}")
  #   list(APPEND LIBCEED_OPTIONS
  #     "BLAS_LIB=${LIBCEED_BLAS_LAPACK_LIBRARIES}"
  #   )
  # endif()
endif()
if(PALACE_WITH_CUDA)
  list(APPEND LIBCEED_OPTIONS
    "CUDA_DIR=${CUDA_DIR}"
  )
  if(NOT "${CMAKE_CUDA_ARCHITECTURES}" STREQUAL "")
    list(GET CMAKE_CUDA_ARCHITECTURES 0 LIBCEED_CUDA_ARCH)
    list(APPEND LIBCEED_OPTIONS
      "CUDA_ARCH=sm_${LIBCEED_CUDA_ARCH}"
    )
  endif()
endif()
if(PALACE_WITH_HIP)
  list(APPEND LIBCEED_OPTIONS
    "ROCM_DIR=${ROCM_DIR}"
  )
  if(NOT "${CMAKE_HIP_ARCHITECTURES}" STREQUAL "")
    list(GET CMAKE_HIP_ARCHITECTURES 0 LIBCEED_HIP_ARCH)
    list(APPEND LIBCEED_OPTIONS
      "HIP_ARCH=${LIBCEED_HIP_ARCH}"
    )
  endif()
endif()
if(PALACE_WITH_MAGMA)
  if(PALACE_BUILD_EXTERNAL_DEPS)
    list(APPEND LIBCEED_OPTIONS
      "MAGMA_DIR=${CMAKE_INSTALL_PREFIX}"
    )
  else()
    list(APPEND LIBCEED_OPTIONS
      "MAGMA_DIR=${MAGMA_DIR}"
    )
  endif()
endif()

string(REPLACE ";" "; " LIBCEED_OPTIONS_PRINT "${LIBCEED_OPTIONS}")
message(STATUS "LIBCEED_OPTIONS: ${LIBCEED_OPTIONS_PRINT}")

# Add OpenMP support to libCEED
set(LIBCEED_PATCH_FILES
  "${CMAKE_SOURCE_DIR}/extern/patch/libCEED/patch_openmp.diff"
)

include(ExternalProject)
ExternalProject_Add(libCEED
  DEPENDS           ${LIBCEED_DEPENDENCIES}
  GIT_REPOSITORY    ${EXTERN_LIBCEED_URL}
  GIT_TAG           ${EXTERN_LIBCEED_GIT_TAG}
  SOURCE_DIR        ${CMAKE_BINARY_DIR}/extern/libCEED
  INSTALL_DIR       ${CMAKE_INSTALL_PREFIX}
  PREFIX            ${CMAKE_BINARY_DIR}/extern/libCEED-cmake
  BUILD_IN_SOURCE   TRUE
  UPDATE_COMMAND    ""
  PATCH_COMMAND     git apply "${LIBCEED_PATCH_FILES}"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ${CMAKE_MAKE_PROGRAM} ${LIBCEED_OPTIONS} install
  TEST_COMMAND      ""
)

set(_LIBCEED_EXTRA_LIBRARIES)
if(PALACE_WITH_LIBXSMM)
  if(PALACE_BUILD_EXTERNAL_DEPS)
    include(GNUInstallDirs)
    set(_LIBXSMM_LIBRARIES ${CMAKE_INSTALL_PREFIX}/lib/libxsmm${CMAKE_SHARED_LIBRARY_SUFFIX})
  else()
    set(_LIBXSMM_LIBRARIES "-lxsmm")
  endif()
  list(APPEND _LIBCEED_EXTRA_LIBRARIES ${_LIBXSMM_LIBRARIES})
endif()
if(PALACE_WITH_MAGMA)
  if(PALACE_BUILD_EXTERNAL_DEPS)
    include(GNUInstallDirs)
    if(BUILD_SHARED_LIBS)
      set(_MAGMA_LIB_SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX})
    else()
      set(_MAGMA_LIB_SUFFIX ${CMAKE_STATIC_LIBRARY_SUFFIX})
    endif()
    set(_MAGMA_LIBRARIES ${CMAKE_INSTALL_PREFIX}/lib/libmagma${_MAGMA_LIB_SUFFIX})
  else()
    set(_MAGMA_LIBRARIES "-lmagma")
  endif()
  list(APPEND _LIBCEED_EXTRA_LIBRARIES ${_MAGMA_LIBRARIES})
endif()
if(NOT "${_LIBCEED_EXTRA_LIBRARIES}" STREQUAL "")
  string(REPLACE ";" "$<SEMICOLON>" _LIBCEED_EXTRA_LIBRARIES "${_LIBCEED_EXTRA_LIBRARIES}")
  set(LIBCEED_EXTRA_LIBRARIES ${_LIBCEED_EXTRA_LIBRARIES} CACHE STRING "List of extra library files for libCEED")
endif()
