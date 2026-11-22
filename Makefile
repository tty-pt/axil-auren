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
LDLIBS-libren := -lxylem -ldl -ljson-c -lpthread
LDLIBS-ren := -Llib -lren -lxylem -ldl -ljson-c -lpthread
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

sib_inc := $(shell for d in ../libxylem ../libstele ../libcorm ../libsepal ../axil; do [ -d "$$d/include" ] && echo "-I$$d/include"; done || true)
sib_inc != for d in ../libxylem ../libstele ../libcorm ../libsepal ../axil; do [ -d "$$d/include" ] && echo "-I$$d/include"; done || true
sib_lib := $(shell for d in ../libxylem ../libstele ../libcorm ../libsepal ../axil; do [ -d "$$d/lib" ] && echo "-L${CURDIR}/$$d/lib"; done || true)
sib_lib != for d in ../libxylem ../libstele ../libcorm ../libsepal ../axil; do [ -d "$$d/lib" ] && echo "-L${CURDIR}/$$d/lib"; done || true

CFLAGS := -g -Iinclude -I${llamacpp}/../include -I${llamacpp}/../ggml/include -I${vulkan}/include ${sib_inc}
CFLAGS-Darwin := -I${omp}/include

ggmlp := ${llamacpp}/ggml/src

LDFLAGS := ${sib_lib}
LDFLAGS-libaxil-auren := -L${llamacpp}/src -L${ggmlp} -L${ggmlp}/ggml-cpu -L${ggmlp}/ggml-base
LDFLAGS-libaxil-auren-Linux := -Wl,--disable-new-dtags -Wl,-rpath,'$$ORIGIN'
LDFLAGS-libaxil-auren-Windows := -Wl,--export-all-symbols
LDFLAGS-Linux := -L${ggmlp}/ggml-vulkan -L${vulkan}/lib
LDFLAGS-libren-Linux := -Wl,-rpath,'$$ORIGIN'
LDFLAGS-ren-Linux := -Wl,-rpath,'$$ORIGIN/../lib' -Wl,-rpath,'$$ORIGIN'
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

install: ${DESTDIR}${PREFIX}/lib/pkgconfig/auren.pc ${DESTDIR}${PREFIX}/lib/pkgconfig/axil-auren.pc install-hook install-services

.PHONY: install-hook install-services
install-services:
	@if [ "${uname}" = "Linux" ] || [ -d /run/systemd/system ]; then \
		echo "[INFO] Installing systemd services for axil-auren..."; \
		install -d ${DESTDIR}${PREFIX}/lib/systemd/user; \
		sed 's|@PREFIX@|${PREFIX}|g' services/systemd/axil-auren.service > ${DESTDIR}${PREFIX}/lib/systemd/user/axil-auren.service; \
		if [ -d $(HOME)/.config/systemd/user ] || [ -n "$$(echo "${PREFIX}" | grep -E '^($(HOME)|~)')" ]; then \
			mkdir -p $(HOME)/.config/systemd/user; \
			sed 's|@PREFIX@|${PREFIX}|g' services/systemd/axil-auren.service > $(HOME)/.config/systemd/user/axil-auren.service; \
		fi; \
	fi
	@if [ -d /etc/init.d ] || [ -x /sbin/openrc-run ] || [ "${distro}" = "alpine" ] || [ "${distro}" = "gentoo" ]; then \
		echo "[INFO] Installing OpenRC service for axil-auren..."; \
		install -d ${DESTDIR}${PREFIX}/share/examples/openrc; \
		sed 's|@PREFIX@|${PREFIX}|g' services/openrc/axil-auren > ${DESTDIR}${PREFIX}/share/examples/openrc/axil-auren; \
		chmod 755 ${DESTDIR}${PREFIX}/share/examples/openrc/axil-auren; \
		if [ -w /etc/init.d ]; then \
			install -m 755 ${DESTDIR}${PREFIX}/share/examples/openrc/axil-auren /etc/init.d/axil-auren; \
		fi; \
	fi
	@if [ "${uname}" = "FreeBSD" ] || [ "${uname}" = "NetBSD" ] || [ "${uname}" = "DragonFly" ]; then \
		echo "[INFO] Installing FreeBSD rc.d service for axil-auren..."; \
		install -d ${DESTDIR}${PREFIX}/etc/rc.d ${DESTDIR}${PREFIX}/share/examples/freebsd; \
		sed 's|@PREFIX@|${PREFIX}|g' services/freebsd/axil-auren > ${DESTDIR}${PREFIX}/share/examples/freebsd/axil-auren; \
		chmod 755 ${DESTDIR}${PREFIX}/share/examples/freebsd/axil-auren; \
		sed 's|@PREFIX@|${PREFIX}|g' services/freebsd/axil-auren > ${DESTDIR}${PREFIX}/etc/rc.d/axil_auren; \
		chmod 755 ${DESTDIR}${PREFIX}/etc/rc.d/axil_auren; \
	elif [ "${uname}" = "OpenBSD" ]; then \
		echo "[INFO] Installing OpenBSD rc.d service for axil-auren..."; \
		install -d ${DESTDIR}${PREFIX}/share/examples/rc.d; \
		sed 's|@PREFIX@|${PREFIX}|g' services/rc.d/axil_auren > ${DESTDIR}${PREFIX}/share/examples/rc.d/axil_auren; \
		chmod 755 ${DESTDIR}${PREFIX}/share/examples/rc.d/axil_auren; \
		if [ -w /etc/rc.d ]; then \
			install -m 755 ${DESTDIR}${PREFIX}/share/examples/rc.d/axil_auren /etc/rc.d/axil_auren; \
		fi; \
	elif [ "${uname}" = "Darwin" ]; then \
		echo "[INFO] Installing launchd service for axil-auren..."; \
		install -d ${DESTDIR}${PREFIX}/share/launchd; \
		sed 's|@PREFIX@|${PREFIX}|g' services/launchd/pt.tty.axil-auren.plist > ${DESTDIR}${PREFIX}/share/launchd/pt.tty.axil-auren.plist; \
		if [ -d $(HOME)/Library/LaunchAgents ]; then \
			sed 's|@PREFIX@|${PREFIX}|g' services/launchd/pt.tty.axil-auren.plist > $(HOME)/Library/LaunchAgents/pt.tty.axil-auren.plist; \
		fi; \
	elif [ "${SYS}" = "Windows" ] || [ "${uname}" = "MinGW64" ] || [ "${uname}" = "Msys" ] || [ "${uname}" = "MingW" ]; then \
		echo "[INFO] Installing Windows services for axil-auren..."; \
		install -d ${DESTDIR}${PREFIX}/share/windows; \
		sed 's|@PREFIX@|${PREFIX}|g' services/windows/axil-auren.xml > ${DESTDIR}${PREFIX}/share/windows/axil-auren.xml; \
		install -m 755 services/windows/install-service.ps1 ${DESTDIR}${PREFIX}/share/windows/install-axil-auren-service.ps1; \
	fi
	@if [ -f services/xdg/axil-auren.desktop ]; then \
		install -d ${DESTDIR}${PREFIX}/etc/xdg/autostart ${DESTDIR}${PREFIX}/share/applications; \
		sed 's|@PREFIX@|${PREFIX}|g' services/xdg/axil-auren.desktop > ${DESTDIR}${PREFIX}/etc/xdg/autostart/axil-auren.desktop; \
		sed 's|@PREFIX@|${PREFIX}|g' services/xdg/axil-auren.desktop > ${DESTDIR}${PREFIX}/share/applications/axil-auren.desktop; \
		if [ -d $(HOME)/.config/autostart ] || [ -n "$$(echo "${PREFIX}" | grep -E '^($(HOME)|~)')" ]; then \
			mkdir -p $(HOME)/.config/autostart; \
			sed 's|@PREFIX@|${PREFIX}|g' services/xdg/axil-auren.desktop > $(HOME)/.config/autostart/axil-auren.desktop; \
		fi; \
	fi


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
mm_prefix := $(shell for d in ../libmm ${PREFIX} ${HOME}/.local /usr/local /usr; do [ -f "$$d/include/ttypt/mm.h" ] && { [ -f "$$d/lib/libmm.${SO}" ] || [ -f "$$d/lib64/libmm.${SO}" ]; } && echo "$$d" && break; done || true)
mm_prefix != for d in ../libmm ${PREFIX} ${HOME}/.local /usr/local /usr; do [ -f "$$d/include/ttypt/mm.h" ] && { [ -f "$$d/lib/libmm.${SO}" ] || [ -f "$$d/lib64/libmm.${SO}" ]; } && echo "$$d" && break; done || true

jarvis_prefix := $(shell for d in ../libjarvis ${PREFIX} ${HOME}/.local /usr/local /usr; do [ -f "$$d/include/ttypt/jarvis.h" ] && { [ -f "$$d/lib/libjarvis.${SO}" ] || [ -f "$$d/lib64/libjarvis.${SO}" ]; } && echo "$$d" && break; done || true)
jarvis_prefix != for d in ../libjarvis ${PREFIX} ${HOME}/.local /usr/local /usr; do [ -f "$$d/include/ttypt/jarvis.h" ] && { [ -f "$$d/lib/libjarvis.${SO}" ] || [ -f "$$d/lib64/libjarvis.${SO}" ]; } && echo "$$d" && break; done || true

mm_rpath_dev-../libmm := -Wl,-rpath,'$$ORIGIN/../../../libmm/lib'
jarvis_rpath_dev-../libjarvis := -Wl,-rpath,'$$ORIGIN/../../../libjarvis/lib'
mm_lib_flag := $(shell [ -n "${mm_prefix}" ] && echo "-L${mm_prefix}/lib" || true)
mm_lib_flag != [ -n "${mm_prefix}" ] && echo "-L${mm_prefix}/lib" || true

REN_CORE_MODS := mods/fs/mod_fs.so mods/bash/mod_bash.so
REN_OPT_MODS := $(shell [ -n "${mm_prefix}" ] && echo mods/mm/mod_mm.so; [ -n "${jarvis_prefix}" ] && echo mods/jarvis/mod_jarvis.so)
REN_OPT_MODS != [ -n "${mm_prefix}" ] && echo mods/mm/mod_mm.so; [ -n "${jarvis_prefix}" ] && echo mods/jarvis/mod_jarvis.so
REN_MODS := ${REN_CORE_MODS} ${REN_OPT_MODS}

mods/fs/mod_fs.so: mods/fs/mod_fs.c include/ttypt/libren.h lib/libren.so
	@mkdir -p mods/fs
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/fs/mod_fs.c $(LDFLAGS) -Llib -lren -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN/../../lib'

mods/bash/mod_bash.so: mods/bash/mod_bash.c include/ttypt/libren.h lib/libren.so
	@mkdir -p mods/bash
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/bash/mod_bash.c $(LDFLAGS) -Llib -lren -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN/../../lib'

mods/mm/mod_mm.so: mods/mm/mod_mm.c include/ttypt/libren.h lib/libren.so
	@mkdir -p mods/mm
	$(CC) $(CFLAGS) -I${mm_prefix}/include -fPIC -shared -o $@ mods/mm/mod_mm.c $(LDFLAGS) -Llib -lren -L${mm_prefix}/lib -lmm -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN/../../lib' ${mm_rpath_dev-${mm_prefix}}

mods/jarvis/mod_jarvis.so: mods/jarvis/mod_jarvis.c include/ttypt/libren.h lib/libren.so
	@mkdir -p mods/jarvis
	$(CC) $(CFLAGS) -I${jarvis_prefix}/include -fPIC -shared -o $@ mods/jarvis/mod_jarvis.c $(LDFLAGS) -Llib -lren -L${jarvis_prefix}/lib ${mm_lib_flag} -ljarvis -lxylem -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN/../../lib' ${jarvis_rpath_dev-${jarvis_prefix}} ${mm_rpath_dev-${mm_prefix}}

all: ${REN_MODS}

install: install-ren-mods
.PHONY: install-ren-mods
install-ren-mods: lib/libren.so ${REN_MODS}
	mkdir -p $(DESTDIR)$(PREFIX)/lib/ren
	for m in ${REN_MODS}; do install -m 755 "$$m" "$(DESTDIR)$(PREFIX)/lib/ren/$$(basename "$$m")"; done

bin/ren: lib/libren.so
