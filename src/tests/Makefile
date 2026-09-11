CXX = g++
BASE_FLAGS = -g -O2 -pthread -I../include -I/usr/include/efi -Wno-return-type -Wno-multichar -Wno-unused-parameter
CXXFLAGS = $(BASE_FLAGS) -Wno-unused-variable
LDFLAGS = -lpthread

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

../memory/%.o: ../memory/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

../utils/%.o: ../utils/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Shared objects
SHARED_OBJS = ../memory/mixed_bitmap.o \
              ../os_error_definitions.o \
              ../utils/lock.o ../utils/bitmap.o ../utils/util.o \
              ../utils/os_utils_portable_min.o \
              bsp_kout_stub.o \
              panic_stub.o

# BCB_test
BCB_OBJS = BCB_test.o ../memory/FreePagesAllocator_BCB.o ../utils/BuddyControlBlock_foundation.o ../utils/BCB_fnd_DeepFirst.o $(SHARED_OBJS)
BCB_test: $(BCB_OBJS)
	$(CXX) -o $@ $(BCB_OBJS) $(LDFLAGS)

# test_kpoolmemmgr
KPOOL_FLAGS = $(BASE_FLAGS) -DTEST_MODE -Wno-unused-variable
test_kpoolmemmgr/test_main.o: test_kpoolmemmgr/test_main.cpp
	$(CXX) $(KPOOL_FLAGS) -c -o $@ $<
../memory/kpoolmemmgr_HCBv3.o: ../memory/kpoolmemmgr_HCBv3.cpp
	$(CXX) $(KPOOL_FLAGS) -c -o $@ $<
../memory/kpoolmemmgr.o: ../memory/kpoolmemmgr.cpp
	$(CXX) $(KPOOL_FLAGS) -c -o $@ $<
KPOOL_OBJS = test_kpoolmemmgr/test_main.o \
             ../memory/kpoolmemmgr_HCBv3.o \
             ../memory/kpoolmemmgr.o \
             $(SHARED_OBJS)
test_kpoolmemmgr.x: $(KPOOL_OBJS)
	$(CXX) -o $@ $(KPOOL_OBJS) $(LDFLAGS)

# BCB_foundation_test
FND_OBJS = BCB_foundation_test.o ../utils/BuddyControlBlock_foundation.o ../utils/BCB_fnd_DeepFirst.o $(SHARED_OBJS)
BCB_foundation_test: $(FND_OBJS)
	$(CXX) -o $@ $(FND_OBJS) $(LDFLAGS)

# BCB_juvenile_test — 幼年/成年态 + 成年仪式
# 测试自身用 -O0 保证源码行级断点精确（库对象仍 -O2）
BCB_juvenile_test.o: BCB_juvenile_test.cpp
	$(CXX) $(BASE_FLAGS) -O0 -Wno-unused-variable -c -o $@ $<
JUV_OBJS = BCB_juvenile_test.o ../utils/BuddyControlBlock_foundation.o ../utils/BCB_fnd_DeepFirst.o $(SHARED_OBJS)
BCB_juvenile_test: $(JUV_OBJS)
	$(CXX) -o $@ $(JUV_OBJS) $(LDFLAGS)

# init_bcb_juvenile_test — plan_and_setup 切分效果（test_set JSON 驱动）
../init/init_bcb_juvenile.o: ../init/init_bcb_juvenile.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
../../user_utils/json_configs_prase.o: ../../user_utils/json_configs_prase.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
init_bcb_juvenile_test.o: init_bcb_juvenile_test.cpp
	$(CXX) $(CXXFLAGS) -O0 -c -o $@ $<
IBJ_OBJS = init_bcb_juvenile_test.o ../init/init_bcb_juvenile.o ../../user_utils/json_configs_prase.o
init_bcb_juvenile_test: $(IBJ_OBJS)
	$(CXX) -o $@ $(IBJ_OBJS) $(LDFLAGS)

# benchmark_triple — 三路对比
BENCH_OBJS = benchmark_triple.o ../utils/BuddyControlBlock_foundation.o $(SHARED_OBJS)
benchmark_triple: $(BENCH_OBJS)
	$(CXX) -o $@ $(BENCH_OBJS) $(LDFLAGS)

# printk_ring_test — printk 宿主验收（file sink + mmap 环，freestanding 风味）
printk_ring_test.o: printk_ring_test.cpp
	$(CXX) $(CXXFLAGS) -O0 -c -o $@ $<
PRINTK_OBJS = printk_ring_test.o ../utils/printk.o ../utils/lock.o
printk_ring_test: $(PRINTK_OBJS)
	$(CXX) -o $@ $(PRINTK_OBJS) $(LDFLAGS)

# init_asset_registry_test — init 资产树 CRUD 测试（host，g++）
# 依赖 strcmp 系列在测试 TU 内以 host 桩实现（对齐 wheels.asm 语义）
../init/init_asset_registry.o: ../init/init_asset_registry.cpp
	$(CXX) $(CXXFLAGS) -O0 -c -o $@ $<
init_asset_registry_test.o: init_asset_registry_test.cpp
	$(CXX) $(CXXFLAGS) -O0 -c -o $@ $<
IAR_OBJS = init_asset_registry_test.o ../init/init_asset_registry.o
init_asset_registry_test: $(IAR_OBJS)
	$(CXX) -o $@ $(IAR_OBJS) $(LDFLAGS)

clean:
	rm -f BCB_test BCB_foundation_test BCB_juvenile_test benchmark_triple test_kpoolmemmgr.x
	rm -f init_bcb_juvenile_test
	rm -f init_asset_registry_test
	rm -f BCB_test.o BCB_foundation_test.o BCB_juvenile_test.o benchmark_triple.o
	rm -f init_bcb_juvenile_test.o ../init/init_bcb_juvenile.o ../../user_utils/json_configs_prase.o
	rm -f init_asset_registry_test.o ../init/init_asset_registry.o
	rm -f ../memory/FreePagesAllocator_BCB.o ../memory/mixed_bitmap.o
	rm -f ../memory/kpoolmemmgr_HCBv3.o ../memory/kpoolmemmgr.o
	rm -f ../os_error_definitions.o ../utils/lock.o ../utils/bitmap.o
	rm -f ../utils/util.o ../utils/BuddyControlBlock_foundation.o
	rm -f bsp_kout_stub.o panic_stub.o
	rm -f test_kpoolmemmgr/test_main.o
	rm -f printk_ring_test printk_ring_test.o ../utils/printk.o

.PHONY: clean
