SPDLOG_PRECOMPILED?=y
USE_SYSTEMD?=y

forkbomb-killer: main.o args.o inotify.o log.o $(if $(SPDLOG_PRECOMPILED),build/spdlog/libspdlog.a,)
ifeq "$(SPDLOG_PRECOMPILED)" "y"
libraries: spdlog
endif

LIBRARIES:=

spdlog:
	git clone 'https://github.com/gabime/spdlog.git' --depth 1 spdlog

build/spdlog/libspdlog.a: spdlog
	cmake -B build/spdlog -S spdlog
	+cmake --build build/spdlog -j $(nproc)
INCLUDEFLAGS=-I ./spdlog/include/
TO_DEEP_CLEAN:=spdlog

OUTPUTS:=forkbomb-killer


# ------------------------ DEFAULTS ------------------------

WFLAGS?=-Wall -Wextra
DBGFLAGS?=-g
OPTFLAGS?=
DEFFLAGS?=-D MORE_EFFORT_REMOVAL
EXTRAFLAGS?=

BUILDDIR?=build

# ----------------------- MAKE RULES -----------------------

ifeq "$(USE_SYSTEMD)" "y"
DEFFLAGS+=-D USE_SYSTEMD
LIBRARIES+=-lsystemd
endif
ifeq "$(SPDLOG_PRECOMPILED)" "y"
DEFFLAGS+=-D SPDLOG_COMPILED_LIB
endif


CSOURCES=$(shell find -maxdepth 1 -name '*.c')
CPPSOURCES=$(shell find -maxdepth 1 -name '*.cpp')

INCLUDEFLAGS+=-iquote "./include/"

ifneq "$(VERBOSE)" "y"
    Q=@
else
    Q=
endif

ifneq "$(CC)" "default"
    CC=gcc
endif
ifneq "$(CXX)" "default"
    CXX=g++
endif
ifneq "$(LD)" "default"
    LD=$(CXX)
endif

VPATH:=$(BUILDDIR) # also search for files in BUILDDIR for dependencies (e.g. object files)
MAKEFLAGS+=-Rr # ignore standard make recipes
CCFLAGS=$(WFLAGS) $(OPTFLAGS) $(DEBUGFLAGS) $(EXTRAFLAGS) $(INCLUDEFLAGS) $(DEFFLAGS)

.PHONY: debug release clean deepclean libraries
debug: $(.DEFAULT_GOAL)
debug: DEBUGFLAGS+=-g
debug: DEFFLAGS+=-D DEBUGGING_CLI

release: $(.DEFAULT_GOAL)
release: OPTFLAGS+=-O2

clean:
	$(Q)rm -f $(OUTPUTS)
	$(Q)rm -rf $(BUILDDIR)

deepclean: clean
	$(Q)rm -rf $(TO_DEEP_CLEAN)

libraries:


$(OUTPUTS): %:
	@printf "[ %3s ] linking   %-20s from [ %s ]\n" $(LD) $@ "$(notdir $^)"
	$(Q)$(LD) $^ -o $@ $(DEBUGFLAGS) $(LIBRARIES)

$(addprefix $(BUILDDIR)/,$(notdir $(CSOURCES:.c=.o))): $(BUILDDIR)/%.o: %.c | $(BUILDDIR) libraries
	@printf "[ %3s ] compiling %-20s from %s\n" $(CC) "$(notdir $@)" "$(notdir $<)"
	$(Q)$(CC) -r $(WFLAGS) $(CCFLAGS) $< $(sort $(filter %.o,$^)) -o $@ -MMD -MT $@ -MF $(BUILDDIR)/$(notdir $(@:.o=.d))

$(addprefix $(BUILDDIR)/,$(notdir $(CPPSOURCES:.cpp=.o))): $(BUILDDIR)/%.o: %.cpp | $(BUILDDIR) libraries
	@printf "[ %3s ] compiling %-20s from %s\n" $(CXX) "$(notdir $@)" "$(notdir $<)"
	$(Q)$(CXX) -std=c++20 -c $(CCFLAGS) $< $(sort $(filter %.o,$^)) -o $@ -MMD -MT $@ -MF $(BUILDDIR)/$(notdir $(@:.o=.d))

$(BUILDDIR):
	$(Q)mkdir -p $@

.PHONY: install
install: release
	$(Q)install -Dvp -m 0755 -o root -g root -t /usr/bin/ forkbomb-killer
	$(Q)install -Dvp --backup=numbered -m 0640 -o root -g root forkbomb-killer.service /etc/systemd/system/forkbomb-killer.service


include $(wildcard $(BUILDDIR)/*.d)
