//////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2018, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory
//
// Created by David Beckingsale, david@llnl.gov
// LLNL-CODE-747640
//
// All rights reserved.
//
// This file is part of Umpire.
//
// For details, see https://github.com/LLNL/Umpire
// Please also see the LICENSE file for MIT license.
//////////////////////////////////////////////////////////////////////////////
#include "gtest/gtest.h"
#include <string>
#include <sstream>

#include "umpire/config.hpp"
#include "umpire/ResourceManager.hpp"

#include "umpire/strategy/AllocationStrategy.hpp"
#include "umpire/strategy/MonotonicAllocationStrategy.hpp"
#include "umpire/strategy/SlotPool.hpp"
#include "umpire/strategy/DynamicPool.hpp"
#include "umpire/strategy/ThreadSafeAllocator.hpp"
#include "umpire/strategy/FixedPool.hpp"
#include "umpire/strategy/AllocationAdvisor.hpp"

#if defined(_OPENMP)
#include <omp.h>
#endif

static int unique_pool_name = 0;
static int initial_min_size = 1024;
static int subsequent_min_size = 512;

const char* AllocationDevices[] = {
  "HOST"
#if defined(UMPIRE_ENABLE_CUDA)
    , "DEVICE"
    , "UM"
    , "PINNED"
#endif
};

class StrategyTest :
  public ::testing::TestWithParam<const char*>
{
  public:
    virtual void SetUp() {
      auto& rm = umpire::ResourceManager::getInstance();
      allocatorName = GetParam();
      poolName << allocatorName << "_pool_" << unique_pool_name++;

     rm.makeAllocator<umpire::strategy::DynamicPool>
                    (  poolName.str()
                     , rm.getAllocator(allocatorName)
                     , initial_min_size
                     , subsequent_min_size);
     allocator = new umpire::Allocator(rm.getAllocator(poolName.str()));
    }

    umpire::Allocator* allocator;
    std::string allocatorName;
    std::stringstream poolName;
};

TEST_P(StrategyTest, Allocate) {
  void* alloc = nullptr;
  alloc = allocator->allocate(100);
  allocator->deallocate(alloc);
}

TEST_P(StrategyTest, Sizes) {
  void* alloc = nullptr;
  ASSERT_NO_THROW({ alloc = allocator->allocate(100); });
  ASSERT_EQ(allocator->getSize(alloc), 100);
  ASSERT_GE(allocator->getCurrentSize(), 100);
  ASSERT_EQ(allocator->getHighWatermark(), 100);
  ASSERT_GE(allocator->getActualSize(), initial_min_size);

  void* alloc2 = nullptr;
  ASSERT_NO_THROW({ alloc2 = allocator->allocate(initial_min_size); });
  ASSERT_NO_THROW({ allocator->deallocate(alloc); });

  ASSERT_GE(allocator->getCurrentSize(), initial_min_size);
  ASSERT_EQ(allocator->getHighWatermark(), initial_min_size+100);
  ASSERT_GE(allocator->getActualSize(), initial_min_size+subsequent_min_size);
  ASSERT_EQ(allocator->getSize(alloc2), initial_min_size);

  ASSERT_NO_THROW({ allocator->deallocate(alloc2); });
}

TEST_P(StrategyTest, Duplicate)
{
  auto& rm = umpire::ResourceManager::getInstance();

  ASSERT_TRUE(rm.isAllocator(allocatorName));
 
  ASSERT_EQ(allocator->getName(), poolName.str());

  ASSERT_ANY_THROW(
      rm.makeAllocator<umpire::strategy::DynamicPool>(
        poolName.str(), rm.getAllocator(allocatorName)));
}

INSTANTIATE_TEST_CASE_P(Allocations, StrategyTest, ::testing::ValuesIn(AllocationDevices));

#if defined(UMPIRE_ENABLE_CUDA)
TEST(SimpoolStrategy, Device)
{
  auto& rm = umpire::ResourceManager::getInstance();

  auto allocator = rm.getAllocator("DEVICE");
  void* alloc = nullptr;

  // Determine how much memory we can allocate from device
  std::size_t max_mem = 0;
  const std::size_t OneGiB = 1 * 1024 * 1024 * 1024;
  try {
    while ( true ) {  // Will "catch" out when allocation fails
      alloc = allocator.allocate(max_mem + OneGiB);
      ASSERT_NO_THROW( { allocator.deallocate(alloc); } );
      max_mem += OneGiB;
    }
  }
  catch (...) {
    ASSERT_GT(max_mem, OneGiB);
  }

  allocator = rm.makeAllocator<umpire::strategy::DynamicPool>(
      "device_simpool", rm.getAllocator("DEVICE"));

  ASSERT_EQ(allocator.getName(), "device_simpool");

  ASSERT_NO_THROW( { alloc = allocator.allocate(100); } );
  ASSERT_GE(allocator.getCurrentSize(), 100);
  ASSERT_EQ(allocator.getSize(alloc), 100);
  ASSERT_GE(allocator.getHighWatermark(), 100);
  ASSERT_NO_THROW( { allocator.deallocate(alloc); } );

  std::size_t alloc_size = max_mem / 4;
  void* alloc1 = nullptr;
  void* alloc2 = nullptr;
  void* alloc3 = nullptr;

  // Hold a little of the first block we allocate
  ASSERT_NO_THROW( { alloc1 = allocator.allocate(1024); } );
  ASSERT_NO_THROW( { alloc2 = allocator.allocate(1024); } );
  ASSERT_NO_THROW( { allocator.deallocate(alloc1); } );
  ASSERT_NO_THROW( { alloc3 = allocator.allocate(100); } );
  ASSERT_NO_THROW( { allocator.deallocate(alloc2); } );

  for (int i = 0; i < 16; ++i) {
    ASSERT_NO_THROW( { alloc1 = allocator.allocate(alloc_size); } );
    ASSERT_NO_THROW( { allocator.deallocate(alloc1); } );
    alloc_size += 1024*1024;
  }

  ASSERT_NO_THROW( { allocator.deallocate(alloc3); } );
}
#endif

TEST(MonotonicStrategy, Host)
{
  auto& rm = umpire::ResourceManager::getInstance();

  auto allocator = rm.makeAllocator<umpire::strategy::MonotonicAllocationStrategy>(
      "host_monotonic_pool", 65536, rm.getAllocator("HOST"));

  void* alloc = allocator.allocate(100);

  ASSERT_GE(allocator.getCurrentSize(), 100);
  ASSERT_EQ(allocator.getSize(alloc), 100);
  ASSERT_GE(allocator.getHighWatermark(), 100);
  ASSERT_EQ(allocator.getName(), "host_monotonic_pool");
}

#if defined(UMPIRE_ENABLE_CUDA)
TEST(MonotonicStrategy, Device)
{
  auto& rm = umpire::ResourceManager::getInstance();

  auto allocator = rm.makeAllocator<umpire::strategy::MonotonicAllocationStrategy>(
      "device_monotonic_pool", 65536, rm.getAllocator("DEVICE"));

  void* alloc = allocator.allocate(100);

  ASSERT_GE(allocator.getCurrentSize(), 100);
  ASSERT_EQ(allocator.getSize(alloc), 100);
  ASSERT_GE(allocator.getHighWatermark(), 100);
  ASSERT_EQ(allocator.getName(), "device_monotonic_pool");
}

TEST(MonotonicStrategy, UM)
{
  auto& rm = umpire::ResourceManager::getInstance();

  auto allocator = rm.makeAllocator<umpire::strategy::MonotonicAllocationStrategy>(
      "um_monotonic_pool", 65536, rm.getAllocator("UM"));

  void* alloc = allocator.allocate(100);

  ASSERT_GE(allocator.getCurrentSize(), 100);
  ASSERT_EQ(allocator.getSize(alloc), 100);
  ASSERT_GE(allocator.getHighWatermark(), 100);
  ASSERT_EQ(allocator.getName(), "um_monotonic_pool");
}

TEST(AllocationAdvisor, Create)
{
  auto& rm = umpire::ResourceManager::getInstance();

  ASSERT_NO_THROW(
    auto read_only_alloc = 
    rm.makeAllocator<umpire::strategy::AllocationAdvisor>(
      "read_only_um", rm.getAllocator("UM"), "READ_MOSTLY"));

  ASSERT_ANY_THROW(
      auto failed_alloc = 
    rm.makeAllocator<umpire::strategy::AllocationAdvisor>(
      "read_only_um", rm.getAllocator("UM"), "FOOBAR"));
}

TEST(AllocationAdvisor, Host)
{
  auto& rm = umpire::ResourceManager::getInstance();
  auto um_allocator = rm.getAllocator("UM");
  auto host_allocator = rm.getAllocator("HOST");

  auto read_only_alloc = 
    rm.makeAllocator<umpire::strategy::AllocationAdvisor>(
      "preferred_location_host", um_allocator, "PREFERRED_LOCATION", host_allocator);

  ASSERT_NO_THROW({
      double* data = static_cast<double*>(
          read_only_alloc.allocate(1024*sizeof(double)));
      read_only_alloc.deallocate(data);
  });

}
#endif

TEST(FixedPool, Host)
{
  struct data { int _[100]; };

  auto& rm = umpire::ResourceManager::getInstance();

  auto allocator = rm.makeAllocator<umpire::strategy::FixedPool<data>>(
      "host_fixed_pool", rm.getAllocator("HOST"));

  void* alloc = allocator.allocate(100);

  ASSERT_GE(allocator.getCurrentSize(), sizeof(data)*64);
  ASSERT_EQ(allocator.getSize(alloc), sizeof(data));
  ASSERT_GE(allocator.getHighWatermark(), sizeof(data)*64);
  ASSERT_EQ(allocator.getName(), "host_fixed_pool");
}

#if defined(_OPENMP)
TEST(ThreadSafeAllocator, Host)
{
  auto& rm = umpire::ResourceManager::getInstance();

  auto allocator = rm.makeAllocator<umpire::strategy::ThreadSafeAllocator>(
      "thread_safe_allocator", rm.getAllocator("HOST"));

#pragma omp parallel
  {
    const size_t size = 1024*omp_get_thread_num();

    double* thread_data = static_cast<double*>(
     allocator.allocate(size*sizeof(double)));

    allocator.deallocate(thread_data);
  }

  SUCCEED();
}

#endif
