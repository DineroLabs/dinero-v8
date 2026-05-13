#include "aipanel.h"
#include "aibackend.h"
#include "scrollsupport.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QRadioButton>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QScrollBar>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QEasingCurve>
#include <QRegularExpression>
#include <QScroller>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QPixmap>
#include <QBuffer>
#include <QDebug>
#include <QNetworkRequest>
#include <QUrl>

static QString chatHistoryPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/ai_chat_history.json";
}

// ──────────────────────────────────────────────────────────────
// Construction
// ──────────────────────────────────────────────────────────────

AiPanel::AiPanel(const QString& datadirHint, QWidget* parent)
    : QWidget(parent)
    , datadirHint_(datadirHint)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setupUI();
    applyBackend();
    loadConversation();
}

// ──────────────────────────────────────────────────────────────
// Panel width animation
// ──────────────────────────────────────────────────────────────

int AiPanel::panelWidth() const { return maximumWidth(); }

void AiPanel::setPanelWidth(int w)
{
    setFixedWidth(w);
    setMaximumWidth(w);
    setMinimumWidth(w);
}

void AiPanel::togglePanel()
{
    if (!slideAnim_) {
        slideAnim_ = new QPropertyAnimation(this, "panelWidth", this);
        slideAnim_->setDuration(200);
        slideAnim_->setEasingCurve(QEasingCurve::InOutCubic);
    }
    slideAnim_->stop();

    if (panelOpen_) {
        slideAnim_->setStartValue(targetWidth_);
        slideAnim_->setEndValue(0);
    } else {
        auto cfg = AiBackend::Config::load();
        if (!cfg.isConfigured()) {
            stack_->setCurrentWidget(configScreen_);
        } else {
            stack_->setCurrentWidget(chatScreen_);
            if (chatLog_.isEmpty()) {
                appendSystemMessage(
                    QString("DineroAI ready — %1").arg(cfg.providerLabel()));
                addSuggestedChips();
            }
        }
        slideAnim_->setStartValue(0);
        slideAnim_->setEndValue(targetWidth_);
    }

    panelOpen_ = !panelOpen_;
    slideAnim_->start();
    Q_EMIT panelToggled(panelOpen_);

    if (panelOpen_ && stack_->currentWidget() == chatScreen_ && inputField_)
        inputField_->setFocus();
}

// ──────────────────────────────────────────────────────────────
// Backend management
// ──────────────────────────────────────────────────────────────

void AiPanel::applyBackend()
{
    if (backend_) {
        backend_->disconnect(this);
        backend_->deleteLater();
        backend_ = nullptr;
    }

    auto cfg = AiBackend::Config::load();
    if (!cfg.isConfigured()) return;

    backend_ = AiBackend::create(cfg, datadirHint_, this);
    backend_->setSystemPrompt(systemPrompt());

    connect(backend_, &AiBackend::textDelta,        this, &AiPanel::onTextDelta);
    connect(backend_, &AiBackend::turnComplete,     this, &AiPanel::onTurnComplete);
    connect(backend_, &AiBackend::errorOccurred,    this, &AiPanel::onError);
    connect(backend_, &AiBackend::streamingStarted, this, &AiPanel::onStreamingStarted);
    connect(backend_, &AiBackend::streamingFinished,this, &AiPanel::onStreamingFinished);
    connect(backend_, &AiBackend::toolActivity,     this, &AiPanel::onToolActivity);
    connect(backend_, &AiBackend::costUpdate,       this, &AiPanel::onCostUpdate);

    if (providerLabel_)
        providerLabel_->setText(cfg.providerLabel());
}

// ──────────────────────────────────────────────────────────────
// UI Setup
// ──────────────────────────────────────────────────────────────

void AiPanel::setupUI()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    stack_ = new QStackedWidget;
    buildConfigScreen();
    buildChatScreen();
    stack_->addWidget(configScreen_);
    stack_->addWidget(chatScreen_);
    stack_->setCurrentWidget(configScreen_);

    root->addWidget(stack_, 1);
}

// ──────────────────────────────────────────────────────────────
// Config screen
// ──────────────────────────────────────────────────────────────

void AiPanel::buildConfigScreen()
{
    configScreen_ = new QWidget;
    auto* layout = new QVBoxLayout(configScreen_);
    layout->setContentsMargins(16, 20, 16, 16);
    layout->setSpacing(10);

    auto* title = new QLabel("DineroAI Setup");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(
        "QLabel { color: #eef2f6; font-size: 17px; font-weight: 700; "
        "background: transparent; border: none; }");
    layout->addWidget(title);

    auto* sub = new QLabel("Choose your AI provider");
    sub->setAlignment(Qt::AlignCenter);
    sub->setStyleSheet(
        "QLabel { color: #868e96; font-size: 12px; background: transparent; border: none; }");
    layout->addWidget(sub);

    layout->addSpacing(4);

    // Provider card
    auto* card = new QWidget;
    card->setStyleSheet(
        "QWidget { background: #20242a; border: 1px solid #353b44; border-radius: 10px; }");
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(14, 12, 14, 12);
    cardLayout->setSpacing(2);

    auto makeRadioRow = [](const QString& label, const QString& hint) {
        auto* w = new QWidget;
        w->setStyleSheet("QWidget { background: transparent; border: none; }");
        auto* row = new QHBoxLayout(w);
        row->setContentsMargins(0,3,0,3);
        row->setSpacing(8);
        auto* rb = new QRadioButton(label);
        rb->setStyleSheet(
            "QRadioButton { color: #d6dde6; font-size: 13px; background: transparent; border: none; }"
            "QRadioButton::indicator { width: 14px; height: 14px; }"
            "QRadioButton::indicator:checked { background: #51cf66; border: 2px solid #51cf66; border-radius: 7px; }"
            "QRadioButton::indicator:unchecked { background: #2b3037; border: 2px solid #4a5260; border-radius: 7px; }");
        auto* hl = new QLabel(hint);
        hl->setStyleSheet(
            "QLabel { color: #5c6370; font-size: 10px; background: transparent; border: none; }");
        row->addWidget(rb);
        row->addStretch();
        row->addWidget(hl);
        return std::make_pair(rb, w);
    };

    auto [rbCli,    wCli]    = makeRadioRow("Claude CLI",   "Your install");
    auto [rbClaude, wClaude] = makeRadioRow("Claude API",   "Your key");
    auto [rbGemini, wGemini] = makeRadioRow("Gemini Flash", "Free tier");
    auto [rbGroq,   wGroq]   = makeRadioRow("Groq",         "Free tier");
    auto [rbOllama, wOllama] = makeRadioRow("Ollama",       "Local / free");
    auto [rbMlx,   wMlx]    = makeRadioRow("MLX",          "Apple Silicon");

    rbCli_    = rbCli;
    rbClaude_ = rbClaude;
    rbGemini_ = rbGemini;
    rbGroq_   = rbGroq;
    rbOllama_ = rbOllama;
    rbMlx_    = rbMlx;

    auto* group = new QButtonGroup(this);
    group->addButton(rbCli_,    0);
    group->addButton(rbClaude_, 1);
    group->addButton(rbGemini_, 2);
    group->addButton(rbGroq_,   3);
    group->addButton(rbOllama_, 4);
    group->addButton(rbMlx_,    5);

    cardLayout->addWidget(wCli);
    cardLayout->addWidget(wClaude);
    cardLayout->addWidget(wGemini);
    cardLayout->addWidget(wGroq);
    cardLayout->addWidget(wOllama);
    cardLayout->addWidget(wMlx);
    layout->addWidget(card);

    // API key row
    keyRow_ = new QWidget;
    keyRow_->setStyleSheet("QWidget { background: transparent; border: none; }");
    {
        auto* kl = new QVBoxLayout(keyRow_);
        kl->setContentsMargins(0,4,0,0); kl->setSpacing(4);
        auto* lbl = new QLabel("API Key");
        lbl->setStyleSheet("QLabel { color: #868e96; font-size: 11px; background: transparent; border: none; }");
        apiKeyEdit_ = new QLineEdit;
        apiKeyEdit_->setPlaceholderText("Paste your API key here…");
        apiKeyEdit_->setEchoMode(QLineEdit::Password);
        apiKeyEdit_->setStyleSheet(
            "QLineEdit { background: #1f2328; color: #d7dde5; border: 1px solid #353b44; "
            "border-radius: 6px; padding: 6px 10px; font-size: 12px; }"
            "QLineEdit:focus { border-color: #51cf66; }");
        kl->addWidget(lbl);
        kl->addWidget(apiKeyEdit_);
    }
    layout->addWidget(keyRow_);

    // Claude path row
    pathRow_ = new QWidget;
    pathRow_->setStyleSheet("QWidget { background: transparent; border: none; }");
    {
        auto* pl = new QVBoxLayout(pathRow_);
        pl->setContentsMargins(0,4,0,0); pl->setSpacing(4);
        auto* lbl = new QLabel("Claude CLI path (blank = auto-detect)");
        lbl->setStyleSheet("QLabel { color: #868e96; font-size: 11px; background: transparent; border: none; }");
        auto* row2 = new QWidget;
        row2->setStyleSheet("QWidget { background: transparent; border: none; }");
        auto* rl = new QHBoxLayout(row2);
        rl->setContentsMargins(0,0,0,0); rl->setSpacing(6);
        claudePathEdit_ = new QLineEdit;
        claudePathEdit_->setPlaceholderText("/opt/homebrew/bin/claude");
        claudePathEdit_->setStyleSheet(
            "QLineEdit { background: #1f2328; color: #d7dde5; border: 1px solid #353b44; "
            "border-radius: 6px; padding: 6px 10px; font-size: 12px; }"
            "QLineEdit:focus { border-color: #51cf66; }");
        auto* browseBtn = new QPushButton("…");
        browseBtn->setFixedSize(30, 30);
        browseBtn->setStyleSheet(
            "QPushButton { background: #2b3037; color: #868e96; border: 1px solid #353b44; "
            "border-radius: 6px; font-size: 13px; }"
            "QPushButton:hover { color: #d6dde6; }");
        connect(browseBtn, &QPushButton::clicked, this, [this]() {
            QString p = QFileDialog::getOpenFileName(this, "Select Claude CLI binary");
            if (!p.isEmpty()) claudePathEdit_->setText(p);
        });
        rl->addWidget(claudePathEdit_, 1);
        rl->addWidget(browseBtn);
        pl->addWidget(lbl);
        pl->addWidget(row2);
    }
    layout->addWidget(pathRow_);

    // Ollama row
    ollamaRow_ = new QWidget;
    ollamaRow_->setStyleSheet("QWidget { background: transparent; border: none; }");
    {
        auto* ol = new QVBoxLayout(ollamaRow_);
        ol->setContentsMargins(0,4,0,0); ol->setSpacing(4);
        auto* lbl = new QLabel("Server URL  /  Model name");
        lbl->setStyleSheet("QLabel { color: #868e96; font-size: 11px; background: transparent; border: none; }");
        ollamaUrlEdit_ = new QLineEdit;
        ollamaUrlEdit_->setPlaceholderText("http://localhost:11434");
        ollamaUrlEdit_->setStyleSheet(
            "QLineEdit { background: #1f2328; color: #d7dde5; border: 1px solid #353b44; "
            "border-radius: 6px; padding: 6px 10px; font-size: 12px; }"
            "QLineEdit:focus { border-color: #51cf66; }");
        ollamaModelEdit_ = new QLineEdit;
        ollamaModelEdit_->setPlaceholderText("llama3.2");
        ollamaModelEdit_->setStyleSheet(ollamaUrlEdit_->styleSheet());
        ol->addWidget(lbl);
        ol->addWidget(ollamaUrlEdit_);
        ol->addWidget(ollamaModelEdit_);
    }
    layout->addWidget(ollamaRow_);

    // wDIN Base address row (only for Claude CLI)
    wdinRow_ = new QWidget;
    wdinRow_->setStyleSheet("QWidget { background: transparent; border: none; }");
    {
        auto* wl = new QVBoxLayout(wdinRow_);
        wl->setContentsMargins(0, 4, 0, 0); wl->setSpacing(4);
        auto* lbl = new QLabel("Base address (wDIN required)");
        lbl->setStyleSheet("QLabel { color: #868e96; font-size: 11px; background: transparent; border: none; }");
        baseAddressEdit_ = new QLineEdit;
        baseAddressEdit_->setPlaceholderText("0x…  (must hold wDIN v2 to unlock)");
        baseAddressEdit_->setStyleSheet(
            "QLineEdit { background: #1f2328; color: #d7dde5; border: 1px solid #353b44; "
            "border-radius: 6px; padding: 6px 10px; font-size: 12px; }"
            "QLineEdit:focus { border-color: #51cf66; }");
        auto* hint = new QLabel(
            "<a href='https://bridge.dinero-coin.com' style='color:#5c7cfa;'>Bridge DIN → wDIN</a>"
            " · contract: 0x0C979…1119");
        hint->setOpenExternalLinks(true);
        hint->setStyleSheet("QLabel { color: #5c6370; font-size: 10px; background: transparent; border: none; }");
        wl->addWidget(lbl);
        wl->addWidget(baseAddressEdit_);
        wl->addWidget(hint);
    }
    layout->addWidget(wdinRow_);

    // Hint
    configHint_ = new QLabel;
    configHint_->setWordWrap(true);
    configHint_->setAlignment(Qt::AlignCenter);
    configHint_->setStyleSheet(
        "QLabel { color: #5c6370; font-size: 10px; background: transparent; border: none; }");
    layout->addWidget(configHint_);

    layout->addStretch();

    saveBtn_ = new QPushButton("Save & Start");
    saveBtn_->setMinimumHeight(38);
    saveBtn_->setStyleSheet(
        "QPushButton { background: #51cf66; color: #181b20; border: none; "
        "border-radius: 8px; font-size: 13px; font-weight: 700; }"
        "QPushButton:hover { background: #69db7c; }"
        "QPushButton:pressed { background: #40c057; }"
        "QPushButton:disabled { background: #2b3037; color: #5c6370; }");
    connect(saveBtn_, &QPushButton::clicked, this, &AiPanel::onSaveConfig);
    layout->addWidget(saveBtn_);

    connect(group, &QButtonGroup::idClicked, this, [this](int) { onProviderChanged(); });

    // Load saved config
    auto cfg = AiBackend::Config::load();
    switch (cfg.provider) {
    case AiBackend::Provider::ClaudeCli: rbCli_->setChecked(true);    break;
    case AiBackend::Provider::ClaudeApi: rbClaude_->setChecked(true); break;
    case AiBackend::Provider::Gemini:    rbGemini_->setChecked(true); break;
    case AiBackend::Provider::Groq:      rbGroq_->setChecked(true);   break;
    case AiBackend::Provider::Ollama:    rbOllama_->setChecked(true); break;
    case AiBackend::Provider::Mlx:       rbMlx_->setChecked(true);    break;
    }
    apiKeyEdit_->setText(cfg.apiKey);
    claudePathEdit_->setText(cfg.claudePath);
    if (!cfg.ollamaUrl.isEmpty())   ollamaUrlEdit_->setText(cfg.ollamaUrl);
    if (!cfg.ollamaModel.isEmpty()) ollamaModelEdit_->setText(cfg.ollamaModel);
    if (!cfg.baseAddress.isEmpty()) baseAddressEdit_->setText(cfg.baseAddress);

    onProviderChanged();
}

void AiPanel::onProviderChanged()
{
    bool isCli    = rbCli_->isChecked();
    bool isOllama = rbOllama_->isChecked();
    bool isMlx    = rbMlx_->isChecked();
    bool needsKey = !isCli && !isOllama && !isMlx;

    keyRow_->setVisible(needsKey);
    pathRow_->setVisible(isCli);
    ollamaRow_->setVisible(isOllama || isMlx);
    wdinRow_->setVisible(isCli);  // wDIN gate only applies to Claude CLI

    if      (rbGemini_->isChecked()) configHint_->setText("Get free key at aistudio.google.com");
    else if (rbGroq_->isChecked())   configHint_->setText("Get free key at console.groq.com");
    else if (rbClaude_->isChecked()) configHint_->setText("Uses your Anthropic API key");
    else if (isCli)                  configHint_->setText("Uses claude CLI on this machine");
    else if (isOllama)               configHint_->setText("Requires Ollama running locally (ollama.ai)");
    else if (isMlx)                  configHint_->setText("pip install mlx-lm  |  python -m mlx_lm.server --model <model>");
    else                             configHint_->clear();

    // Pre-fill default URL for MLX vs Ollama
    if (isMlx && ollamaUrlEdit_ && ollamaUrlEdit_->text().isEmpty())
        ollamaUrlEdit_->setText("http://localhost:8080");
    else if (isOllama && ollamaUrlEdit_ && ollamaUrlEdit_->text().isEmpty())
        ollamaUrlEdit_->setText("http://localhost:11434");
}

void AiPanel::onSaveConfig()
{
    bool isCli = rbCli_->isChecked();

    // For Claude CLI, require a Base address with wDIN v2 balance > 0
    if (isCli) {
        QString addr = baseAddressEdit_ ? baseAddressEdit_->text().trimmed() : QString();
        if (addr.isEmpty() || !addr.startsWith("0x") || addr.length() != 42) {
            configHint_->setStyleSheet(
                "QLabel { color: #ff6b6b; font-size: 11px; background: transparent; border: none; }");
            configHint_->setText("Enter your Base address (0x…) to verify wDIN balance");
            return;
        }
        configHint_->setStyleSheet(
            "QLabel { color: #51cf66; font-size: 11px; background: transparent; border: none; }");
        configHint_->setText("Checking wDIN balance…");
        if (saveBtn_) saveBtn_->setEnabled(false);
        checkWdinBalance(addr);
        return;   // onWdinCheckDone() will proceed
    }

    // Non-CLI providers proceed immediately
    AiBackend::Config cfg;
    if      (rbClaude_->isChecked()) cfg.provider = AiBackend::Provider::ClaudeApi;
    else if (rbGemini_->isChecked()) cfg.provider = AiBackend::Provider::Gemini;
    else if (rbGroq_->isChecked())   cfg.provider = AiBackend::Provider::Groq;
    else if (rbOllama_->isChecked()) cfg.provider = AiBackend::Provider::Ollama;
    else if (rbMlx_->isChecked())    cfg.provider = AiBackend::Provider::Mlx;
    else                             cfg.provider = AiBackend::Provider::Gemini;

    cfg.apiKey      = apiKeyEdit_->text().trimmed();
    cfg.claudePath  = claudePathEdit_->text().trimmed();
    cfg.ollamaUrl   = ollamaUrlEdit_->text().trimmed().isEmpty()
                    ? "http://localhost:11434" : ollamaUrlEdit_->text().trimmed();
    cfg.ollamaModel = ollamaModelEdit_->text().trimmed().isEmpty()
                    ? "llama3.2" : ollamaModelEdit_->text().trimmed();
    cfg.save();
    applyBackend();

    chatLog_.clear();
    sessionCost_ = 0.0;
    if (costLabel_) costLabel_->clear();
    stack_->setCurrentWidget(chatScreen_);
    appendSystemMessage(QString("DineroAI ready — %1").arg(cfg.providerLabel()));
    addSuggestedChips();
    if (inputField_) inputField_->setFocus();
}

// ──────────────────────────────────────────────────────────────
// wDIN gate: async balance check via Base RPC
// ──────────────────────────────────────────────────────────────

void AiPanel::checkWdinBalance(const QString& address)
{
    if (!netMgr_) netMgr_ = new QNetworkAccessManager(this);

    // ABI-encode balanceOf(address): selector 0x70a08231 + address padded to 32 bytes
    QString padded = address.mid(2).toLower().rightJustified(64, '0');
    QString data   = "0x70a08231" + padded.right(64);

    QJsonObject callObj;
    callObj["to"]   = QString("0x0C979443Cb22Cf40e010ce1532ca5a445BBF1119");
    callObj["data"] = data;

    QJsonObject body;
    body["jsonrpc"] = "2.0";
    body["id"]      = 1;
    body["method"]  = "eth_call";
    body["params"]  = QJsonArray{ callObj, QJsonValue("latest") };

    QNetworkRequest req(QUrl("https://mainnet.base.org"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QString savedAddr = address;
    auto* reply = netMgr_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply, savedAddr]() {
        reply->deleteLater();
        quint64 balance = 0;
        if (reply->error() == QNetworkReply::NoError) {
            auto doc = QJsonDocument::fromJson(reply->readAll());
            QString hex = doc.object()["result"].toString();
            if (hex.startsWith("0x")) {
                bool ok = false;
                balance = hex.mid(2).toULongLong(&ok, 16);
            }
        }
        if (baseAddressEdit_) {
            // Store address so it persists regardless of outcome
            AiBackend::Config cfg = AiBackend::Config::load();
            cfg.baseAddress = savedAddr;
            cfg.save();
        }
        onWdinCheckDone(balance);
    });
}

void AiPanel::onWdinCheckDone(quint64 balance)
{
    if (saveBtn_) saveBtn_->setEnabled(true);

    if (balance == 0) {
        configHint_->setStyleSheet(
            "QLabel { color: #ff6b6b; font-size: 11px; background: transparent; border: none; }");
        configHint_->setText(
            "No wDIN found on this address. Bridge DIN → wDIN at bridge.dinero-coin.com");
        return;
    }

    // Balance confirmed — proceed
    double din = static_cast<double>(balance) / 1e8;
    configHint_->setStyleSheet(
        "QLabel { color: #51cf66; font-size: 11px; background: transparent; border: none; }");
    configHint_->setText(QString("✓ %1 wDIN — access granted").arg(din, 0, 'f', 4));

    AiBackend::Config cfg;
    cfg.provider    = AiBackend::Provider::ClaudeCli;
    cfg.claudePath  = claudePathEdit_ ? claudePathEdit_->text().trimmed() : QString();
    cfg.baseAddress = baseAddressEdit_ ? baseAddressEdit_->text().trimmed() : QString();
    cfg.ollamaUrl   = "http://localhost:11434";
    cfg.ollamaModel = "llama3.2";
    cfg.save();
    applyBackend();

    chatLog_.clear();
    sessionCost_ = 0.0;
    if (costLabel_) costLabel_->clear();
    stack_->setCurrentWidget(chatScreen_);
    appendSystemMessage(
        QString("DineroAI ready — Claude CLI  |  %1 wDIN").arg(din, 0, 'f', 4));
    addSuggestedChips();
    if (inputField_) inputField_->setFocus();
}

// ──────────────────────────────────────────────────────────────
// Chat screen
// ──────────────────────────────────────────────────────────────

void AiPanel::buildChatScreen()
{
    chatScreen_ = new QWidget;
    chatScreen_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(chatScreen_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header
    auto* header = new QWidget;
    header->setFixedHeight(40);
    header->setObjectName("aiHeader");
    header->setStyleSheet("#aiHeader { background: #20242a; border-bottom: 1px solid #2f343c; }");
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 0, 8, 0);

    auto* headerTitle = new QLabel("DineroAI");
    headerTitle->setStyleSheet(
        "QLabel { color: #eef2f6; font-size: 14px; font-weight: 700; "
        "background: transparent; border: none; }");
    headerLayout->addWidget(headerTitle);

    providerLabel_ = new QLabel;
    providerLabel_->setStyleSheet(
        "QLabel { color: #51cf66; font-size: 10px; background: transparent; border: none; }");
    headerLayout->addWidget(providerLabel_);

    costLabel_ = new QLabel;
    costLabel_->setStyleSheet(
        "QLabel { color: #5c6370; font-size: 11px; background: transparent; border: none; }");
    headerLayout->addWidget(costLabel_);

    headerLayout->addStretch();

    auto* settingsBtn = new QPushButton("⚙");
    settingsBtn->setFixedSize(26, 26);
    settingsBtn->setToolTip("AI Settings");
    settingsBtn->setStyleSheet(
        "QPushButton { background: transparent; color: #5c6370; border: none; font-size: 14px; }"
        "QPushButton:hover { color: #d6dde6; }");
    connect(settingsBtn, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentWidget(configScreen_);
    });
    headerLayout->addWidget(settingsBtn);

    auto* clearBtn = new QPushButton("Clear");
    clearBtn->setFixedSize(44, 26);
    clearBtn->setStyleSheet(
        "QPushButton { background: #2b3037; color: #868e96; border: 1px solid #353b44; "
        "border-radius: 5px; font-size: 11px; font-weight: 600; }"
        "QPushButton:hover { color: #d6dde6; background: #333942; }");
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        if (backend_) backend_->clearSession();
        chatLog_.clear(); streamingText_.clear();
        sessionCost_ = 0.0;
        if (costLabel_) costLabel_->clear();
        rebuildChatHtml();
        addSuggestedChips();
        QFile::remove(chatHistoryPath());
    });
    headerLayout->addWidget(clearBtn);

    auto* closeBtn = new QPushButton("✕");
    closeBtn->setFixedSize(26, 26);
    closeBtn->setStyleSheet(
        "QPushButton { background: transparent; color: #868e96; border: none; font-size: 13px; font-weight: 700; }"
        "QPushButton:hover { color: #ff6b6b; }");
    connect(closeBtn, &QPushButton::clicked, this, &AiPanel::togglePanel);
    headerLayout->addWidget(closeBtn);

    layout->addWidget(header);

    // Chat display
    chatDisplay_ = new QTextEdit;
    chatDisplay_->setReadOnly(true);
    chatDisplay_->setFocusPolicy(Qt::StrongFocus);
    chatDisplay_->setStyleSheet(
        "QTextEdit { background: #181b20; color: #d6dde6; border: none; "
        "font-family: \"Space Mono\", \"SF Mono\", Menlo, monospace; font-size: 12px; "
        "selection-background-color: #3e4550; }");
    chatDisplay_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    chatDisplay_->document()->setDocumentMargin(12);
    ScrollSupport::enableForScrollArea(chatDisplay_);
    layout->addWidget(chatDisplay_, 1);

    // Suggested chips
    chipsRow_ = new QWidget;
    chipsRow_->setStyleSheet("QWidget { background: transparent; }");
    auto* chipsLayout = new QHBoxLayout(chipsRow_);
    chipsLayout->setContentsMargins(8, 4, 8, 4);
    chipsLayout->setSpacing(6);
    auto makeChip = [this, chipsLayout](const QString& text) {
        auto* chip = new QPushButton(text);
        chip->setStyleSheet(
            "QPushButton { background: #20242a; color: #868e96; border: 1px solid #353b44; "
            "border-radius: 12px; padding: 4px 10px; font-size: 11px; }"
            "QPushButton:hover { color: #d6dde6; background: #2b3037; }");
        chip->setCursor(Qt::PointingHandCursor);
        connect(chip, &QPushButton::clicked, this, [this, text]() { onChipClicked(text); });
        chipsLayout->addWidget(chip);
    };
    makeChip("My balance?");
    makeChip("BTC price?");
    makeChip("Shield 1 DIN");
    makeChip("Market overview?");
    chipsLayout->addStretch();
    layout->addWidget(chipsRow_);

    // Image preview strip (hidden until image attached)
    imagePreviewStrip_ = new QWidget;
    imagePreviewStrip_->setStyleSheet(
        "QWidget { background: #1f2328; border-top: 1px solid #353b44; }");
    imagePreviewStrip_->setFixedHeight(48);
    imagePreviewStrip_->hide();
    {
        auto* row = new QHBoxLayout(imagePreviewStrip_);
        row->setContentsMargins(8, 4, 8, 4);
        row->setSpacing(8);
        imageThumb_ = new QLabel;
        imageThumb_->setFixedSize(36, 36);
        imageThumb_->setScaledContents(true);
        imageThumb_->setStyleSheet("QLabel { border-radius: 4px; border: 1px solid #353b44; }");
        row->addWidget(imageThumb_);
        imageFilename_ = new QLabel;
        imageFilename_->setStyleSheet(
            "QLabel { color: #868e96; font-size: 11px; background: transparent; border: none; }");
        imageFilename_->setMaximumWidth(200);
        row->addWidget(imageFilename_, 1);
        auto* removeBtn = new QPushButton("✕");
        removeBtn->setFixedSize(20, 20);
        removeBtn->setStyleSheet(
            "QPushButton { background: transparent; color: #5c6370; border: none; font-size: 11px; }"
            "QPushButton:hover { color: #ff6b6b; }");
        connect(removeBtn, &QPushButton::clicked, this, [this]() {
            pendingImageBase64_.clear();
            pendingImageMime_.clear();
            pendingImagePath_.clear();
            if (backend_) backend_->clearAttachedImage();
            imagePreviewStrip_->hide();
        });
        row->addWidget(removeBtn);
    }
    layout->addWidget(imagePreviewStrip_);

    // Green separator
    auto* separator = new QWidget;
    separator->setFixedHeight(2);
    separator->setStyleSheet("background: #51cf66;");
    layout->addWidget(separator);

    // Input row
    auto* inputLayout = new QHBoxLayout;
    inputLayout->setContentsMargins(8, 8, 8, 4);
    inputLayout->setSpacing(6);

    inputField_ = new QLineEdit;
    inputField_->setMinimumHeight(36);
    inputField_->setPlaceholderText("Ask anything crypto…");
    inputField_->setStyleSheet(
        "QLineEdit { background: #1f2328; color: #d7dde5; "
        "border: 1px solid #353b44; border-radius: 8px; padding: 0 12px; "
        "font-size: 13px; font-family: \"Space Mono\", \"SF Mono\", Menlo, monospace; }"
        "QLineEdit:focus { border-color: #51cf66; }");
    connect(inputField_, &QLineEdit::returnPressed, this, &AiPanel::onSendClicked);
    inputLayout->addWidget(inputField_, 1);

    attachButton_ = new QPushButton("📎");
    attachButton_->setFixedSize(36, 36);
    attachButton_->setCursor(Qt::PointingHandCursor);
    attachButton_->setToolTip("Attach image (vision models)");
    attachButton_->setStyleSheet(
        "QPushButton { background: #2b3037; color: #d6dde6; border: 1px solid #353b44; "
        "border-radius: 8px; font-size: 15px; }"
        "QPushButton:hover { background: #333942; border-color: #51cf66; }"
        "QPushButton:checked { background: #1a3a2a; border-color: #51cf66; color: #51cf66; }");
    attachButton_->setCheckable(true);
    connect(attachButton_, &QPushButton::clicked, this, &AiPanel::onAttachClicked);
    inputLayout->addWidget(attachButton_);

    voiceButton_ = new QPushButton("Mic");
    voiceButton_->setFixedSize(42, 36);
    voiceButton_->setCursor(Qt::PointingHandCursor);
    voiceButton_->setStyleSheet(
        "QPushButton { background: #2b3037; color: #d6dde6; border: 1px solid #353b44; "
        "border-radius: 8px; font-size: 11px; font-weight: 600; }"
        "QPushButton:hover { background: #333942; border-color: #51cf66; }");
    connect(voiceButton_, &QPushButton::clicked, this, &AiPanel::onVoiceClicked);
    inputLayout->addWidget(voiceButton_);

    sendButton_ = new QPushButton("Send");
    sendButton_->setFixedSize(50, 36);
    sendButton_->setStyleSheet(
        "QPushButton { background: #51cf66; color: #181b20; border: none; "
        "border-radius: 8px; font-size: 12px; font-weight: 700; }"
        "QPushButton:hover { background: #69db7c; }"
        "QPushButton:pressed { background: #40c057; }"
        "QPushButton:disabled { background: #2b3037; color: #5c6370; }");
    connect(sendButton_, &QPushButton::clicked, this, &AiPanel::onSendClicked);
    inputLayout->addWidget(sendButton_);

    layout->addLayout(inputLayout);

    auto* watermark = new QLabel("DineroAI — your crypto assistant");
    watermark->setAlignment(Qt::AlignCenter);
    watermark->setStyleSheet(
        "QLabel { color: #484f58; font-size: 9px; background: transparent; "
        "border: none; padding: 0 0 4px 0; }");
    watermark->setFixedHeight(16);
    layout->addWidget(watermark);
}

// ──────────────────────────────────────────────────────────────
// Actions
// ──────────────────────────────────────────────────────────────

void AiPanel::onStartClicked() {}

void AiPanel::onAttachClicked()
{
    // If image already attached, clicking again clears it
    if (!pendingImageBase64_.isEmpty()) {
        pendingImageBase64_.clear();
        pendingImageMime_.clear();
        pendingImagePath_.clear();
        if (backend_) backend_->clearAttachedImage();
        imagePreviewStrip_->hide();
        attachButton_->setChecked(false);
        return;
    }

    QString path = QFileDialog::getOpenFileName(
        this, "Attach Image", {},
        "Images (*.png *.jpg *.jpeg *.webp *.gif)");
    if (path.isEmpty()) { attachButton_->setChecked(false); return; }

    QImage img(path);
    if (img.isNull()) {
        appendErrorMessage("Could not load image: " + QFileInfo(path).fileName());
        attachButton_->setChecked(false);
        return;
    }

    // Scale down to max 1024px on longest side (keeps API payloads reasonable)
    if (img.width() > 1024 || img.height() > 1024)
        img = img.scaled(1024, 1024, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // Encode to JPEG base64
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 85);
    buf.close();

    pendingImageBase64_ = QString::fromLatin1(bytes.toBase64());
    pendingImageMime_   = "image/jpeg";
    pendingImagePath_   = path;

    // Show thumbnail in preview strip
    QPixmap thumb = QPixmap::fromImage(img.scaled(36, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    imageThumb_->setPixmap(thumb);
    imageFilename_->setText(QFileInfo(path).fileName());
    imagePreviewStrip_->show();
    attachButton_->setChecked(true);
}

void AiPanel::onChipClicked(const QString& text)
{
    if (inputField_) inputField_->setText(text);
    onSendClicked();
}

void AiPanel::onSendClicked()
{
    if (!inputField_) return;
    QString text = inputField_->text().trimmed();
    if (text.isEmpty() || isStreaming_) return;
    if (!backend_ || !backend_->isReady()) {
        appendErrorMessage("No AI provider configured. Click ⚙ to set up.");
        return;
    }
    inputField_->clear();
    if (chipsRow_) chipsRow_->hide();

    // Pass pending image to backend before sending
    if (!pendingImageBase64_.isEmpty())
        backend_->setAttachedImage(pendingImageBase64_, pendingImageMime_);

    appendUserMessage(text);  // renders with thumbnail if image attached
    backend_->sendMessage(text);

    // Clear attachment UI (backend consumed the image data)
    pendingImageBase64_.clear();
    pendingImageMime_.clear();
    pendingImagePath_.clear();
    if (imagePreviewStrip_) imagePreviewStrip_->hide();
    if (attachButton_) attachButton_->setChecked(false);
}

// ──────────────────────────────────────────────────────────────
// Backend callbacks
// ──────────────────────────────────────────────────────────────

void AiPanel::onStreamingStarted()
{
    isStreaming_ = true;
    sendButton_->setEnabled(false);
    inputField_->setEnabled(false);
    startAssistantMessage();
}

void AiPanel::onStreamingFinished()
{
    isStreaming_ = false;
    sendButton_->setEnabled(true);
    inputField_->setEnabled(true);
    inputField_->setFocus();
}

void AiPanel::onTextDelta(const QString& delta)
{
    streamingText_ += delta;
    rebuildChatHtml();
    scrollToBottom();
}

void AiPanel::onTurnComplete(const QString& fullText)
{
    Q_UNUSED(fullText)
    finalizeAssistantMessage();
}

void AiPanel::onError(const QString& error) { appendErrorMessage(error); }

void AiPanel::onToolActivity(const QString& toolName, const QString& input)
{
    QString display;
    if      (toolName == "get_din_balance")     display = "checking balance…";
    else if (toolName == "get_node_status")     display = "checking node…";
    else if (toolName == "get_crypto_prices")   display = "fetching prices…";
    else if (toolName == "get_market_overview") display = "market data…";
    else if (toolName == "get_wdin_info")       display = "wDIN info…";
    else if (toolName == "shield_din")          display = "shielding DIN…";
    else if (toolName == "unshield_din")        display = "unshielding DIN…";
    else if (toolName == "send_din")            display = "sending DIN…";
    else if (toolName.startsWith("mcp__ops__")) display = toolName.mid(10) + "…";
    else display = toolName + (input.isEmpty() ? "…" : ": " + input.left(50));
    appendToolMessage(display);
}

void AiPanel::onCostUpdate(double totalCostUsd)
{
    sessionCost_ = totalCostUsd;
    if (costLabel_ && totalCostUsd > 0)
        costLabel_->setText(QString("$%1").arg(sessionCost_, 0, 'f', 4));
}

// ──────────────────────────────────────────────────────────────
// Voice
// ──────────────────────────────────────────────────────────────

void AiPanel::onVoiceClicked()
{
    if (isListening_) {
        if (voiceProcess_ && voiceProcess_->state() == QProcess::Running)
            voiceProcess_->terminate();
        return;
    }
    QString soxPath = QStandardPaths::findExecutable("sox");
    if (soxPath.isEmpty()) {
        auto* proc = new QProcess(this);
        proc->start("osascript", {"-e",
            "tell application \"System Events\" to key code 63 using {function down}"});
        connect(proc, qOverload<int,QProcess::ExitStatus>(&QProcess::finished),
                proc, &QProcess::deleteLater);
        appendSystemMessage("Press Fn twice to dictate.");
        return;
    }
    isListening_ = true;
    voiceButton_->setText("Stop");
    voiceButton_->setStyleSheet(
        "QPushButton { background: #ff6b6b; color: #fff; border: none; "
        "border-radius: 8px; font-size: 11px; }");
    appendSystemMessage("Listening… (8s max)");
    QString tmpWav = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                   + "/dinero_voice.wav";
    voiceProcess_ = new QProcess(this);
    connect(voiceProcess_, qOverload<int,QProcess::ExitStatus>(&QProcess::finished),
            this, &AiPanel::onVoiceFinished);
    voiceProcess_->start(soxPath, {"-d","-r","16000","-c","1",tmpWav,"trim","0","8"});
}

void AiPanel::onVoiceFinished(int, QProcess::ExitStatus)
{
    isListening_ = false;
    voiceButton_->setText("Mic");
    voiceButton_->setStyleSheet(
        "QPushButton { background: #2b3037; color: #d6dde6; border: 1px solid #353b44; "
        "border-radius: 8px; font-size: 11px; font-weight: 600; }"
        "QPushButton:hover { background: #333942; border-color: #51cf66; }");
    if (voiceProcess_) { voiceProcess_->deleteLater(); voiceProcess_ = nullptr; }
    QString tmpWav = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                   + "/dinero_voice.wav";
    if (!QFile::exists(tmpWav)) { appendSystemMessage("No audio recorded."); return; }
    QString wp = QStandardPaths::findExecutable("whisper");
    if (wp.isEmpty()) wp = QStandardPaths::findExecutable("whisper-cpp");
    if (wp.isEmpty()) { appendSystemMessage("Install whisper for transcription."); return; }
    appendSystemMessage("Transcribing…");
    auto* proc = new QProcess(this);
    proc->start(wp, {tmpWav,"--model","base","--output_format","txt","--language","en"});
    connect(proc, qOverload<int,QProcess::ExitStatus>(&QProcess::finished),
            this, [this, proc, tmpWav]() {
        QString text = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
        proc->deleteLater();
        if (text.isEmpty()) {
            QString txt = tmpWav; txt.replace(".wav",".txt");
            QFile f(txt);
            if (f.open(QIODevice::ReadOnly)) text = QString::fromUtf8(f.readAll()).trimmed();
        }
        if (!text.isEmpty()) inputField_->setText(text);
        else appendSystemMessage("Transcription empty. Try again.");
    });
}

// ──────────────────────────────────────────────────────────────
// Chat rendering
// ──────────────────────────────────────────────────────────────

void AiPanel::appendUserMessage(const QString& text)
{
    ChatEntry e;
    e.type        = ChatEntry::User;
    e.text        = text.toHtmlEscaped();
    e.imageBase64 = pendingImageBase64_; // snapshot — may already be cleared by caller
    chatLog_.append(e);
    rebuildChatHtml(); scrollToBottom(); saveConversation();
}
void AiPanel::startAssistantMessage() { streamingText_.clear(); }
void AiPanel::finalizeAssistantMessage()
{
    if (!streamingText_.isEmpty())
        chatLog_.append({ChatEntry::Assistant, streamingText_});
    streamingText_.clear();
    rebuildChatHtml(); scrollToBottom(); saveConversation();
}
void AiPanel::appendToolMessage(const QString& text)
{
    chatLog_.append({ChatEntry::Tool, text.toHtmlEscaped()});
    rebuildChatHtml(); scrollToBottom();
}
void AiPanel::appendErrorMessage(const QString& error)
{
    chatLog_.append({ChatEntry::Error, error.toHtmlEscaped()});
    rebuildChatHtml(); scrollToBottom();
}
void AiPanel::appendSystemMessage(const QString& text)
{
    chatLog_.append({ChatEntry::System, text.toHtmlEscaped()});
    rebuildChatHtml(); scrollToBottom();
}
void AiPanel::addSuggestedChips() { if (chipsRow_) chipsRow_->show(); }
void AiPanel::scrollToBottom()
{
    if (chatDisplay_)
        chatDisplay_->verticalScrollBar()->setValue(
            chatDisplay_->verticalScrollBar()->maximum());
}

void AiPanel::rebuildChatHtml()
{
    if (!chatDisplay_) return;
    QString html;
    html += "<style>"
            "body{margin:0;padding:0;}"
            ".msg{margin:6px 0;padding:0;clear:both;overflow:hidden;}"
            ".user .bubble{float:right;background:#2b3037;color:#eef2f6;"
            "  border-radius:12px 12px 2px 12px;padding:8px 12px;"
            "  max-width:85%;font-size:12px;}"
            ".ai .bubble{float:left;background:#20242a;color:#d6dde6;"
            "  border:1px solid #353b44;border-radius:12px 12px 12px 2px;"
            "  padding:8px 12px;max-width:90%;font-size:12px;}"
            ".tool{text-align:center;color:#51cf66;font-size:10px;"
            "  margin:2px 0;padding:3px 0;font-family:'Space Mono',monospace;}"
            ".error{text-align:center;color:#ff6b6b;font-size:11px;"
            "  margin:4px 8px;padding:6px;background:#2a1a1a;"
            "  border:1px solid #4a2020;border-radius:6px;white-space:pre-wrap;}"
            ".system{text-align:center;color:#868e96;font-size:11px;"
            "  margin:8px 0;font-style:italic;}"
            "code{background:#161920;color:#82aaff;padding:1px 5px;border-radius:3px;"
            "  font-family:\"Space Mono\",monospace;font-size:11px;}"
            "pre{background:#161920;color:#c3e88d;padding:10px 12px;border-radius:6px;"
            "  border-left:3px solid #51cf66;overflow-x:auto;font-size:11px;"
            "  margin:6px 0;line-height:1.4;}"
            "b{color:#eef2f6;}"
            "</style>";

    for (const auto& e : chatLog_) {
        switch (e.type) {
        case ChatEntry::User: {
            QString bubble = e.text;
            if (!e.imageBase64.isEmpty()) {
                bubble = QString("<img src='data:image/jpeg;base64,%1' "
                                 "style='max-width:200px;max-height:150px;"
                                 "border-radius:6px;display:block;margin-bottom:4px;'><br>%2")
                         .arg(e.imageBase64).arg(e.text);
            }
            html += QString("<div class='msg user'><div class='bubble'>%1</div></div>").arg(bubble);
            break;
        }
        case ChatEntry::Assistant:
            html += QString("<div class='msg ai'><div class='bubble'>%1</div></div>")
                        .arg(markdownToHtml(e.text));
            break;
        case ChatEntry::Tool:
            html += QString("<div class='tool'>⚡ %1</div>").arg(e.text);
            break;
        case ChatEntry::Error:
            html += QString("<div class='error'>%1</div>").arg(e.text);
            break;
        case ChatEntry::System:
            html += QString("<div class='system'>%1</div>").arg(e.text);
            break;
        }
    }
    if (!streamingText_.isEmpty()) {
        html += QString("<div class='msg ai'><div class='bubble'>%1"
                        "<span style='color:#51cf66;'>▌</span></div></div>")
                    .arg(markdownToHtml(streamingText_));
    }
    chatDisplay_->setHtml(html);
    scrollToBottom();
}

QString AiPanel::markdownToHtml(const QString& md) const
{
    QString out = md.toHtmlEscaped();
    static QRegularExpression codeBlock("```(?:\\w*)\n?(.*?)```",
                                        QRegularExpression::DotMatchesEverythingOption);
    out.replace(codeBlock, "<pre>\\1</pre>");
    static QRegularExpression inlineCode("`([^`]+)`");
    out.replace(inlineCode, "<code>\\1</code>");
    static QRegularExpression bold("\\*\\*(.+?)\\*\\*");
    out.replace(bold, "<b>\\1</b>");
    static QRegularExpression italic("(?<!\\*)\\*(?!\\*)(.+?)(?<!\\*)\\*(?!\\*)");
    out.replace(italic, "<i>\\1</i>");
    out.replace("\n", "<br>");
    return out;
}

// ──────────────────────────────────────────────────────────────
// System prompt
// ──────────────────────────────────────────────────────────────

QString AiPanel::systemPrompt() const
{
    return QStringLiteral(
        "You are DineroAI, an intelligent crypto assistant embedded in the Dinero wallet.\n\n"
        "You have live tools:\n"
        "- get_din_balance: DIN wallet balance (transparent + private lanes)\n"
        "- get_node_status: sync height, peers, chain info\n"
        "- get_receive_address: fresh DIN address for receiving\n"
        "- shield_din(amount_din): transparent → private/confidential lane\n"
        "- unshield_din(amount_din, to_address): private → transparent\n"
        "- send_din(to_address, amount_din): send DIN\n"
        "- get_crypto_prices(coins[]): live USD prices for btc, eth, xmr, sol, din, etc.\n"
        "- get_market_overview: total market cap, BTC dominance, 24h trend\n"
        "- get_wdin_info: wDIN (Wrapped DIN) on Base blockchain / Uniswap\n\n"
        "DINERO PROTOCOL:\n"
        "- SHA-256d PoW, 2-min blocks, coin_type=1448\n"
        "- 1 DIN = 100,000,000 una. Always show amounts in DIN.\n"
        "- Transparent: din1p... | Private: dina1...\n"
        "- Shield = transparent→private, Unshield = private→transparent\n"
        "- wDIN contract: 0xCD91b5C0aaD48E49F992BA690647C244f535C90B (Base)\n\n"
        "RULES: Use tools for live data. Be concise. Confirm before shield/send actions."
    );
}

// ──────────────────────────────────────────────────────────────
// Persistence
// ──────────────────────────────────────────────────────────────

void AiPanel::saveConversation()
{
    QJsonArray arr;
    for (const auto& e : chatLog_) {
        QJsonObject o; o["type"] = static_cast<int>(e.type); o["text"] = e.text;
        arr.append(o);
    }
    QJsonObject root; root["chat"] = arr; root["cost"] = sessionCost_;
    QFile f(chatHistoryPath());
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void AiPanel::loadConversation()
{
    QFile f(chatHistoryPath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return;
    chatLog_.clear();
    for (const auto& v : doc.object()["chat"].toArray()) {
        auto o = v.toObject();
        chatLog_.append({static_cast<ChatEntry::Type>(o["type"].toInt()), o["text"].toString()});
    }
    sessionCost_ = doc.object()["cost"].toDouble();
    if (costLabel_ && sessionCost_ > 0)
        costLabel_->setText(QString("$%1").arg(sessionCost_, 0, 'f', 4));
    if (!chatLog_.isEmpty()) { rebuildChatHtml(); scrollToBottom(); }
}
