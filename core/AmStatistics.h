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
/** @file AmStatistics.h - metrics registry */

#ifndef __AmStatistics_h__
#define __AmStatistics_h__

#include "singleton.h"
#include "AmThread.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using std::map;
using std::set;
using std::string;
using std::vector;

/** name/value labels of a counter or of the whole registry.
 *  getLabelsRef() stays valid only while no one updates the labels */
template <class Parent>
class StatLabelsContainer {
protected:
  map<string, string> labels;
  AmMutex labels_mutex;

  StatLabelsContainer() {}
  StatLabelsContainer(const map<string, string>& labels) : labels(labels) {}

  void addLabelInternal(const string& name, const string& value)
  {
    lock_guard<AmMutex> lock(labels_mutex);
    labels.emplace(name, value);
  }

  /** labels of a counter identify its series in the group and are set at
   *  registration; changing them afterwards would split or duplicate the
   *  series, so mutation stays protected - only the registry labels
   *  (AmStatistics) are public.  A callback emitting a whole set of
   *  labeled series (FunctionGroupCounter) does not use this container. */
  virtual Parent& addLabel(const string& name, const string& value) = 0;

  void updateLabel(const string& name, const string& value)
  {
    lock_guard<AmMutex> lock(labels_mutex);
    labels[name] = value;
  }

  void clearLabel(const string& name)
  {
    lock_guard<AmMutex> lock(labels_mutex);
    labels.erase(name);
  }

public:
  map<string, string> getLabels()
  {
    lock_guard<AmMutex> lock(labels_mutex);
    return labels;
  }

  const map<string, string>& getLabelsRef() { return labels; }
};

class StatCounterInterface {
public:
  using iterate_func_type =
    std::function<void(unsigned long long value, const map<string, string>&)>;

  StatCounterInterface() = default;
  StatCounterInterface(const StatCounterInterface&) = delete;
  StatCounterInterface(StatCounterInterface&&) = delete;
  virtual ~StatCounterInterface();

  virtual void iterate(iterate_func_type) = 0;

  /** labels identifying this series in its group, fixed at registration.
   *  A group callback emits labeled series of its own and has none here. */
  virtual map<string, string> getSeriesLabels() = 0;
};

/** monotonically incremented (or set) value; hot path: inc()/dec()/set().
 *  64 bit to survive process uptime; relaxed order is enough, metric values
 *  have no ordering requirements w.r.t. other memory */
class AtomicCounter :
  public StatCounterInterface,
  public StatLabelsContainer<AtomicCounter> {
  std::atomic_ullong value;

public:
  AtomicCounter(const map<string, string>& labels = {});
  AtomicCounter(const AtomicCounter&) = delete;
  AtomicCounter(AtomicCounter&&) = delete;
  ~AtomicCounter() override {}

  void iterate(iterate_func_type callback) override;
  map<string, string> getSeriesLabels() override { return getLabels(); }

  void inc(unsigned long long add = 1) { value.fetch_add(add, std::memory_order_relaxed); }
  void dec(unsigned long long sub = 1) { value.fetch_sub(sub, std::memory_order_relaxed); }
  void set(unsigned long long v) { value.store(v, std::memory_order_relaxed); }
  unsigned long long get() const { return value.load(std::memory_order_relaxed); }

protected:
  AtomicCounter& addLabel(const string& name, const string& value) override;
};

/** value computed by a callback at read (scrape) time; callback must be
 *  side-effect free (readable multiple times per scrape) */
class FunctionCounter :
  public StatCounterInterface,
  public StatLabelsContainer<FunctionCounter> {
public:
  typedef unsigned long long (*CallbackFunction)();

  FunctionCounter(CallbackFunction func, const map<string, string>& labels = {})
    : StatLabelsContainer<FunctionCounter>(labels), func_(func) {}
  FunctionCounter(const FunctionCounter&) = delete;
  FunctionCounter(FunctionCounter&&) = delete;
  ~FunctionCounter() override {}

  void iterate(iterate_func_type callback) override;
  map<string, string> getSeriesLabels() override { return getLabels(); }

protected:
  FunctionCounter& addLabel(const string& name, const string& value) override;

private:
  CallbackFunction func_;
};

/** a callback emitting a whole set of (value, labels) series at read time;
 *  the owner keeps its own storage (e.g. per-label buckets) */
class FunctionGroupCounter : public StatCounterInterface {
public:
  typedef void (*CallbackFunction)(iterate_func_type callback);

  FunctionGroupCounter(CallbackFunction func) : func_(func) {}
  FunctionGroupCounter(const FunctionGroupCounter&) = delete;
  FunctionGroupCounter(FunctionGroupCounter&&) = delete;
  ~FunctionGroupCounter() override {}

  void iterate(iterate_func_type callback) override;

  /** the series (and their labels) belong to the callback's own storage */
  map<string, string> getSeriesLabels() override { return map<string, string>(); }

private:
  CallbackFunction func_;
};

class StatCountersGroupsInterface {
public:
  enum Type {
    Counter,
    Gauge
  };

  using iterate_counters_callback_type =
    std::function<void(unsigned long long value, const map<string, string>&)>;

  StatCountersGroupsInterface(Type type) : type_(type) {}
  StatCountersGroupsInterface(const StatCountersGroupsInterface&) = delete;
  StatCountersGroupsInterface(StatCountersGroupsInterface&&) = delete;

  virtual void iterateCounters(iterate_counters_callback_type callback) = 0;

  static const char* type2str(Type type);

  StatCountersGroupsInterface& setHelp(const string& help)
  {
    help_ = help;
    return *this;
  }
  const string& getHelp() { return help_; }

  void setType(Type type) { type_ = type; }
  Type getType() { return type_; }

private:
  Type type_;
  string help_;
};

/** provider of metric groups, registered into the registry by name */
class StatsCountersGroupsContainerInterface {
public:
  using iterate_groups_callback_type =
    std::function<void(const string& name, StatCountersGroupsInterface& group)>;

  StatsCountersGroupsContainerInterface() = default;
  virtual ~StatsCountersGroupsContainerInterface() {}

  virtual void iterateGroups(const string& name, iterate_groups_callback_type callback) = 0;
};

/** set of counters sharing one metric name (series differ by labels) */
class StatCountersSingleGroup :
  public StatCountersGroupsInterface,
  public StatsCountersGroupsContainerInterface {
public:
  StatCountersSingleGroup(Type type) : StatCountersGroupsInterface(type) {}
  StatCountersSingleGroup(const StatCountersSingleGroup&) = delete;
  StatCountersSingleGroup(StatCountersSingleGroup&&) = delete;

  ~StatCountersSingleGroup() override;

  /** the labels identify the series: registering them again returns the
   *  existing counter, so a repeated registration is a no-op (for a
   *  function series the first callback is kept, the new one ignored).
   *  The first series fixes the label name set of the metric; a series
   *  with another structure fails fast - one metric name carries one
   *  label structure in the exposition. */
  AtomicCounter& addAtomicCounter(const map<string, string>& labels = {});
  FunctionCounter& addFunctionCounter(FunctionCounter::CallbackFunction func,
                                      const map<string, string>& labels = {});

  /** the series (and their labels) of a group callback belong to its owner;
   *  one owner per group, a repeated registration returns it */
  FunctionGroupCounter& addFunctionGroupCounter(FunctionGroupCounter::CallbackFunction func);

  void iterateGroups(const string& name, iterate_groups_callback_type callback) override;
  void iterateCounters(iterate_counters_callback_type callback) override;

  StatCountersSingleGroup& setHelp(const string& help)
  {
    StatCountersGroupsInterface::setHelp(help);
    return *this;
  }

private:
  /** under counters_lock: check the labels against the name set fixed by
   *  the first series (or fix it) */
  void checkSeriesLabels(const map<string, string>& labels);

  vector<StatCounterInterface*> counters;
  AmMutex counters_lock;
  set<string> label_names;
  bool series_labels_set = false;
  bool owner_managed_series = false;
};

class AmStatistics :
  public singleton<AmStatistics>,
  public StatLabelsContainer<AmStatistics> {
  friend class singleton<AmStatistics>;

public:
  AmStatistics& addLabel(const string& name, const string& value) override;

  void iterateGroups(StatsCountersGroupsContainerInterface::iterate_groups_callback_type callback);

  /** get or create group; naming_group prefixes the name ("core" + "x" -> "core_x") */
  StatCountersSingleGroup& group(StatCountersSingleGroup::Type type,
                                 const string& naming_group, const string& name);
  StatCountersSingleGroup& group(StatCountersSingleGroup::Type type, const string& name);

  void addGroupsContainer(const string& name, StatsCountersGroupsContainerInterface* container,
                          bool is_managed_by_am_statistics);

protected:
  AmStatistics();
  virtual ~AmStatistics();

  string getConcatenatedName(const string& naming_group, const string& name);

  struct GroupContainerEntry {
    StatsCountersGroupsContainerInterface* groups_container;
    bool managed_by_am_statistics;
    GroupContainerEntry(StatsCountersGroupsContainerInterface* groups_container,
                        bool managed_by_am_statistics)
      : groups_container(groups_container),
        managed_by_am_statistics(managed_by_am_statistics) {}
  };
  map<string, GroupContainerEntry> counters_groups_containers;

private:
  AmMutex groups_mutex;
};

#define stat_group(type, grouping_name, name) \
  AmStatistics::instance()->group(StatCountersSingleGroup::type, grouping_name, name)

#endif