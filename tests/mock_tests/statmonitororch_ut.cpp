#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#define private public
#include "portsorch.h"
#include "statmonitororch.h"
#undef private
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "port.h"
#include "sai_serialize.h"

#include <cstring>
#include <new>

namespace statmonitororch_test
{
    using namespace std;
    using namespace swss;

    static const int MOCK_PORT_COUNT = 4;

    /*
     * Lightweight stand-in for PortsOrch that avoids SAI, SwitchOrch, and the
     * full port-initialisation path.  Only the m_portList map (returned by
     * getAllPorts()) is constructed; every other field stays zero-initialised.
     */
    static PortsOrch *createMockPortsOrch()
    {
        void *mem = ::operator new(sizeof(PortsOrch));
        memset(mem, 0, sizeof(PortsOrch));
        auto *mock = reinterpret_cast<PortsOrch *>(mem);
        new (&mock->m_portList) map<string, Port>();
        return mock;
    }

    static void destroyMockPortsOrch(PortsOrch *mock)
    {
        if (mock)
        {
            mock->m_portList.~map();
            ::operator delete(mock);
        }
    }

    class StatMonitorOrchTest : public ::testing::Test
    {
    public:
        StatMonitorOrchTest()
        {
            initDb();
        }

        ~StatMonitorOrchTest() override = default;

        void SetUp() override
        {
            testing_db::reset();
            initMockPortsOrch();
            initOrch();
        }

        void TearDown() override
        {
            deinitOrch();
            deinitMockPortsOrch();
        }

    protected:
        shared_ptr<DBConnector> m_configDb;
        shared_ptr<DBConnector> m_stateDb;
        shared_ptr<DBConnector> m_countersDb;

        StatMonitorOrch *m_statMonOrch = nullptr;

        void doConfigTask(const deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new ConsumerStateTable(m_configDb.get(), CFG_PORT_TX_ERR_MONITOR_TABLE_NAME, 1, 1),
                m_statMonOrch, CFG_PORT_TX_ERR_MONITOR_TABLE_NAME
            ));

            consumer->addToSync(entries);
            static_cast<Orch*>(m_statMonOrch)->doTask(*consumer);
        }

        void populateCounters(sai_object_id_t portId, uint64_t errorCount)
        {
            string oidStr = sai_serialize_object_id(portId);
            Table countersTable(m_countersDb.get(), COUNTERS_TABLE);
            vector<FieldValueTuple> fvs;
            fvs.emplace_back("SAI_PORT_STAT_IF_OUT_ERRORS", to_string(errorCount));
            countersTable.set(oidStr, fvs);
        }

        void verifyStateDb(const string &portName, const string &expectedStatus,
                           const string &expectedErrorCount = "")
        {
            Table stateTable(m_stateDb.get(), STATE_PORT_TX_ERR_MONITOR_TABLE_NAME);
            vector<FieldValueTuple> fvs;
            bool exists = stateTable.get(portName, fvs);
            if (expectedStatus.empty())
            {
                EXPECT_FALSE(exists) << "Expected no STATE_DB entry for " << portName;
                return;
            }
            ASSERT_TRUE(exists) << "Expected STATE_DB entry for " << portName;

            string status, errorCount;
            for (const auto &fv : fvs)
            {
                if (fvField(fv) == "status")
                    status = fvValue(fv);
                else if (fvField(fv) == "error_count")
                    errorCount = fvValue(fv);
            }
            EXPECT_EQ(status, expectedStatus) << "Unexpected status for " << portName;
            if (!expectedErrorCount.empty())
            {
                EXPECT_EQ(errorCount, expectedErrorCount)
                    << "Unexpected error_count for " << portName;
            }
        }

        vector<pair<string, sai_object_id_t>> getPhyPorts()
        {
            vector<pair<string, sai_object_id_t>> result;
            auto &portList = gPortsOrch->getAllPorts();
            for (const auto &p : portList)
            {
                if (p.second.m_type == Port::PHY)
                {
                    result.push_back({p.second.m_alias, p.second.m_port_id});
                }
            }
            return result;
        }

    private:
        void initMockPortsOrch()
        {
            gPortsOrch = createMockPortsOrch();

            for (int i = 0; i < MOCK_PORT_COUNT; i++)
            {
                string alias = "Ethernet" + to_string(i * 4);
                Port port(alias, Port::PHY);
                port.m_port_id = static_cast<sai_object_id_t>(0x1000000000001 + i);
                gPortsOrch->m_portList[alias] = port;
            }
        }

        void deinitMockPortsOrch()
        {
            destroyMockPortsOrch(gPortsOrch);
            gPortsOrch = nullptr;
        }

        void initOrch()
        {
            m_statMonOrch = new StatMonitorOrch(m_configDb.get(), m_stateDb.get());
        }

        void deinitOrch()
        {
            delete m_statMonOrch;
            m_statMonOrch = nullptr;
        }

        void initDb()
        {
            m_configDb = make_shared<DBConnector>("CONFIG_DB", 0);
            m_stateDb = make_shared<DBConnector>("STATE_DB", 0);
            m_countersDb = make_shared<DBConnector>("COUNTERS_DB", 0);
        }
    };

    /* TC1: Config SET with valid parameters and enabled=true */
    TEST_F(StatMonitorOrchTest, ConfigSetEnabled)
    {
        auto ports = getPhyPorts();
        ASSERT_GT(ports.size(), 0u);

        for (const auto &p : ports)
        {
            populateCounters(p.second, 100);
        }

        auto entries = deque<KeyOpFieldsValuesTuple>(
        {
            {
                "CONFIG",
                SET_COMMAND,
                {
                    {"threshold", "1000"},
                    {"window_sec", "10"},
                    {"enabled", "enabled"}
                }
            }
        });
        doConfigTask(entries);

        EXPECT_TRUE(m_statMonOrch->m_enabled);
        EXPECT_EQ(m_statMonOrch->m_threshold, 1000u);
        EXPECT_EQ(m_statMonOrch->m_windowSec, 10u);
        EXPECT_EQ(m_statMonOrch->m_portEntries.size(), ports.size());

        for (const auto &p : ports)
        {
            verifyStateDb(p.first, "ok");
        }
    }

    /* TC2: Config SET with enabled=false stops monitoring */
    TEST_F(StatMonitorOrchTest, ConfigSetDisabled)
    {
        auto ports = getPhyPorts();
        for (const auto &p : ports)
        {
            populateCounters(p.second, 0);
        }

        /* Enable first */
        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));
        EXPECT_TRUE(m_statMonOrch->m_enabled);
        EXPECT_GT(m_statMonOrch->m_portEntries.size(), 0u);

        /* Disable */
        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "disabled"}}}
        }));
        EXPECT_FALSE(m_statMonOrch->m_enabled);
        EXPECT_EQ(m_statMonOrch->m_portEntries.size(), 0u);

        for (const auto &p : ports)
        {
            verifyStateDb(p.first, "");
        }
    }

    /* TC3: Config DEL stops monitoring */
    TEST_F(StatMonitorOrchTest, ConfigDel)
    {
        auto ports = getPhyPorts();
        for (const auto &p : ports)
        {
            populateCounters(p.second, 0);
        }

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));
        EXPECT_TRUE(m_statMonOrch->m_enabled);

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", DEL_COMMAND, {}}
        }));
        EXPECT_FALSE(m_statMonOrch->m_enabled);
        EXPECT_EQ(m_statMonOrch->m_portEntries.size(), 0u);
    }

    /* TC4: Invalid config (threshold=0) is rejected */
    TEST_F(StatMonitorOrchTest, InvalidConfig)
    {
        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "0"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));
        EXPECT_FALSE(m_statMonOrch->m_enabled);
        EXPECT_EQ(m_statMonOrch->m_portEntries.size(), 0u);
    }

    /* TC5: Baseline initialized with current counter value */
    TEST_F(StatMonitorOrchTest, BaselineInit)
    {
        auto ports = getPhyPorts();
        ASSERT_GT(ports.size(), 0u);

        const uint64_t initialValue = 500;
        for (const auto &p : ports)
        {
            populateCounters(p.second, initialValue);
        }

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));

        for (const auto &entry : m_statMonOrch->m_portEntries)
        {
            EXPECT_EQ(entry.second.baseline, initialValue);
            EXPECT_EQ(entry.second.status, PortMonitorStatus::OK);
        }

        for (const auto &p : ports)
        {
            verifyStateDb(p.first, "ok");
        }
    }

    /* TC6: Timer tick — delta below threshold, stays OK */
    TEST_F(StatMonitorOrchTest, TimerTickBelowThreshold)
    {
        auto ports = getPhyPorts();
        ASSERT_GT(ports.size(), 0u);

        for (const auto &p : ports)
        {
            populateCounters(p.second, 100);
        }

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));

        /* Advance counters by less than threshold */
        for (const auto &p : ports)
        {
            populateCounters(p.second, 105);
        }

        /* Simulate timer tick */
        SelectableTimer timer(timespec{5, 0});
        m_statMonOrch->doTask(timer);

        for (const auto &p : ports)
        {
            verifyStateDb(p.first, "ok");
        }
    }

    /* TC7: Timer tick — delta exceeds threshold, transitions to ERROR */
    TEST_F(StatMonitorOrchTest, TimerTickAboveThreshold)
    {
        auto ports = getPhyPorts();
        ASSERT_GT(ports.size(), 0u);

        for (const auto &p : ports)
        {
            populateCounters(p.second, 100);
        }

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));

        /* Advance counters by more than threshold */
        for (const auto &p : ports)
        {
            populateCounters(p.second, 200);
        }

        SelectableTimer timer(timespec{5, 0});
        m_statMonOrch->doTask(timer);

        for (const auto &p : ports)
        {
            verifyStateDb(p.first, "error");
        }

        for (const auto &entry : m_statMonOrch->m_portEntries)
        {
            EXPECT_EQ(entry.second.status, PortMonitorStatus::ERROR);
        }
    }

    /* TC8: ERROR -> OK revert when delta drops below threshold */
    TEST_F(StatMonitorOrchTest, ErrorToOkRevert)
    {
        auto ports = getPhyPorts();
        ASSERT_GT(ports.size(), 0u);

        for (const auto &p : ports)
        {
            populateCounters(p.second, 100);
        }

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));

        /* Trigger error */
        for (const auto &p : ports)
        {
            populateCounters(p.second, 200);
        }

        SelectableTimer timer(timespec{5, 0});
        m_statMonOrch->doTask(timer);

        for (const auto &p : ports)
        {
            verifyStateDb(p.first, "error");
        }

        /* Next tick with low delta — revert to OK */
        for (const auto &p : ports)
        {
            populateCounters(p.second, 201);
        }

        m_statMonOrch->doTask(timer);

        for (const auto &p : ports)
        {
            verifyStateDb(p.first, "ok");
        }

        for (const auto &entry : m_statMonOrch->m_portEntries)
        {
            EXPECT_EQ(entry.second.status, PortMonitorStatus::OK);
        }
    }

    /* TC9: Baseline updated after every tick */
    TEST_F(StatMonitorOrchTest, BaselineUpdateAfterTick)
    {
        auto ports = getPhyPorts();
        ASSERT_GT(ports.size(), 0u);

        for (const auto &p : ports)
        {
            populateCounters(p.second, 100);
        }

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));

        for (const auto &p : ports)
        {
            populateCounters(p.second, 105);
        }

        SelectableTimer timer(timespec{5, 0});
        m_statMonOrch->doTask(timer);

        for (const auto &entry : m_statMonOrch->m_portEntries)
        {
            EXPECT_EQ(entry.second.baseline, 105u);
        }
    }

    /* TC10: Counter wrap-around (unsigned underflow) still produces correct delta */
    TEST_F(StatMonitorOrchTest, CounterWrapAround)
    {
        auto ports = getPhyPorts();
        ASSERT_GT(ports.size(), 0u);

        uint64_t nearMax = UINT64_MAX - 5;
        for (const auto &p : ports)
        {
            populateCounters(p.second, nearMax);
        }

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));

        /* Wrap around: counter goes from near-max to 20 => delta = 20 - (MAX-5) = 26 */
        for (const auto &p : ports)
        {
            populateCounters(p.second, 20);
        }

        SelectableTimer timer(timespec{5, 0});
        m_statMonOrch->doTask(timer);

        for (const auto &entry : m_statMonOrch->m_portEntries)
        {
            EXPECT_EQ(entry.second.status, PortMonitorStatus::ERROR);
        }

        uint64_t expectedDelta = 20 - nearMax; /* unsigned wrap = 26 */
        for (const auto &p : ports)
        {
            verifyStateDb(p.first, "error", to_string(expectedDelta));
        }
    }

    /* TC11: Re-enable reinitializes baselines */
    TEST_F(StatMonitorOrchTest, ReEnableReinitializesBaseline)
    {
        auto ports = getPhyPorts();
        ASSERT_GT(ports.size(), 0u);

        for (const auto &p : ports)
        {
            populateCounters(p.second, 100);
        }

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));

        for (const auto &entry : m_statMonOrch->m_portEntries)
        {
            EXPECT_EQ(entry.second.baseline, 100u);
        }

        /* Disable */
        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "disabled"}}}
        }));
        EXPECT_EQ(m_statMonOrch->m_portEntries.size(), 0u);

        /* Change counters while disabled */
        for (const auto &p : ports)
        {
            populateCounters(p.second, 999);
        }

        /* Re-enable */
        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));

        for (const auto &entry : m_statMonOrch->m_portEntries)
        {
            EXPECT_EQ(entry.second.baseline, 999u);
        }
    }

    /* TC12: All ports init with status OK in STATE_DB */
    TEST_F(StatMonitorOrchTest, AllPortsInitStatusOk)
    {
        auto ports = getPhyPorts();
        ASSERT_GT(ports.size(), 0u);

        for (const auto &p : ports)
        {
            populateCounters(p.second, 0);
        }

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));

        EXPECT_EQ(m_statMonOrch->m_portEntries.size(), ports.size());

        for (const auto &p : ports)
        {
            verifyStateDb(p.first, "ok");
        }
    }

    /* TC13: Threshold update takes effect on next evaluation */
    TEST_F(StatMonitorOrchTest, ThresholdUpdate)
    {
        auto ports = getPhyPorts();
        ASSERT_GT(ports.size(), 0u);

        for (const auto &p : ports)
        {
            populateCounters(p.second, 100);
        }

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "50"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));

        /* Delta of 20 — below threshold of 50 */
        for (const auto &p : ports)
        {
            populateCounters(p.second, 120);
        }

        SelectableTimer timer(timespec{5, 0});
        m_statMonOrch->doTask(timer);

        for (const auto &p : ports)
        {
            verifyStateDb(p.first, "ok");
        }

        /* Lower threshold to 10 and re-enable */
        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "10"}, {"window_sec", "5"}, {"enabled", "enabled"}}}
        }));

        /* Advance by 15 — now above new threshold of 10 */
        for (const auto &p : ports)
        {
            populateCounters(p.second, 135);
        }

        m_statMonOrch->doTask(timer);

        for (const auto &p : ports)
        {
            verifyStateDb(p.first, "error");
        }
    }

    /* TC14: Partial config — only threshold field, window_sec and enabled preserved */
    TEST_F(StatMonitorOrchTest, PartialConfigUpdateThresholdOnly)
    {
        auto ports = getPhyPorts();
        ASSERT_GT(ports.size(), 0u);

        for (const auto &p : ports)
        {
            populateCounters(p.second, 100);
        }

        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "50"}, {"window_sec", "10"}, {"enabled", "enabled"}}}
        }));

        EXPECT_TRUE(m_statMonOrch->m_enabled);
        EXPECT_EQ(m_statMonOrch->m_threshold, 50u);
        EXPECT_EQ(m_statMonOrch->m_windowSec, 10u);

        /* Send only threshold — window_sec and enabled should be preserved */
        doConfigTask(deque<KeyOpFieldsValuesTuple>(
        {
            {"CONFIG", SET_COMMAND, {{"threshold", "20"}}}
        }));

        EXPECT_TRUE(m_statMonOrch->m_enabled);
        EXPECT_EQ(m_statMonOrch->m_threshold, 20u);
        EXPECT_EQ(m_statMonOrch->m_windowSec, 10u);
        EXPECT_GT(m_statMonOrch->m_portEntries.size(), 0u);
    }
}
