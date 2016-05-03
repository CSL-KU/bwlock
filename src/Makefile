TOP_DIR := $(shell pwd)
USER_DIR := $(TOP_DIR)/user
TEST_DIR := $(TOP_DIR)/test
USER_COMMON_SRC := $(wildcard $(USER_DIR)/common/*.c)
USER_COARSE_SRC := $(wildcard $(USER_DIR)/coarse/*.c)
TEST_SRC := $(wildcard $(TEST_DIR)/*.c)
USER_INCLUDES := -I$(TOP_DIR)/include/common

MOD_EXE := $(patsubst %.c,%.ko,$(wildcard $(TOP_DIR)/kernel/*.c))

KVERSION := $(shell uname -r)
BLDDIR := /lib/modules/$(KVERSION)/build

all: setup module bench move
	@echo "> Build Complete. Outputs can be seen in exe directory"

setup:
	@mkdir exe

module:
	@echo "> Building Bandwidth Lock Module"
	@make -C $(BLDDIR) M=$(PWD) modules

move:
	@mv $(MOD_EXE) exe/.
clean:
	@echo "> Executing Clean Target"
	make -C $(BLDDIR) M=$(PWD) clean
	@rm -rf exe
	@echo "> Cleaning Complete"

bench: bwlocktest bwlockset 

bwlocktest:  $(USER_COMMON_SRC) $(TEST_SRC)
	@echo "> Building Test Program"
	@$(CC) -std=gnu99 -O2 $(USER_INCLUDES) -g $^ -o exe/$@ -lrt

bwlockset: $(USER_COMMON_SRC) $(USER_COARSE_SRC)
	@echo "> Building Coarse Lock Program"
	@$(CC) -std=gnu99 -O2 $(USER_INCLUDES) -g $^ -o exe/$@ -lrt
