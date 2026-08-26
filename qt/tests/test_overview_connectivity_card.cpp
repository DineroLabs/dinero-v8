// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "overviewconnectivitycard.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QtTest/QtTest>

using dinero::qt::OverviewConnectivityCard;

class TestOverviewConnectivityCard : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void init() {
        QSettings().setValue(QStringLiteral("network/tor_consent_v1"), true);
    }

    void maps_network_paths_and_automatic_tor() {
        OverviewConnectivityCard card;
        auto* toggle = card.findChild<QCheckBox*>(QStringLiteral("overviewTorToggle"));
        auto* summary = card.findChild<QLabel*>(QStringLiteral("overviewConnectivitySummary"));
        QVERIFY(toggle);
        QVERIFY(summary);

        card.setNetworkInfo(QJsonObject{
            {QStringLiteral("direct_reachable"), true},
            {QStringLiteral("relay_fallback_eligible"), true},
            {QStringLiteral("onion_service"), QJsonObject{
                {QStringLiteral("requested"), true},
                {QStringLiteral("active"), true},
                {QStringLiteral("mode"), QStringLiteral("automatic")}}}});

        QVERIFY(toggle->isEnabled());
        QVERIFY(toggle->isChecked());
        QVERIFY(summary->text().contains(QStringLiteral("Direct active")));
        QVERIFY(summary->text().contains(QStringLiteral("Relay fallback ready")));
        QVERIFY(summary->text().contains(QStringLiteral("Tor active")));
    }

    void toggles_emit_only_safe_modes_and_wait_for_authoritative_state() {
        OverviewConnectivityCard card;
        card.setNetworkInfo(QJsonObject{{QStringLiteral("onion_service"), QJsonObject{
            {QStringLiteral("requested"), false}, {QStringLiteral("active"), false}}}});
        card.setRelayServiceStatus(QJsonObject{
            {QStringLiteral("mode"), QStringLiteral("off")},
            {QStringLiteral("enabled"), false}});
        auto* tor = card.findChild<QCheckBox*>(QStringLiteral("overviewTorToggle"));
        auto* relay = card.findChild<QCheckBox*>(QStringLiteral("overviewRelayToggle"));
        QSignalSpy torSpy(&card, &OverviewConnectivityCard::torModeRequested);
        QSignalSpy relaySpy(&card, &OverviewConnectivityCard::relayModeRequested);

        tor->click();
        relay->click();
        QCOMPARE(torSpy.count(), 1);
        QCOMPARE(torSpy.takeFirst().at(0).toString(), QStringLiteral("automatic"));
        QCOMPARE(relaySpy.count(), 1);
        QCOMPARE(relaySpy.takeFirst().at(0).toString(), QStringLiteral("automatic"));
        QVERIFY(!tor->isEnabled());
        QVERIFY(!relay->isEnabled());
    }

    void failures_reenable_controls_without_exposing_details() {
        OverviewConnectivityCard card;
        card.setNetworkInfo(QJsonObject{{QStringLiteral("onion_service"), QJsonObject{}}});
        card.setRelayServiceStatus(QJsonObject{
            {QStringLiteral("mode"), QStringLiteral("automatic")},
            {QStringLiteral("enabled"), true}});
        card.setTorActionError(false);
        card.setRelayActionError(false);

        auto* tor = card.findChild<QCheckBox*>(QStringLiteral("overviewTorToggle"));
        auto* relay = card.findChild<QCheckBox*>(QStringLiteral("overviewRelayToggle"));
        auto* torStatus = card.findChild<QLabel*>(QStringLiteral("overviewTorStatus"));
        auto* relayStatus = card.findChild<QLabel*>(QStringLiteral("overviewRelayStatus"));
        QVERIFY(tor->isEnabled());
        QVERIFY(relay->isEnabled());
        QVERIFY(torStatus->text().contains(QStringLiteral("Ordinary P2P continues")));
        QVERIFY(relayStatus->text().contains(QStringLiteral("Existing network paths")));
    }

    void exposes_only_the_active_public_onion_address() {
        OverviewConnectivityCard card;
        const QString address = QString(56, QLatin1Char('a')) + QStringLiteral(".onion");
        auto* row = card.findChild<QWidget*>(QStringLiteral("overviewOnionAddressRow"));
        auto* label = card.findChild<QLabel*>(QStringLiteral("overviewOnionAddress"));
        auto* copy = card.findChild<QPushButton*>(QStringLiteral("overviewCopyOnionAddress"));
        QVERIFY(row);
        QVERIFY(label);
        QVERIFY(copy);
        QVERIFY(row->isHidden());

        card.setOnionServiceStatus(QJsonObject{
            {QStringLiteral("requested"), true},
            {QStringLiteral("active"), true},
            {QStringLiteral("mode"), QStringLiteral("automatic")},
            {QStringLiteral("address"), address},
            {QStringLiteral("authentication"), QStringLiteral("must-never-render")},
            {QStringLiteral("control_port"), 12345}});

        QVERIFY(!row->isHidden());
        QVERIFY(label->text().contains(QStringLiteral("Public Dinero P2P address")));
        QVERIFY(label->text().contains(QChar(0x2026)));
        QVERIFY(!label->text().contains(address));
        QCOMPARE(label->toolTip(), address);
        QVERIFY(!label->text().contains(QStringLiteral("must-never-render")));
        QVERIFY(!label->text().contains(QStringLiteral("12345")));
        copy->click();
        QCOMPARE(QApplication::clipboard()->text(), address);

        card.setOnionServiceStatus(QJsonObject{
            {QStringLiteral("requested"), true},
            {QStringLiteral("active"), false},
            {QStringLiteral("address"), address}});
        QVERIFY(row->isHidden());
    }

    void older_daemon_disables_only_the_unsupported_controls() {
        OverviewConnectivityCard card;
        card.setNetworkInfo(QJsonObject{});
        card.setRelayActionError(true);
        QVERIFY(!card.findChild<QCheckBox*>(QStringLiteral("overviewTorToggle"))->isEnabled());
        QVERIFY(!card.findChild<QCheckBox*>(QStringLiteral("overviewRelayToggle"))->isEnabled());
        QVERIFY(!card.findChild<QWidget*>(QStringLiteral("overviewAdvancedNetworkControls")));
    }
};

QTEST_MAIN(TestOverviewConnectivityCard)
#include "test_overview_connectivity_card.moc"
