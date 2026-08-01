# OpenBoolCAD build
# Plain make + pkg-config. No generators.
#
# Two platforms from one source tree:
#   make            native binary  ./openboolcad
#   make web        browser build  web/openboolcad.html   (needs emsdk on PATH)
#
# The web build differs in its toolchain and its libraries, not in its sources:
# every src/*.cpp compiles for both, and what cannot be shared sits behind
# __EMSCRIPTEN__ in gl_compat.h, imgui_backend.h and platform.cpp.

TARGET   := openboolcad
BUILD    ?= debug
PLATFORM ?= native

IMGUI    := third_party/imgui
MANIFOLD := third_party/manifold

CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-unused-parameter
CXXFLAGS += -Isrc -I$(IMGUI) -I$(IMGUI)/backends -I$(MANIFOLD)/include
# -1 selects Manifold's serial backend; 1 would require TBB.
CXXFLAGS += -DMANIFOLD_PAR=-1

ifeq ($(PLATFORM),web)

  CXX := em++
  # Ports rather than pkg-config: emscripten builds and caches these itself.
  # sqlite3 is needed because the save format is a serialised in-memory database.
  PORTS    := -sUSE_SDL=2 -sUSE_ZLIB=1 -sUSE_SQLITE3=1
  CXXFLAGS += $(PORTS) -fexceptions
  OBJDIR   := obj/web-$(BUILD)

  LDFLAGS := $(PORTS) -fexceptions
  LDFLAGS += -sMIN_WEBGL_VERSION=1 -sMAX_WEBGL_VERSION=2
  # Meshes and undo snapshots are unbounded in a way a fixed heap is not.
  LDFLAGS += -sALLOW_MEMORY_GROWTH=1
  # The default 64 KB stack is not enough for the recursive scene walks or
  # Manifold's boolean tree.
  LDFLAGS += -sSTACK_SIZE=8MB
  # MEMFS is the staging area for both file directions in platform.cpp.
  LDFLAGS += -sFORCE_FILESYSTEM=1
  LDFLAGS += -sEXPORTED_RUNTIME_METHODS=ccall,FS,UTF8ToString,HEAPU8
  LDFLAGS += -sEXPORTED_FUNCTIONS=_main,_obc_platform_file_ready
  LDFLAGS += --shell-file web/shell.html

  ifeq ($(BUILD),release)
    CXXFLAGS += -O3 -DNDEBUG
    LDFLAGS  += -O3 -sASSERTIONS=0
  else
    CXXFLAGS += -O1 -g
    LDFLAGS  += -O1 -sASSERTIONS=1
  endif

  # opengl3 is the backend upstream supports on Emscripten; see imgui_backend.h.
  IMGUI_BACKEND := $(IMGUI)/backends/imgui_impl_opengl3.cpp

else

  CXX      ?= g++
  PKGS     := sdl2 gl sqlite3 zlib
  CXXFLAGS += $(shell pkg-config --cflags $(PKGS))
  LDFLAGS  := $(shell pkg-config --libs $(PKGS)) -lm

  ifeq ($(BUILD),release)
    CXXFLAGS += -O2 -DNDEBUG
    OBJDIR   := obj/release
  else
    CXXFLAGS += -O0 -g
    OBJDIR   := obj/debug
  endif

  IMGUI_BACKEND := $(IMGUI)/backends/imgui_impl_opengl2.cpp

endif

SRC := $(wildcard src/*.cpp)

# Fonts are compiled in rather than shipped beside the binary: the browser build
# has no filesystem to read them from. See tools/embed_fonts.py.
FONT_DIR   := third_party/fonts
FONT_FILES := $(wildcard $(FONT_DIR)/*.ttf) $(wildcard $(FONT_DIR)/*.otf)
FONT_GEN   := $(OBJDIR)/generated/fonts_data.cpp
FONT_OBJ   := $(OBJDIR)/generated/fonts_data.o

# Manifold, 3D core only. src/cross_section is not vendored: it needs Clipper2
# and only serves 2D operations, which SVG import may want later.
MANIFOLD_SRC := $(MANIFOLD)/src/boolean3.cpp \
                $(MANIFOLD)/src/boolean_result.cpp \
                $(MANIFOLD)/src/constructors.cpp \
                $(MANIFOLD)/src/csg_tree.cpp \
                $(MANIFOLD)/src/edge_op.cpp \
                $(MANIFOLD)/src/execution_impl.cpp \
                $(MANIFOLD)/src/face_op.cpp \
                $(MANIFOLD)/src/impl.cpp \
                $(MANIFOLD)/src/manifold.cpp \
                $(MANIFOLD)/src/minkowski.cpp \
                $(MANIFOLD)/src/polygon.cpp \
                $(MANIFOLD)/src/properties.cpp \
                $(MANIFOLD)/src/quickhull.cpp \
                $(MANIFOLD)/src/sdf.cpp \
                $(MANIFOLD)/src/smoothing.cpp \
                $(MANIFOLD)/src/sort.cpp \
                $(MANIFOLD)/src/subdivision.cpp \
                $(MANIFOLD)/src/tree2d.cpp
SRC += $(MANIFOLD_SRC)

SRC += $(IMGUI)/imgui.cpp \
       $(IMGUI)/imgui_draw.cpp \
       $(IMGUI)/imgui_tables.cpp \
       $(IMGUI)/imgui_widgets.cpp \
       $(IMGUI)/backends/imgui_impl_sdl2.cpp \
       $(IMGUI_BACKEND)

OBJ := $(patsubst %.cpp,$(OBJDIR)/%.o,$(SRC))
OBJ += $(FONT_OBJ)
DEP := $(OBJ:.o=.d)

WEB_TARGET := web/openboolcad.html

.PHONY: all run clean rebuild test web web-run web-clean

all: $(TARGET)

# Headless geometry tests: no SDL, GL or ImGui, so only the geometry sources
# and Manifold are linked. Native only - it is a command line binary.
TEST_TARGET := test_geometry
TEST_SRC    := tests/test_merge.cpp
TEST_DEPS   := src/mesh.cpp src/workplane.cpp src/mesh_repair.cpp src/csg.cpp src/scene.cpp \
               src/camera.cpp src/project.cpp src/export_stl.cpp \
               src/import_stl.cpp src/import_svg.cpp src/svg.cpp src/undo.cpp \
               src/gear.cpp src/text3d.cpp src/fonts.cpp src/bevel.cpp src/polyhedron.cpp
TEST_OBJ    := $(patsubst %.cpp,$(OBJDIR)/%.o,$(TEST_SRC) $(TEST_DEPS)) \
               $(patsubst %.cpp,$(OBJDIR)/%.o,$(MANIFOLD_SRC)) $(FONT_OBJ)

# The test object needs its header deps too, or a change to a header it includes
# leaves it stale and linked against a struct that has since changed shape - which
# looks exactly like a geometry bug, right down to the runaway allocation.
DEP += $(TEST_OBJ:.o=.d)

$(TEST_TARGET): $(TEST_OBJ)
	$(CXX) $(TEST_OBJ) -o $@ $(shell pkg-config --libs sqlite3 zlib gl) -lm

# Capped on purpose: a geometry bug turns into unbounded allocation or an
# endless loop, and an uncapped run takes the whole machine down with it.
test: $(TEST_TARGET)
	@bash -c 'ulimit -v 2000000; timeout 120 ./$(TEST_TARGET)'

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Vendored code is not ours to fix, so it builds without our warning set.
# GNU make prefers this rule over the generic one: the stem is shorter.
CXXFLAGS_VENDOR := $(filter-out -Wall -Wextra,$(CXXFLAGS)) -w

$(OBJDIR)/third_party/%.o: third_party/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_VENDOR) -MMD -MP -c $< -o $@

# Megabytes of byte arrays: no warning set, and no header deps to track.
$(FONT_GEN): $(FONT_FILES) tools/embed_fonts.py
	@mkdir -p $(dir $@)
	@python3 tools/embed_fonts.py $@ $(FONT_FILES)

$(FONT_OBJ): $(FONT_GEN)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_VENDOR) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

# Web build. Recursive so one Makefile carries both toolchains; emsdk has to be
# on PATH, i.e. `source ~/emsdk/emsdk_env.sh` first.
web:
	@command -v em++ >/dev/null 2>&1 || { \
	  echo "em++ not found. Run: source ~/emsdk/emsdk_env.sh"; exit 1; }
	@$(MAKE) PLATFORM=web $(WEB_TARGET)

ifeq ($(PLATFORM),web)
$(WEB_TARGET): $(OBJ) web/shell.html
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)
endif

# A page on file:// cannot fetch its own .wasm, so serving is part of running it.
web-run: web
	@echo "Serving http://localhost:8080/openboolcad.html - Ctrl+C to stop"
	@cd web && python3 -m http.server 8080

clean:
	rm -rf obj $(TARGET)

web-clean:
	rm -rf obj/web-debug obj/web-release \
	       web/openboolcad.html web/openboolcad.js web/openboolcad.wasm \
	       web/openboolcad.wasm.map

rebuild: clean all

-include $(DEP)
