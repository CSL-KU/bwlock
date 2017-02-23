##
# FILE		: Makefile
#
# BRIEF		: This Makefile builds bwlock kernel module and test executables.
#		  The generated executables are placed in the 'exe' directory
##

TOP_DIR 	:= $(shell pwd)
USER_DIR 	:= $(TOP_DIR)/user
TEST_DIR 	:= $(TOP_DIR)/test
USER_SRC 	:= $(wildcard $(USER_DIR)/*.c)
TEST_HR_SRC	:= $(wildcard $(TEST_DIR)/hr/*.c)
TEST_COMMON_SRC := $(wildcard $(TEST_DIR)/common/*.c)
USER_INCLUDES 	:= -I$(TOP_DIR)/include/common

MOD_EXE 	:= $(patsubst %.c,%.ko,$(wildcard $(TOP_DIR)/kernel/*.c))

KVERSION 	:= $(shell uname -r)
BLDDIR 		:= /lib/modules/$(KVERSION)/build

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

bench: bwlocktest hrtest bwlockset 

bwlocktest: $(TEST_COMMON_SRC)
	@echo "> Building Common Test Program"
	@$(CC) -std=gnu99 -O2 $(USER_INCLUDES) -g $^ -o exe/$@ -lrt

hrtest: $(TEST_HR_SRC)
	@echo "> Building HR-Test Program"
	@$(CC) -std=gnu99 -O2 $(USER_INCLUDES) -g $^ -o exe/$@ -lrt

bwlockset: $(USER_SRC)
	@echo "> Building Coarse Lock Program"
	@$(CC) -std=gnu99 -O2 $(USER_INCLUDES) -g $^ -o exe/$@ -lrt
