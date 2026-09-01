// Copyright (c) 2026 Dinero Labs.

#include "poolpanel.h"

#include "rpcclient.h"

#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>
#include <QVBoxLayout>

namespace {

constexpr qint64 kUnaPerDin = 100000000;

qint64 safeInt(const QJsonValue& v) {
    if (v.isDouble()) {
        return static_cast<qint64>(v.toDouble());
    }
    if (v.isString()) {
        return v.toString().toLongLong();
    }
    return 0;
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
    auto* why_group = new QGroupBox("Hosting a pool");
    auto* why_layout = new QVBoxLayout(why_group);
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
    why_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    root->addWidget(why_group);

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
    lbl_connected_miners_ = new QLabel("\xE2\x80\x93");
    lbl_fee_ = new QLabel("\xE2\x80\x93");
    lbl_window_ = new QLabel("\xE2\x80\x93");
    lbl_producer_ = new QLabel("\xE2\x80\x93");
    lbl_shares_ = new QLabel("\xE2\x80\x93");
    lbl_blocks_ = new QLabel("\xE2\x80\x93");
    grid->addWidget(new QLabel("Miners connected:"), 0, 0);
    grid->addWidget(lbl_connected_miners_, 0, 1);
    grid->addWidget(new QLabel("Your fee:"), 0, 2);
    grid->addWidget(lbl_fee_, 0, 3);
    grid->addWidget(new QLabel("Payout window:"), 1, 0);
    grid->addWidget(lbl_window_, 1, 1);
    grid->addWidget(new QLabel("Template producer:"), 1, 2);
    grid->addWidget(lbl_producer_, 1, 3);
    grid->addWidget(new QLabel("Shares (this run):"), 2, 0);
    grid->addWidget(lbl_shares_, 2, 1);
    grid->addWidget(new QLabel("Blocks found (this run):"), 2, 2);
    grid->addWidget(lbl_blocks_, 2, 3);

    // The address the pool is ACTUALLY paying, straight from /status. An
    // operator should never have to trust the unit file to know this.
    lbl_payout_current_ = new QLabel("\xE2\x80\x93");
    lbl_payout_current_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lbl_payout_current_->setStyleSheet("font-family: monospace;");
    grid->addWidget(new QLabel("Fee paid to:"), 3, 0);
    grid->addWidget(lbl_payout_current_, 3, 1, 1, 3);

    auto* change_row = new QHBoxLayout();
    payout_input_ = new QLineEdit();
    payout_input_->setPlaceholderText("din1p\xE2\x80\xA6 new fee address");
    change_row->addWidget(payout_input_, 1);
    btn_change_payout_ = new QPushButton("Change");
    change_row->addWidget(btn_change_payout_, 0);
    grid->addWidget(new QLabel("Change to:"), 4, 0);
    grid->addLayout(change_row, 4, 1, 1, 3);

    lbl_payout_message_ = new QLabel();
    lbl_payout_message_->setTextFormat(Qt::RichText);
    lbl_payout_message_->setWordWrap(true);
    lbl_payout_message_->setStyleSheet("color: #9fb3c8; font-size: 11px;");
    lbl_payout_message_->setVisible(false);
    grid->addWidget(lbl_payout_message_, 5, 0, 1, 4);

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
    grid->addWidget(miners_table_, 6, 0, 1, 4);
    // Hidden, not merely disabled. Most people looking at this tab do not
    // run a pool; showing them an empty status grid and an empty table is
    // noise that buries the one thing meant for them (the explainer above).
    status_group_->setVisible(false);
    root->addWidget(status_group_);

    // ---- Earnings, from the chain ------------------------------------
    auto* earn_group = new QGroupBox("Fee earnings (verified on-chain)");
    auto* earn_layout = new QVBoxLayout(earn_group);
    auto* earn_hint = new QLabel(
        "Read from the blockchain by your own node \xE2\x80\x94 not reported by the pool. This "
        "keeps working even when your pool is down, which is when you most want to look.");
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
    connect(payout_input_, &QLineEdit::returnPressed, this, &PoolPanel::onChangePayoutClicked);
    connect(net_, &QNetworkAccessManager::finished, this, &PoolPanel::onOpsReplyFinished);
    connect(&refresh_timer_, &QTimer::timeout, this, &PoolPanel::refresh);
    connect(rpc_, &RpcClient::rpcResult, this, &PoolPanel::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &PoolPanel::onRpcError);
}

void PoolPanel::refresh() {
    // Only poll once the operator has successfully asked at least once;
    // otherwise a user who never runs a pool gets an error every 15s.
    if (ops_attempted_ && !ops_token_input_->text().trimmed().isEmpty()) {
        onFetchStatusClicked();
    }
}

void PoolPanel::setStatusMessage(const QString& html) {
    lbl_status_message_->setText(html);
}

void PoolPanel::onFetchStatusClicked() {
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
    if (!url.isValid() || url.scheme().isEmpty()) {
        setStatusMessage("<span style='color:#e06c75;'>That endpoint is not a valid URL.</span>");
        return;
    }
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    req.setTransferTimeout(8000);
    net_->get(req);
}

namespace {
/// Marks which request a reply belongs to. One QNetworkAccessManager serves
/// both the status GET and the payout POST, and `finished` fires for both.
constexpr QNetworkRequest::Attribute kKindAttr = QNetworkRequest::User;
constexpr int kKindStatus = 0;
constexpr int kKindPayout = 1;
}  // namespace

void PoolPanel::setPayoutMessage(const QString& html) {
    lbl_payout_message_->setText(html);
    lbl_payout_message_->setVisible(!html.isEmpty());
}

void PoolPanel::onChangePayoutClicked() {
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
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setAttribute(kKindAttr, kKindPayout);
    QJsonObject body;
    body["address"] = addr;
    setPayoutMessage("<span style='color:#9fb3c8;'>Asking the pool to verify that address\xE2\x80\xA6</span>");
    net_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void PoolPanel::handlePayoutReply(QNetworkReply* reply, int http) {
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};

    if (http == 403) {
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
        setPayoutMessage("<span style='color:#e06c75;'>Rejected: wrong ops token.</span>");
        return;
    }
    if (http != 200) {
        const QString why = obj.value("error").toString();
        setPayoutMessage(QString("<span style='color:#e06c75;'>Not changed: %1</span>")
                             .arg(why.isEmpty() ? QString("pool returned HTTP %1").arg(http)
                                                : why.toHtmlEscaped()));
        return;
    }
    const QString applied = obj.value("payout_address").toString();
    live_payout_address_ = applied;
    lbl_payout_current_->setText(applied.toHtmlEscaped());
    payout_input_->clear();
    setPayoutMessage("<span style='color:#8fbf7f;'>Fee address changed. It applies from the pool's "
                     "next block template, and survives a restart.</span>");
    refresh();
}

void PoolPanel::onOpsReplyFinished(QNetworkReply* reply) {
    reply->deleteLater();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const int kind = reply->request().attribute(kKindAttr, kKindStatus).toInt();
    if (kind == kKindPayout) {
        if (reply->error() != QNetworkReply::NoError && http == 0) {
            setPayoutMessage(QString("<span style='color:#e06c75;'>Could not reach the pool: %1</span>")
                                 .arg(reply->errorString().toHtmlEscaped()));
            return;
        }
        handlePayoutReply(reply, http);
        return;
    }
    if (reply->error() != QNetworkReply::NoError && http == 0) {
        status_group_->setVisible(false);
        setStatusMessage(QString("<span style='color:#e06c75;'>Could not reach the pool: %1</span>"
                                 "<br/><span style='color:#9fb3c8; font-size:11px;'>If the pool is on "
                                 "another machine, remember the endpoint is loopback-only \xE2\x80\x94 open an "
                                 "SSH tunnel.</span>")
                             .arg(reply->errorString().toHtmlEscaped()));
        return;
    }
    if (http == 401) {
        status_group_->setVisible(false);
        setStatusMessage("<span style='color:#e06c75;'>Rejected: wrong ops token.</span>");
        return;
    }
    if (http != 200) {
        status_group_->setVisible(false);
        setStatusMessage(QString("<span style='color:#e06c75;'>Pool returned HTTP %1.</span>").arg(http));
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        setStatusMessage("<span style='color:#e06c75;'>Pool returned something that isn't JSON.</span>");
        return;
    }
    applyStatus(doc.object());
}

void PoolPanel::applyStatus(const QJsonObject& s) {
    status_group_->setVisible(true);
    const qint64 fee_bps = safeInt(s.value("fee_bps"));
    const qint64 age = safeInt(s.value("template_heartbeat_age_secs"));
    const QString phase = s.value("template_phase").toString();

    lbl_connected_miners_->setText(QString::number(safeInt(s.value("connected_miners"))));
    // Older pools (< 0.1.3) do not report this. Say so rather than showing an
    // empty field that reads as "no fee address configured".
    const QString payout = s.value("payout_address").toString();
    live_payout_address_ = payout;
    lbl_payout_current_->setText(
        payout.isEmpty()
            ? QString("<span style='color:#9fb3c8;'>not reported by this pool version</span>")
            : payout.toHtmlEscaped());
    lbl_fee_->setText(QString("%1%").arg(fee_bps / 100.0, 0, 'f', 2));
    lbl_window_->setText(QString("%1 shares over %2s")
                             .arg(safeInt(s.value("window_entries")))
                             .arg(safeInt(s.value("window_span_secs"))));
    // A producer that has not checked in recently is the early warning
    // that the pool is about to be restarted by its own watchdog.
    const QString colour = age > 120 ? "#d8a37b" : "#7bd88f";
    lbl_producer_->setText(QString("<span style='color:%1;'>%2</span>"
                                   "<span style='color:#9fb3c8; font-size:11px;'> (%3s ago)</span>")
                               .arg(colour, phase.toHtmlEscaped(), QString::number(age)));
    lbl_shares_->setText(QString("%1 accepted / %2 rejected")
                             .arg(safeInt(s.value("accepted_shares_total")))
                             .arg(safeInt(s.value("rejected_shares_total"))));
    lbl_blocks_->setText(QString::number(safeInt(s.value("blocks_found_total"))));

    const QJsonArray miners = s.value("miners").toArray();
    miners_table_->setRowCount(miners.size());
    for (int i = 0; i < miners.size(); ++i) {
        const QJsonObject m = miners.at(i).toObject();
        const qint64 bps = safeInt(m.value("bps"));
        miners_table_->setItem(i, 0, new QTableWidgetItem(m.value("payout_script_hex").toString()));
        miners_table_->setItem(i, 1, new QTableWidgetItem(QString("%1%").arg(bps / 100.0, 0, 'f', 2)));
        miners_table_->setItem(i, 2, new QTableWidgetItem(m.value("window_weight").toString()));
    }
    // Size to the contributors actually present (capped), so the card does
    // not reserve a block of empty rows for miners that are not there.
    const int shown = std::max(1, std::min(static_cast<int>(miners.size()), 6));
    miners_table_->setFixedHeight(24 * shown + 28);

    setStatusMessage(QString("<span style='color:#7bd88f;'>Connected.</span>"
                             "<span style='color:#9fb3c8; font-size:11px;'> pool %1, up %2s</span>")
                         .arg(s.value("pool_version").toString().toHtmlEscaped(),
                              QString::number(safeInt(s.value("uptime_secs")))));
}

void PoolPanel::onCheckEarningsClicked() {
    const QString addr = fee_address_input_->text().trimmed();
    if (addr.isEmpty()) {
        lbl_earnings_->setText("<span style='color:#d8a37b;'>Enter your fee address.</span>");
        return;
    }
    lbl_earnings_->setText("checking the chain\xE2\x80\xA6");
    rpc_->call("blockchain.getaddressbalance", QJsonArray{addr});
}

void PoolPanel::onRpcResult(const QString& method, const QJsonValue& result) {
    if (method != "blockchain.getaddressbalance") {
        return;
    }
    if (!result.isObject()) {
        lbl_earnings_->setText("<span style='color:#e06c75;'>Unexpected reply from the node.</span>");
        return;
    }
    const QJsonObject obj = result.toObject();
    // Field naming varies across daemon versions; accept the plausible
    // spellings rather than silently reporting zero.
    qint64 received = 0;
    for (const char* key : {"received_una", "total_received_una", "received", "balance_una", "balance"}) {
        if (obj.contains(QLatin1String(key))) {
            received = safeInt(obj.value(QLatin1String(key)));
            if (received != 0) {
                break;
            }
        }
    }
    lbl_earnings_->setText(
        QString("<span style='font-size:15px; font-weight:600; color:#7bd88f;'>%1</span>"
                "<br/><span style='color:#9fb3c8; font-size:11px;'>Total received by this address, "
                "read from your node. If you use this address only for pool fees, this is what "
                "hosting has earned you.</span>")
            .arg(formatDin(received)));
}

void PoolPanel::onRpcError(const QString& method, int code, const QString& message) {
    if (method != "blockchain.getaddressbalance") {
        return;
    }
    lbl_earnings_->setText(QString("<span style='color:#e06c75;'>Node could not answer: %1 [%2]</span>")
                               .arg(message.toHtmlEscaped())
                               .arg(code));
}
