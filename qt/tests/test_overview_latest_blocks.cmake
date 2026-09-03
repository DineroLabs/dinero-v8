if(NOT DEFINED SOURCE_FILE OR NOT EXISTS "${SOURCE_FILE}")
  message(FATAL_ERROR "SOURCE_FILE must name mainwindow.cpp")
endif()
if(NOT DEFINED HEADER_FILE OR NOT EXISTS "${HEADER_FILE}")
  message(FATAL_ERROR "HEADER_FILE must name mainwindow.h")
endif()

file(READ "${SOURCE_FILE}" source)
file(READ "${HEADER_FILE}" header)

foreach(required
    "auto* blocksCard = new QGroupBox(\"Latest Blocks\")"
    "overviewBlocksLayout_->addWidget(tblRecentBlocks_)"
    "connect(btnOpenExplorer, &QPushButton::clicked, this, &MainWindow::showExplorerWindow)"
    "explorerWindow_ = makeScrollableTab(explorer)"
    "void MainWindow::showExplorerWindow()"
    "const int rowCount = std::min(6, height + 1)")
  string(FIND "${source}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing Overview/latest-blocks behavior: ${required}")
  endif()
endforeach()

string(FIND "${source}" "tabs->addTab(makeScrollableTab(explorer), \"Explorer\")" explorer_tab)
if(NOT explorer_tab EQUAL -1)
  message(FATAL_ERROR "Explorer was reintroduced as a top-level tab")
endif()

foreach(required
    "void showExplorerWindow();"
    "QWidget* explorerWindow_ = nullptr;"
    "QVBoxLayout* overviewBlocksLayout_ = nullptr;")
  string(FIND "${header}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing Overview/latest-blocks declaration: ${required}")
  endif()
endforeach()
