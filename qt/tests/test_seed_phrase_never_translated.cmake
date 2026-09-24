# Guards the rule that a recovery phrase is English for every user, in every
# interface language.
#
# Why this is a test and not a comment. A BIP39 phrase is only recoverable
# against the wordlist it was generated from. If an interface translation ever
# reached the phrase — by shipping a localized wordlist, or by passing the
# phrase through tr() — a user who switched language could be shown words that
# do not restore their wallet, or could have a phrase rejected that is actually
# correct. That is a fund-loss defect, so it gets a gate rather than trust.
#
# Today the property holds structurally: the phrase arrives from the daemon over
# RPC as runtime data, and tr() can only ever act on compile-time literals. This
# test exists to keep that true as the translation work spreads through the UI.
#
# Run standalone:
#   cmake -DQT_SRC_DIR=<repo>/qt/src -DWALLET_SRC_DIR=<repo>/src/wallet \
#         -P test_seed_phrase_never_translated.cmake

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED QT_SRC_DIR OR NOT DEFINED WALLET_SRC_DIR)
  message(FATAL_ERROR "QT_SRC_DIR and WALLET_SRC_DIR must both be set")
endif()

set(_failures "")

# ---------------------------------------------------------------------------
# 1. The phrase must never be passed through tr().
#    tr() on a variable is the only way source code could translate runtime
#    data, so any tr(<something mentioning seed/mnemonic>) is a violation.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE _qt_sources "${QT_SRC_DIR}/*.cpp" "${QT_SRC_DIR}/*.h")
foreach(_file IN LISTS _qt_sources)
  file(STRINGS "${_file}" _hits REGEX "tr[ \t]*\\([ \t]*[A-Za-z_]*([Ss]eed|[Mm]nemonic)")
  foreach(_hit IN LISTS _hits)
    get_filename_component(_name "${_file}" NAME)
    list(APPEND _failures "${_name}: recovery phrase passed through tr(): ${_hit}")
  endforeach()
endforeach()

# ---------------------------------------------------------------------------
# 2. No wordlist may live in the GUI, where it could be marked translatable.
#    A wordlist shows up as a long run of short quoted lowercase words.
# ---------------------------------------------------------------------------
foreach(_file IN LISTS _qt_sources)
  file(STRINGS "${_file}" _wordish REGEX "^[ \t]*\"[a-z]{3,8}\",[ \t]*\"[a-z]{3,8}\",[ \t]*\"[a-z]{3,8}\",")
  list(LENGTH _wordish _n)
  if(_n GREATER 3)
    get_filename_component(_name "${_file}" NAME)
    list(APPEND _failures "${_name}: looks like an embedded wordlist (${_n} lines); a wordlist must not live in translatable UI code")
  endif()
endforeach()

# ---------------------------------------------------------------------------
# 3. Exactly one wordlist, and it is the English one. A second language's
#    wordlist appearing beside it is the change this rule forbids.
# ---------------------------------------------------------------------------
file(GLOB _wordlists "${WALLET_SRC_DIR}/bip39_*wordlist*.txt")
list(LENGTH _wordlists _wl_count)
if(NOT _wl_count EQUAL 1)
  list(APPEND _failures "expected exactly one BIP39 wordlist, found ${_wl_count}: ${_wordlists}")
else()
  list(GET _wordlists 0 _wl)
  get_filename_component(_wl_name "${_wl}" NAME)
  if(NOT _wl_name MATCHES "english")
    list(APPEND _failures "the only BIP39 wordlist is not the English one: ${_wl_name}")
  endif()
endif()

list(LENGTH _failures _fail_count)
if(_fail_count GREATER 0)
  foreach(_f IN LISTS _failures)
    message(SEND_ERROR "  ${_f}")
  endforeach()
  message(FATAL_ERROR "seed-phrase translation guard FAILED (${_fail_count} problem(s))")
endif()

message(STATUS "seed-phrase guard: phrase never translated, no wordlist in the UI, one English wordlist")
