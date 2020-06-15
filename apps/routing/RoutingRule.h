#pragma once

#include <string>

namespace routing
{

struct RoutingRule
{
    std::string type;
    std::string match;
    std::string destination;
    std::string app;
};

} // namespace routing


