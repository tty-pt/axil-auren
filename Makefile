uname := $(shell test "${cross}" = "" && uname || echo ${cross})
uname != test "${cross}" = "" && uname || echo ${cross}

all-MinGW64 := libaxil-auren
all-${uname} ?= libaxil-auren libren ren
all := ${all-${uname}}

INSTALL_BIN-MinGW64 :=
INSTALL_BIN-${uname} ?= ren auren-path auren-list
INSTALL_BIN := ${INSTALL_BIN-${uname}}

engine-test-obj-y :=
LDLIBS-engine-test := -laxil-auren -lsepal -lcorm -ljson-c -lpthread -lm
LDFLAGS-engine-test-Linux = -Wl,-rpath,${pwd}/lib

LDLIBS-auren-chat-Windows := -lws2_32

libaxil-auren-obj-y := src/libauren.o src/auren-engine.o src/auren-registry.o src/openai_embed.o src/openai_chat.o src/auren-collections.o src/systemone.o src/systemone-choice.o src/auren-upstream.o
libren-obj-y :=
ren-obj-y :=
libaxil-auren-obj-y-Linux := src/vulkan.o
libaxil-auren-obj-y-Darwin := src/metal.o

LDLIBS-libaxil-auren := -laxil -lxylem -lcorm -lsepal -lstele -ljson-c -lcurl \
	-lllama -lpthread -lm
LDLIBS-libren := -lxylem -lstele -lcorm -ldl -lmm -lm
LDLIBS-ren := -Llib -lren -lxylem -lstele -lcorm -ldl -lmm -lm
LDLIBS-libaxil-auren-Unix := -lggml -lggml-cpu -lggml-base
LDLIBS-libaxil-auren-Linux := -ldl -lstdc++ -lgomp -lvulkan -lggml-vulkan
LDLIBS-libaxil-auren-Darwin := -ldl -lc++ -lggml-metal -lggml-blas -lomp \
	-framework Foundation \
	-framework CoreFoundation \
	-framework IOKit \
	-framework Metal \
	-framework Accelerate
LDLIBS-libaxil-auren-OpenBSD := -lc++ -lc++abi
LDLIBS-libaxil-auren-Windows := -l:ggml.a -l:ggml-cpu.a -l:ggml-base.a -lstdc++ -lws2_32

llamacpp := submodules/llama.cpp/build

arch := $(shell uname -m)
arch != uname -m

distro := $(shell sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | tr -d '"')
distro != sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | tr -d '"' || true

COMPILER_C := $(shell if [ "${uname}" = "MinGW64" ]; then echo /mingw64/bin/gcc; elif clang --version >/dev/null 2>&1; then echo clang; else echo "${CC}"; fi)
COMPILER_C != if [ "${uname}" = "MinGW64" ]; then echo /mingw64/bin/gcc; elif clang --version >/dev/null 2>&1; then echo clang; else echo "${CC}"; fi

COMPILER_CXX := $(shell if [ "${uname}" = "MinGW64" ]; then echo /mingw64/bin/g++; elif clang++ --version >/dev/null 2>&1; then echo clang++; else echo "${CXX}"; fi)
COMPILER_CXX != if [ "${uname}" = "MinGW64" ]; then echo /mingw64/bin/g++; elif clang++ --version >/dev/null 2>&1; then echo clang++; else echo "${CXX}"; fi

has_vpdpbusd := $(shell if [ "${arch}" = "x86_64" ] || [ "${arch}" = "amd64" ]; then echo 'void test(void){ __asm__("{vex} vpdpbusd %ymm0, %ymm1, %ymm2"); }' | ${COMPILER_C} -c -x c - -o /dev/null 2>/dev/null && echo 1 || echo 0; else echo 1; fi)
has_vpdpbusd != if [ "${arch}" = "x86_64" ] || [ "${arch}" = "amd64" ]; then echo 'void test(void){ __asm__("{vex} vpdpbusd %ymm0, %ymm1, %ymm2"); }' | ${COMPILER_C} -c -x c - -o /dev/null 2>/dev/null && echo 1 || echo 0; else echo 1; fi

GGML_NATIVE-1 := ON
GGML_NATIVE-0 := OFF
GGML_NATIVE ?= ${GGML_NATIVE-${has_vpdpbusd}}

GGML_VNNI-ON :=
GGML_VNNI-OFF := -DGGML_AVX_VNNI=OFF -DGGML_AVX512_VNNI=OFF

SDK_VERSION := 1.4.328.1
SDK_URL := https://sdk.lunarg.com/sdk/download/${SDK_VERSION}/linux/vulkansdk-linux-${arch}-${SDK_VERSION}.tar.xz
vulkan := third_party/${SDK_VERSION}/${arch}

prefix-Darwin-arm64  := /opt/homebrew
prefix-Darwin-x86_64 := /usr/local
prefix-Darwin := ${prefix-Darwin-${arch}}
omp := ${prefix-Darwin}/Cellar/libomp/
omp-version := $(shell ls ${omp} 2>/dev/null | head -n 1)
omp-version != ls ${omp} 2>/dev/null | head -n 1 || true
omp := ${omp}/${omp-version}

CFLAGS := -g -I${llamacpp}/../include -I${llamacpp}/../ggml/include -I${vulkan}/include -I../libxylem/include -I../libstele/include -I../libcorm/include -I../libmm/include -I../libjarvis/include -Iinclude
CFLAGS-Darwin := -I${omp}/include

ggmlp := ${llamacpp}/ggml/src

LDFLAGS-libaxil-auren := -L${llamacpp}/src -L${ggmlp} -L${ggmlp}/ggml-cpu -L${ggmlp}/ggml-base
LDFLAGS-libaxil-auren-Linux := -Wl,--disable-new-dtags -Wl,-rpath,'$$ORIGIN'
LDFLAGS-libaxil-auren-Windows := -Wl,--export-all-symbols
LDFLAGS-Linux := -L${ggmlp}/ggml-vulkan -L${vulkan}/lib
LDFLAGS-libren := -L${CURDIR}/../libmm/lib -L${CURDIR}/../libxylem/lib -L${CURDIR}/../libstele/lib -L${CURDIR}/../libcorm/lib
LDFLAGS-libren-Linux := -Wl,-rpath,${CURDIR}/lib -Wl,-rpath,${CURDIR}/../libmm/lib -Wl,-rpath,${CURDIR}/../libxylem/lib -Wl,-rpath,${CURDIR}/../libstele/lib -Wl,-rpath,${CURDIR}/../libcorm/lib
LDFLAGS-ren := -L${CURDIR}/../libmm/lib -L${CURDIR}/../libxylem/lib -L${CURDIR}/../libstele/lib -L${CURDIR}/../libcorm/lib
LDFLAGS-ren-Linux := $(LDFLAGS-libren-Linux)
LDFLAGS-Darwin := -L${ggmlp}/ggml-metal -L${ggmlp}/ggml-blas -L${omp}/lib

CMAKE_FLAGS-Linux := -DGGML_VULKAN=ON
CMAKE_FLAGS-Darwin := -DGGML_METAL=ON
CMAKE_FLAGS-OpenBSD := -DGGML_VULKAN=OFF -DGGML_OPENMP=OFF
CMAKE_FLAGS-MinGW64 := -G "MSYS Makefiles" -DGGML_VULKAN=OFF -DGGML_OPENMP=OFF

WASM := auren.wasm
WASM_SRCS := npm/wasm/auren_wasm.c

third_party-Linux := ${vulkan}/include/shaderc/shaderc.h

include ./../mk/include.mk

WASM_CFLAGS += -Inpm/node_modules/@tty-pt/stele/include

wasm: ${wasmtarget}
	@cp -f ${wasmtarget} npm/dist/auren.wasm 2>/dev/null || true

extra-bin-MinGW64 :=
extra-bin-${uname} ?= bin/auren-path bin/auren-list
all: ${extra-bin-${uname}}

bin/auren-path: scripts/auren-path
	install -d bin
	install -m 755 scripts/auren-path $@

bin/auren-list: scripts/auren-list
	install -d bin
	install -m 755 scripts/auren-list $@

${DESTDIR}${PREFIX}/lib/pkgconfig/auren.pc: auren.pc
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 auren.pc $@

${DESTDIR}${PREFIX}/lib/pkgconfig/axil-auren.pc: axil-auren.pc
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 axil-auren.pc $@

install: ${DESTDIR}${PREFIX}/lib/pkgconfig/auren.pc ${DESTDIR}${PREFIX}/lib/pkgconfig/axil-auren.pc install-hook

.PHONY: install-hook
install-hook:
	@if [ "${uname}" = "MinGW64" ] && [ ! -f /mingw64/bin/libcrypto-3-x64.dll ]; then \
		curl -fsSL https://registry.npmjs.org/@tty-pt/axil/-/axil-1.8.4.tgz | tar -xzf - --strip-components=3 -C /mingw64/bin package/prebuilds/win32-x64/libcrypto-3-x64.dll package/prebuilds/win32-x64/libssl-3-x64.dll; \
	fi

src/libauren.o: $(llamacpp)/src/libllama.a

$(llamacpp)/src/libllama.a: ${llamacpp}/Makefile
	${MAKE} -C ${llamacpp} -j2 llama; \
	if [ -x "${llamacpp}/Release/vulkan-shaders-gen" ]; then \
		GLSLC_BIN=glslc; \
		if [ "${distro}" != "alpine" ] && [ -x "${vulkan}/bin/glslc" ]; then GLSLC_BIN="${pwd}/${vulkan}/bin/glslc"; fi; \
		sh scripts/vkgen-repair.sh ${llamacpp} "$$GLSLC_BIN"; \
		${MAKE} -C ${llamacpp} -j2 llama; \
	fi

$(llamacpp)/Makefile: ${third_party-${uname}}
	echo "[INFO] Using GGML_NATIVE=${GGML_NATIVE} (toolchain vpdpbusd support: ${has_vpdpbusd})"; \
	mkdir -p ${llamacpp} 2>/dev/null || true
	if [ "${uname}" = "OpenBSD" ]; then \
		if [ ! -x /usr/bin/c++ ] && [ -x /usr/bin/cc ]; then \
			ln -sf /usr/bin/cc /usr/bin/c++; \
			ln -sf /usr/bin/cc /usr/bin/clang++; \
			ln -sf /usr/bin/cc /usr/bin/clang; \
		fi; \
		if ! printf '#include <vector>\nint main(){std::vector<int> v;return (int)v.size();}\n' | c++ -x c++ - -o /tmp/cxx_probe 2>/dev/null; then \
			REL=$$(uname -r | tr -d .); \
			URL="$$(cat /etc/installurl)/$$(uname -r)/$$(uname -m)/comp$${REL}.tgz"; \
			echo "[INFO] Extracting OpenBSD comp$${REL}.tgz for C++ toolchain..."; \
			ftp -o - "$$URL" | tar -xzphf - -C /; \
		fi; \
		rm -f /tmp/cxx_probe; \
	fi; \
	VULKAN_INC_FLAG=""; \
	VULKAN_GLSLC_FLAG=""; \
	if [ -f "${vulkan}/include/vulkan/vulkan.h" ]; then \
		export VULKAN_SDK="${pwd}/${vulkan}"; \
		if [ "${distro}" != "alpine" ]; then \
			export PATH="$$VULKAN_SDK/bin:$$PATH"; \
			export LD_LIBRARY_PATH="$$VULKAN_SDK/lib:$$LD_LIBRARY_PATH"; \
		else \
			VULKAN_GLSLC_FLAG="-DVulkan_GLSLC_EXECUTABLE=/usr/bin/glslc"; \
		fi; \
		VULKAN_INC_FLAG="-DVulkan_INCLUDE_DIR=${pwd}/${vulkan}/include"; \
	fi; \
	export CC=${COMPILER_C} && \
	export CXX=${COMPILER_CXX} && \
	cd ${llamacpp} && \
	cmake .. ${CMAKE_FLAGS-${uname}} \
		-DCMAKE_INSTALL_PREFIX:PATH=${PREFIX}/share/auren \
		-DGGML_NATIVE=${GGML_NATIVE} \
		${GGML_VNNI-${GGML_NATIVE}} \
		-DLLAMA_CURL=OFF \
		-DLLAMA_BUILD_EXAMPLES=OFF \
		-DLLAMA_BUILD_TESTS=OFF \
		-DLLAMA_BUILD_SERVER=OFF \
		-DLLAMA_BUILD_TOOLS=OFF \
		-DBUILD_SHARED_LIBS=OFF \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		$$VULKAN_INC_FLAG $$VULKAN_GLSLC_FLAG

$(vulkan)/include/shaderc/shaderc.h:
	mkdir -p third_party || true
	(curl -fsSL "$(SDK_URL)" -o third_party/vulkan-sdk.tar.xz || wget -qO third_party/vulkan-sdk.tar.xz "$(SDK_URL)")
	tar -xf third_party/vulkan-sdk.tar.xz -C third_party

.PHONY: test npm-build npm-test
test:
	$(MAKE) -C tests run

npm-build:
	@echo "=== Building npm package @tty-pt/auren-client ==="

npm-test: npm-build
	@node --test npm/tests/auren-client.test.mjs

# ren modules
mods/fs/mod_fs.so: mods/fs/mod_fs.c include/ttypt/libren.h lib/libren.so
	@mkdir -p mods/fs
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/fs/mod_fs.c $(LDFLAGS) -Llib -lren -lxylem -lstele -lcorm -lmm -L${CURDIR}/../libmm/lib -Wl,-rpath,${CURDIR}/lib -Wl,-rpath,${CURDIR}/../libmm/lib

mods/bash/mod_bash.so: mods/bash/mod_bash.c include/ttypt/libren.h lib/libren.so
	@mkdir -p mods/bash
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/bash/mod_bash.c $(LDFLAGS) -Llib -lren -lxylem -lstele -lcorm -lmm -L${CURDIR}/../libmm/lib -Wl,-rpath,${CURDIR}/lib -Wl,-rpath,${CURDIR}/../libmm/lib

mods/mm/mod_mm.so: mods/mm/mod_mm.c include/ttypt/libren.h lib/libren.so
	@mkdir -p mods/mm
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/mm/mod_mm.c $(LDFLAGS) -Llib -lren -lxylem -lstele -lcorm -lmm -L${CURDIR}/../libmm/lib -Wl,-rpath,${CURDIR}/lib -Wl,-rpath,${CURDIR}/../libmm/lib

mods/jarvis/mod_jarvis.so: mods/jarvis/mod_jarvis.c include/ttypt/libren.h lib/libren.so
	@mkdir -p mods/jarvis
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/jarvis/mod_jarvis.c $(LDFLAGS) -Llib -lren -ljarvis -lxylem -lstele -lcorm -lmm -L${CURDIR}/../libjarvis/lib -L${CURDIR}/../libmm/lib -Wl,-rpath,${CURDIR}/lib -Wl,-rpath,${CURDIR}/../libjarvis/lib -Wl,-rpath,${CURDIR}/../libmm/lib

all: mods/fs/mod_fs.so mods/bash/mod_bash.so mods/mm/mod_mm.so mods/jarvis/mod_jarvis.so

install: install-ren-mods
.PHONY: install-ren-mods
install-ren-mods: lib/libren.so mods/fs/mod_fs.so mods/bash/mod_bash.so mods/mm/mod_mm.so mods/jarvis/mod_jarvis.so
	mkdir -p $(DESTDIR)$(PREFIX)/lib/ren
	install -m 755 mods/fs/mod_fs.so $(DESTDIR)$(PREFIX)/lib/ren/mod_fs.so
	install -m 755 mods/bash/mod_bash.so $(DESTDIR)$(PREFIX)/lib/ren/mod_bash.so
	install -m 755 mods/mm/mod_mm.so $(DESTDIR)$(PREFIX)/lib/ren/mod_mm.so
	install -m 755 mods/jarvis/mod_jarvis.so $(DESTDIR)$(PREFIX)/lib/ren/mod_jarvis.so

bin/ren: lib/libren.so
