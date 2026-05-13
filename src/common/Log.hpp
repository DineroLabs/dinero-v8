#pragma once
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace logd {
inline std::mutex& mtx() { static std::mutex m; return m; }
inline const char* lvl(const char* s) { return s; }
inline std::string ts() {
  using namespace std::chrono;
  auto now = system_clock::now();
  std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}
inline void write(const char* level, const std::string& msg) {
  std::lock_guard<std::mutex> lk(mtx());
  std::clog << "[" << ts() << "] [" << level << "] " << msg << '\n';
}
} // namespace logd

#ifdef DINERO_USE_QT_LOG
  #include <QtCore/QLoggingCategory>
  #define LOG_I(msg)  qInfo().noquote()    << QString::fromStdString(msg)
  #define LOG_W(msg)  qWarning().noquote() << QString::fromStdString(msg)
  #define LOG_E(msg)  qCritical().noquote()<< QString::fromStdString(msg)
  #define LOG_D(msg)  qDebug().noquote()   << QString::fromStdString(msg)
#else
  #define LOG_I(msg)  ::logd::write("INFO",  (msg))
  #define LOG_W(msg)  ::logd::write("WARN",  (msg))
  #define LOG_E(msg)  ::logd::write("ERROR", (msg))
  #define LOG_D(msg)  ::logd::write("DEBUG", (msg))
#endif
