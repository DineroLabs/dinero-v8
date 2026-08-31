file(READ "${HEADER_FILE}" MAINWINDOW_HEADER)

string(FIND "${MAINWINDOW_HEADER}"
  "QTextEdit* txtMiningOutput_ = nullptr;"
  POINTER_INITIALIZER_POSITION)
if(POINTER_INITIALIZER_POSITION EQUAL -1)
  message(FATAL_ERROR
    "txtMiningOutput_ must be initialized to nullptr: QTabWidget emits "
    "currentChanged while setupUI() is still constructing its tabs")
endif()
