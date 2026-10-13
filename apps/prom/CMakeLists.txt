set (prom_SRCS
prom.cpp
)

SET(sems_module_name prom)
SET(sems_module_libs ${sems_module_libs} event)
INCLUDE(${CMAKE_SOURCE_DIR}/cmake/module.rules.txt)