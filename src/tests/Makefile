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
PRINTK_OBJS = printk_ring_test.o ../utils/printk.o ../utils/vprintkv2.o ../utils/lock.o
printk_ring_test: $(PRINTK_OBJS)
	$(CXX) -o $@ $(PRINTK_OBJS) $(LDFLAGS)

# vprintkv2_test — 核心格式化引擎用户态验收（puts 输出 + 栈缓冲）
# 只依赖 freestanding 核心，无需 lock.o
vprintkv2_test.o: vprintkv2_test.cpp
	$(CXX) $(CXXFLAGS) -O0 -c -o $@ $<
VK2_OBJS = vprintkv2_test.o ../utils/vprintkv2.o
vprintkv2_test: $(VK2_OBJS)
	$(CXX) -o $@ $(VK2_OBJS) $(LDFLAGS)

# ring_v2_test — DmesgRingBuffer_v2 宿主验收（共用资产）
ring_v2_test.o: ring_v2_test.cpp
	$(CXX) $(CXXFLAGS) -O0 -c -o $@ $<
RINGV2_OBJS = ring_v2_test.o ../utils/kcirclebufflogMgr_v2.o
ring_v2_test: $(RINGV2_OBJS)
	$(CXX) -o $@ $(RINGV2_OBJS) $(LDFLAGS)

# init_printk_test — init_printk 宿主验收（mmap 文件当 ring buffer + 自实现 now_ts_us）
../init/%.o: ../init/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
init_printk_test.o: init_printk_test.cpp
	$(CXX) $(CXXFLAGS) -O0 -c -o $@ $<
IPK_OBJS = init_printk_test.o ../init/util/printk.o ../utils/kcirclebufflogMgr_v2.o ../utils/vprintkv2.o
init_printk_test: $(IPK_OBJS)
	$(CXX) -o $@ $(IPK_OBJS) $(LDFLAGS)

# ring_bind_test — 复现 init.elf 真实路径 init_ring_bind + init_printk
ring_bind_test.o: ring_bind_test.cpp
	$(CXX) $(CXXFLAGS) -O0 -c -o $@ $<
RBT_OBJS = ring_bind_test.o ../init/util/ring_log.o ../init/util/printk.o ../utils/kcirclebufflogMgr_v2.o ../utils/vprintkv2.o
ring_bind_test: $(RBT_OBJS)
	$(CXX) -o $@ $(RBT_OBJS) $(LDFLAGS)

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
	rm -f vprintkv2_test vprintkv2_test.o vprintkv2_test.out.txt ../utils/vprintkv2.o
	rm -f ring_v2_test ring_v2_test.o ring_v2_test.out.txt ../utils/kcirclebufflogMgr_v2.o
	rm -f init_printk_test init_printk_test.o init_printk_test.out.txt init_printk_ring.bin ../init/util/printk.o
	rm -f ring_bind_test ring_bind_test.o ../init/util/ring_log.o

.PHONY: clean
