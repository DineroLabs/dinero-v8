file(READ "${SOURCE_FILE}" MAINWINDOW_SOURCE)

string(FIND "${MAINWINDOW_SOURCE}" "palette(base)" PALETTE_BASE_POSITION)
if(NOT PALETTE_BASE_POSITION EQUAL -1)
  message(FATAL_ERROR
    "Mining output must not inherit palette(base); a light Windows palette "
    "would make its near-white text unreadable")
endif()

foreach(REQUIRED_SNIPPET IN ITEMS
    "background-color: %3"
    "miningOutputPalette.setColor(QPalette::Base, QColor(kMiningOutputIdleBackground))"
    "miningOutputPalette.setColor(QPalette::Window, QColor(kMiningOutputIdleBackground))"
    "miningOutputPalette.setColor(QPalette::Text, QColor(kMiningOutputTextColor))"
    "miningOutputViewport->setPalette(miningOutputPalette)")
  string(FIND "${MAINWINDOW_SOURCE}" "${REQUIRED_SNIPPET}" SNIPPET_POSITION)
  if(SNIPPET_POSITION EQUAL -1)
    message(FATAL_ERROR
      "Mining output style is missing required explicit color assignment: "
      "${REQUIRED_SNIPPET}")
  endif()
endforeach()
