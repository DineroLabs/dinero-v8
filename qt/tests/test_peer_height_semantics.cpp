// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license.

#include "peerheightsemantics.h"

#include <QtTest/QtTest>

class TestPeerHeightSemantics : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void header_progress_never_inflates_blocks_seen() {
        QCOMPARE(dinero::qt::peerBlocksSeen(104198, 104330), 104330);
        QCOMPARE(dinero::qt::peerHeadersSeen(104198, 104789, 104789), 104789);
    }

    void handshake_height_remains_a_fallback() {
        QCOMPARE(dinero::qt::peerBlocksSeen(104198, -1), 104198);
        QCOMPARE(dinero::qt::peerHeadersSeen(104198, -1, -1), 104198);
    }

    void best_known_only_affects_headers_seen() {
        QCOMPARE(dinero::qt::peerBlocksSeen(104198, 104330), 104330);
        QCOMPARE(dinero::qt::peerHeadersSeen(104198, 104400, 104789), 104789);
    }
};

QTEST_APPLESS_MAIN(TestPeerHeightSemantics)
#include "test_peer_height_semantics.moc"
