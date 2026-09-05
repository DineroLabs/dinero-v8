// Copyright (c) 2026 Dinero Labs.

#include "poolpanel.h"

#include "rpcclient.h"

#include <QFormLayout>
#include <QCryptographicHash>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <QVBoxLayout>

namespace {

constexpr qint64 kUnaPerDin = 100000000;
/// Marks which request a reply belongs to. One QNetworkAccessManager serves
/// both the status GET and the payout POST, and `finished` fires for both.
constexpr QNetworkRequest::Attribute kKindAttr = QNetworkRequest::User;
constexpr int kKindStatus = 0;
constexpr int kKindPayout = 1;
constexpr int kKindFee = 2;

std::optional<qint64> strictInt(const QJsonValue& v) {
    if (v.isDouble()) {
        const double number = v.toDouble();
        constexpr double kQint64UpperExclusive = 9223372036854775808.0;
        if (!std::isfinite(number) ||
            number < static_cast<double>(std::numeric_limits<qint64>::min()) ||
            number >= kQint64UpperExclusive) {
            return std::nullopt;
        }
        const qint64 integer = static_cast<qint64>(number);
        if (static_cast<double>(integer) == number) {
            return integer;
        }
    }
    if (v.isString()) {
        bool ok = false;
        const qint64 number = v.toString().toLongLong(&ok);
        if (ok) {
            return number;
        }
    }
    return std::nullopt;
}

QString valueOrUnavailable(const QJsonObject& object, const char* key) {
    const auto value = strictInt(object.value(QLatin1String(key)));
    return value ? QString::number(*value) : QStringLiteral("Unavailable");
}

QString sparkline(const QList<qint64>& values) {
    if (values.isEmpty()) return QStringLiteral("—");
    static const QString bars = QString::fromUtf8("▁▂▃▄▅▆▇█");
    const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
    QString out;
    for (const qint64 value : values) {
        const int index = *hi == *lo ? 3 : static_cast<int>((value - *lo) * 7 / (*hi - *lo));
        out += bars.at(std::clamp(index, 0, 7));
    }
    return out;
}

}  // namespace

QString PoolPanel::formatDin(qint64 una) {
    QLocale locale(QLocale::English);
    const qint64 whole = una / kUnaPerDin;
    const qint64 frac = una % kUnaPerDin;
    return QStringLiteral("%1.%2 DIN")
        .arg(locale.toString(whole), QStringLiteral("%1").arg(frac, 8, 10, QLatin1Char('0')));
}

PoolPanel::PoolPanel(RpcClient* rpc, QWidget* parent)
    : QWidget(parent), rpc_(rpc), net_(new QNetworkAccessManager(this)) {
    setupUi();
    setupConnections();
    restorePayoutJournal();
    restoreFeeJournal();
    refresh_timer_.start(REFRESH_INTERVAL_MS);
}

PoolPanel::~PoolPanel() = default;

void PoolPanel::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);
    root->setContentsMargins(12, 12, 12, 12);

    // 👥 rather than a pickaxe: the Mining tab owns that, and this panel
    // is about a group of miners sharing a block, not about hashing.
    auto* title = new QLabel("<h2>\xF0\x9F\x91\xA5 Pool</h2>");
    root->addWidget(title);

    // ---- Why host one -----------------------------------------------
    // This section is for the majority of users, who do not run a pool
    // and have no idea it is an option.
    why_group_ = new QGroupBox("Hosting a pool");
    auto* why_layout = new QVBoxLayout(why_group_);
    auto* invitation = new QLabel(
        "<span style='font-size:18px; font-weight:700;'>Bring the cockpit online.</span> "
        "Run your own pool, connect miners, and earn a transparent operator fee.");
    invitation->setWordWrap(true);
    invitation->setTextFormat(Qt::RichText);
    invitation->setStyleSheet("color: #d7e3ef;");
    why_layout->addWidget(invitation);
    auto* why = new QLabel(
        "<p style='margin-top:0;'>A Dinero pool pays every contributing miner "
        "<b>directly in the block's coinbase</b>, split by share weight, the moment a "
        "block is found.</p>"
        "<ul style='margin-left:-18px;'>"
        "<li><b>You never hold your miners' coins.</b> No balances, no payout run, "
        "nothing to lose or be blamed for losing.</li>"
        "<li><b>Your fee is provable.</b> It is an output in the block \xE2\x80\x94 any miner "
        "can verify what you took, without trusting you.</li>"
        "<li><b>You stop depending on someone else's server</b>, and you choose which "
        "transactions go in your blocks.</li>"
        "<li><b>You can pool with people who never have to trust you</b> \xE2\x80\x94 sharing "
        "variance without anyone holding anyone's funds.</li>"
        "</ul>"
        "<p>Hosting a pool means running a full node too: the pool gets block templates "
        "from your node and submits found blocks through it. That is the point \xE2\x80\x94 every "
        "pool operator is a node operator.</p>"
        "<p style='color:#9fb3c8;'>Setup is one command on a Linux server. See "
        "<code>docs/RUN-A-POOL.md</code> in the dinero-sv2 repository.</p>");
    why->setWordWrap(true);
    why->setTextFormat(Qt::RichText);
    why_layout->addWidget(why);
    why_group_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    root->addWidget(why_group_);

    // ---- Connect to your pool ---------------------------------------
    auto* conn_group = new QGroupBox("Your pool");
    auto* conn_layout = new QVBoxLayout(conn_group);
    auto* conn_hint = new QLabel(
        "Point this at your pool's read-only status endpoint. It is loopback-only on the "
        "pool host by design, so from another machine open an SSH tunnel first:"
        "<br/><code>ssh -N -L 4445:127.0.0.1:4445 you@your.host</code>");
    conn_hint->setWordWrap(true);
    conn_hint->setStyleSheet("color: #9fb3c8;");
    conn_layout->addWidget(conn_hint);

    // Left-aligned with expanding fields: QFormLayout otherwise centres the
    // rows and leaves the endpoint field too narrow to show its own default.
    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(10);
    ops_url_input_ = new QLineEdit("http://127.0.0.1:4445");
    ops_url_input_->setPlaceholderText("http://127.0.0.1:4445");
    ops_url_input_->setMinimumWidth(320);
    form->addRow("Status endpoint:", ops_url_input_);

    auto* tokenRow = new QHBoxLayout();
    ops_token_input_ = new QLineEdit();
    ops_token_input_->setEchoMode(QLineEdit::Password);
    ops_token_input_->setPlaceholderText("contents of /etc/dinero-sv2/ops-token");
    ops_token_input_->setMinimumWidth(320);
    tokenRow->addWidget(ops_token_input_, 1);
    btn_fetch_status_ = new QPushButton("Connect");
    btn_fetch_status_->setStyleSheet("font-weight: bold; padding: 6px 14px;");
    tokenRow->addWidget(btn_fetch_status_, 0);
    form->addRow("Ops token:", tokenRow);
    conn_layout->addLayout(form);

    lbl_status_message_ = new QLabel("\xE2\x80\x93");
    lbl_status_message_->setTextFormat(Qt::RichText);
    lbl_status_message_->setWordWrap(true);
    lbl_status_message_->setStyleSheet("color: #9fb3c8;");
    conn_layout->addWidget(lbl_status_message_);
    btn_about_ = new QPushButton("About trustless pooled mining");
    btn_about_->setFlat(true);
    btn_about_->setVisible(false);
    conn_layout->addWidget(btn_about_, 0, Qt::AlignLeft);
    conn_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    root->addWidget(conn_group);

    // ---- Live status -------------------------------------------------
    status_group_ = new QGroupBox("Live status");
    auto* grid = new QGridLayout(status_group_);
    // Bare QLabels inherit a filled background from the page style, which
    // makes captions read as disabled text inputs.
    status_group_->setStyleSheet("QGroupBox > QLabel { background: transparent; }");
    // Default spacing leaves ~85px between rows of one-line values, which
    // spreads six readings over an unreadable amount of screen.
    // Hug the content. Left expanding, the group takes all the slack the
    // page has and the grid shares it out between rows, which pushes six
    // one-line readings ~87px apart.
    status_group_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    grid->setContentsMargins(12, 8, 12, 10);
    grid->setVerticalSpacing(4);
    grid->setHorizontalSpacing(8);
    // Value sits next to its caption; the slack goes to the gutter between
    // the two label/value pairs, not between a label and its own number.
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 0);
    grid->setColumnStretch(3, 1);
    lbl_health_ = new QLabel("OFFLINE");
    lbl_health_->setStyleSheet("font-size:18px; font-weight:700; color:#e06c75;");
    lbl_health_detail_ = new QLabel("Not connected");
    grid->addWidget(lbl_health_, 0, 0);
    grid->addWidget(lbl_health_detail_, 0, 1, 1, 3);

    lbl_connected_miners_ = new QLabel("\xE2\x80\x93");
    lbl_fee_ = new QLabel("\xE2\x80\x93");
    lbl_window_ = new QLabel("\xE2\x80\x93");
    lbl_producer_ = new QLabel("\xE2\x80\x93");
    lbl_shares_ = new QLabel("\xE2\x80\x93");
    lbl_blocks_ = new QLabel("\xE2\x80\x93");
    lbl_daemon_ = new QLabel("Unavailable");
    lbl_stratum_ = new QLabel("Unavailable");
    grid->addWidget(new QLabel("Connected sessions:"), 1, 0);
    grid->addWidget(lbl_connected_miners_, 1, 1);
    grid->addWidget(new QLabel("Operator fee:"), 1, 2);
    grid->addWidget(lbl_fee_, 1, 3);
    grid->addWidget(new QLabel("PPLNS window:"), 2, 0);
    grid->addWidget(lbl_window_, 2, 1);
    grid->addWidget(new QLabel("Template producer:"), 2, 2);
    grid->addWidget(lbl_producer_, 2, 3);
    grid->addWidget(new QLabel("Shares (this run):"), 3, 0);
    grid->addWidget(lbl_shares_, 3, 1);
    grid->addWidget(new QLabel("Blocks found (this run):"), 3, 2);
    grid->addWidget(lbl_blocks_, 3, 3);
    grid->addWidget(new QLabel("Daemon:"), 4, 0);
    grid->addWidget(lbl_daemon_, 4, 1);
    grid->addWidget(new QLabel("Stratum:"), 4, 2);
    grid->addWidget(lbl_stratum_, 4, 3);

    // The address the pool is ACTUALLY paying, straight from /status. An
    // operator should never have to trust the unit file to know this.
    lbl_payout_current_ = new QLabel("\xE2\x80\x93");
    lbl_payout_current_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lbl_payout_current_->setStyleSheet("font-family: monospace;");
    grid->addWidget(new QLabel("Fee paid to:"), 5, 0);
    grid->addWidget(lbl_payout_current_, 5, 1, 1, 3);

    auto* change_row = new QHBoxLayout();
    payout_input_ = new QLineEdit();
    payout_input_->setPlaceholderText("din1p\xE2\x80\xA6 new fee address");
    change_row->addWidget(payout_input_, 1);
    btn_change_payout_ = new QPushButton("Change");
    change_row->addWidget(btn_change_payout_, 0);
    grid->addWidget(new QLabel("Change to:"), 6, 0);
    grid->addLayout(change_row, 6, 1, 1, 3);

    lbl_payout_message_ = new QLabel();
    lbl_payout_message_->setTextFormat(Qt::RichText);
    lbl_payout_message_->setWordWrap(true);
    lbl_payout_message_->setStyleSheet("color: #9fb3c8; font-size: 11px;");
    lbl_payout_message_->setVisible(false);
    grid->addWidget(lbl_payout_message_, 7, 0, 1, 4);

    auto* fee_row = new QHBoxLayout();
    fee_percent_input_ = new QDoubleSpinBox();
    fee_percent_input_->setRange(0.0, 100.0);
    fee_percent_input_->setDecimals(2);
    fee_percent_input_->setSingleStep(0.25);
    fee_percent_input_->setSuffix("%");
    fee_row->addWidget(fee_percent_input_);
    btn_change_fee_ = new QPushButton("Change fee");
    fee_row->addWidget(btn_change_fee_);
    fee_row->addStretch();
    grid->addWidget(new QLabel("Set operator fee:"), 8, 0);
    grid->addLayout(fee_row, 8, 1, 1, 3);
    lbl_fee_message_ = new QLabel();
    lbl_fee_message_->setTextFormat(Qt::RichText);
    lbl_fee_message_->setWordWrap(true);
    lbl_fee_message_->setVisible(false);
    grid->addWidget(lbl_fee_message_, 9, 0, 1, 4);

    lbl_last_share_ = new QLabel("Unavailable");
    lbl_last_block_ = new QLabel("Unavailable");
    lbl_rejections_ = new QLabel("None reported");
    lbl_rejections_->setWordWrap(true);
    grid->addWidget(new QLabel("Last accepted share:"), 10, 0);
    grid->addWidget(lbl_last_share_, 10, 1);
    grid->addWidget(new QLabel("Last block result:"), 10, 2);
    grid->addWidget(lbl_last_block_, 10, 3);
    grid->addWidget(new QLabel("Rejection reasons:"), 11, 0);
    grid->addWidget(lbl_rejections_, 11, 1, 1, 3);

    auto* history = new QGroupBox("Share activity history (stored locally)");
    auto* history_grid = new QGridLayout(history);
    lbl_history_5m_ = new QLabel("—");
    lbl_history_1h_ = new QLabel("—");
    lbl_history_24h_ = new QLabel("—");
    history_grid->addWidget(new QLabel("5 min"), 0, 0); history_grid->addWidget(lbl_history_5m_, 0, 1);
    history_grid->addWidget(new QLabel("1 hour"), 1, 0); history_grid->addWidget(lbl_history_1h_, 1, 1);
    history_grid->addWidget(new QLabel("24 hours"), 2, 0); history_grid->addWidget(lbl_history_24h_, 2, 1);
    grid->addWidget(history, 12, 0, 1, 4);

    miners_table_ = new QTableWidget(0, 3);
    miners_table_->setHorizontalHeaderLabels({"Contributor payout script", "Next-block share", "Window weight"});
    miners_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    miners_table_->verticalHeader()->setVisible(false);
    // The script is the long column; the two numeric ones get fixed widths
    // so the header stops truncating to "tributor payout sc".
    miners_table_->horizontalHeader()->setStretchLastSection(false);
    miners_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    miners_table_->setColumnWidth(1, 150);
    miners_table_->setColumnWidth(2, 150);
    miners_table_->verticalHeader()->setDefaultSectionSize(24);
    miners_table_->setMaximumHeight(24 * 5 + 28);
    auto* contributors = new QGroupBox("PPLNS contributors (not connected sessions)");
    auto* contributors_layout = new QVBoxLayout(contributors);
    contributors_layout->addWidget(miners_table_);
    grid->addWidget(contributors, 13, 0, 1, 4);
    // Hidden, not merely disabled. Most people looking at this tab do not
    // run a pool; showing them an empty status grid and an empty table is
    // noise that buries the one thing meant for them (the explainer above).
    status_group_->setVisible(false);
    root->addWidget(status_group_);

    // ---- Earnings, from the chain ------------------------------------
    auto* earn_group = new QGroupBox("Current unspent fee balance (verified on-chain)");
    auto* earn_layout = new QVBoxLayout(earn_group);
    auto* earn_hint = new QLabel(
        "Current confirmed, unspent outputs at this address, read from your node. This is not "
        "lifetime earnings: spent outputs are intentionally excluded.");
    earn_hint->setWordWrap(true);
    earn_hint->setStyleSheet("color: #9fb3c8;");
    earn_layout->addWidget(earn_hint);

    auto* earn_row = new QHBoxLayout();
    earn_row->addWidget(new QLabel("Fee address:"));
    fee_address_input_ = new QLineEdit();
    fee_address_input_->setPlaceholderText("the din1p... you passed as --payout-address");
    earn_row->addWidget(fee_address_input_);
    btn_check_earnings_ = new QPushButton("Check");
    earn_row->addWidget(btn_check_earnings_);
    earn_layout->addLayout(earn_row);

    lbl_earnings_ = new QLabel("\xE2\x80\x93");
    lbl_earnings_->setTextFormat(Qt::RichText);
    lbl_earnings_->setWordWrap(true);
    lbl_earnings_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    earn_layout->addWidget(lbl_earnings_);
    // Same reasoning as Live status: revealed once the user identifies as
    // an operator by hitting Connect.
    earn_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    earn_group->setVisible(false);
    earnings_group_ = earn_group;
    root->addWidget(earn_group);

    root->addStretch();
}

void PoolPanel::setupConnections() {
    connect(btn_fetch_status_, &QPushButton::clicked, this, &PoolPanel::onFetchStatusClicked);
    connect(btn_check_earnings_, &QPushButton::clicked, this, &PoolPanel::onCheckEarningsClicked);
    connect(btn_change_payout_, &QPushButton::clicked, this, &PoolPanel::onChangePayoutClicked);
    connect(btn_change_fee_, &QPushButton::clicked, this, &PoolPanel::onChangeFeeClicked);
    connect(btn_about_, &QPushButton::clicked, this, [this] {
        why_group_->setVisible(!why_group_->isVisible());
    });
    connect(payout_input_, &QLineEdit::returnPressed, this, &PoolPanel::onChangePayoutClicked);
    connect(net_, &QNetworkAccessManager::finished, this, &PoolPanel::onOpsReplyFinished);
    connect(&refresh_timer_, &QTimer::timeout, this, &PoolPanel::refresh);
    connect(rpc_, &RpcClient::rpcResult, this, &PoolPanel::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &PoolPanel::onRpcError);
}

void PoolPanel::refresh() {
    // Only poll once the operator has successfully asked at least once;
    // otherwise a user who never runs a pool gets an error every 15s.
    if (ops_attempted_ && !status_in_flight_ &&
        !ops_token_input_->text().trimmed().isEmpty()) {
        onFetchStatusClicked();
    }
}

void PoolPanel::setStatusMessage(const QString& html) {
    lbl_status_message_->setText(html);
}

bool PoolPanel::validateOpsUrl(const QUrl& url, QLabel* error_target) const {
    const QString scheme = url.scheme().toLower();
    const QString host = url.host().toLower();
    QHostAddress address;
    const bool numeric_loopback = address.setAddress(host) && address.isLoopback();
    const bool loopback = host == QStringLiteral("localhost") || numeric_loopback;
    if (!url.isValid() || host.isEmpty() || !url.userInfo().isEmpty() ||
        (scheme != QStringLiteral("https") &&
         !(scheme == QStringLiteral("http") && loopback))) {
        error_target->setText(
            "<span style='color:#e06c75;'>Use HTTPS, or plain HTTP only through a "
            "loopback/SSH-tunnel endpoint such as 127.0.0.1.</span>");
        return false;
    }
    return true;
}

void PoolPanel::onFetchStatusClicked() {
    if (status_in_flight_) {
        return;
    }
    const QString base = ops_url_input_->text().trimmed();
    const QString token = ops_token_input_->text().trimmed();
    if (base.isEmpty() || token.isEmpty()) {
        setStatusMessage("<span style='color:#d8a37b;'>Endpoint and token are both required.</span>");
        return;
    }
    ops_attempted_ = true;
    // They run a pool. Reveal the operator surfaces even if this particular
    // fetch fails, so they can see the error in context and retry.
    if (earnings_group_) {
        earnings_group_->setVisible(true);
    }
    QUrl url(base + (base.endsWith('/') ? "status" : "/status"));
    if (!validateOpsUrl(url, lbl_status_message_)) {
        return;
    }
    restorePayoutJournal();
    restoreFeeJournal();
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    req.setTransferTimeout(8000);
    req.setAttribute(kKindAttr, kKindStatus);
    status_in_flight_ = true;
    btn_fetch_status_->setEnabled(false);
    net_->get(req);
}

void PoolPanel::setPayoutMessage(const QString& html) {
    lbl_payout_message_->setText(html);
    lbl_payout_message_->setVisible(!html.isEmpty());
}

void PoolPanel::onChangePayoutClicked() {
    if (payout_in_flight_) {
        return;
    }
    const QString addr = payout_input_->text().trimmed();
    if (addr.isEmpty()) {
        setPayoutMessage("<span style='color:#d8a37b;'>Enter the address you want fees paid to.</span>");
        return;
    }
    if (addr == live_payout_address_) {
        setPayoutMessage("<span style='color:#9fb3c8;'>That is already the live address.</span>");
        return;
    }
    // Money-routing change: confirm explicitly, and name both addresses, so a
    // mis-paste is visible before it is sent rather than after.
    const auto choice = QMessageBox::question(
        this, "Change fee address",
        QString("Pay your operator fee to a different address?\n\n"
                "From:  %1\nTo:      %2\n\n"
                "Takes effect on the pool's next block template. Your miners' "
                "payouts are unaffected.")
            .arg(live_payout_address_.isEmpty() ? QString("(unknown)") : live_payout_address_, addr),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes) {
        return;
    }

    const QString base = ops_url_input_->text().trimmed();
    const QString token = ops_token_input_->text().trimmed();
    if (base.isEmpty() || token.isEmpty()) {
        setPayoutMessage("<span style='color:#d8a37b;'>Connect to the pool first.</span>");
        return;
    }
    QUrl url(base + (base.endsWith('/') ? "payout-address" : "/payout-address"));
    if (!validateOpsUrl(url, lbl_payout_message_)) {
        lbl_payout_message_->setVisible(true);
        return;
    }
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setAttribute(kKindAttr, kKindPayout);
    req.setTransferTimeout(8000);
    QJsonObject body;
    body["address"] = addr;
    if (!persistPayoutJournal(QStringLiteral("submitting"), live_payout_address_, addr)) {
        setPayoutMessage("<span style='color:#e06c75;'>Not sent: the local safety journal could not be saved.</span>");
        return;
    }
    setPayoutMessage("<span style='color:#9fb3c8;'>Asking the pool to verify that address\xE2\x80\xA6</span>");
    payout_in_flight_ = true;
    btn_change_payout_->setEnabled(false);
    net_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void PoolPanel::onChangeFeeClicked() {
    if (fee_in_flight_) return;
    const qint64 requested = qRound64(fee_percent_input_->value() * 100.0);
    if (live_fee_bps_ < 0) {
        lbl_fee_message_->setText("<span style='color:#d8a37b;'>Connect to the pool before changing its fee.</span>");
        lbl_fee_message_->setVisible(true);
        return;
    }
    if (requested == live_fee_bps_) {
        lbl_fee_message_->setText("That is already the live operator fee.");
        lbl_fee_message_->setVisible(true);
        return;
    }
    const auto choice = QMessageBox::question(
        this, "Change operator fee",
        QString("Change the fee applied to future shared-reward templates?\n\nFrom:  %1%\nTo:      %2%\n\nMiner payouts adjust on the next template. Already-issued templates are unchanged.")
            .arg(live_fee_bps_ / 100.0, 0, 'f', 2)
            .arg(requested / 100.0, 0, 'f', 2),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes) return;

    const QString base = ops_url_input_->text().trimmed();
    const QString token = ops_token_input_->text().trimmed();
    QUrl url(base + (base.endsWith('/') ? "fee-bps" : "/fee-bps"));
    if (base.isEmpty() || token.isEmpty() || !validateOpsUrl(url, lbl_fee_message_)) {
        lbl_fee_message_->setVisible(true);
        return;
    }
    if (!persistFeeJournal(QStringLiteral("submitting"), live_fee_bps_, requested)) {
        lbl_fee_message_->setText("<span style='color:#e06c75;'>Not sent: the local fee-policy journal could not be saved.</span>");
        lbl_fee_message_->setVisible(true);
        return;
    }
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setAttribute(kKindAttr, kKindFee);
    req.setTransferTimeout(8000);
    QJsonObject body;
    body["fee_bps"] = requested;
    fee_in_flight_ = true;
    btn_change_fee_->setEnabled(false);
    lbl_fee_message_->setText("Review accepted locally; asking the pool to apply the fee policy…");
    lbl_fee_message_->setVisible(true);
    net_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void PoolPanel::handleFeeReply(QNetworkReply* reply, int http) {
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    const QJsonObject object = doc.isObject() ? doc.object() : QJsonObject{};
    if (http == 403) {
        persistFeeJournal(QStringLiteral("rejected"), fee_journal_from_, fee_journal_to_);
        lbl_fee_message_->setText("<span style='color:#d8a37b;'>Runtime fee changes are disabled on this pool. Re-run its installer with <code>--allow-fee-change</code>.</span>");
    } else if (http == 401) {
        persistFeeJournal(QStringLiteral("rejected"), fee_journal_from_, fee_journal_to_);
        lbl_fee_message_->setText("<span style='color:#e06c75;'>Rejected: wrong ops token.</span>");
    } else if (http != 200) {
        persistFeeJournal(QStringLiteral("rejected"), fee_journal_from_, fee_journal_to_);
        lbl_fee_message_->setText(QString("<span style='color:#e06c75;'>Fee not changed: %1</span>")
            .arg(object.value("error").toString(QString("HTTP %1").arg(http)).toHtmlEscaped()));
    } else {
        const auto applied = strictInt(object.value("fee_bps"));
        if (!applied || *applied != fee_journal_to_) {
            persistFeeJournal(QStringLiteral("conflict"), fee_journal_from_, fee_journal_to_);
            lbl_fee_message_->setText("<span style='color:#e06c75;'>Safety conflict: the pool did not confirm the exact fee you reviewed. Further changes are locked pending inspection.</span>");
            btn_change_fee_->setEnabled(false);
        } else {
            live_fee_bps_ = *applied;
            persistFeeJournal(QStringLiteral("applied"), fee_journal_from_, *applied);
            lbl_fee_message_->setText("<span style='color:#8fbf7f;'>Operator fee changed for the next template and persisted across restart.</span>");
        }
    }
    lbl_fee_message_->setVisible(true);
    refresh();
}

void PoolPanel::handlePayoutReply(QNetworkReply* reply, int http) {
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};

    if (http == 403) {
        persistPayoutJournal(QStringLiteral("rejected"), journal_from_, journal_to_);
        // Not a failure the operator can fix from here — tell them the exact
        // switch, since the whole point is that it is off unless asked for.
        setPayoutMessage(
            "<span style='color:#d8a37b;'>This pool does not accept address changes.</span><br/>"
            "<span style='color:#9fb3c8;'>Re-run the installer with "
            "<code>--allow-payout-change</code>, or edit <code>--payout-address</code> in "
            "<code>/etc/systemd/system/dinero-sv2-pool.service</code> and restart. It is off by "
            "default because it lets whoever holds your ops token retarget your fee.</span>");
        return;
    }
    if (http == 401) {
        persistPayoutJournal(QStringLiteral("rejected"), journal_from_, journal_to_);
        setPayoutMessage("<span style='color:#e06c75;'>Rejected: wrong ops token.</span>");
        return;
    }
    if (http != 200) {
        persistPayoutJournal(QStringLiteral("rejected"), journal_from_, journal_to_);
        const QString why = obj.value("error").toString();
        setPayoutMessage(QString("<span style='color:#e06c75;'>Not changed: %1</span>")
                             .arg(why.isEmpty() ? QString("pool returned HTTP %1").arg(http)
                                                : why.toHtmlEscaped()));
        return;
    }
    const QString applied = obj.value("payout_address").toString();
    if (applied.isEmpty()) {
        persistPayoutJournal(QStringLiteral("uncertain"), journal_from_, journal_to_);
        setPayoutMessage("<span style='color:#d8a37b;'>Pool accepted the request without confirming the address. Reconnecting to reconcile it.</span>");
        refresh();
        return;
    }
    if (!journal_to_.isEmpty() && applied != journal_to_) {
        persistPayoutJournal(QStringLiteral("conflict"), journal_from_, journal_to_);
        setPayoutMessage("<span style='color:#e06c75;'>Safety conflict: the pool confirmed a different address than the one you reviewed. No further change is allowed until the pool host is inspected.</span>");
        btn_change_payout_->setEnabled(false);
        refresh();
        return;
    }
    live_payout_address_ = applied;
    lbl_payout_current_->setText(applied.toHtmlEscaped());
    payout_input_->clear();
    setPayoutMessage("<span style='color:#8fbf7f;'>Fee address changed. It applies from the pool's "
                     "next block template, and survives a restart.</span>");
    persistPayoutJournal(QStringLiteral("applied"), journal_from_, applied);
    refresh();
}

void PoolPanel::onOpsReplyFinished(QNetworkReply* reply) {
    reply->deleteLater();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const int kind = reply->request().attribute(kKindAttr, kKindStatus).toInt();
    if (kind == kKindPayout) {
        payout_in_flight_ = false;
        btn_change_payout_->setEnabled(true);
        if (reply->error() != QNetworkReply::NoError && http == 0) {
            persistPayoutJournal(QStringLiteral("uncertain"), journal_from_, journal_to_);
            setPayoutMessage(QString("<span style='color:#d8a37b;'>Outcome uncertain: %1. The next status refresh will reconcile the live address.</span>")
                                 .arg(reply->errorString().toHtmlEscaped()));
            refresh();
            return;
        }
        handlePayoutReply(reply, http);
        return;
    }
    if (kind == kKindFee) {
        fee_in_flight_ = false;
        btn_change_fee_->setEnabled(true);
        if (reply->error() != QNetworkReply::NoError && http == 0) {
            persistFeeJournal(QStringLiteral("uncertain"), fee_journal_from_, fee_journal_to_);
            lbl_fee_message_->setText("<span style='color:#d8a37b;'>Fee-change outcome uncertain. Reconnecting to reconcile the live policy.</span>");
            lbl_fee_message_->setVisible(true);
            btn_change_fee_->setEnabled(false);
            refresh();
            return;
        }
        handleFeeReply(reply, http);
        return;
    }
    status_in_flight_ = false;
    btn_fetch_status_->setEnabled(true);
    if (reply->error() != QNetworkReply::NoError && http == 0) {
        markStatusStale(reply->errorString());
        setStatusMessage(QString("<span style='color:#e06c75;'>Could not reach the pool: %1</span>"
                                 "<br/><span style='color:#9fb3c8; font-size:11px;'>If the pool is on "
                                 "another machine, remember the endpoint is loopback-only \xE2\x80\x94 open an "
                                 "SSH tunnel.</span>")
                             .arg(reply->errorString().toHtmlEscaped()));
        return;
    }
    if (http == 401) {
        markStatusStale(QStringLiteral("authentication rejected"));
        setStatusMessage("<span style='color:#e06c75;'>Rejected: wrong ops token.</span>");
        return;
    }
    if (http != 200) {
        markStatusStale(QStringLiteral("HTTP %1").arg(http));
        setStatusMessage(QString("<span style='color:#e06c75;'>Pool returned HTTP %1.</span>").arg(http));
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        markStatusStale(QStringLiteral("invalid JSON"));
        setStatusMessage("<span style='color:#e06c75;'>Pool returned something that isn't JSON.</span>");
        return;
    }
    QString schema_error;
    if (!validateStatus(doc.object(), &schema_error)) {
        markStatusStale(schema_error);
        setStatusMessage(QString("<span style='color:#e06c75;'>Malformed pool status: %1</span>")
                             .arg(schema_error.toHtmlEscaped()));
        return;
    }
    applyStatus(doc.object());
}

bool PoolPanel::validateStatus(const QJsonObject& s, QString* error) const {
    const auto schema = strictInt(s.value("schema_version"));
    if (schema && *schema != 2) {
        *error = QStringLiteral("unsupported schema_version %1").arg(*schema);
        return false;
    }
    const QList<const char*> common = {"pool_version", "uptime_secs", "connected_miners", "fee_bps",
        "window_entries", "window_span_secs", "template_heartbeat_age_secs",
        "template_phase", "accepted_shares_total", "rejected_shares_total", "blocks_found_total", "miners"};
    for (const char* key : common) {
        if (!s.contains(QLatin1String(key))) {
            *error = QStringLiteral("missing required field %1").arg(QLatin1String(key));
            return false;
        }
    }
    for (const char* key : {"uptime_secs", "connected_miners", "fee_bps", "window_entries", "window_span_secs",
                            "template_heartbeat_age_secs", "accepted_shares_total",
                            "rejected_shares_total", "blocks_found_total"}) {
        if (!strictInt(s.value(QLatin1String(key)))) {
            *error = QStringLiteral("field %1 is not an integer").arg(QLatin1String(key));
            return false;
        }
    }
    if (!s.value("pool_version").isString() || !s.value("template_phase").isString() ||
        !s.value("miners").isArray()) {
        *error = QStringLiteral("pool_version/template_phase/miners have invalid types");
        return false;
    }
    for (const QJsonValue& value : s.value("miners").toArray()) {
        const QJsonObject miner = value.toObject();
        if (!value.isObject() || !miner.value("payout_script_hex").isString() ||
            !strictInt(miner.value("bps")) || !miner.value("window_weight").isString()) {
            *error = QStringLiteral("miners contains an invalid contributor entry");
            return false;
        }
    }
    if (schema) {
        for (const char* key : {"generated_at_unix", "daemon_blocks", "daemon_headers",
                                "template_height", "template_id", "last_template_at_unix"}) {
            if (!strictInt(s.value(QLatin1String(key)))) {
                *error = QStringLiteral("v2 field %1 is missing or invalid").arg(QLatin1String(key));
                return false;
            }
        }
        for (const char* key : {"daemon_connected", "daemon_endpoint", "stratum_bind",
                                "payout_address", "template_prev_hash", "last_share",
                                "last_block", "rejection_reasons"}) {
            if (!s.contains(QLatin1String(key))) {
                *error = QStringLiteral("v2 field %1 is missing").arg(QLatin1String(key));
                return false;
            }
        }
        if (!s.value("daemon_connected").isBool() || !s.value("daemon_endpoint").isString() ||
            !s.value("stratum_bind").isString() || !s.value("payout_address").isString() ||
            !s.value("template_prev_hash").isString() || !s.value("rejection_reasons").isObject() ||
            (!s.value("last_share").isNull() && !s.value("last_share").isObject()) ||
            (!s.value("last_block").isNull() && !s.value("last_block").isObject())) {
            *error = QStringLiteral("v2 health/event fields have invalid types");
            return false;
        }
        const QJsonObject last_share = s.value("last_share").toObject();
        if (!last_share.isEmpty() &&
            (!strictInt(last_share.value("accepted_at_unix")) || !last_share.value("kind").isString() ||
             !last_share.value("hash").isString())) {
            *error = QStringLiteral("v2 last_share has invalid fields");
            return false;
        }
        const QJsonObject last_block = s.value("last_block").toObject();
        if (!last_block.isEmpty() &&
            (!strictInt(last_block.value("observed_at_unix")) || !last_block.value("status").isString() ||
             !last_block.value("hash").isString() || !last_block.value("reason").isString())) {
            *error = QStringLiteral("v2 last_block has invalid fields");
            return false;
        }
        const QJsonObject rejection_reasons = s.value("rejection_reasons").toObject();
        for (auto it = rejection_reasons.begin(); it != rejection_reasons.end(); ++it) {
            if (!strictInt(it.value())) {
                *error = QStringLiteral("v2 rejection_reasons contains a non-integer count");
                return false;
            }
        }
    }
    return true;
}

void PoolPanel::markStatusStale(const QString& reason) {
    if (!has_valid_status_) {
        lbl_health_->setText(QStringLiteral("OFFLINE"));
        lbl_health_->setStyleSheet("font-size:18px; font-weight:700; color:#e06c75;");
        lbl_health_detail_->setText(reason.toHtmlEscaped());
        return;
    }
    status_group_->setVisible(true);
    lbl_health_->setText(QStringLiteral("STALE"));
    lbl_health_->setStyleSheet("font-size:18px; font-weight:700; color:#d8a37b;");
    lbl_health_detail_->setText(QString("Last valid reading %1 · %2")
                                    .arg(last_valid_status_.toString(Qt::ISODate), reason.toHtmlEscaped()));
}

void PoolPanel::updateHealth(const QJsonObject& s, bool legacy) {
    if (legacy) {
        lbl_health_->setText(QStringLiteral("DEGRADED"));
        lbl_health_->setStyleSheet("font-size:18px; font-weight:700; color:#d8a37b;");
        lbl_health_detail_->setText(QStringLiteral("Legacy status schema; daemon and Stratum health unavailable"));
        lbl_daemon_->setText(QStringLiteral("Unavailable on schema v1"));
        lbl_stratum_->setText(QStringLiteral("Unavailable on schema v1"));
        return;
    }
    const bool daemon = s.value("daemon_connected").toBool();
    const qint64 blocks = *strictInt(s.value("daemon_blocks"));
    const qint64 headers = *strictInt(s.value("daemon_headers"));
    const qint64 age = *strictInt(s.value("template_heartbeat_age_secs"));
    const qint64 generated = *strictInt(s.value("generated_at_unix"));
    const qint64 response_age = std::max<qint64>(0, QDateTime::currentSecsSinceEpoch() - generated);
    const bool healthy = daemon && age <= 120 && response_age <= 45 && blocks == headers;
    lbl_health_->setText(healthy ? QStringLiteral("HEALTHY") : QStringLiteral("DEGRADED"));
    lbl_health_->setStyleSheet(QString("font-size:18px; font-weight:700; color:%1;")
                                   .arg(healthy ? "#7bd88f" : "#d8a37b"));
    lbl_health_detail_->setText(QString("Response age %1s · template age %2s")
                                    .arg(QString::number(response_age), QString::number(age)));
    lbl_daemon_->setText(QString("%1 · %2/%3")
                             .arg(daemon ? "connected" : "offline", QString::number(blocks), QString::number(headers)));
    lbl_stratum_->setText(s.value("stratum_bind").toString().toHtmlEscaped());
}

void PoolPanel::applyStatus(const QJsonObject& s) {
    status_group_->setVisible(true);
    has_valid_status_ = true;
    last_valid_status_ = QDateTime::currentDateTimeUtc();
    const bool legacy = !s.contains("schema_version");
    const qint64 fee_bps = *strictInt(s.value("fee_bps"));
    const qint64 age = *strictInt(s.value("template_heartbeat_age_secs"));
    const QString phase = s.value("template_phase").toString();
    updateHealth(s, legacy);

    lbl_connected_miners_->setText(valueOrUnavailable(s, "connected_miners"));
    // Older pools (< 0.1.3) do not report this. Say so rather than showing an
    // empty field that reads as "no fee address configured".
    const QString payout = s.value("payout_address").toString();
    live_payout_address_ = payout;
    lbl_payout_current_->setText(
        payout.isEmpty()
            ? QString("<span style='color:#9fb3c8;'>not reported by this pool version</span>")
            : payout.toHtmlEscaped());
    lbl_fee_->setText(QString("%1%").arg(fee_bps / 100.0, 0, 'f', 2));
    live_fee_bps_ = fee_bps;
    if (!fee_percent_input_->hasFocus()) fee_percent_input_->setValue(fee_bps / 100.0);
    lbl_window_->setText(QString("%1 shares over %2s")
                             .arg(valueOrUnavailable(s, "window_entries"), valueOrUnavailable(s, "window_span_secs")));
    // A producer that has not checked in recently is the early warning
    // that the pool is about to be restarted by its own watchdog.
    const QString colour = age > 120 ? "#d8a37b" : "#7bd88f";
    lbl_producer_->setText(QString("<span style='color:%1;'>%2</span>"
                                   "<span style='color:#9fb3c8; font-size:11px;'> (%3s ago)</span>")
                               .arg(colour, phase.toHtmlEscaped(), QString::number(age)));
    lbl_shares_->setText(QString("%1 accepted / %2 rejected")
                             .arg(valueOrUnavailable(s, "accepted_shares_total"),
                                  valueOrUnavailable(s, "rejected_shares_total")));
    lbl_blocks_->setText(valueOrUnavailable(s, "blocks_found_total"));

    if (!legacy) {
        const QJsonObject share = s.value("last_share").toObject();
        const auto share_time = strictInt(share.value("accepted_at_unix"));
        lbl_last_share_->setText(share.isEmpty() ? QStringLiteral("None yet")
            : QString("%1 · %2").arg(share.value("kind").toString().toHtmlEscaped(),
                                      share_time ? QDateTime::fromSecsSinceEpoch(*share_time).toUTC().toString(Qt::ISODate)
                                                 : QStringLiteral("time unavailable")));
        const QJsonObject block = s.value("last_block").toObject();
        lbl_last_block_->setText(block.isEmpty() ? QStringLiteral("None yet")
            : QString("%1 · %2").arg(block.value("status").toString().toHtmlEscaped(),
                                      block.value("hash").toString().left(16).toHtmlEscaped()));
        QStringList reasons;
        const QJsonObject rejection = s.value("rejection_reasons").toObject();
        for (auto it = rejection.begin(); it != rejection.end(); ++it) {
            const auto count = strictInt(it.value());
            reasons << QString("%1: %2").arg(it.key().toHtmlEscaped(), count ? QString::number(*count) : QStringLiteral("Unavailable"));
        }
        lbl_rejections_->setText(reasons.isEmpty() ? QStringLiteral("None reported") : reasons.join(QStringLiteral(" · ")));
    } else {
        lbl_last_share_->setText(QStringLiteral("Unavailable on schema v1"));
        lbl_last_block_->setText(QStringLiteral("Unavailable on schema v1"));
        lbl_rejections_->setText(QStringLiteral("Unavailable on schema v1"));
    }

    const QJsonArray miners = s.value("miners").toArray();
    miners_table_->setRowCount(miners.size());
    for (int i = 0; i < miners.size(); ++i) {
        const QJsonObject m = miners.at(i).toObject();
        const qint64 bps = strictInt(m.value("bps")).value_or(0);
        miners_table_->setItem(i, 0, new QTableWidgetItem(m.value("payout_script_hex").toString()));
        miners_table_->setItem(i, 1, new QTableWidgetItem(QString("%1%").arg(bps / 100.0, 0, 'f', 2)));
        miners_table_->setItem(i, 2, new QTableWidgetItem(m.value("window_weight").toString()));
    }
    // Size to the contributors actually present (capped), so the card does
    // not reserve a block of empty rows for miners that are not there.
    const int shown = std::max(1, std::min(static_cast<int>(miners.size()), 6));
    miners_table_->setFixedHeight(24 * shown + 28);
    reconcilePayoutJournal(payout);
    reconcileFeeJournal(fee_bps);
    recordHistory(s);
    renderHistory();
    why_group_->setVisible(false);
    btn_about_->setVisible(true);

    setStatusMessage(QString("<span style='color:#7bd88f;'>Connected.</span>"
                             "<span style='color:#9fb3c8; font-size:11px;'> pool %1, up %2s</span>")
                         .arg(s.value("pool_version").toString().toHtmlEscaped(),
                              valueOrUnavailable(s, "uptime_secs")));
}

QString PoolPanel::endpointKey() const {
    const QByteArray normalized = ops_url_input_->text().trimmed().toLower().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(normalized, QCryptographicHash::Sha256).toHex());
}

void PoolPanel::recordHistory(const QJsonObject& s) {
    const auto accepted = strictInt(s.value("accepted_shares_total"));
    const auto rejected = strictInt(s.value("rejected_shares_total"));
    const auto sessions = strictInt(s.value("connected_miners"));
    if (!accepted || !rejected || !sessions) return;
    QSettings settings;
    const QString key = QStringLiteral("pool/history/") + endpointKey();
    QVariantList samples = settings.value(key).toList();
    QVariantMap sample;
    sample["at"] = QDateTime::currentSecsSinceEpoch();
    sample["accepted"] = *accepted;
    sample["rejected"] = *rejected;
    sample["sessions"] = *sessions;
    samples.append(sample);
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - 24 * 60 * 60;
    while (!samples.isEmpty() && samples.first().toMap().value("at").toLongLong() < cutoff) {
        samples.removeFirst();
    }
    while (samples.size() > 5760) samples.removeFirst();
    settings.setValue(key, samples);
    settings.sync();
}

void PoolPanel::renderHistory() {
    QSettings settings;
    const QVariantList samples = settings.value(QStringLiteral("pool/history/") + endpointKey()).toList();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    auto render = [&](qint64 seconds) {
        QList<qint64> values;
        qint64 first = -1;
        qint64 last = -1;
        for (const QVariant& item : samples) {
            const QVariantMap sample = item.toMap();
            if (sample.value("at").toLongLong() < now - seconds) continue;
            const qint64 accepted = sample.value("accepted").toLongLong();
            if (first < 0) first = accepted;
            last = accepted;
            values.append(accepted);
        }
        const qsizetype start = std::max<qsizetype>(0, values.size() - 32);
        const QString chart = sparkline(values.mid(start));
        return QString("<span style='font-family:monospace;color:#7bd88f;'>%1</span>  +%2 accepted")
            .arg(chart.toHtmlEscaped(), QString::number(first < 0 ? 0 : last - first));
    };
    lbl_history_5m_->setText(render(5 * 60));
    lbl_history_1h_->setText(render(60 * 60));
    lbl_history_24h_->setText(render(24 * 60 * 60));
}

bool PoolPanel::persistPayoutJournal(const QString& stage, const QString& from, const QString& to) {
    QSettings settings;
    const QString base = QStringLiteral("pool/payoutJournal/") + endpointKey() + QStringLiteral("/");
    settings.setValue(base + "stage", stage);
    settings.setValue(base + "from", from);
    settings.setValue(base + "to", to);
    settings.setValue(base + "updatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    settings.sync();
    if (settings.status() != QSettings::NoError) return false;
    journal_stage_ = stage;
    journal_from_ = from;
    journal_to_ = to;
    return true;
}

void PoolPanel::restorePayoutJournal() {
    QSettings settings;
    const QString base = QStringLiteral("pool/payoutJournal/") + endpointKey() + QStringLiteral("/");
    journal_stage_ = settings.value(base + "stage").toString();
    journal_from_ = settings.value(base + "from").toString();
    journal_to_ = settings.value(base + "to").toString();
    btn_change_payout_->setEnabled(true);
    if (journal_stage_ == QStringLiteral("submitting") || journal_stage_ == QStringLiteral("uncertain")) {
        journal_stage_ = QStringLiteral("uncertain");
        setPayoutMessage("<span style='color:#d8a37b;'>A previous fee-address change has an uncertain outcome. Connect to reconcile it before trying again.</span>");
        btn_change_payout_->setEnabled(false);
    }
}

void PoolPanel::reconcilePayoutJournal(const QString& observed) {
    if (journal_stage_ != QStringLiteral("submitting") && journal_stage_ != QStringLiteral("uncertain")) return;
    if (observed == journal_to_) {
        persistPayoutJournal(QStringLiteral("applied"), journal_from_, journal_to_);
        setPayoutMessage("<span style='color:#7bd88f;'>Reconciled: the requested fee address is live.</span>");
        payout_input_->clear();
        btn_change_payout_->setEnabled(true);
    } else if (observed == journal_from_) {
        persistPayoutJournal(QStringLiteral("not-applied"), journal_from_, journal_to_);
        setPayoutMessage("<span style='color:#d8a37b;'>Reconciled: the previous address is still live; the change was not applied.</span>");
        btn_change_payout_->setEnabled(true);
    } else if (!observed.isEmpty()) {
        persistPayoutJournal(QStringLiteral("conflict"), journal_from_, journal_to_);
        setPayoutMessage("<span style='color:#e06c75;'>Reconciliation conflict: the live fee address matches neither the old nor requested address. Review the pool host before changing it again.</span>");
        btn_change_payout_->setEnabled(false);
    }
}

bool PoolPanel::persistFeeJournal(const QString& stage, qint64 from, qint64 to) {
    QSettings settings;
    const QString base = QStringLiteral("pool/feeJournal/") + endpointKey() + QStringLiteral("/");
    settings.setValue(base + "stage", stage);
    settings.setValue(base + "from", from);
    settings.setValue(base + "to", to);
    settings.setValue(base + "updatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    settings.sync();
    if (settings.status() != QSettings::NoError) return false;
    fee_journal_stage_ = stage;
    fee_journal_from_ = from;
    fee_journal_to_ = to;
    return true;
}

void PoolPanel::restoreFeeJournal() {
    QSettings settings;
    const QString base = QStringLiteral("pool/feeJournal/") + endpointKey() + QStringLiteral("/");
    fee_journal_stage_ = settings.value(base + "stage").toString();
    fee_journal_from_ = settings.value(base + "from", -1).toLongLong();
    fee_journal_to_ = settings.value(base + "to", -1).toLongLong();
    if (btn_change_fee_) btn_change_fee_->setEnabled(true);
    if (fee_journal_stage_ == QStringLiteral("submitting") || fee_journal_stage_ == QStringLiteral("uncertain")) {
        fee_journal_stage_ = QStringLiteral("uncertain");
        lbl_fee_message_->setText("<span style='color:#d8a37b;'>A previous fee change has an uncertain outcome. Connect to reconcile it.</span>");
        lbl_fee_message_->setVisible(true);
        btn_change_fee_->setEnabled(false);
    }
}

void PoolPanel::reconcileFeeJournal(qint64 observed) {
    if (fee_journal_stage_ != QStringLiteral("submitting") && fee_journal_stage_ != QStringLiteral("uncertain")) return;
    if (observed == fee_journal_to_) {
        persistFeeJournal(QStringLiteral("applied"), fee_journal_from_, fee_journal_to_);
        lbl_fee_message_->setText("<span style='color:#7bd88f;'>Reconciled: the requested operator fee is live.</span>");
        btn_change_fee_->setEnabled(true);
    } else if (observed == fee_journal_from_) {
        persistFeeJournal(QStringLiteral("not-applied"), fee_journal_from_, fee_journal_to_);
        lbl_fee_message_->setText("<span style='color:#d8a37b;'>Reconciled: the previous operator fee remains live.</span>");
        btn_change_fee_->setEnabled(true);
    } else {
        persistFeeJournal(QStringLiteral("conflict"), fee_journal_from_, fee_journal_to_);
        lbl_fee_message_->setText("<span style='color:#e06c75;'>Fee-policy reconciliation conflict. Inspect the pool before changing it again.</span>");
        btn_change_fee_->setEnabled(false);
    }
    lbl_fee_message_->setVisible(true);
}

void PoolPanel::onCheckEarningsClicked() {
    if (earnings_in_flight_) {
        return;
    }
    const QString addr = fee_address_input_->text().trimmed();
    if (addr.isEmpty()) {
        lbl_earnings_->setText("<span style='color:#d8a37b;'>Enter your fee address.</span>");
        return;
    }
    lbl_earnings_->setText("checking the chain\xE2\x80\xA6");
    earnings_in_flight_ = true;
    btn_check_earnings_->setEnabled(false);
    rpc_->call("blockchain.getaddressbalance", QJsonArray{addr});
}

void PoolPanel::onRpcResult(const QString& method, const QJsonValue& result) {
    if (method != "blockchain.getaddressbalance") {
        return;
    }
    earnings_in_flight_ = false;
    btn_check_earnings_->setEnabled(true);
    if (!result.isObject()) {
        lbl_earnings_->setText("<span style='color:#e06c75;'>Unexpected reply from the node.</span>");
        return;
    }
    const QJsonObject obj = result.toObject();
    std::optional<qint64> balance;
    for (const char* key : {"balance_una", "balance"}) {
        if (obj.contains(QLatin1String(key))) {
            balance = strictInt(obj.value(QLatin1String(key)));
            if (balance) break;
        }
    }
    if (!balance) {
        lbl_earnings_->setText("<span style='color:#e06c75;'>The node did not return a valid confirmed unspent balance.</span>");
        return;
    }
    lbl_earnings_->setText(
        QString("<span style='font-size:15px; font-weight:600; color:#7bd88f;'>%1</span>"
                "<br/><span style='color:#9fb3c8; font-size:11px;'>Confirmed unspent balance at this address. "
                "It excludes any pool fees that have already been spent.</span>")
            .arg(formatDin(*balance)));
}

void PoolPanel::onRpcError(const QString& method, int code, const QString& message) {
    if (method != "blockchain.getaddressbalance") {
        return;
    }
    earnings_in_flight_ = false;
    btn_check_earnings_->setEnabled(true);
    lbl_earnings_->setText(QString("<span style='color:#e06c75;'>Node could not answer: %1 [%2]</span>")
                               .arg(message.toHtmlEscaped())
                               .arg(code));
}
