/*
 * Copyright (c) 2013 Rob Clark <robdclark@gmail.cabelsom>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

/*
 * PM4/micro-engine disassembler:
 */

typedef struct PACKED {
	uint32_t d0, d1, d2;
} pm4_instr_t;

static uint32_t label_offsets[0x512];
static int num_label_offsets;

static int label_idx(uint32_t offset, int create)
{
	int i;
	for (i = 0; i < num_label_offsets; i++)
		if (offset == label_offsets[i])
			return i;
	if (!create)
		return -1;
	label_offsets[i] = offset;
	num_label_offsets = i+1;
	return i;
}

static struct {
	uint32_t offset;
	uint32_t num_jump_labels;
	uint32_t jump_labels[256];
} jump_labels[1024];
int num_jump_labels;

static const char *jump_label_names[4096] = {
		[16] = "CP_NOP",
		[31] = "CP_RUN_OPENCL",
		[33] = "CP_REG_RMW",
		[34] = "CP_DRAW_INDX",
		[35] = "CP_VIZ_QUERY",
		[37] = "CP_SET_STATE",
		[38] = "CP_WAIT_FOR_IDLE",
		[39] = "CP_IM_LOAD",
		[43] = "CP_IM_LOAD_IMMEDIATE",
		[44] = "CP_IM_STORE",
		[45] = "CP_SET_CONSTANT",
		[46] = "CP_LOAD_CONSTANT_CONTEXT",
		[47] = "CP_SET_BIN_DATA",
		[48] = "CP_LOAD_STATE",
		[50] = "CP_COND_INDIRECT_BUFFER_PFD",
		[52] = "CP_DRAW_INDX_BIN",
		[53] = "CP_DRAW_INDX_2_BIN",
		[54] = "CP_DRAW_INDX_2",
		[55] = "CP_INDIRECT_BUFFER_PFD",
		[58] = "CP_COND_INDIRECT_BUFFER_PFE",
		[59] = "CP_INVALIDATE_STATE",
		[60] = "CP_WAIT_REG_MEM",
		[61] = "CP_MEM_WRITE",
		[62] = "CP_REG_TO_MEM",
		[63] = "CP_INDIRECT_BUFFER",
		[64] = "CP_INTERRUPT",
		[68] = "CP_COND_EXEC",
		[69] = "CP_COND_WRITE",
		[70] = "CP_EVENT_WRITE",
		[72] = "CP_ME_INIT",
		[74] = "CP_SET_SHADER_BASES",
		[75] = "CP_SET_DRAW_INIT_FLAGS",  // CP_SET_BIN_BASE_OFFSET on a20x
		[76] = "CP_SET_BIN",
		[79] = "CP_MEM_WRITE_CNTR",
		[80] = "CP_SET_BIN_MASK",
		[81] = "CP_SET_BIN_SELECT",
		[82] = "CP_WAIT_REG_EQ",
		[83] = "CP_WAT_REG_GTE",
		[88] = "CP_EVENT_WRITE_SHD",
		[89] = "CP_EVENT_WRITE_CFL",
		[91] = "CP_EVENT_WRITE_ZPD",
		[92] = "CP_WAIT_UNTIL_READ",
		[93] = "CP_WAIT_IB_PFD_COMPLETE",
		[94] = "CP_CONTEXT_UPDATE",
		[95] = "CP_SET_PROTECTED_MODE",
};

/*
type3 pkt's that we know are sent by user/kernel:

[robclark@thunkpad:~/src/freedreno/dumps/a220]$ for f in *.rd; do cffdump --summary $f | grep opcode | awk '{print $2}'; done | sort | uniq
CP_DRAW_INDX
CP_EVENT_WRITE
CP_IM_LOAD_IMMEDIATE
CP_INDIRECT_BUFFER
CP_INVALIDATE_STATE
CP_MEM_WRITE
CP_NOP
CP_SET_CONSTANT
CP_SET_DRAW_INIT_FLAGS
CP_SET_SHADER_BASES
CP_WAIT_FOR_IDLE
CP_WAIT_REG_EQ
----
CP_REG_TO_MEM
CP_CONTEXT_UPDATE  (#ifdef CONFIG_MSM_KGSL_DISABLE_SHADOW_WRITES)
CP_LOAD_CONSTANT_CONTEXT
CP_IM_LOAD
CP_REG_RMW
CP_IM_STORE
CP_ME_INIT
CP_INTERRUPT


[robclark@thunkpad:~/src/freedreno/dumps/a320]$ for f in *.rd; do cffdump --summary $f | grep opcode | awk '{print $2}'; done | sort | uniq
CP_DRAW_INDX
CP_EVENT_WRITE
CP_INDIRECT_BUFFER
CP_INVALIDATE_STATE
CP_LOAD_STATE
CP_MEM_WRITE
CP_NOP
CP_REG_RMW
CP_SET_BIN
CP_SET_BIN_DATA
CP_WAIT_FOR_IDLE
----
CP_REG_TO_MEM
CP_LOAD_CONSTANT_CONTEXT
CP_COND_EXEC
CP_SET_CONSTANT
CP_DRAW_INDX_2
CP_CONTEXT_UPDATE
CP_ME_INIT
CP_INTERRUPT

 */
static int gpuver;

static const struct {
	int a2xx;
	int a3xx;
	const char *name;
} cp_info[4096] = {
		[16] = { 1, 1, "CP_NOP" },
		[33] = { 1, 1, "CP_REG_RMW" },
		[34] = { 1, 1, "CP_DRAW_INDX" },
		[35] = { 0, 0, "CP_VIZ_QUERY" },
		[37] = { 0, 0, "CP_SET_STATE" },
		[38] = { 1, 1, "CP_WAIT_FOR_IDLE" },
		[39] = { 1, 0, "CP_IM_LOAD" },
		[43] = { 1, 0, "CP_IM_LOAD_IMMEDIATE" },
		[44] = { 1, 0, "CP_IM_STORE" },
		[45] = { 1, 1, "CP_SET_CONSTANT" },
		[46] = { 1, 1, "CP_LOAD_CONSTANT_CONTEXT" },
		[47] = { 0, 1, "CP_SET_BIN_DATA" },
		[48] = { 0, 1, "CP_LOAD_STATE" },
		[50] = { 0, 0, "CP_COND_INDIRECT_BUFFER_PFD" },
		[52] = { 0, 0, "CP_DRAW_INDX_BIN" },
		[53] = { 0, 0, "CP_DRAW_INDX_2_BIN" },
		[54] = { 0, 1, "CP_DRAW_INDX_2" },
		[55] = { 0, 0, "CP_INDIRECT_BUFFER_PFD" },
		[58] = { 0, 0, "CP_COND_INDIRECT_BUFFER_PFE" },
		[59] = { 1, 1, "CP_INVALIDATE_STATE" },
		[60] = { 0, 0, "CP_WAIT_REG_MEM" },
		[61] = { 1, 1, "CP_MEM_WRITE" },
		[62] = { 1, 1, "CP_REG_TO_MEM" },
		[63] = { 1, 1, "CP_INDIRECT_BUFFER" },
		[64] = { 1, 1, "CP_INTERRUPT" },
		[68] = { 0, 1, "CP_COND_EXEC" },
		[69] = { 0, 0, "CP_COND_WRITE" },
		[70] = { 1, 1, "CP_EVENT_WRITE" },
		[72] = { 1, 1, "CP_ME_INIT" },
		[74] = { 1, 0, "CP_SET_SHADER_BASES" },
		[75] = { 1, 0, "CP_SET_DRAW_INIT_FLAGS" },  // CP_SET_BIN_BASE_OFFSET on a20x
		[76] = { 0, 1, "CP_SET_BIN" },
		[79] = { 0, 0, "CP_MEM_WRITE_CNTR" },
		[80] = { 0, 0, "CP_SET_BIN_MASK" },
		[81] = { 0, 0, "CP_SET_BIN_SELECT" },
		[82] = { 1, 0, "CP_WAIT_REG_EQ" },
		[83] = { 0, 0, "CP_WAT_REG_GTE" },
		[88] = { 0, 0, "CP_EVENT_WRITE_SHD" },
		[89] = { 0, 0, "CP_EVENT_WRITE_CFL" },
		[91] = { 0, 0, "CP_EVENT_WRITE_ZPD" },
		[92] = { 0, 0, "CP_WAIT_UNTIL_READ" },
		[93] = { 0, 0, "CP_WAIT_IB_PFD_COMPLETE" },
		[94] = { 3, 1, "CP_CONTEXT_UPDATE" },  // maybe optional on a2xx depend on CONFIG_MSM_KGSL_DISABLE_SHADOW_WRITES
		[95] = { 0, 0, "CP_SET_PROTECTED_MODE" },  // XXX check, probably used on both a3xx and a2xx..
};

#define CP_NOP 16
/* seems to start in same place on all fw files I have.. but probably need
 * to find a better way to figure this out:
 */
#define JUMP_TABLE_START (ninstr - 0x20)

static void add_jump_table_entry(uint32_t n, uint32_t offset)
{
	int i;

	offset &= 0x7ff;

	if (n > 128) /* can't possibly be a PM4 PKT3.. */
		return;

	for (i = 0; i < num_jump_labels; i++)
		if (jump_labels[i].offset == offset)
			goto add_label;

	num_jump_labels = i + 1;
	jump_labels[i].offset = offset;
	jump_labels[i].num_jump_labels = 0;

add_label:
	jump_labels[i].jump_labels[jump_labels[i].num_jump_labels++] = n;
	assert(jump_labels[i].num_jump_labels < 256);
//printf("add %d -> %04x (%d)\n", n, offset, i);

}

static int get_jump_table_entry(uint32_t offset)
{
	int i;

	offset &= 0x7ff;

	for (i = 0; i < num_jump_labels; i++)
		if (jump_labels[i].offset == offset)
			return i;

	return -1;
}

static void disasm_pm4(uint32_t *buf, int sizedwords)
{
	int i, ninstr = (sizedwords - 1) / 3;
	pm4_instr_t *instrs = (pm4_instr_t *)&buf[1];

	printf("Version: %08x\n", buf[0]);

	num_label_offsets = 0;
	num_jump_labels = 0;

	/* figure out the branch targets: */
if (1) {
	for (i = 0; i < ninstr; i++) {
		pm4_instr_t *instr = &instrs[i];

		if (i >= JUMP_TABLE_START) {
			int base = (i - JUMP_TABLE_START) * 4; // + CP_NOP;
			add_jump_table_entry(base + 2, instr->d0 >> 16);
			add_jump_table_entry(base + 3, instr->d0 & 0xffff);
			add_jump_table_entry(base + 0, instr->d1 >> 16);
			add_jump_table_entry(base + 1, instr->d1 & 0xffff);
		} else if (instr->d1 & 0x00400000) {
			label_idx(instr->d2, 1);
		}
	}

	for (i = 0; i < ninstr; i++) {
		pm4_instr_t *instr = &instrs[i];
		int idx = label_idx(i, 0);
		int jump_label_idx = get_jump_table_entry(i);

		if (jump_label_idx >= 0) {
			int j;
			for (j = 0; j < jump_labels[jump_label_idx].num_jump_labels; j++) {
				uint32_t jump_label = jump_labels[jump_label_idx].jump_labels[j];
				if (jump_label_names[jump_label]) {
					printf("; %s\n", jump_label_names[jump_label]);
				} else {
					printf("; #%d\n", jump_label);
				}
			}
		}

		printf("%04x:", i);
		if (idx >= 0) {
			printf(" l%02d:", idx);
		} else {
			printf("     ");
		}
		printf("\t%08x %08x %08x", instr->d0, instr->d1, instr->d2);
		if (instr->d1 & 0x00400000)
			printf("\t; branch #l%02d", label_idx(instr->d2, 0));
		printf("\n");
	}
}

	/* lets try dumping jump table in various ways and see if we find a
	 * way to make things line up:
	 */
#if 1
	for (i = ninstr - 1; i >= JUMP_TABLE_START; i--) {
		pm4_instr_t *instr = &instrs[i];
		int base = ((ninstr - i) * 4) - 1;

		void print_entry(int n, uint32_t offset)
		{
			int required = (gpuver == 200) ? cp_info[n].a2xx : cp_info[n].a3xx;

			offset &= 0x7ff;

			if (required && (offset == 0x0002))
				printf("XXX   ");
			else
				printf("      ");

			if (offset == 0x0002)
				printf("%d  ....  ", required);
			else
				printf("%d  %04x  ", required, offset);

			if (cp_info[n].name && required)
				printf("%s\n", cp_info[n].name);
			else
				printf("#%d\n", n);
		}

//		print_entry(base - 4, instr->d0 >> 16);
//		print_entry(base - 3, instr->d0 & 0xffff);
//		print_entry(base - 2, instr->d1 >> 16);
//		print_entry(base - 1, instr->d1 & 0xffff);

//		print_entry(base - 4, instr->d0 & 0xffff);
//		print_entry(base - 3, instr->d0 >> 16);
//		print_entry(base - 2, instr->d1 & 0xffff);
//		print_entry(base - 1, instr->d1 >> 16);

		print_entry(base - 4, instr->d1 & 0xffff);
		print_entry(base - 3, instr->d1 >> 16);
		print_entry(base - 2, instr->d0 & 0xffff);
		print_entry(base - 1, instr->d0 >> 16);
	}
#endif
}

/*
 * prefetch-parser disassembler:
 */

static void disasm_pfp(uint32_t *buf, int sizedwords)
{
	struct {
		uint16_t offset;
		uint16_t label;
	} *table_entries = (void *)&buf[0x101];
	int num_table_entries = sizedwords - 0x101;
	int i, j;

	assert(sizedwords >= 0x101);

	for (i = 0; i < 0x101; i++) {
		for (j = 0; j < num_table_entries; j++) {
			if (table_entries[j].offset == i) {
				int n = table_entries[j].label;
				if (jump_label_names[n]) {
					printf("; %s\n", jump_label_names[n]);
				} else {
					printf("; #%d\n", n);
				}
				break;
			}
		}
		printf("%04x: %08x\n", i, buf[i]);
	}

	for (i = 0; i < num_table_entries; i++) {
		int n = table_entries[i].label;
		printf("%04x: %04x %04x", 0x101 + i, n, table_entries[i].offset);
		if (jump_label_names[n]) {
			printf("  ; %s", jump_label_names[n]);
		}
		printf("\n");
	}
}

#define CHUNKSIZE 4096

static char * readfile(const char *path, int *sz)
{
	char *buf = NULL;
	int fd, ret, n = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;

	while (1) {
		buf = realloc(buf, n + CHUNKSIZE);
		ret = read(fd, buf + n, CHUNKSIZE);
		if (ret < 0) {
			free(buf);
			*sz = 0;
			return NULL;
		} else if (ret < CHUNKSIZE) {
			n += ret;
			*sz = n;
			return buf;
		} else {
			n += CHUNKSIZE;
		}
	}
}

int main(int argc, char **argv)
{
	void *buf;
	char *file;
	int sz;

	if (argc != 2) {
		printf("usage: fwdump <pm4.fw>\n");
		return -1;
	}

	file = argv[1];

	if (strstr(file, "a300")) {
		printf("matching a3xx\n");
		gpuver = 300;
	} else {
		printf("matching a2xx\n");
		gpuver = 200;
	}

	buf = readfile(file, &sz);

	if (strstr(file, "pm4")) {
		printf("Disassembling PM4 %s:\n", file);
		disasm_pm4(buf, sz/4);
	} else if (strstr(file, "pfp")) {
		printf("Disassembling PFP %s:\n", file);
		disasm_pfp(buf, sz/4);
	}

	return 0;
}
