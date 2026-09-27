#pragma once

#include "PartyTypes.hpp"

#include <string>
#include <string_view>

namespace wxl_party
{
    std::string SerializePartyMessage(const PartyWireMessage& message);
    bool ParsePartyMessage(std::string_view json, PartyWireMessage& message, std::string& error);
}
