#pragma once
#include "libslic3r/CommonDefs.hpp"
#include "libslic3r/VortekMultiNozzle.hpp"
#include "slic3r/Utils/json_diff.hpp"

#include "DevDefs.h"
#include "DevNozzleSystem.h"
#include "DevFirmware.h"

#include <map>
#include <set>

// wx
#include <wx/string.h>
#include <wx/event.h>

wxDECLARE_EVENT(DEV_RACK_EVENT_READING_FINISHED, wxCommandEvent);

namespace Slic3r
{
class DevNozzleSystem;

class VortekNozzleRack: public wxEvtHandler
{
public:
    enum RackStatus : int
    {
        RACK_STATUS_UNKNOWN = -1,
        RACK_STATUS_IDLE = 0,
        RACK_STATUS_HOTEND_CENTRE = 1,
        RACK_STATUS_TOOLHEAD_CENTRE = 2,
        RACK_STATUS_CALIBRATE_HOTEND_RACK = 3,
        RACK_STATUS_CUT_MATERIAL = 4,
        RACK_STATUS_UNLOCK_HOTEND = 5,
        RACK_STATUS_LIFT_HOTEND_RACK = 6,
        RACK_STATUS_PLACE_HOTEND = 7,
        RACK_STATUS_PICK_HOTEND = 8,
        RACK_STATUS_LOCK_HOTEND = 9,
        RACK_STATUS_PRINTING = 10,
        RACK_STATUS_END,
    };

    enum RackPos : int
    {
        RACK_POS_UNKNOWN = 0,
        RACK_POS_A_TOP = 1,
        RACK_POS_B_TOP = 2,
        RACK_POS_CENTRE = 3,
        RACK_POS_END,
    };

    enum RackCaliStatus
    {
        Rack_CALI_UNKNOWN = -1,
        Rack_CALI_NOT = 0,
        Rack_CALI_OK = 1,
    };

public:
    VortekNozzleRack(DevNozzleSystem* nozzle_system, MachineObject* owner);
    ~VortekNozzleRack() = default;

public:
    // Is supported by the printer
    bool IsSupported() const { return m_is_supported; };
    void SetSupported(bool supported) { m_is_supported = supported; }

    // getters
    DevNozzleSystem* GetNozzleSystem() const { return m_nozzle_system; }
    MachineObject* GetOwner() const { return m_owner; }

    RackPos     GetPosition() const { return m_position; }
    RackStatus  GetStatus() const { return m_status; }
    RackCaliStatus GetCaliStatus() const { return m_cali_status;}

    DevNozzle GetNozzle(int idx) const;
    const std::map<int, DevNozzle>& GetRackNozzles() const { return m_rack_nozzles; }

    // status
    bool HasUnreliableNozzles() const;
    bool HasUnknownNozzles() const;
    int GetKnownNozzleCount() const;

    // refreshing
    int GetReadingIdx() const { return m_reading_idx; }
    int GetReadingCount() const { return m_reading_count; }
    void SetReadingInfo(int idx, int count) {
        m_reading_idx = idx;
        m_reading_count = count;
    }
    void SendReadingFinished();

    // firmware
    void AddNozzleFirmwareInfo(int nozzle_id, const DevFirmwareVersionInfo& info) { m_rack_nozzles_firmware[nozzle_id] = info; }
    void ClearNozzleFirmwareInfo() { m_rack_nozzles_firmware.clear(); m_extruder_nozzle_firmware = DevFirmwareVersionInfo(); }
    DevFirmwareVersionInfo GetNozzleFirmwareInfo(int nozzle_id) const;
    void SetExtruderNozzleFirmwareInfo(const DevFirmwareVersionInfo& info) { m_extruder_nozzle_firmware = info; }
    DevFirmwareVersionInfo GetExtruderNozzleFirmwareInfo() const { return m_extruder_nozzle_firmware; }

    // setters
    void  Reset();
    void  AddRackNozzle(DevNozzle& nozzle) { SetNozzleOnRack(nozzle.m_nozzle_id, true); m_rack_nozzles[nozzle.m_nozzle_id] = nozzle; };
    void  ClearRackNozzles() { m_rack_nozzles.clear(); m_on_rack_nozzle_ids.clear(); }
    void  SetNozzleOnRack(int nozzle_id, bool on_rack) {
        if (on_rack) m_on_rack_nozzle_ids.insert(nozzle_id);
        else m_on_rack_nozzle_ids.erase(nozzle_id);
    }
    bool  IsNozzleOnRack(int nozzle_id) const { return m_on_rack_nozzle_ids.count(nozzle_id) > 0; }

public:
    void ParseRackInfo(const nlohmann::json& rack_info);

    void CtrlRackPosMove(RackPos new_pos) const;
    void CtrlRackPosGoHome() const;

    void CtrlRackConfirmNozzle(int rack_nozzle_id) const;
    void CtrlRackConfirmAll() const;

    void CrtlRackReadNozzle(int rack_nozzle_id) const;
    void CtrlRackReadAll(bool gui_check = false) const;
    bool CtrlCanReadAll() const;

    // the upgrade is not supported
    int CtrlRackUpgradeExtruderNozzle() const;
    int CtrlRackUpgradeRackNozzle(int rack_nozzle_id) const;;
    int CtrlRackUpgradeAll() const;;
    bool CtrlCanUpdateAll() const;

private:
    void ParseRackInfoV1_0(const nlohmann::json& rack_info);

    int CtrlRackUpgrade(const std::string& module_str) const;

    bool CheckRackMoveWarningDlg() const;

private:
    DevNozzleSystem* m_nozzle_system = nullptr;
    MachineObject*   m_owner = nullptr;

    bool m_is_supported = false; // Indicates if the nozzle rack is supported by the printer
    RackPos m_position = RACK_POS_UNKNOWN;
    RackStatus m_status = RACK_STATUS_UNKNOWN;
    RackCaliStatus m_cali_status = Rack_CALI_UNKNOWN;

    std::map<int, DevNozzle> m_rack_nozzles; // Map of nozzle ID to DevNozzle objects
    std::map<int, DevFirmwareVersionInfo> m_rack_nozzles_firmware;
    DevFirmwareVersionInfo m_extruder_nozzle_firmware;
    std::set<int> m_on_rack_nozzle_ids;
    int m_reading_idx = 0;
    int m_reading_count = 0;
};
}
