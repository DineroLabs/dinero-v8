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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
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

    auto* form = new QFormLayout();
    ops_url_input_ = new QLineEdit("http://127.0.0.1:4445");
    ops_url_input_->setPlaceholderText("http://127.0.0.1:4445");
    form->addRow("Status endpoint:", ops_url_input_);
    ops_token_input_ = new QLineEdit();
    ops_token_input_->setEchoMode(QLineEdit::Password);
    ops_token_input_->setPlaceholderText("contents of /etc/dinero-sv2/ops-token");
    form->addRow("Ops token:", ops_token_input_);
    conn_layout->addLayout(form);

    auto* row = new QHBoxLayout();
    btn_fetch_status_ = new QPushButton("Connect");
    btn_fetch_status_->setStyleSheet("font-weight: bold; padding: 6px 14px;");
    row->addStretch();
    row->addWidget(btn_fetch_status_);
    conn_layout->addLayout(row);

    lbl_status_message_ = new QLabel("\xE2\x80\x93");
    lbl_status_message_->setTextFormat(Qt::RichText);
    lbl_status_message_->setWordWrap(true);
    lbl_status_message_->setStyleSheet("color: #9fb3c8;");
    conn_layout->addWidget(lbl_status_message_);
    root->addWidget(conn_group);

    // ---- Live status -------------------------------------------------
    status_group_ = new QGroupBox("Live status");
    auto* grid = new QGridLayout(status_group_);
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

    miners_table_ = new QTableWidget(0, 3);
    miners_table_->setHorizontalHeaderLabels({"Contributor payout script", "Next-block share", "Window weight"});
    miners_table_->horizontalHeader()->setStretchLastSection(true);
    miners_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    miners_table_->setMaximumHeight(160);
    grid->addWidget(miners_table_, 3, 0, 1, 4);
    status_group_->setEnabled(false);
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
    root->addWidget(earn_group);

    root->addStretch();
}

void PoolPanel::setupConnections() {
    connect(btn_fetch_status_, &QPushButton::clicked, this, &PoolPanel::onFetchStatusClicked);
    connect(btn_check_earnings_, &QPushButton::clicked, this, &PoolPanel::onCheckEarningsClicked);
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

void PoolPanel::onOpsReplyFinished(QNetworkReply* reply) {
    reply->deleteLater();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && http == 0) {
        status_group_->setEnabled(false);
        setStatusMessage(QString("<span style='color:#e06c75;'>Could not reach the pool: %1</span>"
                                 "<br/><span style='color:#9fb3c8; font-size:11px;'>If the pool is on "
                                 "another machine, remember the endpoint is loopback-only \xE2\x80\x94 open an "
                                 "SSH tunnel.</span>")
                             .arg(reply->errorString().toHtmlEscaped()));
        return;
    }
    if (http == 401) {
        status_group_->setEnabled(false);
        setStatusMessage("<span style='color:#e06c75;'>Rejected: wrong ops token.</span>");
        return;
    }
    if (http != 200) {
        status_group_->setEnabled(false);
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
    status_group_->setEnabled(true);
    const qint64 fee_bps = safeInt(s.value("fee_bps"));
    const qint64 age = safeInt(s.value("template_heartbeat_age_secs"));
    const QString phase = s.value("template_phase").toString();

    lbl_connected_miners_->setText(QString::number(safeInt(s.value("connected_miners"))));
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
