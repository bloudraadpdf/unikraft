ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
LIBRARY := $(ROOT)/vendor/ext4
BUILD ?= $(ROOT)/build/ext4-tests
SOURCES := $(wildcard $(LIBRARY)/src/*.c)
HEADERS := $(wildcard $(LIBRARY)/include/*.h)
CFLAGS := -std=gnu11 -O1 -g -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_DEBUG_PRINTF=0

.PHONY: test
test: $(BUILD)/extents
	test ! -e $(BUILD)/extents.ext4
	truncate -s 32M $(BUILD)/extents.ext4
	mke2fs -q -t ext4 -b 1024 -O ^metadata_csum_seed,^orphan_file $(BUILD)/extents.ext4
	$(BUILD)/extents $(BUILD)/extents.ext4 create
	e2fsck -fn $(BUILD)/extents.ext4
	$(BUILD)/extents $(BUILD)/extents.ext4 truncate
	e2fsck -fn $(BUILD)/extents.ext4
	$(BUILD)/extents $(BUILD)/extents.ext4 clear
	e2fsck -fn $(BUILD)/extents.ext4
	$(BUILD)/extents $(BUILD)/extents.ext4 ranges
	e2fsck -fn $(BUILD)/extents.ext4
	$(BUILD)/extents $(BUILD)/extents.ext4 reset
	debugfs -w -R 'fallocate extents 0 127' $(BUILD)/extents.ext4
	debugfs -w -R 'sif extents size 131072' $(BUILD)/extents.ext4
	$(BUILD)/extents $(BUILD)/extents.ext4 unwritten
	e2fsck -fn $(BUILD)/extents.ext4
	$(BUILD)/extents $(BUILD)/extents.ext4 attributes-create
	e2fsck -fn $(BUILD)/extents.ext4
	$(BUILD)/extents $(BUILD)/extents.ext4 attributes-clear
	e2fsck -fn $(BUILD)/extents.ext4

$(BUILD)/extents: $(ROOT)/lib/ukfs-ext4/tests/extents.c $(SOURCES) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(LIBRARY)/include $< $(SOURCES) -o $@

.PHONY: shared-attributes
shared-attributes: $(BUILD)/extents
	python3 $(ROOT)/lib/ukfs-ext4/tests/shared-attributes.py $(BUILD)/extents.ext4 $(BUILD)/extents
