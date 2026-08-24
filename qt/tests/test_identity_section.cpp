// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "identitysection.h"
#include "dashboardtypes.h"

#include <QLabel>
#include <QWidget>
#include <QtTest/QtTest>

using dinero::qt::dashboard::IdentitySection;
using dinero::qt::dashboard::NodeIdentity;

class TestIdentitySection : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void technical_identity_details_are_hidden_by_default() {
        IdentitySection s;
        auto* details = s.findChild<QWidget*>(QStringLiteral("advancedIdentityDetails"));
        QVERIFY(details != nullptr);
        QVERIFY(details->isHidden());
        s.setAdvancedVisible(true);
        QVERIFY(!details->isHidden());
    }

    void formats_node_id_in_4char_groups() {
        IdentitySection s;
        NodeIdentity id;
        id.node_id_hex = "fd4fc04df38bacbf72d4ecae451d1589570bcaba";
        s.onIdentityUpdated(id);

        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().startsWith("fd4f")) {
                QCOMPARE(l->text(),
                    QString("fd4f c04d f38b acbf 72d4 ecae 451d 1589 570b caba"));
                found = true;
            }
        }
        QVERIFY(found);
    }

    void reachability_direct_includes_addr_and_port() {
        IdentitySection s;
        NodeIdentity id;
        id.reachability = NodeIdentity::DIRECT;
        id.local_addr   = "162.200.227.214";
        id.local_port   = 20999;
        s.onIdentityUpdated(id);

        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().contains("Connected directly")) {
                QVERIFY(l->text().contains("162.200.227.214:20999"));
                found = true;
            }
        }
        QVERIFY(found);
    }

    void relaying_off_when_not_active() {
        IdentitySection s;
        NodeIdentity id;
        id.is_relay_active = false;
        s.onIdentityUpdated(id);

        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().contains("RELAYING · OFF")) found = true;
        }
        QVERIFY(found);
    }

    void relaying_on_includes_count_and_grace() {
        IdentitySection s;
        NodeIdentity id;
        id.is_relay_active   = true;
        id.registrants_count = 2;
        id.grace_count       = 1;
        s.onIdentityUpdated(id);

        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().contains("RELAYING for 2 peers · 1 in grace"))
                found = true;
        }
        QVERIFY(found);
    }

    void mining_line_off_when_inactive() {
        IdentitySection s;
        NodeIdentity id;
        id.is_mining = false;
        s.onIdentityUpdated(id);

        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().contains("MINING · OFF")) found = true;
        }
        QVERIFY(found);
    }

    void daemon_degraded_marks_unreachable_red() {
        IdentitySection s;
        s.onDaemonStateChanged(false);
        const auto labels = s.findChildren<QLabel*>();
        bool found = false;
        for (auto* l : labels) {
            if (l->text().contains("Offline") &&
                l->styleSheet().contains("color: #c33")) {
                found = true;
            }
        }
        QVERIFY(found);
    }

    void relay_status_is_plain_language_and_reassuring() {
        NodeIdentity id;
        id.reachability = NodeIdentity::BEHIND_RELAY;
        const QString text = IdentitySection::reachabilityLine(id);
        QVERIFY(text.contains(QStringLiteral("Connected securely through a Dinero relay")));
        QVERIFY(text.contains(QStringLiteral("recovery is automatic")));
        QVERIFY(!text.contains(QStringLiteral("BEHIND-RELAY")));
        QVERIFY(!text.contains(QStringLiteral("NAT'd")));
    }
};

QTEST_MAIN(TestIdentitySection)
#include "test_identity_section.moc"
