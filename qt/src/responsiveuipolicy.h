#pragma once

#include <QJsonArray>
#include <QString>

#include <algorithm>

namespace dinero::qt {

inline constexpr int kUtxoPageSize = 200;
inline constexpr int kHashEngineIntervalMs = 100;
inline constexpr int kBlockFoundHighlightMs = 1000;

struct UtxoPage {
  QJsonArray rows;
  int total_rows = 0;
  int page_index = 0;
  int page_count = 1;
  int first_row = 0;
};

inline UtxoPage paginateUtxos(const QJsonArray& rows,
                              int requested_page,
                              int page_size = kUtxoPageSize) {
  UtxoPage result;
  result.total_rows = rows.size();
  page_size = std::max(1, page_size);
  result.page_count = std::max(1, (result.total_rows + page_size - 1) / page_size);
  result.page_index = std::clamp(requested_page, 0, result.page_count - 1);
  result.first_row = result.page_index * page_size;
  const int end = std::min(result.total_rows, result.first_row + page_size);
  for (int i = result.first_row; i < end; ++i) {
    result.rows.append(rows.at(i));
  }
  return result;
}

inline bool shouldPollUtxos(bool utxo_tab_active, bool explicit_refresh) {
  return utxo_tab_active || explicit_refresh;
}

inline bool shouldRunMiningCinematic(bool mining, bool mining_tab_active) {
  return mining && mining_tab_active;
}

inline bool shouldRunHashEngine(bool mining, bool mining_tab_active,
                                bool /*block_highlight_active*/) {
  // A found block changes row styling only. It must never pause hashing or
  // the live visualization.
  return mining && mining_tab_active;
}

inline int hashSampleCapacity(int viewport_height, int row_height) {
  return std::max(1, (std::max(0, viewport_height) - 12) /
                         std::max(1, row_height));
}

inline QString compactDifficultyText(quint32 bits) {
  return bits == 0 ? QStringLiteral("-")
                   : QString("0x%1").arg(bits, 8, 16, QChar('0'));
}

} // namespace dinero::qt
