# Common warning set applied to every first-party target.
# Third-party targets are never touched by this.

function(dv_set_target_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /w14242 # conversion, possible loss of data
      /w14263 # member function does not override any base class virtual member
      /w14265 # class has virtual functions but destructor is not virtual
      /w14287 # unsigned/negative constant mismatch
      /w14296 # expression is always false
      /w14545 # expression before comma evaluates to a function missing arguments
      /w14640 # thread unsafe static member initialization
      /w14826 # conversion is sign-extended
      /w14928 # illegal copy-initialization
      # Off. The message dispatch in shared/src/protocol/message.cpp is a chain
      # of `if constexpr` over a variant, and every instantiation but the last
      # returns before reaching the final fallback, so MSVC reports the fallback
      # as unreachable once per alternative. Seventeen warnings about the one
      # line that has to be there for the seventeenth case to work.
      /wd4702 # unreachable code
    )
    # std::getenv is standard C++ and MSVC's CRT deprecates it in favour of a
    # function that only exists on Windows. The deprecation is the vendor's
    # opinion, not a defect in the call.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(DV_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-align
      -Wunused
      -Woverloaded-virtual
      -Wpedantic
      -Wconversion
      -Wsign-conversion
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
      -Wimplicit-fallthrough
      # Off, and it comes from -Wextra. This codebase initialises aggregates
      # with designated initialisers and names only the members it means to
      # set; the standard value-initialises the rest, which is the point of
      # the syntax. Leaving the warning on would mean writing `.member = {}`
      # at every call site for every field anyone ever adds, which is churn
      # that makes the initialisers say less rather than more.
      -Wno-missing-field-initializers
    )
    if(DV_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
