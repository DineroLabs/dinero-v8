# Guards the terms that must read identically in every language.
#
# UTXO and Covenants are protocol vocabulary. They name exact things in the
# consensus rules, and no target language has an accepted equivalent. A
# localized rendering does not translate them, it describes something else:
# "Bedingungen" for Covenants means general conditions, which is not what a
# covenant is. A user reading a translated interface would be looking at a
# different concept than the one the protocol implements.
#
# So these are not left to each translator's judgment and a glossary they may
# never read. Every catalog is checked: a universal term must either be absent
# (falling back to English) or translated to itself.
#
# Run standalone:
#   cmake -DTRANSLATIONS_DIR=<repo>/qt/translations \
#         -P test_universal_terms_untranslated.cmake

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED TRANSLATIONS_DIR)
  message(FATAL_ERROR "TRANSLATIONS_DIR must be set")
endif()

# Fixed in EVERY language. Adding to this list is a protocol-vocabulary
# decision, not a translation preference.
set(_universal_terms "UTXO" "UTXOs" "Covenants" "Covenant")

file(GLOB _catalogs "${TRANSLATIONS_DIR}/*.ts")
list(LENGTH _catalogs _catalog_count)
if(_catalog_count EQUAL 0)
  message(FATAL_ERROR "no .ts catalogs found in ${TRANSLATIONS_DIR}")
endif()

set(_failures "")
set(_checked 0)

foreach(_catalog IN LISTS _catalogs)
  get_filename_component(_catalog_name "${_catalog}" NAME)
  file(READ "${_catalog}" _text)
  # Collapse whitespace so a <source>/<translation> pair is matchable on one line.
  string(REGEX REPLACE "[\n\r\t]+" " " _text "${_text}")
  string(REGEX REPLACE "  +" " " _text "${_text}")

  foreach(_term IN LISTS _universal_terms)
    # Only exact whole-string entries: a sentence that merely mentions UTXO is
    # ordinary prose and must stay translatable.
    string(REGEX MATCHALL
      "<source>${_term}</source> <translation[^>]*>[^<]*</translation>"
      _pairs "${_text}")
    foreach(_pair IN LISTS _pairs)
      math(EXPR _checked "${_checked}+1")
      string(REGEX REPLACE ".*<translation[^>]*>([^<]*)</translation>.*" "\\1" _value "${_pair}")
      # Empty means unfinished, which falls back to English. That is allowed.
      if(NOT _value STREQUAL "" AND NOT _value STREQUAL "${_term}")
        list(APPEND _failures
             "${_catalog_name}: universal term '${_term}' was translated to '${_value}' - it must stay '${_term}' in every language")
      endif()
    endforeach()
  endforeach()
endforeach()

list(LENGTH _failures _fail_count)
if(_fail_count GREATER 0)
  foreach(_f IN LISTS _failures)
    message(SEND_ERROR "  ${_f}")
  endforeach()
  message(FATAL_ERROR "universal-term guard FAILED (${_fail_count} problem(s))")
endif()

message(STATUS "universal-term guard: ${_checked} entr(ies) across ${_catalog_count} catalog(s) keep UTXO/Covenants in English")
