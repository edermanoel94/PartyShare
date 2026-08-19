# AddressSanitizer and UndefinedBehaviorSanitizer, enabled through the
# sanitizer presets. Section 26 of SPEC.md requires both.

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
