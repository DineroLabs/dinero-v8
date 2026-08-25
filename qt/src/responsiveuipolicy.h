#pragma once

#include <QJsonArray>

#include <algorithm>

namespace dinero::qt {

inline constexpr int kUtxoPageSize = 200;

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

} // namespace dinero::qt
