# Build hardening and warning policy.
#
# The flag set is mandated by ENGINEERING_RULES.md §5. Applied through one INTERFACE target
# that every anvil target links, so nothing can quietly opt out — including an
# application that consumes anvil via add_subdirectory and forgets to set its own.

include_guard(GLOBAL)

set(ANVIL_SANITIZERS "" CACHE STRING "Semicolon list: address;undefined;thread")
set_property(CACHE ANVIL_SANITIZERS PROPERTY STRINGS "" "address;undefined" "thread")

add_library(anvil_flags INTERFACE)
add_library(anvil::flags ALIAS anvil_flags)

# --- Warnings -------------------------------------------------------------
# -Werror=reorder is required: member init order must match declaration order
# (ENGINEERING_RULES.md §3.2). Mismatched order is undefined-order initialisation, and it
# is load-bearing in at least one place — MongoPool declares `instance_` before
# `pool_` because the driver instance must outlive every client drawn from it.
target_compile_options(anvil_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wold-style-cast
    -Wcast-align
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wnull-dereference
    -Wformat=2
    -Wimplicit-fallthrough
    -Wunused

    # Errors, not warnings — each maps to a rule that is not negotiable.
    -Werror=reorder              # ENGINEERING_RULES.md §3.2 declaration order
    -Werror=return-type          # falling off a non-void function is UB
    -Werror=uninitialized
    -Werror=return-local-addr    # the string_view-into-request-body class
    -Werror=dangling-else
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(anvil_flags INTERFACE
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wuseless-cast
        -Werror=dangling-pointer  # GCC 12+: catches views outliving their owner
    )
endif()

# --- Hardening ------------------------------------------------------------
# _FORTIFY_SOURCE needs optimisation to do anything and warns when it cannot,
# so it is applied to optimised configurations only.
target_compile_options(anvil_flags INTERFACE
    -fstack-protector-strong
    -fno-strict-aliasing          # BSON/binary punning must stay defined
    $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
    $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
)

target_link_options(anvil_flags INTERFACE
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,noexecstack
    -Wl,--as-needed
)

# --- Sanitizers -----------------------------------------------------------
# ASan+UBSan on the full suite, TSan on the concurrency tests (ENGINEERING_RULES.md §5).
# TSan is mutually exclusive with ASan.
if(ANVIL_SANITIZERS)
    if("thread" IN_LIST ANVIL_SANITIZERS AND "address" IN_LIST ANVIL_SANITIZERS)
        message(FATAL_ERROR "ThreadSanitizer and AddressSanitizer cannot be combined")
    endif()

    string(REPLACE ";" "," _anvil_san "${ANVIL_SANITIZERS}")
    target_compile_options(anvil_flags INTERFACE
        -fsanitize=${_anvil_san}
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
        -g)
    target_link_options(anvil_flags INTERFACE -fsanitize=${_anvil_san})

    if("undefined" IN_LIST ANVIL_SANITIZERS)
        # An unchecked integer overflow on a size or offset is a memory-safety
        # bug, not a warning (ENGINEERING_RULES.md §5).
        target_compile_options(anvil_flags INTERFACE
            -fno-sanitize-recover=undefined
            -fsanitize=float-divide-by-zero)
    endif()
endif()

# --- Helper ---------------------------------------------------------------
# Every anvil target goes through this so the flags cannot be forgotten.
#
# ANVIL_CONFIG_INCLUDE_DIR is PUBLIC rather than PRIVATE because anvil's own
# headers include <anvil_app_config.h>: a consumer that pulls in
# anvil/core/locale.h must resolve it too (docs/01-seams.md §2).
function(anvil_target target)
    target_link_libraries(${target} PRIVATE anvil::flags)
    target_include_directories(${target} PUBLIC
        "${ANVIL_SOURCE_DIR}/include"
        "${ANVIL_CONFIG_INCLUDE_DIR}")
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON)
endfunction()
