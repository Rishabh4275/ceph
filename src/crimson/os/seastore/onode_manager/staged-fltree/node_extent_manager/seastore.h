// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include "crimson/os/seastore/logging.h"

#include "crimson/os/seastore/onode_manager/staged-fltree/node_extent_manager.h"
#include "crimson/os/seastore/onode_manager/staged-fltree/node_delta_recorder.h"

/**
 * seastore.h
 *
 * Seastore backend implementations.
 */

namespace crimson::os::seastore::onode {

class SeastoreSuper final: public Super {
 public:
  SeastoreSuper(Transaction& t, RootNodeTracker& tracker,
                laddr_t root_addr, TransactionManager& tm)
    : Super(t, tracker), root_addr{root_addr}, tm{tm} {}
  ~SeastoreSuper() override = default;
 protected:
  laddr_t get_root_laddr() const override {
    return root_addr;
  }
  void write_root_laddr(context_t c, laddr_t addr) override;
 private:
  laddr_t root_addr;
  TransactionManager& tm;
};

class SeastoreNodeExtent final: public NodeExtent {
 public:
  SeastoreNodeExtent(ceph::bufferptr &&ptr)
    : NodeExtent(std::move(ptr)) {}
  SeastoreNodeExtent(const SeastoreNodeExtent& other)
    : NodeExtent(other) {}
  ~SeastoreNodeExtent() override = default;
 protected:
  NodeExtentRef mutate(context_t, DeltaRecorderURef&&) override;

  DeltaRecorder* get_recorder() const override {
    return recorder.get();
  }

  CachedExtentRef duplicate_for_write() override {
    return CachedExtentRef(new SeastoreNodeExtent(*this));
  }
  extent_types_t get_type() const override {
    return extent_types_t::ONODE_BLOCK_STAGED;
  }
  ceph::bufferlist get_delta() override {
    assert(recorder);
    return recorder->get_delta();
  }
  void apply_delta(const ceph::bufferlist&) override;
 private:
  DeltaRecorderURef recorder;
};

class SeastoreNodeExtentManager final: public NodeExtentManager {
 public:
  SeastoreNodeExtentManager(TransactionManager& tm, laddr_t min)
    : tm{tm}, addr_min{min} {};
  ~SeastoreNodeExtentManager() override = default;
  TransactionManager& get_tm() { return tm; }
 protected:
  bool is_read_isolated() const override { return true; }

  tm_future<NodeExtentRef> read_extent(
      Transaction& t, laddr_t addr, extent_len_t len) override {
    TRACET("reading {}B at {:#x} ...", t, len, addr);
    return tm.read_extent<SeastoreNodeExtent>(t, addr, len
    ).safe_then([addr, len, this, &t](auto&& e) {
      TRACET("read {}B at {:#x}", t, e->get_length(), e->get_laddr());
      assert(e->get_laddr() == addr);
      assert(e->get_length() == len);
      std::ignore = addr;
      std::ignore = len;
      return NodeExtentRef(e);
    });
  }

  tm_future<NodeExtentRef> alloc_extent(
      Transaction& t, extent_len_t len) override {
    TRACET("allocating {}B ...", t, len);
    return tm.alloc_extent<SeastoreNodeExtent>(t, addr_min, len
    ).safe_then([len, this, &t](auto extent) {
      DEBUGT("allocated {}B at {:#x}",
             t, extent->get_length(), extent->get_laddr());
      assert(extent->get_length() == len);
      std::ignore = len;
      return NodeExtentRef(extent);
    });
  }

  tm_future<> retire_extent(
      Transaction& t, NodeExtentRef _extent) override {
    LogicalCachedExtentRef extent = _extent;
    auto addr = extent->get_laddr();
    auto len = extent->get_length();
    DEBUGT("retiring {}B at {:#x} ...", t, len, addr);
    return tm.dec_ref(t, extent).safe_then([addr, len, this, &t] (unsigned cnt) {
      assert(cnt == 0);
      TRACET("retired {}B at {:#x} ...", t, len, addr);
    });
  }

  tm_future<Super::URef> get_super(
      Transaction& t, RootNodeTracker& tracker) override {
    TRACET("get root ...", t);
    return tm.read_onode_root(t).safe_then([this, &t, &tracker](auto root_addr) {
      TRACET("got root {:#x}", t, root_addr);
      return Super::URef(new SeastoreSuper(t, tracker, root_addr, tm));
    });
  }

  std::ostream& print(std::ostream& os) const override {
    return os << "SeastoreNodeExtentManager";
  }

 private:
  static LOG_PREFIX(OTree::Seastore);

  TransactionManager& tm;
  const laddr_t addr_min;
};

}
