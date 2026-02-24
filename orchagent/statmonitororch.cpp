#include "statmonitororch.h"
#include "schema.h"
#include "sai_serialize.h"
#include "portsorch.h"
#include "logger.h"
#include "timer.h"

#include <inttypes.h>
#include <chrono>

#define SAI_PORT_STAT_IF_OUT_ERRORS_STR "SAI_PORT_STAT_IF_OUT_ERRORS"

using namespace std;
using namespace swss;

extern PortsOrch *gPortsOrch;

StatMonitorOrch::StatMonitorOrch(DBConnector *configDb, DBConnector *stateDb)
    : Orch(configDb, CFG_PORT_TX_ERR_MONITOR_TABLE_NAME),
      m_countersDb(make_shared<DBConnector>("COUNTERS_DB", 0)),
      m_countersTable(make_unique<Table>(m_countersDb.get(), COUNTERS_TABLE)),
      m_stateTable(make_unique<Table>(stateDb, STATE_PORT_TX_ERR_MONITOR_TABLE_NAME)),
      m_timer(new SelectableTimer(timespec{.tv_sec = 1, .tv_nsec = 0}))
{
    SWSS_LOG_ENTER();

    auto executor = new ExecutableTimer(m_timer, this, "PORT_TX_ERR_POLL");
    Orch::addExecutor(executor);
}

void StatMonitorOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            handleConfigSet(kfvFieldsValues(t));
        }
        else if (op == DEL_COMMAND)
        {
            m_enabled = false;
            stopMonitoring();
        }

        it = consumer.m_toSync.erase(it);
    }
}

void StatMonitorOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    if (!m_enabled)
    {
        return;
    }

    for (auto &kv : m_portEntries)
    {
        sai_object_id_t portId = kv.first;
        PortMonitorEntry &entry = kv.second;

        uint64_t currentValue = readPortCounter(portId);

        /* Unsigned subtraction handles wrap-around naturally */
        uint64_t delta = currentValue - entry.baseline;
        entry.baseline = currentValue;

        if (delta >= m_threshold)
        {
            // Error flow, in case the status was not already in error, log the error
            if (entry.status != PortMonitorStatus::ERROR)
            {
                SWSS_LOG_ERROR("StatMonitorOrch: %s TX error count %" PRIu64
                               " exceeded threshold %" PRIu64 " in %us window",
                               entry.portName.c_str(), delta, m_threshold, m_windowSec);
            }
            updatePortState(entry, PortMonitorStatus::ERROR, delta);
        }
        else if (entry.status != PortMonitorStatus::OK)
        {
            SWSS_LOG_NOTICE("StatMonitorOrch: %s TX error status reverted to OK",
                            entry.portName.c_str());
            updatePortState(entry, PortMonitorStatus::OK, 0);
        }
    }

    restartTimer(m_windowSec);
    m_windowStartTime = chrono::steady_clock::now();
}

void StatMonitorOrch::handleConfigSet(const vector<FieldValueTuple> &values)
{
    uint64_t newThreshold = m_threshold;
    uint32_t newWindowSec = m_windowSec;
    bool newEnabled = m_enabled;

    for (const auto &fv : values)
    {
        const auto &field = fvField(fv);
        const auto &value = fvValue(fv);

        if (field == "threshold")
        {
            try { newThreshold = stoull(value); }
            catch (const std::exception &e)
            {
                SWSS_LOG_ERROR("StatMonitorOrch: bad %s value '%s': %s",
                               field.c_str(), value.c_str(), e.what());
            }
        }
        else if (field == "window_sec")
        {
            try
            {
                unsigned long parsed = stoul(value);
                if (parsed > UINT32_MAX)
                    throw std::out_of_range("window_sec exceeds UINT32_MAX");
                newWindowSec = static_cast<uint32_t>(parsed);
            }
            catch (const std::exception &e)
            {
                SWSS_LOG_ERROR("StatMonitorOrch: bad %s value '%s': %s",
                               field.c_str(), value.c_str(), e.what());
            }
        }
        else if (field == "enabled")
        {
            if (value == "enabled" || value == "disabled")
            {
                newEnabled = (value == "enabled");
            }
            else
            {
                SWSS_LOG_ERROR("StatMonitorOrch: bad %s value '%s', ignoring",
                              field.c_str(), value.c_str());
            }
        }
    }

    if (newThreshold == 0 || newWindowSec < 1)
    {
        SWSS_LOG_ERROR("StatMonitorOrch: invalid config threshold=%" PRIu64
                       " window_sec=%u, skipping", newThreshold, newWindowSec);
        return;
    }

    if (!newEnabled)
    {
        m_enabled = false;
        stopMonitoring();
        return;
    }

    bool wasEnabled = m_enabled;
    uint32_t oldWindow = m_windowSec;

    m_threshold = newThreshold;
    m_windowSec = newWindowSec;
    m_enabled = true;

    if (!wasEnabled)
    {
        initMonitoring();
    }
    else if (newWindowSec == oldWindow)
    {
        SWSS_LOG_NOTICE("StatMonitorOrch: threshold updated to %" PRIu64
                        ", effective next tick", m_threshold);
    }
    else
    {
        auto now = chrono::steady_clock::now();
        uint64_t elapsedSec = chrono::duration_cast<chrono::seconds>(now - m_windowStartTime).count();

        if (elapsedSec >= newWindowSec)
        {
            refreshBaselines();
            restartTimer(newWindowSec);
            m_windowStartTime = chrono::steady_clock::now();
            SWSS_LOG_NOTICE("StatMonitorOrch: window shortened to %us "
                            "(elapsed %" PRIu64 "s), baselines refreshed",
                            newWindowSec, elapsedSec);
        }
        else
        {
            uint32_t remaining = newWindowSec - static_cast<uint32_t>(elapsedSec);
            restartTimer(remaining);
            SWSS_LOG_NOTICE("StatMonitorOrch: window extended to %us "
                            "(elapsed %" PRIu64 "s), next tick in %us",
                            newWindowSec, elapsedSec, remaining);
        }
    }
}

void StatMonitorOrch::initMonitoring()
{
    SWSS_LOG_ENTER();

    m_timer->stop();
    m_portEntries.clear();

    auto &portList = gPortsOrch->getAllPorts();
    for (const auto &pair : portList)
    {
        const Port &port = pair.second;

        if (port.m_type != Port::PHY)
        {
            continue;
        }

        sai_object_id_t portId = port.m_port_id;
        uint64_t currentValue = readPortCounter(portId);

        m_portEntries[portId] = PortMonitorEntry{.baseline = currentValue, .status = PortMonitorStatus::OK, .portName = port.m_alias};
        updatePortState(m_portEntries[portId], PortMonitorStatus::OK, 0);
    }

    SWSS_LOG_NOTICE("StatMonitorOrch: initialized monitoring for %zu ports "
                    "(threshold=%" PRIu64 " window=%us)",
                    m_portEntries.size(), m_threshold, m_windowSec);

    restartTimer(m_windowSec);
    m_windowStartTime = chrono::steady_clock::now();
}

void StatMonitorOrch::refreshBaselines()
{
    SWSS_LOG_ENTER();

    for (auto &kv : m_portEntries)
    {
        kv.second.baseline = readPortCounter(kv.first);
    }
}

void StatMonitorOrch::restartTimer(uint32_t intervalSec)
{
    auto interval = timespec{.tv_sec = static_cast<time_t>(intervalSec), .tv_nsec = 0};

    m_timer->stop();
    m_timer->setInterval(interval);
    m_timer->start();
}

void StatMonitorOrch::updatePortState(PortMonitorEntry &entry, PortMonitorStatus status,
                                      uint64_t errorCount)
{
    entry.status = status;

    vector<FieldValueTuple> fvs;
    fvs.emplace_back("status", status == PortMonitorStatus::OK ? "ok" : "error");
    fvs.emplace_back("error_count", to_string(errorCount));
    m_stateTable->set(entry.portName, fvs);
}

void StatMonitorOrch::stopMonitoring()
{
    SWSS_LOG_ENTER();

    if (m_portEntries.empty())
    {
        return;
    }

    m_timer->stop();

    for (const auto &entry : m_portEntries)
    {
        m_stateTable->del(entry.second.portName);
    }

    m_portEntries.clear();

    SWSS_LOG_NOTICE("StatMonitorOrch: monitoring stopped");
}

uint64_t StatMonitorOrch::readPortCounter(sai_object_id_t portId)
{
    string oidStr = sai_serialize_object_id(portId);
    string value;

    if (!m_countersTable->hget(oidStr, SAI_PORT_STAT_IF_OUT_ERRORS_STR, value))
    {
        return 0;
    }

    try
    {
        return stoull(value);
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_WARN("StatMonitorOrch: failed to parse counter for %s: %s",
                     oidStr.c_str(), e.what());
        return 0;
    }
}

