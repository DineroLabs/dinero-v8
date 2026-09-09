#include "privatecovenantwidget.h"
#include "rpcclient.h"
#include "covenantformpolicy.h"
#include "shieldedtransferpolicy.h"
#include <QComboBox>
#include <QCryptographicHash>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <limits>

PrivateCovenantWidget::PrivateCovenantWidget(RpcClient* rpc,QWidget* parent):QWidget(parent),rpc_(rpc) {
    auto* layout=new QVBoxLayout(this);
    auto* intro=new QLabel("Private covenants pay one or two fixed shielded recipients. Amounts and recipients are hidden; the earliest spend height and fee are public. Your wallet recovers the encrypted contract from its funding note. A public funding source reveals the amount entering the shielded pool.");
    intro->setWordWrap(true); layout->addWidget(intro);
    status_=new QLabel("Waiting for network capability"); status_->setWordWrap(true); layout->addWidget(status_);
    auto* form=new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setFormAlignment(Qt::AlignLeft|Qt::AlignTop);
    source_=new QComboBox; source_->addItem("Public balance","public"); source_->addItem("Private balance","private");
    form->addRow("Funding source:",source_);
    owner_=new QLineEdit; owner_->setObjectName("privateCovenantOwner");
    owner_->setPlaceholderText("Your shielded address; its wallet will control the contract");
    form->addRow("Contract owner:",owner_);
    auto* ownAddress=new QPushButton("Use my shielded address");
    ownAddress->setObjectName("privateCovenantOwnAddress");
    form->addRow(QString(),ownAddress);
    ownerStatus_=new QLabel; ownerStatus_->setObjectName("privateCovenantOwnerStatus");
    ownerStatus_->setWordWrap(true); form->addRow(QString(),ownerStatus_);
    connect(ownAddress,&QPushButton::clicked,this,[this]{
        if(scope_.isEmpty()) { ownerStatus_->setText("Select or load a wallet first."); return; }
        ownerStatus_->setText("Getting your shielded address…");
        ownerRequestScope_=scope_;
        rpc_->callNamed("wallet.getshieldedaddress",{{"account",0},{"j",0}});
    });
    height_=new QSpinBox; height_->setObjectName("privateCovenantHeight"); height_->setRange(0,std::numeric_limits<int>::max());
    height_->setToolTip("Absolute block height, not a duration. Zero permits spending after confirmation.");
    form->addRow("Earliest spend height:",height_);
    fee_=new QLineEdit("0.01000000"); fee_->setObjectName("privateCovenantFee");
    fee_->setToolTip("Reserved in the contract. The exact payment outputs cannot be reduced later to raise this fee. Funding fee is additional and estimated by the daemon.");
    form->addRow("Reserved spend fee (DIN):",fee_);
    fundingFee_=new QLineEdit("0.01000000"); fundingFee_->setObjectName("privateCovenantFundingFee");
    fundingFee_->setToolTip("Exact fee paid to fund this contract. If below relay policy, the daemon rejects the transaction without raising the fee.");
    form->addRow("Funding fee (DIN):",fundingFee_); layout->addLayout(form);
    outputs_=new QTableWidget(2,2); outputs_->setObjectName("privateCovenantOutputs");
    outputs_->setHorizontalHeaderLabels({"Shielded recipient","Payment (DIN)"});
    outputs_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); outputs_->setMaximumHeight(130);
    for(int row=0;row<2;++row) for(int col=0;col<2;++col) outputs_->setItem(row,col,new QTableWidgetItem);
    layout->addWidget(outputs_);
    fund_=new QPushButton("Review private covenant"); fund_->setObjectName("privateCovenantFund");
    connect(fund_,&QPushButton::clicked,this,&PrivateCovenantWidget::fund); layout->addWidget(fund_);
    auto* refreshButton=new QPushButton("Refresh private contracts");
    connect(refreshButton,&QPushButton::clicked,this,&PrivateCovenantWidget::refresh); layout->addWidget(refreshButton);
    resolve_=new QPushButton("Resolve previous submission…"); resolve_->setObjectName("privateCovenantResolve");
    layout->addWidget(resolve_);
    connect(resolve_,&QPushButton::clicked,this,[this]{
        if(!uncertain_ || !pendingMethod_.isEmpty()) return;
        const auto reviewedJournal=journalKey();
        if(QMessageBox::warning(this,"Resolve uncertain submission",
            "First check transaction history and refresh recovered contracts. The previous transaction may already have reached the network.\n\n"
            "Only continue after checking its outcome. This clears the local hold; it does not resubmit anything. A new funding operation creates a new contract and could duplicate a previously accepted payment.",
            QMessageBox::Ok|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Ok) return;
        if(reviewedJournal!=journalKey()) return;
        QSettings settings; settings.setValue(reviewedJournal,"manually_resolved"); settings.sync();
        if(settings.status()!=QSettings::NoError) { status_->setText("Cannot save resolution; the hold remains."); return; }
        uncertain_=false;
        for(int row=0;row<2;++row) for(int column=0;column<2;++column) outputs_->item(row,column)->setText({});
        status_->setText("Previous outcome reviewed. Compose a new operation if needed; nothing was resubmitted."); updateEnabled();
    });
    contracts_=new QTableWidget(0,4); contracts_->setObjectName("privateCovenantInventory");
    contracts_->setHorizontalHeaderLabels({"Value (DIN)","Earliest height","Status","Action"});
    contracts_->setEditTriggers(QAbstractItemView::NoEditTriggers); contracts_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(contracts_);
    connect(rpc_,&RpcClient::connectionFailed,this,[this](const QString&){ active_=false; updateEnabled(); });
    connect(rpc_,&RpcClient::rpcError,this,[this](const QString& method,int code,const QString& message){
        if(method=="wallet.getshieldedaddress" && !ownerRequestScope_.isEmpty() && ownerRequestScope_==scope_) {
            ownerRequestScope_.clear(); ownerStatus_->setText("Could not get your shielded address: "+message);
        }
        if(method=="wallet.shieldedbalance") { active_=false; updateEnabled(); }
        if(method==pendingMethod_) {
            // A transport/RPC failure is not proof of rejection. Keep the durable
            // submitting marker and require transaction reconciliation before retry.
            const QStringList beforeSubmissionErrors{
                "wallet_locked", "private_covenants_not_active", "invalid_private_covenant", "confirmed_private_covenant_required",
                "private_covenant_commitment_changed", "private_covenant_locked_or_invalid",
                "commitment_hex_required", "invalid_commitment_hex", "leaf_index_required"};
            const bool rejected = (code == -32603 && beforeSubmissionErrors.contains(message)) || (code == 401 && message == "unauthorized");
            if(rejected) { QSettings settings; settings.setValue(pendingJournal_,"rejected"); settings.sync(); }
            pendingMethod_.clear(); uncertain_=!rejected;
            status_->setText(rejected ? "Rejected before submission: "+message+". Resolve the condition, then review again." :
                "Outcome uncertain: "+message+". Check transaction history and recovered contracts; this operation will not be retried automatically.");
            updateEnabled();
        }
    });
    connect(rpc_,&RpcClient::rpcResult,this,[this](const QString& method,const QJsonValue& result){
        if(scope_.isEmpty() && method!=pendingMethod_) return;
        const auto object=result.toObject();
        if(method=="wallet.getshieldedaddress" && !ownerRequestScope_.isEmpty() && ownerRequestScope_==scope_) {
            const auto address=object.value("address").toString();
            if(!address.isEmpty() && (!object.contains("error") || object.value("error").isNull())) { owner_->setText(address); ownerStatus_->setText("Your shielded address is ready."); }
            else ownerStatus_->setText("Could not get your shielded address: "+object.value("error_message").toString(object.value("error").toString("Daemon returned no address.")));
            ownerRequestScope_.clear();
        } else if(method=="wallet.shieldedbalance") {
            active_=object.value("error").isUndefined() && object.value("private_covenants_enabled").isBool() && object.value("private_covenants_enabled").toBool();
            if(pendingMethod_.isEmpty() && !uncertain_) status_->setText(active_ ? "Private covenants available on this network" : "Private covenants unavailable: network activation or node readiness required");
            updateEnabled();
        } else if(method=="wallet.listshielded") {
            notes_=object.value("notes").toArray(); render();
        } else if(method==pendingMethod_) {
            const QString txid=object.value("txid").toString(), error=object.value("error").toString();
            QSettings settings;
            if(!error.isEmpty()) {
                settings.setValue(pendingJournal_,"rejected");
                status_->setText("Rejected: "+error+" "+object.value("error_message").toString());
            } else if(txid.size()==64) {
                settings.setValue(pendingJournal_,"accepted:"+txid);
                if(pendingMethod_=="wallet.covenant.privatefund" && pendingJournal_==journalKey())
                    for(int row=0;row<2;++row) for(int column=0;column<2;++column) outputs_->item(row,column)->setText({});
                status_->setText("Submitted: "+txid+". Wait for confirmation.");
            } else {
                uncertain_=true; status_->setText("No transaction ID returned. Outcome uncertain; check recovered contracts before taking further action.");
            }
            settings.sync(); pendingMethod_.clear(); pendingJournal_.clear(); updateEnabled();
            rpc_->call("wallet.listshielded");
        }
    });
    auto* timer=new QTimer(this);
    connect(timer,&QTimer::timeout,this,[this]{ if(isVisible() && pendingMethod_.isEmpty()) refresh(); });
    timer->start(10000);
    updateEnabled();
}
QString PrivateCovenantWidget::journalKey() const {
    const auto key=rpc_->datadir().toUtf8()+QByteArray(1,'\0')+scope_.toUtf8();
    return "privateCovenant/"+QString(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+"/operation";
}
void PrivateCovenantWidget::setWalletScope(const QString& scope) {
    if(scope_==scope) return;
    scope_=scope; ownerRequestScope_.clear(); active_=false; notes_={}; owner_->clear(); ownerStatus_->clear();
    height_->setValue(0); fee_->setText("0.01000000"); fundingFee_->setText("0.01000000"); source_->setCurrentIndex(0);
    for(int r=0;r<2;++r) for(int c=0;c<2;++c) outputs_->item(r,c)->setText({});
    // Preserve a live operation's original journal even across wallet switches.
    uncertain_=!scope_.isEmpty() && QSettings().value(journalKey()).toString()=="submitting";
    status_->setText(uncertain_ ? "A previous operation has an uncertain outcome. Check transaction history and recovered contracts; automatic retry is blocked." : "Waiting for network capability");
    render(); updateEnabled();
}
void PrivateCovenantWidget::refresh() {
    if(scope_.isEmpty()) return;
    rpc_->call("wallet.shieldedbalance"); rpc_->call("wallet.listshielded");
}
void PrivateCovenantWidget::updateEnabled() {
    resolve_->setVisible(uncertain_);
    resolve_->setEnabled(uncertain_ && pendingMethod_.isEmpty() && !scope_.isEmpty());
    fund_->setEnabled(active_ && !scope_.isEmpty() && pendingMethod_.isEmpty() && !uncertain_);
    render();
}
void PrivateCovenantWidget::render() {
    contracts_->setRowCount(0);
    for(const auto& value:notes_) {
        const auto note=value.toObject(); if(!note.value("private_covenant").toBool()) continue;
        const int row=contracts_->rowCount(); contracts_->insertRow(row);
        contracts_->setItem(row,0,new QTableWidgetItem(CovenantFormPolicy::formatUna(note.value("value_una").toInteger())));
        contracts_->setItem(row,1,new QTableWidgetItem(QString::number(note.value("minimum_height").toInteger())));
        const bool spent=note.value("spent").toBool(), confirmed=note.value("confirmed").toBool();
        contracts_->setItem(row,2,new QTableWidgetItem(spent ? "Spent / reserved" : confirmed ? (note.value("mature").toBool() ? "Ready" : "Timelocked") : "Awaiting confirmation"));
        auto* button=new QPushButton("Review payment");
        button->setEnabled(active_ && confirmed && note.value("mature").toBool() && !spent && pendingMethod_.isEmpty() && !uncertain_);
        connect(button,&QPushButton::clicked,this,[this,note]{spend(note);}); contracts_->setCellWidget(row,3,button);
    }
}
void PrivateCovenantWidget::fund() {
    if(!fund_->isEnabled()) return;
    const auto reviewedScope=journalKey();
    qint64 fee=0,total=0,fundingFee=0;
    if(!ShieldedTransferPolicy::parseDinToUna(fee_->text(),&fee) || fee<=0 || !ShieldedTransferPolicy::parseDinToUna(fundingFee_->text(),&fundingFee) || owner_->text().trimmed().isEmpty()) {
        status_->setText("Enter a contract owner and positive funding and reserved spend fees."); return;
    }
    QJsonArray outputs; QStringList review;
    for(int r=0;r<2;++r) {
        const auto address=outputs_->item(r,0)->text().trimmed(), amount=outputs_->item(r,1)->text().trimmed();
        if(address.isEmpty() && amount.isEmpty()) continue;
        qint64 value=0;
        if(address.isEmpty() || !CovenantFormPolicy::appendAmount(amount,total,value)) { status_->setText("Complete each recipient row with a positive amount of at most 8 decimals."); return; }
        outputs.append(QJsonObject{{"address",address},{"value_una",value}});
        review.append(address+"  "+CovenantFormPolicy::formatUna(value)+" DIN");
    }
    if(outputs.isEmpty() || total>std::numeric_limits<qint64>::max()-fee || fundingFee>std::numeric_limits<qint64>::max()-fee-total) { status_->setText("Enter one or two valid payments within the amount range."); return; }
    const QString text="Funding source: "+source_->currentText()+"\nOwner: "+owner_->text().trimmed()+
        "\nTotal locked: "+CovenantFormPolicy::formatUna(total+fee)+" DIN\nReserved spend fee: "+CovenantFormPolicy::formatUna(fee)+
        " DIN\nFunding fee: "+CovenantFormPolicy::formatUna(fundingFee)+" DIN\nTotal debit: "+CovenantFormPolicy::formatUna(total+fee+fundingFee)+" DIN\nEarliest block height: "+QString::number(height_->value())+"\n\n"+review.join("\n")+
        "\n\nThe exact recipients, amounts and spend fee cannot be changed after funding.";
    if(QMessageBox::question(this,"Review private covenant",text,QMessageBox::Ok|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Ok) return;
    if(reviewedScope!=journalKey()) return;
    submit("wallet.covenant.privatefund",{{"owner_address",owner_->text().trimmed()},{"source",source_->currentData().toString()},
        {"minimum_height",height_->value()},{"spend_fee_una",fee},{"fee_una",fundingFee},{"outputs",outputs}});
}
void PrivateCovenantWidget::spend(const QJsonObject& note) {
    const auto reviewedScope=journalKey();
    QStringList outputs;
    for(const auto& value:note.value("outputs").toArray()) {
        const auto output=value.toObject(); outputs.append(output.value("address").toString()+"  "+CovenantFormPolicy::formatUna(output.value("value_una").toInteger())+" DIN");
    }
    const QString text="Pay the committed recipients:\n"+outputs.join("\n")+"\nReserved fee: "+
        CovenantFormPolicy::formatUna(note.value("spend_fee_una").toInteger())+" DIN\nEarliest block: "+QString::number(note.value("minimum_height").toInteger());
    if(QMessageBox::question(this,"Review private covenant payment",text,QMessageBox::Ok|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Ok) return;
    if(reviewedScope!=journalKey()) return;
    submit("wallet.covenant.privatespend",{{"leaf_index",note.value("leaf_index")},{"commitment_hex",note.value("commitment_hex")}});
}
void PrivateCovenantWidget::submit(const QString& method,const QJsonObject& params) {
    if(!active_ || !pendingMethod_.isEmpty() || uncertain_) return;
    QSettings settings; pendingJournal_=journalKey(); settings.setValue(pendingJournal_,"submitting"); settings.sync();
    if(settings.status()!=QSettings::NoError) { status_->setText("Cannot save the operation journal; nothing submitted."); return; }
    pendingMethod_=method; status_->setText("Building and submitting proof. Please wait…"); updateEnabled(); rpc_->callNamed(method,params);
}
