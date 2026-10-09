#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# GRAPHICS is a list of directories containing graphics files
# GFXBUILD is the directory where converted graphics files will be placed
#   If set to $(BUILD), it will statically link in the converted
#   files as if they were data files.
#
# NO_SMDH: if set to anything, no SMDH file is generated.
# ROMFS is the directory which contains the RomFS, relative to the Makefile (Optional)
# APP_TITLE is the name of the app stored in the SMDH file (Optional)
# APP_DESCRIPTION is the description of the app stored in the SMDH file (Optional)
# APP_AUTHOR is the author of the app stored in the SMDH file (Optional)
# ICON is the filename of the icon (.png), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.png
#     - icon.png
#     - <libctru folder>/default_icon.png
#---------------------------------------------------------------------------------
TARGET		:=	Tube3D
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	external/rapidjson/include
GRAPHICS	:=	gfx
GFXBUILD	:=	$(BUILD)
APP_TITLE	:=	Tube3D
APP_DESCRIPTION	:=	Tube3D
APP_AUTHOR	:=	shekmenon039
ICON		:=	icon.png
#GFXBUILD	:=	$(ROMFS)/gfx

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

# Extra warnings. Our code builds clean with all of them; tools/lint.sh adds a few stricter,
# noisier ones on top.
WARN	:=	-Wall -Wextra -Wno-unused-parameter -Wshadow -Wformat=2 -Wduplicated-cond \
			-Wduplicated-branches -Wlogical-op -Wnull-dereference -Wpointer-arith -Wundef \
			-Wmissing-declarations -Wunused-macros
# make WERROR=1 (as CI does) turns them into errors; the libraries in lib/ have their own flags
ifeq ($(WERROR),1)
WARN	+=	-Werror
endif

CFLAGS	:=	-g $(WARN) -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__
# Leave out assert() checks (in practice RapidJSON's) and their texts and file names
CFLAGS	+=	-DNDEBUG
# Record source paths relative to the client folder. Otherwise __FILE__ in rapidjson's asserts
# would bake the build machine's home folder into the app.
CFLAGS	+=	-ffile-prefix-map=$(abspath $(TOPDIR))=.

# The version: bump it for each release and tag the commit vX.Y.Z. Settings > More shows it
# together with the commit it was built from, e.g. "1.0.0 (e206264)" (buildinfo.c joins the
# two), and the CIA's title version is made from it as well.
VERSION  := 1.0.0
COMMIT   := $(shell git -C $(TOPDIR) rev-parse --short HEAD 2>/dev/null)
BUILD_ID := $(VERSION)$(if $(COMMIT), ($(COMMIT)))
CFLAGS	+=	-DTUBE3D_VERSION=\"$(VERSION)\" -DTUBE3D_COMMIT=\"$(COMMIT)\"

# An optional YouTube Data API key, compiled in from client/secrets.mk. That file is git-ignored
# and must never be committed; secrets.mk.example shows the format. An apikey= line in
# sdmc:/3ds/Tube3D/config.txt overrides it. Run make clean && make after changing the key.
-include $(TOPDIR)/secrets.mk
ifneq ($(strip $(YT_DATA_API_KEY)),)
CFLAGS	+=	-DYT_DATA_API_KEY=\"$(strip $(YT_DATA_API_KEY))\"
endif

# The only C++ is the YouTube parser in yt.cpp. It isn't speed-critical, so optimise it for size.
CXXFLAGS	:= $(CFLAGS) -Os -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lavformat -lavcodec -lswresample -lavutil -lcurl -lsmb2 -lssh2 -lmbedtls -lmbedx509 -lmbedcrypto -lz -lturbojpeg \
	-lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
# lib/ holds the trimmed FFmpeg, libsmb2 and libssh2 built by the tools/build_*.sh scripts
LIBDIRS	:= $(CURDIR)/lib/ffmpeg $(CURDIR)/lib/libsmb2 $(CURDIR)/lib/libssh2 $(PORTLIBS) $(CTRULIB)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
ifeq ($(GFXBUILD),$(BUILD))
#---------------------------------------------------------------------------------
export T3XFILES :=  $(GFXFILES:.t3s=.t3x)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
export ROMFS_T3XFILES	:=	$(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
			$(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
			$(addsuffix .h,$(subst .,_,$(BINFILES))) \
			$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all prepare clean clean-app clean-libs clean-ffmpeg clean-libsmb2 clean-libssh2 cia

#---------------------------------------------------------------------------------
# The libraries the app links that devkitPro doesn't ship: a trimmed FFmpeg, libsmb2 and libssh2.
# Their sources are git submodules in external/ (git submodule update --init). Each is built
# into lib/ the first time it's missing, by its script in tools/. The script patches and compiles
# a copy of the submodule in build-libs/ (the submodule itself stays untouched); that scratch
# folder is deleted once the library is installed, and kept only if the build fails.
#---------------------------------------------------------------------------------
.DEFAULT_GOAL := all   # (the library rules below come first, but plain "make" means all)
LIB_FFMPEG  := lib/ffmpeg/lib/libavcodec.a
LIB_LIBSMB2 := lib/libsmb2/lib/libsmb2.a
LIB_LIBSSH2 := lib/libssh2/lib/libssh2.a
TOOLS       := $(abspath $(TOPDIR)/../tools)

$(LIB_FFMPEG):
	@sh $(TOOLS)/build_ffmpeg.sh $(abspath build-libs/ffmpeg) && rm -rf build-libs/ffmpeg && { rmdir build-libs 2>/dev/null || true; }
$(LIB_LIBSMB2):
	@sh $(TOOLS)/build_libsmb2.sh $(abspath build-libs/libsmb2) && rm -rf build-libs/libsmb2 && { rmdir build-libs 2>/dev/null || true; }
$(LIB_LIBSSH2):
	@sh $(TOOLS)/build_libssh2.sh $(abspath build-libs/libssh2) && rm -rf build-libs/libssh2 && { rmdir build-libs 2>/dev/null || true; }

all: prepare
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# The libraries and the generated headers (shaders, data files) without compiling the app:
# what tools/lint.sh needs, so CI can lint before it builds
prepare: $(LIB_FFMPEG) $(LIB_LIBSMB2) $(LIB_LIBSSH2) $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile headers

#---------------------------------------------------------------------------------
# An installable CIA for FBI. Needs makerom and bannertool on PATH.
#---------------------------------------------------------------------------------
CIA_ID    := 0xF3D30
CIA_NAME  := $(APP_TITLE)
CIA_OUT   := $(TARGET)
CIA_RSF   := cia/tube3d.rsf
# The title version, which FBI and System Settings show as major.minor.micro. It's encoded as
# major * 1024 + minor * 16 + micro, so minor can go up to 63 and micro up to 15. The HOME menu
# keeps a title's icon and description until its version changes, so every release needs a new
# VERSION.
VERSION_WORDS := $(subst ., ,$(VERSION))
CIA_VERSION := $(shell echo $$(( $(word 1,$(VERSION_WORDS)) * 1024 + $(word 2,$(VERSION_WORDS)) * 16 + $(word 3,$(VERSION_WORDS)) )))

# The build to publish: the CIA, plus an .elf without the compiler's debug info
release:
	@$(MAKE) --no-print-directory cia
	@$(PREFIX)strip --strip-debug $(TARGET).elf   # the .3dsx and .cia never had it; this trims the .elf too

cia: all
	@bannertool makebanner -ci cia/banner.cgfx -a cia/banner.wav -o $(BUILD)/banner.bnr
	@bannertool makesmdh -s "$(CIA_NAME)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" \
		-i $(ICON) -o $(BUILD)/icon.icn
	@makerom -f cia -o $(CIA_OUT).cia -target t -exefslogo -elf $(TARGET).elf \
		-rsf $(CIA_RSF) -banner $(BUILD)/banner.bnr -icon $(BUILD)/icon.icn \
		-DAPP_UNIQUE_ID=$(CIA_ID) "-DAPP_RSF_TITLE=$(CIA_NAME)" -ver $(CIA_VERSION)
	@echo built ... $(CIA_OUT).cia "($(BUILD_ID), title version $(CIA_VERSION))"

$(BUILD):
	@mkdir -p $@

ifneq ($(GFXBUILD),$(BUILD))
$(GFXBUILD):
	@mkdir -p $@
endif

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
# make clean       everything: the app and the three libraries (the next build rebuilds them)
# make clean-app   just the app (quick; the libraries stay)
# make clean-libs  just the libraries, or one of them: clean-ffmpeg, clean-libsmb2, clean-libssh2
clean: clean-app clean-libs

clean-app:
	@echo clean app ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(TARGET).cia $(TARGET)_B.cia $(GFXBUILD)

clean-libs: clean-ffmpeg clean-libsmb2 clean-libssh2
	@rm -fr build-libs

clean-ffmpeg:
	@echo clean ffmpeg ...
	@rm -fr lib/ffmpeg build-libs/ffmpeg

clean-libsmb2:
	@echo clean libsmb2 ...
	@rm -fr lib/libsmb2 build-libs/libsmb2

clean-libssh2:
	@echo clean libssh2 ...
	@rm -fr lib/libssh2 build-libs/libssh2

#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

.PHONY: headers
headers: $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

# buildinfo.o carries the build ID, so it's rebuilt every time
buildinfo.o	:	FORCE
FORCE:

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
.PRECIOUS	:	%.t3x %.shbin
#---------------------------------------------------------------------------------
%.t3x.o	%_t3x.h :	%.t3x
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

#---------------------------------------------------------------------------------
%.shbin.o %_shbin.h : %.shbin
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
