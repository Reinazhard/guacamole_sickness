# SPDX-License-Identifier: GPL-2.0-only
#
# fixdep: used to generate dependency information during build process

hostprogs-always-y	+= fixdep depcheck

# depcheck uses the host-side hash and hashtable helpers. This tree does not put
# tools/include on the default host include path the way newer ones do, and the
# plain <hash.h> spelling depcheck.c uses needs tools/include/linux, so add it
# with -idirafter: it must not shadow the system <string.h> with the kernel's.
HOSTCFLAGS_depcheck.o	+= -idirafter $(srctree)/tools/include/linux -I$(srctree)/tools/include

# randstruct: the seed is needed before building the gcc-plugin or
# before running a Clang kernel build.
gen-randstruct-seed	:= $(srctree)/scripts/gen-randstruct-seed.sh
quiet_cmd_create_randstruct_seed = GENSEED $@
cmd_create_randstruct_seed = \
	$(CONFIG_SHELL) $(gen-randstruct-seed) \
		$@ $(objtree)/include/generated/randstruct_hash.h
$(obj)/randstruct.seed: $(gen-randstruct-seed) FORCE
	$(call if_changed,create_randstruct_seed)
always-$(CONFIG_RANDSTRUCT) += randstruct.seed
