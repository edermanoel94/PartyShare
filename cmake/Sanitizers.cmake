# AddressSanitizer and UndefinedBehaviorSanitizer, enabled through the
# sanitizer presets. Section 26 of SPEC.md requires both.

# Handed to gtest_discover_tests as extra PROPERTIES, and empty when sanitizers
# are off so the same call site works either way. It is a variable rather than
# something dv_enable_sanitizers does, because that function is called on static
# libraries as well as on executables, and a test property belongs to neither.
#
# See lsan.supp for what is suppressed and why. Not a compiled
# __lsan_default_suppressions either: that symbol would have to live in exactly
# one translation unit per binary, and every target here already links several
# libraries that dv_enable_sanitizers touches.
if(DV_ENABLE_SANITIZERS AND NOT MSVC)
  set(DV_SANITIZER_TEST_PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_LIST_DIR}/lsan.supp")
else()
  set(DV_SANITIZER_TEST_PROPERTIES "")
endif()

function(dv_enable_sanitizers target)
  if(NOT DV_ENABLE_SANITIZERS)
    return()
  endif()

  if(MSVC)
    # MSVC ships AddressSanitizer only. UBSan has no MSVC equivalent.
    target_compile_options(${target} PRIVATE /fsanitize=address)
    return()
  endif()

  set(flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_compile_options(${target} PRIVATE ${flags})
  target_link_options(${target} PRIVATE -fsanitize=address,undefined)
endfunction()
