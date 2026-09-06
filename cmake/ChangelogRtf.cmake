# ChangelogRtf.cmake
#
# dv_changelog_to_rtf(<markdown> <rtf>)
#
# Renders CHANGELOG.md as the RTF that the Windows installer shows on its
# "What's new" page. A Windows Installer ScrollableText control reads RTF and
# nothing else, and RTF is ASCII: every character outside it has to be written
# as \uN?, so the conversion decodes UTF-8 by hand. It is in CMake rather than
# in Python or PowerShell so that the installer built on a machine without
# either is the same installer the release job builds.
#
# What the changelog contains is decided by its own conventions, and only three
# of them matter here: the preface up to the first "## " heading is dropped, a
# "## " line is a version heading, and a "- " line is an entry. Anything else
# inside a version is a paragraph.

# One code point as the RTF escape a RichEdit control reads: \uN? with N as a
# signed 16 bit value, and a surrogate pair for anything past the BMP. The "?"
# is what a reader without Unicode prints instead.
function(_dv_rtf_unicode code_point out_var)
  if(code_point LESS 65536)
    if(code_point GREATER 32767)
      math(EXPR code_point "${code_point} - 65536")
    endif()
    set(${out_var} "\\u${code_point}?" PARENT_SCOPE)
    return()
  endif()
  math(EXPR rest "${code_point} - 65536")
  math(EXPR high "0xD800 + (${rest} >> 10) - 65536")
  math(EXPR low "0xDC00 + (${rest} & 0x3FF) - 65536")
  set(${out_var} "\\u${high}?\\u${low}?" PARENT_SCOPE)
endfunction()

# One line of text, escaped for RTF: the three characters RTF reserves, and
# every UTF-8 sequence turned into its code point.
function(_dv_rtf_escape line out_var)
  string(HEX "${line}" hex)
  string(LENGTH "${hex}" hex_length)
  set(result "")
  set(pending 0)
  set(remaining 0)
  set(i 0)
  while(i LESS hex_length)
    string(SUBSTRING "${hex}" ${i} 2 pair)
    math(EXPR byte "0x${pair}")
    math(EXPR i "${i} + 2")
    if(remaining GREATER 0)
      math(EXPR pending "(${pending} << 6) | (${byte} & 0x3F)")
      math(EXPR remaining "${remaining} - 1")
      if(remaining EQUAL 0)
        _dv_rtf_unicode(${pending} escaped)
        string(APPEND result "${escaped}")
      endif()
    elseif(byte LESS 128)
      string(ASCII ${byte} ch)
      if(ch STREQUAL "\\")
        set(ch "\\\\")
      elseif(ch STREQUAL "{")
        set(ch "\\{")
      elseif(ch STREQUAL "}")
        set(ch "\\}")
      endif()
      string(APPEND result "${ch}")
    elseif(byte LESS 224)
      math(EXPR pending "${byte} & 0x1F")
      set(remaining 1)
    elseif(byte LESS 240)
      math(EXPR pending "${byte} & 0x0F")
      set(remaining 2)
    else()
      math(EXPR pending "${byte} & 0x07")
      set(remaining 3)
    endif()
  endwhile()
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

function(dv_changelog_to_rtf markdown rtf)
  file(READ "${markdown}" text)

  # Tahoma at 9 points, the face the WiX dialogs around it use. Half points in
  # \fs, twips in the indents.
  set(out "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fnil\\fcharset0 Tahoma;}}\\f0\\fs18\n")
  set(in_body FALSE)

  # Line by line without turning the text into a list, because a CMake list
  # splits on semicolons and an entry is allowed to contain one.
  while(TRUE)
    string(FIND "${text}" "\n" newline)
    if(newline EQUAL -1)
      set(line "${text}")
      set(text "")
    else()
      string(SUBSTRING "${text}" 0 ${newline} line)
      math(EXPR after "${newline} + 1")
      string(SUBSTRING "${text}" ${after} -1 text)
    endif()
    # A checkout with core.autocrlf leaves the file in CRLF.
    string(REGEX REPLACE "\r$" "" line "${line}")

    if(line MATCHES "^## (.*)$")
      set(in_body TRUE)
      _dv_rtf_escape("${CMAKE_MATCH_1}" escaped)
      string(APPEND out "\\pard\\sb160\\sa40\\b ${escaped}\\b0\\par\n")
    elseif(NOT in_body)
      # The preface is for whoever opens the file, not for the installer.
    elseif(line MATCHES "^- (.*)$")
      _dv_rtf_escape("${CMAKE_MATCH_1}" escaped)
      string(APPEND out "\\pard\\li230\\fi-230\\tx230\\sb20\\bullet\\tab ${escaped}\\par\n")
    elseif(NOT line STREQUAL "")
      _dv_rtf_escape("${line}" escaped)
      string(APPEND out "\\pard\\sb20 ${escaped}\\par\n")
    endif()

    if(text STREQUAL "")
      break()
    endif()
  endwhile()

  string(APPEND out "}\n")
  file(WRITE "${rtf}" "${out}")
endfunction()
