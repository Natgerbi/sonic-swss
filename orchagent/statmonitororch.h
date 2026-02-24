#ifndef STAT_MONITOR_ORCH_H
#define STAT_MONITOR_ORCH_H

#include "orch.h"
#include "table.h"
#include "dbconnector.h"

#include <map>
#include <string>
#include <cstdint>
#include <chrono>

enum class PortMonitorStatus : uint8_t
{
    OK    = 0,
    ERROR = 1
};

struct PortMonitorEntry
{
    uint64_t          baseline;
    PortMonitorStatus status;
    std::string       portName;
};

class StatMonitorOrch : public Orch
{
public:
    StatMonitorOrch(swss::DBConnector *configDb, swss::DBConnector *stateDb);
    ~StatMonitorOrch() override = default;

    void doTask(Consumer &consumer) override;
    void doTask(swss::SelectableTimer &timer) override;

private:
    void handleConfigSet(const std::vector<swss::FieldValueTuple> &values);
    void initMonitoring();
    void stopMonitoring();
    void refreshBaselines();
    void updatePortState(PortMonitorEntry &entry, PortMonitorStatus status, uint64_t errorCount);
    uint64_t readPortCounter(sai_object_id_t portId);
    void restartTimer(uint32_t intervalSec);

    uint64_t m_threshold = 0;
    uint32_t m_windowSec = 0;
    bool     m_enabled   = false;

    std::map<sai_object_id_t, PortMonitorEntry> m_portEntries;

    std::chrono::steady_clock::time_point m_windowStartTime;

    std::shared_ptr<swss::DBConnector> m_countersDb;
    std::unique_ptr<swss::Table>       m_countersTable;
    std::unique_ptr<swss::Table>       m_stateTable;

    swss::SelectableTimer             *m_timer = nullptr;
};

#endif /* STAT_MONITOR_ORCH_H */
