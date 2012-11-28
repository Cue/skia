# Makefile that wraps the Gyp and build steps for Unix and Mac (but not Windows)
# Uses "make" to build on Unix, and "xcodebuild" to build on Mac.
#
# Some usage examples (tested on both Linux and Mac):
#
#   # Clean everything
#   make clean
#
#   # Build and run tests (in Debug mode)
#   make tests
#   out/Debug/tests
#
#   # Build and run tests (in Release mode)
#   make tests BUILDTYPE=Release
#   out/Release/tests
#
#   # Build bench and SampleApp (both in Release mode), and then run them
#   make SampleApp bench BUILDTYPE=Release
#   out/Release/bench -repeat 2
#   out/Release/SampleApp
#
#   # Build all targets (in Debug mode)
#   make
#
# If you want more fine-grained control, you can run gyp and then build the
# gyp-generated projects yourself.
#
# See https://sites.google.com/site/skiadocs/ for complete documentation.

SKIA_OUT ?= out
BUILDTYPE ?= Debug
CWD := $(shell pwd)

# Soon we should be able to get rid of VALID_TARGETS, and just pass control
# to the gyp-generated Makefile for *any* target name.
# But that will be a bit complicated, so let's keep it for a future CL.
# Tracked as https://code.google.com/p/skia/issues/detail?id=947 ('eliminate
# need for VALID_TARGETS in toplevel Makefile')
VALID_TARGETS := \
                 bench \
                 debugger \
                 everything \
                 gm \
                 most \
                 SampleApp \
                 SkiaAndroidApp \
                 skia_base_libs \
                 tests \
                 tools

# Default target.  This must be listed before all other targets.
.PHONY: default
default: most

# As noted in http://code.google.com/p/skia/issues/detail?id=330 , building
# multiple targets in parallel was failing.  The special .NOTPARALLEL target
# tells gnu make not to run targets within _this_ Makefile in parallel, but the
# recursively invoked Makefile within out/ _is_ allowed to run in parallel
# (so you can still get some speedup that way).
.NOTPARALLEL:

uname := $(shell uname)
ifneq (,$(findstring CYGWIN, $(uname)))
  $(error Cannot build using Make on Windows. See https://sites.google.com/site/skiadocs/user-documentation/quick-start-guides/windows)
endif

# If user requests "make all", chain to our explicitly-declared "everything"
# target. See https://code.google.com/p/skia/issues/detail?id=932 ("gyp
# automatically creates "all" target on some build flavors but not others")
.PHONY: all
all: everything

.PHONY: clean
clean:
	rm -rf out xcodebuild
ifneq (out, $(SKIA_OUT))
	rm -rf $(SKIA_OUT)
endif

# Run gyp no matter what.
.PHONY: gyp
gyp:
	$(CWD)/gyp_skia

# Run gyp if necessary.
#
# On Linux, only run gyp if we haven't already generated the platform-specific
# Makefiles.  If the underlying gyp configuration has changed since these
# Makefiles were generated, they will rerun gyp on their own.
#
# This does not work for Mac, though... so for now, we ALWAYS rerun gyp on Mac.
# TODO(epoger): Figure out a better solution for Mac... maybe compare the
# gypfile timestamps to the xcodebuild project timestamps?
.PHONY: gyp_if_needed
gyp_if_needed:
ifneq (,$(findstring Linux, $(uname)))
	$(MAKE) $(SKIA_OUT)/Makefile
endif
ifneq (,$(findstring Darwin, $(uname)))
	$(CWD)/gyp_skia
endif

$(SKIA_OUT)/Makefile:
	$(CWD)/gyp_skia

# For all specific targets: run gyp if necessary, and then pass control to
# the gyp-generated buildfiles.
#
# For the Mac, we create a convenience symlink to the generated binary.
.PHONY: $(VALID_TARGETS)
$(VALID_TARGETS):: gyp_if_needed
ifneq (,$(findstring skia_os=android, $(GYP_DEFINES)))
	$(MAKE) -C $(SKIA_OUT) $@ BUILDTYPE=$(BUILDTYPE)
else ifneq (,$(findstring Linux, $(uname)))
	$(MAKE) -C $(SKIA_OUT) $@ BUILDTYPE=$(BUILDTYPE)
else ifneq (,$(findstring make, $(GYP_GENERATORS)))
	$(MAKE) -C $(SKIA_OUT) $@ BUILDTYPE=$(BUILDTYPE)
else ifneq (,$(findstring Darwin, $(uname)))
	rm -f out/$(BUILDTYPE) || if test -d out/$(BUILDTYPE); then echo "run 'make clean' or otherwise delete out/$(BUILDTYPE)"; exit 1; fi
	xcodebuild -project out/gyp/$@.xcodeproj -configuration $(BUILDTYPE)
	ln -s $(CWD)/xcodebuild/$(BUILDTYPE) out/$(BUILDTYPE)
else
	echo "unknown platform $(uname)"
	exit 1
endif
