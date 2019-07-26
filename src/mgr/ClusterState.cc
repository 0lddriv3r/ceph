// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 John Spray <john.spray@inktank.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#include "messages/MMgrDigest.h"
#include "messages/MMonMgrReport.h"
#include "messages/MPGStats.h"

#include "mgr/ClusterState.h"
#include <boost/range/adaptor/reversed.hpp>

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mgr
#undef dout_prefix
#define dout_prefix *_dout << "mgr " << __func__ << " "

ClusterState::ClusterState(
  MonClient *monc_,
  Objecter *objecter_,
  const MgrMap& mgrmap)
  : monc(monc_),
    objecter(objecter_),
    lock("ClusterState"),
    mgr_map(mgrmap),
    pgservice(pg_map),
    asok_hook(NULL)
{}

void ClusterState::set_objecter(Objecter *objecter_)
{
  Mutex::Locker l(lock);

  objecter = objecter_;
}

void ClusterState::set_fsmap(FSMap const &new_fsmap)
{
  Mutex::Locker l(lock);

  fsmap = new_fsmap;
}

void ClusterState::set_mgr_map(MgrMap const &new_mgrmap)
{
  Mutex::Locker l(lock);
  mgr_map = new_mgrmap;
}

void ClusterState::set_service_map(ServiceMap const &new_service_map)
{
  Mutex::Locker l(lock);
  servicemap = new_service_map;
}

void ClusterState::load_digest(MMgrDigest *m)
{
  health_json = std::move(m->health_json);
  mon_status_json = std::move(m->mon_status_json);
}

void ClusterState::ingest_pgstats(MPGStats *stats)
{
  Mutex::Locker l(lock);

  const int from = stats->get_orig_source().num();

  pending_inc.update_stat(from, stats->epoch, std::move(stats->osd_stat));

  for (auto p : stats->pg_stat) {
    pg_t pgid = p.first;
    const auto &pg_stats = p.second;

    // In case we're hearing about a PG that according to last
    // OSDMap update should not exist
    if (existing_pools.count(pgid.pool()) == 0) {
      dout(15) << " got " << pgid
	       << " reported at " << pg_stats.reported_epoch << ":"
               << pg_stats.reported_seq
               << " state " << pg_state_string(pg_stats.state)
               << " but pool not in " << existing_pools
               << dendl;
      continue;
    }
    // In case we already heard about more recent stats from this PG
    // from another OSD
    const auto q = pg_map.pg_stat.find(pgid);
    if (q != pg_map.pg_stat.end() &&
	q->second.get_version_pair() > pg_stats.get_version_pair()) {
      dout(15) << " had " << pgid << " from "
	       << q->second.reported_epoch << ":"
               << q->second.reported_seq << dendl;
      continue;
    }

    pending_inc.pg_stat_updates[pgid] = pg_stats;
  }
}

void ClusterState::update_delta_stats()
{
  pending_inc.stamp = ceph_clock_now();
  pending_inc.version = pg_map.version + 1; // to make apply_incremental happy
  dout(10) << " v" << pending_inc.version << dendl;

  dout(30) << " pg_map before:\n";
  JSONFormatter jf(true);
  jf.dump_object("pg_map", pg_map);
  jf.flush(*_dout);
  *_dout << dendl;
  dout(30) << " incremental:\n";
  JSONFormatter jf(true);
  jf.dump_object("pending_inc", pending_inc);
  jf.flush(*_dout);
  *_dout << dendl;

  pg_map.apply_incremental(g_ceph_context, pending_inc);
  pending_inc = PGMap::Incremental();
}

void ClusterState::notify_osdmap(const OSDMap &osd_map)
{
  assert(lock.is_locked_by_me());

  pending_inc.stamp = ceph_clock_now();
  pending_inc.version = pg_map.version + 1; // to make apply_incremental happy
  dout(10) << " v" << pending_inc.version << dendl;

  PGMapUpdater::check_osd_map(g_ceph_context, osd_map, pg_map, &pending_inc);

  // update our list of pools that exist, so that we can filter pg_map updates
  // in synchrony with this OSDMap.
  existing_pools.clear();
  for (auto& p : osd_map.get_pools()) {
    existing_pools.insert(p.first);
  }

  // brute force this for now (don't bother being clever by only
  // checking osds that went up/down)
  set<int> need_check_down_pg_osds;
  PGMapUpdater::check_down_pgs(osd_map, pg_map, true,
			       need_check_down_pg_osds, &pending_inc);

  dout(30) << " pg_map before:\n";
  JSONFormatter jf(true);
  jf.dump_object("pg_map", pg_map);
  jf.flush(*_dout);
  *_dout << dendl;
  dout(30) << " incremental:\n";
  JSONFormatter jf(true);
  jf.dump_object("pending_inc", pending_inc);
  jf.flush(*_dout);
  *_dout << dendl;

  pg_map.apply_incremental(g_ceph_context, pending_inc);
  pending_inc = PGMap::Incremental();
  // TODO: Complete the separation of PG state handling so
  // that a cut-down set of functionality remains in PGMonitor
  // while the full-blown PGMap lives only here.
}

class ClusterSocketHook : public AdminSocketHook {
  ClusterState *cluster_state;
public:
  explicit ClusterSocketHook(ClusterState *o) : cluster_state(o) {}
  bool call(std::string_view admin_command, const cmdmap_t& cmdmap,
	    std::string_view format, bufferlist& out) override {
    stringstream ss;
    bool r = true;
    try {
      r = cluster_state->asok_command(admin_command, cmdmap, format, ss);
    } catch (const bad_cmd_get& e) {
      ss << e.what();
      r = true;
    }
    out.append(ss);
    return r;
  }
};

void ClusterState::final_init()
{
  AdminSocket *admin_socket = g_ceph_context->get_admin_socket();
  asok_hook = new ClusterSocketHook(this);
  int r = admin_socket->register_command("dump_osd_network",
		      "dump_osd_network name=value,type=CephInt,req=false", asok_hook,
		      "Dump osd heartbeat network ping times");
  ceph_assert(r == 0);
}

void ClusterState::shutdown()
{
  // unregister commands
  g_ceph_context->get_admin_socket()->unregister_commands(asok_hook);
  delete asok_hook;
  asok_hook = NULL;
}

bool ClusterState::asok_command(std::string_view admin_command, const cmdmap_t& cmdmap,
		       std::string_view format, ostream& ss)
{
  std::lock_guard l(lock);
  Formatter *f = Formatter::create(format, "json-pretty", "json-pretty");
  if (admin_command == "dump_osd_network") {
    int64_t value = 0;
    // Default to health warning level if nothing specified
    if (!(cmd_getval(g_ceph_context, cmdmap, "value", value))) {
      value = static_cast<int64_t>(g_ceph_context->_conf.get_val<uint64_t>("mon_warn_on_slow_ping_time"));
      if (value == 0) {
        double ratio = g_conf().get_val<double>("mon_warn_on_slow_ping_ratio");
	value = g_conf().get_val<int64_t>("osd_heartbeat_grace");
	value *= 1000000 * ratio; // Seconds of grace to microseconds at ratio
      }
    }
    if (value < 0)
      value = 0;

    struct mgr_ping_time_t {
      uint32_t pingtime;
      int from;
      int to;
      bool back;
      std::array<uint32_t,3> times;
      std::array<uint32_t,3> min;
      std::array<uint32_t,3> max;
      uint32_t last;

      bool operator<(const mgr_ping_time_t& rhs) const {
        if (pingtime < rhs.pingtime)
          return true;
        if (pingtime > rhs.pingtime)
          return false;
        if (from < rhs.from)
          return true;
        if (from > rhs.from)
          return false;
        if (to < rhs.to)
          return true;
        if (to > rhs.to)
          return false;
        return back;
      }
    };

    set<mgr_ping_time_t> sorted;
    for (auto i : pg_map.osd_stat) {
      for (auto j : i.second.hb_pingtime) {
	mgr_ping_time_t item;
	item.pingtime = std::max(j.second.back_pingtime[0], j.second.back_pingtime[1]);
	item.pingtime = std::max(item.pingtime, j.second.back_pingtime[2]);
	if (!value || item.pingtime >= value) {
	  item.from = i.first;
	  item.to = j.first;
	  item.times[0] = j.second.back_pingtime[0];
	  item.times[1] = j.second.back_pingtime[1];
	  item.times[2] = j.second.back_pingtime[2];
	  item.min[0] = j.second.back_min[0];
	  item.min[1] = j.second.back_min[1];
	  item.min[2] = j.second.back_min[2];
	  item.max[0] = j.second.back_max[0];
	  item.max[1] = j.second.back_max[1];
	  item.max[2] = j.second.back_max[2];
	  item.last = j.second.back_last;
	  item.back = true;
	  sorted.emplace(item);
	}

	if (j.second.front_last == 0)
	  continue;
	item.pingtime = std::max(j.second.front_pingtime[0], j.second.front_pingtime[1]);
	item.pingtime = std::max(item.pingtime, j.second.front_pingtime[2]);
	if (!value || item.pingtime >= value) {
	  item.from = i.first;
	  item.to = j.first;
	  item.times[0] = j.second.front_pingtime[0];
	  item.times[1] = j.second.front_pingtime[1];
	  item.times[2] = j.second.front_pingtime[2];
	  item.min[0] = j.second.front_min[0];
	  item.min[1] = j.second.front_min[1];
	  item.min[2] = j.second.front_min[2];
	  item.max[0] = j.second.front_max[0];
	  item.max[1] = j.second.front_max[1];
	  item.max[2] = j.second.front_max[2];
	  item.last = j.second.front_last;
	  item.back = false;
	  sorted.emplace(item);
	}
      }
    }

    // Network ping times (1min 5min 15min)
    f->open_object_section("network_ping_times");
    f->dump_int("threshold", value);
    f->open_array_section("entries");
    for (auto &sitem : boost::adaptors::reverse(sorted)) {
      ceph_assert(!value || sitem.pingtime >= value);

      f->open_object_section("entry");
      f->dump_int("from osd", sitem.from);
      f->dump_int("to osd", sitem.to);
      f->dump_string("interface", (sitem.back ? "back" : "front"));
      f->open_object_section("average");
      f->dump_unsigned("1min", sitem.times[0]);
      f->dump_unsigned("5min", sitem.times[1]);
      f->dump_unsigned("15min", sitem.times[2]);
      f->close_section(); // average
      f->open_object_section("min");
      f->dump_unsigned("1min", sitem.min[0]);
      f->dump_unsigned("5min", sitem.min[1]);
      f->dump_unsigned("15min", sitem.min[2]);
      f->close_section(); // min
      f->open_object_section("max");
      f->dump_unsigned("1min", sitem.max[0]);
      f->dump_unsigned("5min", sitem.max[1]);
      f->dump_unsigned("15min", sitem.max[2]);
      f->close_section(); // max
      f->dump_unsigned("last", sitem.last);
      f->close_section(); // entry
    }
    f->close_section(); // entries
    f->close_section(); // network_ping_times
  } else {
    ceph_abort_msg("broken asok registration");
  }
  f->flush(ss);
  delete f;
  return true;
}
