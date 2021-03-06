/*
 * Copyright (c) 2016, Linaro Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <kernel/dt.h>
#include <kernel/linker.h>
#include <libfdt.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <string.h>
#include <trace.h>

const struct dt_driver *dt_find_compatible_driver(const void *fdt, int offs)
{
	const struct dt_device_match *dm;
	const struct dt_driver *drv;

	for_each_dt_driver(drv)
		for (dm = drv->match_table; dm; dm++)
			if (!fdt_node_check_compatible(fdt, offs,
						       dm->compatible))
				return drv;

	return NULL;
}

const struct dt_driver *__dt_driver_start(void)
{
	return &__rodata_dtdrv_start;
}

const struct dt_driver *__dt_driver_end(void)
{
	return &__rodata_dtdrv_end;
}

bool dt_have_prop(const void *fdt, int offs, const char *propname)
{
	const void *prop;

	prop = fdt_getprop(fdt, offs, propname, NULL);

	return prop;
}

int dt_map_dev(const void *fdt, int offs, vaddr_t *base, size_t *size)
{
	enum teecore_memtypes mtype;
	paddr_t pbase;
	vaddr_t vbase;
	ssize_t sz;
	int st;

	assert(cpu_mmu_enabled());

	st = _fdt_get_status(fdt, offs);
	if (st == DT_STATUS_DISABLED)
		return -1;

	pbase = _fdt_reg_base_address(fdt, offs);
	if (pbase == (paddr_t)-1)
		return -1;
	sz = _fdt_reg_size(fdt, offs);
	if (sz < 0)
		return -1;

	if ((st & DT_STATUS_OK_SEC) && !(st & DT_STATUS_OK_NSEC))
		mtype = MEM_AREA_IO_SEC;
	else
		mtype = MEM_AREA_IO_NSEC;

	/* Check if we have a mapping, create one if needed */
	if (!core_mmu_add_mapping(mtype, pbase, sz)) {
		EMSG("Failed to map %zu bytes at PA 0x%"PRIxPA,
		     (size_t)sz, pbase);
		return -1;
	}
	vbase = (vaddr_t)phys_to_virt(pbase, mtype);
	if (!vbase) {
		EMSG("Failed to get VA for PA 0x%"PRIxPA, pbase);
		return -1;
	}

	*base = vbase;
	*size = sz;
	return 0;
}

/* Read a physical address (n=1 or 2 cells) */
static paddr_t _fdt_read_paddr(const uint32_t *cell, int n)
{
	paddr_t addr;

	if (n < 1 || n > 2)
		goto bad;

	addr = fdt32_to_cpu(*cell);
	cell++;
	if (n == 2) {
#ifdef ARM32
		if (addr) {
			/* High order 32 bits can't be nonzero */
			goto bad;
		}
		addr = fdt32_to_cpu(*cell);
#else
		addr = (addr << 32) | fdt32_to_cpu(*cell);
#endif
	}

	if (!addr)
		goto bad;

	return addr;
bad:
	return (paddr_t)-1;

}

paddr_t _fdt_reg_base_address(const void *fdt, int offs)
{
	const void *reg;
	int ncells;
	int len;
	int parent;

	parent = fdt_parent_offset(fdt, offs);
	if (parent < 0)
		return (paddr_t)-1;

	reg = fdt_getprop(fdt, offs, "reg", &len);
	if (!reg)
		return (paddr_t)-1;

	ncells = fdt_address_cells(fdt, parent);
	if (ncells < 0)
		return (paddr_t)-1;

	return _fdt_read_paddr(reg, ncells);
}

ssize_t _fdt_reg_size(const void *fdt, int offs)
{
	const uint32_t *reg;
	uint32_t sz;
	int n;
	int len;
	int parent;

	parent = fdt_parent_offset(fdt, offs);
	if (parent < 0)
		return (paddr_t)-1;

	reg = (const uint32_t *)fdt_getprop(fdt, offs, "reg", &len);
	if (!reg)
		return -1;

	n = fdt_address_cells(fdt, parent);
	if (n < 1 || n > 2)
		return -1;

	reg += n;

	n = fdt_size_cells(fdt, parent);
	if (n < 1 || n > 2)
		return -1;

	sz = fdt32_to_cpu(*reg);
	if (n == 2) {
		if (sz)
			return -1;
		reg++;
		sz = fdt32_to_cpu(*reg);
	}

	return sz;
}

static bool is_okay(const char *st, int len)
{
	return !strncmp(st, "ok", len) || !strncmp(st, "okay", len);
}

int _fdt_get_status(const void *fdt, int offs)
{
	const char *prop;
	int st = 0;
	int len;

	prop = fdt_getprop(fdt, offs, "status", &len);
	if (!prop || is_okay(prop, len)) {
		/* If status is not specified, it defaults to "okay" */
		st |= DT_STATUS_OK_NSEC;
	}

	prop = fdt_getprop(fdt, offs, "secure-status", &len);
	if (!prop) {
		/*
		 * When secure-status is not specified it defaults to the same
		 * value as status
		 */
		if (st & DT_STATUS_OK_NSEC)
			st |= DT_STATUS_OK_SEC;
	} else {
		if (is_okay(prop, len))
			st |= DT_STATUS_OK_SEC;
	}

	return st;
}
