#pragma once

#include "config.h"

#include "handler.hpp"
#include "libpldmresponder/pdr.hpp"
#include "libpldmresponder/pdr_utils.hpp"
#include "utils.hpp"

#include <stdint.h>

#include <map>

#include "libpldm/platform.h"
#include "libpldm/states.h"

namespace pldm
{
namespace responder
{
namespace platform
{

using DbusPath = std::string;
using EffecterObjs = std::vector<DbusPath>;

class Handler : public CmdHandler
{
  public:
    Handler(const std::string& dir, pldm_pdr* repo) : pdrRepo(repo)
    {
        generate(dir, pdrRepo);

        handlers.emplace(PLDM_GET_PDR,
                         [this](const pldm_msg* request, size_t payloadLength) {
                             return this->getPDR(request, payloadLength);
                         });
        handlers.emplace(PLDM_SET_STATE_EFFECTER_STATES,
                         [this](const pldm_msg* request, size_t payloadLength) {
                             return this->setStateEffecterStates(request,
                                                                 payloadLength);
                         });
    }

    const EffecterObjs& getEffecterObjs(uint16_t effecterId) const
    {
        return effecterObjs.at(effecterId);
    }

    void addEffecterObjs(uint16_t effecterId, EffecterObjs&& paths)
    {
        effecterObjs.emplace(effecterId, std::move(paths));
    }

    uint16_t getNextEffecterId()
    {
        return ++nextEffecterId;
    }

    /** @brief Parse PDR JSONs and build PDR repository
     *
     *  @param[in] dir - directory housing platform specific PDR JSON files
     *  @param[in] repo - instance of concrete implementation of Repo
     */
    void generate(const std::string& dir, Repo& repo);

    /** @brief Parse PDR JSONs and build state effecter PDR repository
     *
     *  @param[in] json - platform specific PDR JSON files
     *  @param[in] repo - instance of state effecter implementation of Repo
     */
    void generateStateEffecterRepo(const Json& json, Repo& repo);

    /** @brief Handler for GetPDR
     *
     *  @param[in] request - Request message payload
     *  @param[in] payloadLength - Request payload length
     *  @param[out] Response - Response message written here
     */
    Response getPDR(const pldm_msg* request, size_t payloadLength);

    /** @brief Handler for setStateEffecterStates
     *
     *  @param[in] request - Request message
     *  @param[in] payloadLength - Request payload length
     *  @return Response - PLDM Response message
     */
    Response setStateEffecterStates(const pldm_msg* request,
                                    size_t payloadLength);

    /** @brief Function to set the effecter requested by pldm requester
     *  @param[in] dBusIntf - The interface object
     *  @param[in] effecterId - Effecter ID sent by the requester to act on
     *  @param[in] stateField - The state field data for each of the states,
     * equal to composite effecter count in number
     *  @return - Success or failure in setting the states. Returns failure in
     * terms of PLDM completion codes if atleast one state fails to be set
     */
    template <class DBusInterface>
    int setStateEffecterStatesHandler(
        const DBusInterface& dBusIntf, uint16_t effecterId,
        const std::vector<set_effecter_state_field>& stateField)
    {
        using namespace pldm::responder::pdr;
        using namespace std::string_literals;
        using DBusProperty = std::variant<std::string, bool>;
        using StateSetId = uint16_t;
        using StateSetNum = uint8_t;
        using PropertyMap =
            std::map<StateSetId, std::map<StateSetNum, DBusProperty>>;
        static const PropertyMap stateNumToDbusProp = {
            {PLDM_BOOT_PROGRESS_STATE,
             {{PLDM_BOOT_NOT_ACTIVE,
               "xyz.openbmc_project.State.OperatingSystem.Status.OSStatus."
               "Standby"s},
              {PLDM_BOOT_COMPLETED,
               "xyz.openbmc_project.State.OperatingSystem.Status.OSStatus."
               "BootComplete"s}}},
            {PLDM_SYSTEM_POWER_STATE,
             {{PLDM_OFF_SOFT_GRACEFUL,
               "xyz.openbmc_project.State.Chassis.Transition.Off"s}}}};
        using namespace pldm::responder::pdr;

        state_effecter_possible_states* states = nullptr;
        pldm_state_effecter_pdr* pdr = nullptr;
        uint8_t compEffecterCnt = stateField.size();
        PdrEntry pdrEntry{};
        auto pdrRecord = pdrRepo.getFirstRecord(pdrEntry);
        while (pdrRecord)
        {
            pdr = reinterpret_cast<pldm_state_effecter_pdr*>(pdrEntry.data);
            if (pdr->effecter_id != effecterId)
            {
                pdr = nullptr;
                pdrRecord = pdrRepo.getNextRecord(pdrRecord, pdrEntry);
                continue;
            }

            states = reinterpret_cast<state_effecter_possible_states*>(
                pdr->possible_states);
            if (compEffecterCnt > pdr->composite_effecter_count)
            {
                std::cerr << "The requester sent wrong composite effecter"
                          << " count for the effecter, EFFECTER_ID="
                          << effecterId << "COMP_EFF_CNT=" << compEffecterCnt
                          << "\n";
                return PLDM_ERROR_INVALID_DATA;
            }
            break;
        }

        if (!pdr)
        {
            return PLDM_PLATFORM_INVALID_EFFECTER_ID;
        }

        std::map<StateSetId, std::function<int(const std::string& objPath,
                                               const uint8_t currState)>>
            effecterToDbusEntries = {
                {PLDM_BOOT_PROGRESS_STATE,
                 [&](const std::string& objPath, const uint8_t currState) {
                     auto stateSet =
                         stateNumToDbusProp.find(PLDM_BOOT_PROGRESS_STATE);
                     if (stateSet == stateNumToDbusProp.end())
                     {
                         std::cerr << "Couldn't find D-Bus mapping for "
                                   << "PLDM_BOOT_PROGRESS_STATE, EFFECTER_ID="
                                   << effecterId << "\n";
                         return PLDM_ERROR;
                     }
                     auto iter = stateSet->second.find(
                         stateField[currState].effecter_state);
                     if (iter == stateSet->second.end())
                     {
                         std::cerr << "Invalid state field passed or field not "
                                   << "found for PLDM_BOOT_PROGRESS_STATE, "
                                      "EFFECTER_ID="
                                   << effecterId << " FIELD="
                                   << stateField[currState].effecter_state
                                   << " OBJECT_PATH=" << objPath.c_str()
                                   << "\n";
                         return PLDM_ERROR_INVALID_DATA;
                     }
                     auto dbusProp = "OperatingSystemState";
                     std::variant<std::string> value{
                         std::get<std::string>(iter->second)};
                     auto dbusInterface =
                         "xyz.openbmc_project.State.OperatingSystem.Status";
                     try
                     {
                         dBusIntf.setDbusProperty(objPath.c_str(), dbusProp,
                                                  dbusInterface, value);
                     }
                     catch (const std::exception& e)
                     {
                         std::cerr
                             << "Error setting property, ERROR=" << e.what()
                             << " PROPERTY=" << dbusProp
                             << " INTERFACE=" << dbusInterface
                             << " PATH=" << objPath.c_str() << "\n";
                         return PLDM_ERROR;
                     }
                     return PLDM_SUCCESS;
                 }},
                {PLDM_SYSTEM_POWER_STATE,
                 [&](const std::string& objPath, const uint8_t currState) {
                     auto stateSet =
                         stateNumToDbusProp.find(PLDM_SYSTEM_POWER_STATE);
                     if (stateSet == stateNumToDbusProp.end())
                     {
                         std::cerr << "Couldn't find D-Bus mapping for "
                                   << "PLDM_SYSTEM_POWER_STATE, EFFECTER_ID="
                                   << effecterId << "\n";
                         return PLDM_ERROR;
                     }
                     auto iter = stateSet->second.find(
                         stateField[currState].effecter_state);
                     if (iter == stateSet->second.end())
                     {
                         std::cerr << "Invalid state field passed or field not "
                                   << "found for PLDM_SYSTEM_POWER_STATE, "
                                      "EFFECTER_ID="
                                   << effecterId << " FIELD="
                                   << stateField[currState].effecter_state
                                   << " OBJECT_PATH=" << objPath.c_str()
                                   << "\n";
                         return PLDM_ERROR_INVALID_DATA;
                     }
                     auto dbusProp = "RequestedPowerTransition";
                     std::variant<std::string> value{
                         std::get<std::string>(iter->second)};
                     auto dbusInterface = "xyz.openbmc_project.State.Chassis";
                     try
                     {
                         dBusIntf.setDbusProperty(objPath.c_str(), dbusProp,
                                                  dbusInterface, value);
                     }
                     catch (const std::exception& e)
                     {
                         std::cerr
                             << "Error setting property, ERROR=" << e.what()
                             << " PROPERTY=" << dbusProp
                             << " INTERFACE=" << dbusInterface
                             << " PATH=" << objPath.c_str() << "\n";
                         return PLDM_ERROR;
                     }
                     return PLDM_SUCCESS;
                 }}};

        int rc = PLDM_SUCCESS;
        const auto& paths = getEffecterObjs(effecterId);
        for (uint8_t currState = 0; currState < compEffecterCnt; ++currState)
        {
            std::vector<StateSetNum> allowed{};
            // computation is based on table 79 from DSP0248 v1.1.1
            uint8_t bitfieldIndex = stateField[currState].effecter_state / 8;
            uint8_t bit =
                stateField[currState].effecter_state - (8 * bitfieldIndex);
            if (states->possible_states_size < bitfieldIndex ||
                !(states->states[bitfieldIndex].byte & (1 << bit)))
            {
                std::cerr << "Invalid state set value, EFFECTER_ID="
                          << effecterId
                          << " VALUE=" << stateField[currState].effecter_state
                          << " COMPOSITE_EFFECTER_ID=" << currState
                          << " DBUS_PATH=" << paths[currState].c_str() << "\n";
                rc = PLDM_PLATFORM_SET_EFFECTER_UNSUPPORTED_SENSORSTATE;
                break;
            }
            auto iter = effecterToDbusEntries.find(states->state_set_id);
            if (iter == effecterToDbusEntries.end())
            {
                uint16_t setId = states->state_set_id;
                std::cerr << "Did not find the state set for the"
                          << " state effecter pdr, STATE=" << setId
                          << " EFFECTER_ID=" << effecterId << "\n";
                rc = PLDM_PLATFORM_INVALID_STATE_VALUE;
                break;
            }
            if (stateField[currState].set_request == PLDM_REQUEST_SET)
            {
                rc = iter->second(paths[currState], currState);
                if (rc != PLDM_SUCCESS)
                {
                    break;
                }
            }
            uint8_t* nextState =
                reinterpret_cast<uint8_t*>(states) +
                sizeof(state_effecter_possible_states) -
                sizeof(states->states) +
                (states->possible_states_size * sizeof(states->states));
            states =
                reinterpret_cast<state_effecter_possible_states*>(nextState);
        }
        return rc;
    }

  private:
    pdr_utils::Repo pdrRepo;
    uint16_t nextEffecterId{};
    std::map<uint16_t, EffecterObjs> effecterObjs{};
};

} // namespace platform
} // namespace responder
} // namespace pldm
