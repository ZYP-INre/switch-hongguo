# SPDX-License-Identifier: GPL-3.0-only
#
# 红果内容层 · Switch homebrew
#
# 需要 devkitPro（devkitA64 + libnx）与 switch-curl。
# 结构遵循 devkitPro 官方模板：LIBDIRS 填的是**根目录**，
# -I$(dir)/include 与 -L$(dir)/lib 由下面的规则自己拼。

ifeq ($(strip $(DEVKITPRO)),)
$(error 请先设置 DEVKITPRO，例如: export DEVKITPRO=$HOME/devkitpro/root/opt/devkitpro)
endif

TARGET      := hongguo-probe
BUILD       := build
SOURCES     := source ../hongguo-core/src
INCLUDES    := ../hongguo-core/include ../hongguo-core/third_party

# .nro 在 hbmenu 里显示的元信息
APP_TITLE   := 红果内容层诊断
APP_AUTHOR  := hongguo-switch
APP_VERSION := 0.1.0

# ⚠ 不要加 -ffast-math。签名链路里有多处浮点参与运算
#   （nextRandom32 的 rng()*2^32、protobuf 的 float 编码、envLaunch 的取整），
#   该选项允许编译器改写浮点语义，会让 Switch 上算出的签名与桌面端不一致 ——
#   而症状是"mac 上好好的、Switch 上签名就是不对"，极难定位。
ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

DEFINES     := -D__SWITCH__ -DHG_PLATFORM_SWITCH

# $(INCLUDE) 在父 make 里是空的，由子 make 从环境继承后才有值 —— 这是
# devkitPro 模板的约定：头文件路径经 export INCLUDE 传给子 make。
CFLAGS      := -g -Wall -O2 -ffunction-sections -fno-fast-math $(ARCH) $(DEFINES) $(INCLUDE)
CXXFLAGS    := $(CFLAGS) -std=gnu++17

# ⚠ 必须保留异常。devkitPro 的模板默认加 -fno-exceptions，
#   但 hongguo-core 全程用 std::runtime_error 做错误处理，关掉就编不过。
CXXFLAGS    += -fexceptions

ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# ⚠ 必须用编译器驱动来链接。devkitPro 的规则里写的是 $(LD)，但它并未定义 LD，
#   于是会落到系统 ld —— 而 LDFLAGS 里的 -specs= / -march= / -fPIE 只有
#   gcc 驱动认识，直接调 ld 会报 "unknown options"。
#   用 = 而非 := ，因为 CXX 要等 switch_rules 引入 base_tools 后才定义。
LD          = $(CXX)

# libcurl 及其依赖（switch-curl 依赖 switch-zlib 与 libnx）
LIBS        := -lcurl -lz -lnx

# ⚠ 填根目录，不是 lib 子目录。
#   这里写死路径而不用 $(PORTLIBS)/$(LIBNX)：那两个变量由文件末尾才 include 的
#   switch_rules 定义，而 := 是立即展开，用在上面会变成空。
LIBDIRS     := $(DEVKITPRO)/portlibs/switch $(DEVKITPRO)/libnx

# ---- 以下不需要改 ----

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

# $(BUILD) 是目录，若不作 PHONY，make 会认为它"已存在即已达成"而不再进入子 make
.PHONY: all clean $(BUILD)

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo 清理…
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).elf $(TARGET).nacp $(TARGET).map

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif

include $(DEVKITPRO)/libnx/switch_rules
