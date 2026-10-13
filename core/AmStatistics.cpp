/*
 * Copyright (C) 2019-2025 Michael Furmur (yeti-switch/sems)
 * Copyright (C) 2019-2021 Alexey Avolokitin (yeti-switch/sems)
 * Copyright (C) 2026 Vadim Saranov
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
 *
 * For a license to use the SEMS software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
/** @file AmStatistics.cpp - metrics registry */

#include "AmStatistics.h"
#include "log.h"

#include <stdexcept>

StatCounterInterface::~StatCounterInterface() {}

AtomicCounter::AtomicCounter(const map<string, string>& labels)
  : StatLabelsContainer<AtomicCounter>(labels)
{
}

AtomicCounter& AtomicCounter::addLabel(const string& name, const string& value)
{
  addLabelInternal(name, value);
  return *this;
}

void AtomicCounter::iterate(iterate_func_type callback)
{
  callback(get(), getLabels());
}

FunctionCounter& FunctionCounter::addLabel(const string& name, const string& value)
{
  addLabelInternal(name, value);
  return *this;
}

void FunctionCounter::iterate(iterate_func_type callback)
{
  callback(func_(), getLabels());
}

void FunctionGroupCounter::iterate(iterate_func_type callback)
{
  func_(callback);
}

const char* StatCountersGroupsInterface::type2str(Type type)
{
  switch (type) {
  case Counter: return "counter";
  case Gauge:   return "gauge";
  }
  return "unknown";
}

StatCountersSingleGroup::~StatCountersSingleGroup()
{
  for (auto& counter : counters)
    delete counter;
}

/** under counters_lock: the first series fixes the label name set of the
 *  metric (one metric name carries one label structure in the exposition),
 *  later series have to match it */
void StatCountersSingleGroup::checkSeriesLabels(const map<string, string>& labels)
{
  set<string> names;
  for (auto& l : labels)
    names.insert(l.first);

  if (series_labels_set && names != label_names) {
    string want, have;
    for (auto& n : names) want += (want.empty() ? "" : ",") + n;
    for (auto& n : label_names) have += (have.empty() ? "" : ",") + n;
    ERROR("counters group carries series with labels [%s], cannot add a "
          "series with labels [%s]", have.c_str(), want.c_str());
    throw std::logic_error("inconsistent label structure of a counters group");
  }

  label_names = names;
  series_labels_set = true;
}

AtomicCounter& StatCountersSingleGroup::addAtomicCounter(const map<string, string>& labels)
{
  lock_guard<AmMutex> l(counters_lock);

  if (owner_managed_series) {
    ERROR("counters group series belong to a FunctionGroupCounter owner, "
          "a counter of the group itself cannot be added");
    throw std::logic_error("counters group series are owner managed");
  }

  checkSeriesLabels(labels);

  /* the labels identify the series: a repeated registration returns the
   * counter registered before, a duplicate of the series is never added */
  for (auto& counter : counters) {
    if (counter->getSeriesLabels() != labels)
      continue;
    AtomicCounter* atomic = dynamic_cast<AtomicCounter*>(counter);
    if (!atomic) {
      ERROR("series of the counters group is already registered as a "
            "function counter");
      throw std::logic_error("series already registered as another kind");
    }
    return *atomic;
  }

  auto counter = new AtomicCounter(labels);
  counters.emplace_back(counter);
  return *counter;
}

FunctionCounter& StatCountersSingleGroup::addFunctionCounter(FunctionCounter::CallbackFunction func,
                                                             const map<string, string>& labels)
{
  lock_guard<AmMutex> l(counters_lock);

  if (owner_managed_series) {
    ERROR("counters group series belong to a FunctionGroupCounter owner, "
          "a counter of the group itself cannot be added");
    throw std::logic_error("counters group series are owner managed");
  }

  checkSeriesLabels(labels);

  /* identity is the label set, not the callback (functions cannot be
   * compared): the series keeps the callback registered first */
  for (auto& counter : counters) {
    if (counter->getSeriesLabels() != labels)
      continue;
    FunctionCounter* function = dynamic_cast<FunctionCounter*>(counter);
    if (!function) {
      ERROR("series of the counters group is already registered as an "
            "atomic counter");
      throw std::logic_error("series already registered as another kind");
    }
    return *function;
  }

  auto counter = new FunctionCounter(func, labels);
  counters.emplace_back(counter);
  return *counter;
}

FunctionGroupCounter& StatCountersSingleGroup::addFunctionGroupCounter(FunctionGroupCounter::CallbackFunction func)
{
  lock_guard<AmMutex> l(counters_lock);

  if (!counters.empty() && !owner_managed_series) {
    /* the owner emits series with a label structure of its own, which
     * cannot be reconciled with the one the group already carries */
    ERROR("counters group already carries series of its own, a series "
          "owner cannot take it over");
    throw std::logic_error("counters group already has own series");
  }

  /* the owner keeps its own storage for the series: one owner per group,
   * a repeated registration returns the owner registered before */
  for (auto& counter : counters) {
    FunctionGroupCounter* owner = dynamic_cast<FunctionGroupCounter*>(counter);
    if (owner)
      return *owner;
  }

  owner_managed_series = true;
  auto counter = new FunctionGroupCounter(func);
  counters.emplace_back(counter);
  return *counter;
}

void StatCountersSingleGroup::iterateGroups(const string& name, iterate_groups_callback_type callback)
{
  callback(name, *this);
}

void StatCountersSingleGroup::iterateCounters(iterate_counters_callback_type callback)
{
  lock_guard<AmMutex> l(counters_lock);

  for (auto& counter : counters) {
    counter->iterate(callback);
  }
}

AmStatistics::AmStatistics() {}

AmStatistics::~AmStatistics()
{
  for (auto& it : counters_groups_containers) {
    if (it.second.managed_by_am_statistics)
      delete it.second.groups_container;
  }
}

string AmStatistics::getConcatenatedName(const string& naming_group, const string& name)
{
  return naming_group + "_" + name;
}

AmStatistics& AmStatistics::addLabel(const string& name, const string& value)
{
  addLabelInternal(name, value);
  return *this;
}

void AmStatistics::iterateGroups(StatsCountersGroupsContainerInterface::iterate_groups_callback_type callback)
{
  map<string, StatsCountersGroupsContainerInterface*> groups_container;
  {
    lock_guard<AmMutex> lock(groups_mutex);
    for (auto& it : counters_groups_containers) {
      groups_container.emplace(it.first, it.second.groups_container);
    }
  }

  for (auto& it : groups_container) {
    it.second->iterateGroups(it.first, callback);
  }
}

StatCountersSingleGroup& AmStatistics::group(StatCountersSingleGroup::Type type,
                                             const string& naming_group, const string& name)
{
  return group(type, getConcatenatedName(naming_group, name));
}

StatCountersSingleGroup& AmStatistics::group(StatCountersSingleGroup::Type type, const string& name)
{
  lock_guard<AmMutex> lock(groups_mutex);

  StatCountersSingleGroup* counter = new StatCountersSingleGroup(type);
  auto it = counters_groups_containers.try_emplace(name, counter, true);

  if (!it.second) {
    delete counter;
    counter = nullptr;
  }

  auto& existent_group = *dynamic_cast<StatCountersSingleGroup*>(it.first->second.groups_container);

  if (!it.second && existent_group.getType() != type) {
    ERROR("attempt to add counter '%s' with type '%s' to existing counters group with another type '%s'",
          name.data(), StatCountersGroupsInterface::type2str(type),
          StatCountersGroupsInterface::type2str(existent_group.getType()));
    throw std::logic_error("attempt to redefine existent StatCountersSingleGroup");
  }

  return existent_group;
}

void AmStatistics::addGroupsContainer(const string& name, StatsCountersGroupsContainerInterface* container,
                                      bool is_managed_by_am_statistics)
{
  auto it = counters_groups_containers.try_emplace(name, container, is_managed_by_am_statistics);
  if (!it.second) {
    ERROR("attempt to add groups container %p by existing name: %s", container, name.data());
    throw std::logic_error("attempt to redefine existent StatCountersGroup");
  }
}