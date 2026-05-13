#pragma once

#include <QWidget>
#include <QTextEdit>
#include <QTabWidget>
#include <QPushButton>
#include <QComboBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include <QFileDialog>
#include <QMessageBox>

namespace dinero {

/**
 * @brief Live log viewer for daemon, miner, and GUI events
 *
 * This widget provides real-time log viewing with:
 * - Three separate tabs (Daemon, Miner, GUI)
 * - Auto-scrolling with pause/resume
 * - Log level filtering (INFO, WARNING, ERROR, DEBUG)
 * - Export to file
 * - Clear logs
 * - Color-coded output
 */
class DebugConsole : public QWidget {
  Q_OBJECT
  Q_DISABLE_COPY(DebugConsole)

public:
  explicit DebugConsole(QWidget* parent = nullptr);
  ~DebugConsole() override = default;

  enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR
  };

  enum class LogSource {
    DAEMON,
    MINER,
    GUI
  };

public Q_SLOTS:
  /// Log a message to the appropriate source tab
  void logMessage(LogSource source, LogLevel level, const QString& message);

  /// Log daemon output (stdout/stderr)
  void logDaemonOutput(const QString& output);

  /// Log miner output
  void logMinerOutput(const QString& output);

  /// Log GUI event
  void logGuiEvent(LogLevel level, const QString& event);

private Q_SLOTS:
  void onPauseScrollClicked();
  void onClearLogsClicked();
  void onExportLogsClicked();
  void onFilterChanged(int index);

private:
  void setupUi();
  void appendLogLine(QTextEdit* textEdit, LogLevel level, const QString& message);
  QString formatTimestamp() const;
  QString getColorForLevel(LogLevel level) const;
  QString getLevelString(LogLevel level) const;

  // UI components
  QTabWidget* tabWidget_;

  // Daemon tab
  QTextEdit* daemonLog_;
  QPushButton* pauseDaemonBtn_;
  QPushButton* clearDaemonBtn_;
  QPushButton* exportDaemonBtn_;
  QComboBox* daemonFilterCombo_;
  bool daemonScrollPaused_;

  // Miner tab
  QTextEdit* minerLog_;
  QPushButton* pauseMinerBtn_;
  QPushButton* clearMinerBtn_;
  QPushButton* exportMinerBtn_;
  QComboBox* minerFilterCombo_;
  bool minerScrollPaused_;

  // GUI tab
  QTextEdit* guiLog_;
  QPushButton* pauseGuiBtn_;
  QPushButton* clearGuiBtn_;
  QPushButton* exportGuiBtn_;
  QComboBox* guiFilterCombo_;
  bool guiScrollPaused_;

  // Current filter levels
  LogLevel daemonFilterLevel_;
  LogLevel minerFilterLevel_;
  LogLevel guiFilterLevel_;
};

} // namespace dinero
