#include <QTest>
#include <QThread>

#include "syncengine/HybridLogicalClock.h"

using namespace syncengine;

class TestHLC : public QObject {
    Q_OBJECT
private slots:
    void testMonotonic()
    {
        HybridLogicalClock clock;
        uint64_t prev = clock.now();
        for (int i = 0; i < 1000; ++i) {
            uint64_t cur = clock.now();
            QVERIFY(cur > prev);
            prev = cur;
        }
    }

    void testPackUnpack()
    {
        uint64_t phys = 1700000000000ULL; // ~2023 in ms
        uint16_t logical = 42;
        uint64_t packed = HybridLogicalClock::pack(phys, logical);
        QCOMPARE(HybridLogicalClock::physicalComponent(packed), phys);
        QCOMPARE(HybridLogicalClock::logicalComponent(packed), logical);
    }

    void testReceiveAdvancesClock()
    {
        HybridLogicalClock clockA;
        HybridLogicalClock clockB;

        uint64_t a1 = clockA.now();
        uint64_t a2 = clockA.now();
        uint64_t a3 = clockA.now();

        // B receives a3 -- its clock should advance past a3
        uint64_t b1 = clockB.receive(a3);
        QVERIFY(b1 > a3);
    }

    void testReceiveWithOlderTimestamp()
    {
        HybridLogicalClock clockA;
        HybridLogicalClock clockB;

        // Advance B well past A
        for (int i = 0; i < 100; ++i)
            clockB.now();

        uint64_t bCurrent = clockB.current();
        uint64_t aOld = clockA.now();

        // B receives an old timestamp from A -- should still advance
        uint64_t bNext = clockB.receive(aOld);
        QVERIFY(bNext > bCurrent);
    }

    void testLogicalCounterWrapsOnPhysicalAdvance()
    {
        HybridLogicalClock clock;
        // Generate many events quickly to bump logical counter
        for (int i = 0; i < 100; ++i)
            clock.now();

        // Wait a bit so physical clock advances
        QThread::msleep(2);

        uint64_t ts = clock.now();
        // After physical advance, logical should reset to 0
        QCOMPARE(HybridLogicalClock::logicalComponent(ts), uint16_t(0));
    }
};

QTEST_MAIN(TestHLC)

#include "tst_hlc.moc"
