// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#ifndef CEPH_OBJEXP_H
#define CEPH_OBJEXP_H

#include <atomic>
#include <string>
#include <cerrno>
#include <sstream>
#include <iostream>

#include "auth/Crypto.h"

#include "common/armor.h"
#include "common/ceph_json.h"
#include "common/config.h"
#include "common/ceph_argparse.h"
#include "common/Formatter.h"
#include "common/errno.h"

#include "common/ceph_mutex.h"
#include "common/Cond.h"
#include "common/Thread.h"

#include "global/global_init.h"

#include "include/common_fwd.h"
#include "include/utime.h"
#include "include/str_list.h"

#include "rgw_sal_rados.h"

class RGWSI_RADOS;
class RGWSI_Zone;
class RGWBucketInfo;
class cls_timeindex_entry;

class RGWObjExpStore {
  CephContext *cct;
  RGWSI_RADOS *rados_svc;
  rgw::sal::RadosStore* store;
public:
  RGWObjExpStore(CephContext *_cct, RGWSI_RADOS *_rados_svc, rgw::sal::RadosStore* _store) : cct(_cct),
                                                                                      rados_svc(_rados_svc),
                                                                                      store(_store) {}

  int objexp_hint_add(const DoutPrefixProvider *dpp, 
                      const ceph::real_time& delete_at,
                      const std::string& tenant_name,
                      const std::string& bucket_name,
                      const std::string& bucket_id,
                      const rgw_obj_index_key& obj_key);

  int objexp_hint_list(const DoutPrefixProvider *dpp, 
                       const std::string& oid,
                       const ceph::real_time& start_time,
                       const ceph::real_time& end_time,
                       const int max_entries,
                       const std::string& marker,
                       std::list<cls_timeindex_entry>& entries, /* out */
                       std::string *out_marker,                 /* out */
                       bool *truncated);                   /* out */

  int objexp_hint_trim(const DoutPrefixProvider *dpp, 
                       const std::string& oid,
                       const ceph::real_time& start_time,
                       const ceph::real_time& end_time,
                       const std::string& from_marker,
                       const std::string& to_marker);
};

class RGWObjectExpirer {
protected:
  rgw::sal::Store* store;
  RGWObjExpStore exp_store;

  class OEWorker : public Thread, public DoutPrefixProvider {
    CephContext *cct;
    RGWObjectExpirer *oe;
    ceph::mutex lock = ceph::make_mutex("OEWorker");
    ceph::condition_variable cond;

  public:
    OEWorker(CephContext * const cct,
             RGWObjectExpirer * const oe)
      : cct(cct),
        oe(oe) {
    }

    void *entry() override;
    void stop();

    CephContext *get_cct() const override;
    unsigned get_subsys() const override;
    std::ostream& gen_prefix(std::ostream& out) const override;
  };

  OEWorker *worker{nullptr};
  std::atomic<bool> down_flag = { false };

public:
  explicit RGWObjectExpirer(rgw::sal::Store* _store)
    : store(_store),
      exp_store(_store->ctx(), static_cast<rgw::sal::RadosStore*>(store)->svc()->rados, static_cast<rgw::sal::RadosStore*>(store)),
      worker(NULL) {
  }
  ~RGWObjectExpirer() {
    stop_processor();
  }

  int hint_add(const DoutPrefixProvider *dpp, 
               const ceph::real_time& delete_at,
               const std::string& tenant_name,
               const std::string& bucket_name,
               const std::string& bucket_id,
               const rgw_obj_index_key& obj_key) {
    return exp_store.objexp_hint_add(dpp, delete_at, tenant_name, bucket_name,
                                     bucket_id, obj_key);
  }

  int garbage_single_object(const DoutPrefixProvider *dpp, objexp_hint_entry& hint);

  void garbage_chunk(const DoutPrefixProvider *dpp, 
                     std::list<cls_timeindex_entry>& entries, /* in  */
                     bool& need_trim);                        /* out */

  void trim_chunk(const DoutPrefixProvider *dpp, 
                  const std::string& shard,
                  const utime_t& from,
                  const utime_t& to,
                  const std::string& from_marker,
                  const std::string& to_marker);

  bool process_single_shard(const DoutPrefixProvider *dpp, 
                            const std::string& shard,
                            const utime_t& last_run,
                            const utime_t& round_start);

  bool inspect_all_shards(const DoutPrefixProvider *dpp, 
                          const utime_t& last_run,
                          const utime_t& round_start);

  bool going_down();
  void start_processor();
  void stop_processor();
};
#endif /* CEPH_OBJEXP_H */
