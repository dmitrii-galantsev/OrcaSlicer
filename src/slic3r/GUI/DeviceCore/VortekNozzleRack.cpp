#include "VortekNozzleRack.h"
#include "DevUtil.h"
#include "VortekDeviceHooks.hpp"

#include "slic3r/GUI/DeviceManager.hpp"

wxDEFINE_EVENT(DEV_RACK_EVENT_READING_FINISHED, wxCommandEvent);
namespace Slic3r
{

VortekNozzleRack::VortekNozzleRack(DevNozzleSystem* nozzle_system, MachineObject* owner)
    : wxEvtHandler(), m_nozzle_system(nozzle_system), m_owner(owner)
{

}

void VortekNozzleRack::Reset()
{
    m_position = RACK_POS_UNKNOWN;
    m_status = RACK_STATUS_UNKNOWN;
    m_reading_idx = 0;
    m_reading_count = 0;
    m_rack_nozzles.clear();
    m_rack_nozzles_firmware.clear();
    m_extruder_nozzle_firmware = DevFirmwareVersionInfo();
}

void VortekNozzleRack::SendReadingFinished()
{
   wxCommandEvent evt(DEV_RACK_EVENT_READING_FINISHED);
   evt.SetEventObject(this);
   wxPostEvent(this, evt);
}

DevNozzle VortekNozzleRack::GetNozzle(int idx) const
{
    auto iter = m_rack_nozzles.find(idx);
    if (iter == m_rack_nozzles.end()) {
        DevNozzle nozzle;
        return nozzle;
    }

    return iter->second;
}

DevFirmwareVersionInfo VortekNozzleRack::GetNozzleFirmwareInfo(int nozzle_id) const
{
    auto iter = m_rack_nozzles_firmware.find(nozzle_id);
    return iter != m_rack_nozzles_firmware.end() ? iter->second : DevFirmwareVersionInfo();
}

bool VortekNozzleRack::HasUnreliableNozzles() const
{
    for (const auto& nozzle : m_rack_nozzles)
    {
        if (!Vortek::DeviceHooks::is_nozzle_info_reliable(nozzle.second))
        {
            return true;
        }
    }
    return false;
}

bool VortekNozzleRack::HasUnknownNozzles() const
{
    for (const auto& nozzle : m_rack_nozzles)
    {
        if (Vortek::DeviceHooks::is_nozzle_unknown(nozzle.second))
        {
            return true;
        }
    }
    return false;
}

int VortekNozzleRack::GetKnownNozzleCount() const
{
    int count = 0;
    for (const auto& nozzle : m_rack_nozzles)
    {
        if (!Vortek::DeviceHooks::is_nozzle_empty(nozzle.second) && !Vortek::DeviceHooks::is_nozzle_unknown(nozzle.second))
        {
            count++;
        }
    }

    return count;
}

void VortekNozzleRack::ParseRackInfo(const nlohmann::json& rack_info)
{
    ParseRackInfoV1_0(rack_info);
}

void VortekNozzleRack::ParseRackInfoV1_0(const nlohmann::json& rack_info)
{
    DevJsonValParser::ParseVal(rack_info, "stat", m_status, RACK_STATUS_UNKNOWN);
    if (m_status < RACK_STATUS_UNKNOWN || m_status >= RACK_STATUS_END)
    {
        m_status = RACK_STATUS_UNKNOWN; // Reset to default if out of range
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Invalid rack status: " << m_status << ", reset";
    }

    DevJsonValParser::ParseVal(rack_info, "pos", m_position, RACK_POS_UNKNOWN);
    if (m_position < RACK_POS_UNKNOWN || m_position >= RACK_POS_END)
    {
        m_position = RACK_POS_UNKNOWN; // Reset to default if out of range
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Invalid rack position: " << m_position << ", reset";
    }

    DevJsonValParser::ParseVal(rack_info, "info", m_cali_status);
}

};
