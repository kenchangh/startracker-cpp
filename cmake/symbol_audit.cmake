# SPDX-License-Identifier: Apache-2.0
# Fails if a static library references heap, exception or RTTI symbols.
# Usage: cmake -DNM=<nm> -DLIB=<lib.a> [-DSTRIP_UNDERSCORE=ON] [-DALLOWLIST=<file>] -P symbol_audit.cmake

if(NOT NM OR NOT LIB)
  message(FATAL_ERROR "symbol_audit: NM and LIB are required")
endif()

execute_process(COMMAND "${NM}" -P "${LIB}" OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "symbol_audit: '${NM} -P ${LIB}' failed (${rc})")
endif()

set(allowed "")
if(ALLOWLIST AND EXISTS "${ALLOWLIST}")
  file(STRINGS "${ALLOWLIST}" lines)
  foreach(l IN LISTS lines)
    if(l MATCHES "^([^ #]+)")
      list(APPEND allowed "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endif()

# Placement new (_ZnwmPv, _ZnamPv) never allocates; it shows up as a symbol in -O0 builds.
set(forbidden
  "^_Zn[wa][mj]$"               # operator new / new[] (size only)
  "^_Zn[wa][mj]St"              # aligned / nothrow operator new
  "^_Zn[wa][mj]RKSt"            # nothrow operator new
  "^_Zd[la]"                    # operator delete / delete[]
  "^(malloc|calloc|realloc|reallocarray|free|posix_memalign|aligned_alloc|memalign|valloc|pvalloc|strdup|strndup)$"
  "^__cxa_(throw|allocate_exception|begin_catch|end_catch|rethrow|free_exception)$"
  "^__gxx_personality_v0$"
  "^_Unwind_Resume$"
  "^_ZT[IS]"                    # typeinfo / typeinfo name
)

string(REPLACE "\n" ";" lines "${out}")
set(bad "")
set(nsyms 0)
foreach(line IN LISTS lines)
  # POSIX format: "name type [value [size]]"; archive member headers end with ':'.
  if(NOT line MATCHES "^([^ ]+) ([A-Za-z])( |$)")
    continue()
  endif()
  set(name "${CMAKE_MATCH_1}")
  if(STRIP_UNDERSCORE AND name MATCHES "^_(.*)$")
    set(name "${CMAKE_MATCH_1}")
  endif()
  math(EXPR nsyms "${nsyms} + 1")
  foreach(re IN LISTS forbidden)
    if(name MATCHES "${re}")
      list(FIND allowed "${name}" idx)
      if(idx EQUAL -1)
        list(APPEND bad "${name}")
      endif()
    endif()
  endforeach()
endforeach()

if(nsyms EQUAL 0)
  message(FATAL_ERROR "symbol_audit: no symbols parsed from ${LIB}; nm output format not understood")
endif()
if(bad)
  list(REMOVE_DUPLICATES bad)
  foreach(s IN LISTS bad)
    message(STATUS "forbidden symbol: ${s}")
  endforeach()
  message(FATAL_ERROR "symbol_audit: ${LIB} references forbidden symbols")
endif()
message(STATUS "symbol_audit: ${LIB}: ${nsyms} symbols, none forbidden")
