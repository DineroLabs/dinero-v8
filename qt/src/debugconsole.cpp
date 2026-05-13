#include "debugconsole.h"
#include <QLabel>
#include <QTextStream>
#include <QScrollBar>

namespace dinero {

DebugConsole::DebugConsole(QWidget* parent)
  : QWidget(parent),
    daemonScrollPaused_(false),
    minerScrollPaused_(false),
    guiScrollPaused_(false),
    daemonFilterLevel_(LogLevel::INFO),
    minerFilterLevel_(LogLevel::INFO),
    guiFilterLevel_(LogLevel::INFO) {
  setupUi();

  // Log initial startup message
  logGuiEvent(LogLevel::INFO, "Debug Console initialized");
}

void DebugConsole::setupUi() {
  auto* mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(5, 5, 5, 5);
  mainLayout->setSpacing(5);

  // Create tab widget
  tabWidget_ = new QTabWidget(this);

  //
  // ═══════════════════════ DAEMON TAB ═══════════════════════
  //
  {
    auto* daemonTab = new QWidget();
    auto* layout = new QVBoxLayout(daemonTab);

    // Toolbar
    auto* toolbar = new QHBoxLayout();
    auto* label = new QLabel("Log Level:");
    daemonFilterCombo_ = new QComboBox();
    daemonFilterCombo_->addItem("DEBUG", (int)LogLevel::DEBUG);
    daemonFilterCombo_->addItem("INFO", (int)LogLevel::INFO);
    daemonFilterCombo_->addItem("WARNING", (int)LogLevel::WARNING);
    daemonFilterCombo_->addItem("ERROR", (int)LogLevel::ERROR);
    daemonFilterCombo_->setCurrentIndex(1); // Default to INFO

    pauseDaemonBtn_ = new QPushButton("Pause Scroll");
    pauseDaemonBtn_->setCheckable(true);
    clearDaemonBtn_ = new QPushButton("Clear");
    exportDaemonBtn_ = new QPushButton("Export...");

    toolbar->addWidget(label);
    toolbar->addWidget(daemonFilterCombo_);
    toolbar->addStretch();
    toolbar->addWidget(pauseDaemonBtn_);
    toolbar->addWidget(clearDaemonBtn_);
    toolbar->addWidget(exportDaemonBtn_);

    // Log text edit
    daemonLog_ = new QTextEdit();
    daemonLog_->setReadOnly(true);
    daemonLog_->setFont(QFont("Courier", 10));
    daemonLog_->document()->setMaximumBlockCount(10000); // Limit to 10k lines

    layout->addLayout(toolbar);
    layout->addWidget(daemonLog_);

    tabWidget_->addTab(daemonTab, "Daemon Logs");

    // Connect signals
    connect(pauseDaemonBtn_, &QPushButton::toggled, this, [this](bool checked) {
      daemonScrollPaused_ = checked;
      pauseDaemonBtn_->setText(checked ? "Resume Scroll" : "Pause Scroll");
    });
    connect(clearDaemonBtn_, &QPushButton::clicked, this, [this]() {
      daemonLog_->clear();
      logDaemonOutput("[Logs cleared by user]");
    });
    connect(exportDaemonBtn_, &QPushButton::clicked, this, [this]() {
      onExportLogsClicked();
    });
    connect(daemonFilterCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
      daemonFilterLevel_ = (LogLevel)daemonFilterCombo_->itemData(index).toInt();
    });
  }

  //
  // ═══════════════════════ MINER TAB ═══════════════════════
  //
  {
    auto* minerTab = new QWidget();
    auto* layout = new QVBoxLayout(minerTab);

    // Toolbar
    auto* toolbar = new QHBoxLayout();
    auto* label = new QLabel("Log Level:");
    minerFilterCombo_ = new QComboBox();
    minerFilterCombo_->addItem("DEBUG", (int)LogLevel::DEBUG);
    minerFilterCombo_->addItem("INFO", (int)LogLevel::INFO);
    minerFilterCombo_->addItem("WARNING", (int)LogLevel::WARNING);
    minerFilterCombo_->addItem("ERROR", (int)LogLevel::ERROR);
    minerFilterCombo_->setCurrentIndex(1);

    pauseMinerBtn_ = new QPushButton("Pause Scroll");
    pauseMinerBtn_->setCheckable(true);
    clearMinerBtn_ = new QPushButton("Clear");
    exportMinerBtn_ = new QPushButton("Export...");

    toolbar->addWidget(label);
    toolbar->addWidget(minerFilterCombo_);
    toolbar->addStretch();
    toolbar->addWidget(pauseMinerBtn_);
    toolbar->addWidget(clearMinerBtn_);
    toolbar->addWidget(exportMinerBtn_);

    // Log text edit
    minerLog_ = new QTextEdit();
    minerLog_->setReadOnly(true);
    minerLog_->setFont(QFont("Courier", 10));
    minerLog_->document()->setMaximumBlockCount(10000);

    layout->addLayout(toolbar);
    layout->addWidget(minerLog_);

    tabWidget_->addTab(minerTab, "Miner Logs");

    // Connect signals
    connect(pauseMinerBtn_, &QPushButton::toggled, this, [this](bool checked) {
      minerScrollPaused_ = checked;
      pauseMinerBtn_->setText(checked ? "Resume Scroll" : "Pause Scroll");
    });
    connect(clearMinerBtn_, &QPushButton::clicked, this, [this]() {
      minerLog_->clear();
      logMinerOutput("[Logs cleared by user]");
    });
    connect(exportMinerBtn_, &QPushButton::clicked, this, [this]() {
      onExportLogsClicked();
    });
    connect(minerFilterCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
      minerFilterLevel_ = (LogLevel)minerFilterCombo_->itemData(index).toInt();
    });
  }

  //
  // ═══════════════════════ GUI TAB ═══════════════════════
  //
  {
    auto* guiTab = new QWidget();
    auto* layout = new QVBoxLayout(guiTab);

    // Toolbar
    auto* toolbar = new QHBoxLayout();
    auto* label = new QLabel("Log Level:");
    guiFilterCombo_ = new QComboBox();
    guiFilterCombo_->addItem("DEBUG", (int)LogLevel::DEBUG);
    guiFilterCombo_->addItem("INFO", (int)LogLevel::INFO);
    guiFilterCombo_->addItem("WARNING", (int)LogLevel::WARNING);
    guiFilterCombo_->addItem("ERROR", (int)LogLevel::ERROR);
    guiFilterCombo_->setCurrentIndex(1);

    pauseGuiBtn_ = new QPushButton("Pause Scroll");
    pauseGuiBtn_->setCheckable(true);
    clearGuiBtn_ = new QPushButton("Clear");
    exportGuiBtn_ = new QPushButton("Export...");

    toolbar->addWidget(label);
    toolbar->addWidget(guiFilterCombo_);
    toolbar->addStretch();
    toolbar->addWidget(pauseGuiBtn_);
    toolbar->addWidget(clearGuiBtn_);
    toolbar->addWidget(exportGuiBtn_);

    // Log text edit
    guiLog_ = new QTextEdit();
    guiLog_->setReadOnly(true);
    guiLog_->setFont(QFont("Courier", 10));
    guiLog_->document()->setMaximumBlockCount(10000);

    layout->addLayout(toolbar);
    layout->addWidget(guiLog_);

    tabWidget_->addTab(guiTab, "GUI Logs");

    // Connect signals
    connect(pauseGuiBtn_, &QPushButton::toggled, this, [this](bool checked) {
      guiScrollPaused_ = checked;
      pauseGuiBtn_->setText(checked ? "Resume Scroll" : "Pause Scroll");
    });
    connect(clearGuiBtn_, &QPushButton::clicked, this, [this]() {
      guiLog_->clear();
      logGuiEvent(LogLevel::INFO, "[Logs cleared by user]");
    });
    connect(exportGuiBtn_, &QPushButton::clicked, this, [this]() {
      onExportLogsClicked();
    });
    connect(guiFilterCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
      guiFilterLevel_ = (LogLevel)guiFilterCombo_->itemData(index).toInt();
    });
  }

  mainLayout->addWidget(tabWidget_);

  // Set window properties
  setWindowTitle("Debug Console - Live Logs");
  resize(900, 600);
}

void DebugConsole::logMessage(LogSource source, LogLevel level, const QString& message) {
  switch (source) {
    case LogSource::DAEMON:
      if (level >= daemonFilterLevel_) {
        appendLogLine(daemonLog_, level, message);
      }
      break;
    case LogSource::MINER:
      if (level >= minerFilterLevel_) {
        appendLogLine(minerLog_, level, message);
      }
      break;
    case LogSource::GUI:
      if (level >= guiFilterLevel_) {
        appendLogLine(guiLog_, level, message);
      }
      break;
  }
}

void DebugConsole::logDaemonOutput(const QString& output) {
  // Parse daemon output and extract log level if present
  QString trimmed = output.trimmed();
  if (trimmed.isEmpty()) return;

  LogLevel level = LogLevel::INFO;

  // Detect log level from output
  if (trimmed.contains("ERROR", Qt::CaseInsensitive) ||
      trimmed.contains("FATAL", Qt::CaseInsensitive) ||
      trimmed.contains("CRITICAL", Qt::CaseInsensitive)) {
    level = LogLevel::ERROR;
  } else if (trimmed.contains("WARN", Qt::CaseInsensitive) ||
             trimmed.contains("WARNING", Qt::CaseInsensitive)) {
    level = LogLevel::WARNING;
  } else if (trimmed.contains("DEBUG", Qt::CaseInsensitive)) {
    level = LogLevel::DEBUG;
  }

  logMessage(LogSource::DAEMON, level, trimmed);
}

void DebugConsole::logMinerOutput(const QString& output) {
  QString trimmed = output.trimmed();
  if (trimmed.isEmpty()) return;

  LogLevel level = LogLevel::INFO;

  // Detect log level
  if (trimmed.contains("ERROR", Qt::CaseInsensitive) ||
      trimmed.contains("FAIL", Qt::CaseInsensitive)) {
    level = LogLevel::ERROR;
  } else if (trimmed.contains("WARN", Qt::CaseInsensitive)) {
    level = LogLevel::WARNING;
  } else if (trimmed.contains("DEBUG", Qt::CaseInsensitive) ||
             trimmed.contains("HASH", Qt::CaseInsensitive)) {
    level = LogLevel::DEBUG;
  }

  logMessage(LogSource::MINER, level, trimmed);
}

void DebugConsole::logGuiEvent(LogLevel level, const QString& event) {
  logMessage(LogSource::GUI, level, event);
}

void DebugConsole::appendLogLine(QTextEdit* textEdit, LogLevel level, const QString& message) {
  if (!textEdit) return;

  // Format: [HH:MM:SS] [LEVEL] Message
  QString timestamp = formatTimestamp();
  QString levelStr = getLevelString(level);
  QString color = getColorForLevel(level);

  QString formattedLine = QString("<span style='color: gray;'>%1</span> "
                                  "<span style='color: %2; font-weight: bold;'>[%3]</span> "
                                  "<span>%4</span>")
                             .arg(timestamp)
                             .arg(color)
                             .arg(levelStr)
                             .arg(message.toHtmlEscaped());

  textEdit->append(formattedLine);

  // Auto-scroll to bottom if not paused
  bool shouldScroll = false;
  if (textEdit == daemonLog_ && !daemonScrollPaused_) shouldScroll = true;
  if (textEdit == minerLog_ && !minerScrollPaused_) shouldScroll = true;
  if (textEdit == guiLog_ && !guiScrollPaused_) shouldScroll = true;

  if (shouldScroll) {
    QScrollBar* scrollBar = textEdit->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
  }
}

QString DebugConsole::formatTimestamp() const {
  return QDateTime::currentDateTime().toString("[HH:mm:ss]");
}

QString DebugConsole::getColorForLevel(LogLevel level) const {
  switch (level) {
    case LogLevel::DEBUG:   return "#808080"; // Gray
    case LogLevel::INFO:    return "#00AA00"; // Green
    case LogLevel::WARNING: return "#FFA500"; // Orange
    case LogLevel::ERROR:   return "#FF0000"; // Red
    default:                return "#000000"; // Black
  }
}

QString DebugConsole::getLevelString(LogLevel level) const {
  switch (level) {
    case LogLevel::DEBUG:   return "DEBUG";
    case LogLevel::INFO:    return "INFO ";
    case LogLevel::WARNING: return "WARN ";
    case LogLevel::ERROR:   return "ERROR";
    default:                return "?????";
  }
}

void DebugConsole::onPauseScrollClicked() {
  // Handled by lambda in setupUi()
}

void DebugConsole::onClearLogsClicked() {
  // Handled by lambda in setupUi()
}

void DebugConsole::onExportLogsClicked() {
  // Determine which tab is active
  int currentIndex = tabWidget_->currentIndex();
  QString tabName;
  QTextEdit* logWidget = nullptr;

  switch (currentIndex) {
    case 0: // Daemon
      tabName = "daemon";
      logWidget = daemonLog_;
      break;
    case 1: // Miner
      tabName = "miner";
      logWidget = minerLog_;
      break;
    case 2: // GUI
      tabName = "gui";
      logWidget = guiLog_;
      break;
    default:
      return;
  }

  // Prompt user for file location
  QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
  QString defaultName = QString("dinero_%1_logs_%2.txt").arg(tabName).arg(timestamp);

  QString fileName = QFileDialog::getSaveFileName(
    this,
    "Export Logs",
    defaultName,
    "Text Files (*.txt);;All Files (*)"
  );

  if (fileName.isEmpty()) return;

  // Write logs to file
  QFile file(fileName);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    QMessageBox::warning(this, "Export Failed",
      QString("Could not write to file:\n%1").arg(fileName));
    return;
  }

  QTextStream out(&file);
  out << logWidget->toPlainText();
  file.close();

  QMessageBox::information(this, "Export Successful",
    QString("Logs exported to:\n%1").arg(fileName));

  logGuiEvent(LogLevel::INFO, QString("Exported %1 logs to %2").arg(tabName).arg(fileName));
}

void DebugConsole::onFilterChanged(int index) {
  // Handled by lambdas in setupUi()
}

} // namespace dinero
