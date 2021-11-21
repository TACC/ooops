#ifndef __HOOK_INT
#define __HOOK_INT

#include "udis86.h"

// The max number of shared objects we intercept. 
#define MAX_MODULE	(16)

// The max number of hooks we intercept. 
#define MAX_PATCH	(48)

// The max length of shared objects' path
#define MAX_LEN_PATH_NAME	(256)

// The max length of names of the functions to intercept
#define MAX_LEN_FUNC_NAME	(128)

// The minimal memory size we need to allocate to hold trampolines. 
#define MIN_MEM_SIZE	(0x1000)

// The max range of a signed integer can represent is 0x7FFFFFFF. We decrease it 
// a little bit to be safe. 
#define NULL_RIP_VAR_OFFSET	(0x7FF00000)

// The length of instructions to jmp to a new function is 14 bytes. 
// ff 25 00 00 00 00       jmp    QWORD PTR [rip+0x0]     6 bytes
// 8 bytes for the address of new function. 
#define BOUNCE_CODE_LEN	(14)

// The relative offset of new function address in bouncing code. It is after 
// jmp    QWORD PTR [rip+0x0]
#define OFFSET_NEW_FUNC_ADDR	(6)

// The max number of bytes to disassemble for the entry code of orginal functions with udis86
#define MAX_LEN_TO_DISASSEMBLE	(24)

// The max length of an instruction
#define MAX_INSN_LEN (15)

// The length of jmp instruction we use. 
#define JMP_INSTRCTION_LEN (5)

// The max length of bytes to hold the instruments to call original function. 
// 1) saved instruction
// 2) jump to resuming address
#define MAX_TRAMPOLINE_LEN	((MAX_INSN_LEN) + (JMP_INSTRCTION_LEN))

typedef struct	{
	char module_name[MAX_LEN_PATH_NAME];
	unsigned long int module_base_addr;
	char func_name_list[MAX_PATCH][MAX_LEN_FUNC_NAME];
	int is_patch_disabled[MAX_PATCH];
	void* old_func_addr_list[MAX_PATCH];
	void *old_func_addr_min, *old_func_addr_max;
	long int *ptr_old_func_add_list[MAX_PATCH];
	long int old_func_len_list[MAX_PATCH];
	void* new_func_addr_list[MAX_PATCH];
	int num_hook;
	// which patch memory block
	int idx_patch_blk;
}MODULE_PATCH_INFO, *PMODULE_PATCH_INFO;

typedef struct {
	// save the orginal function entry code and jump instrument
	unsigned char trampoline[MAX_TRAMPOLINE_LEN];
	// the code can jmp to my hook function. +3 for padding
	unsigned char bounce[BOUNCE_CODE_LEN+2];
	// to save 5 bytes of the entry instruction of original function
	char org_code[12];
	// the address of orginal function
	void *addr_org_func;
	// the number of bytes copied of the entry instructions of the original function. Needed to remove hook. 
	int saved_code_len;
	// the offset of rip addressed variable. Relative address has to be corrected when copied into trampoline from original address
	int  offset_rIP_var;
}TRAMPOLINE, *PTRAMPOLINE;

typedef struct	{
	void *patch_addr;
	void *patch_addr_end;
	int num_trampoline;
}PATCH_BLOCK,*PPATCH_BLOCK;

// Taken from udis86.
static int input_hook_x(ud_t* u);

/*
 * query_all_org_func_addr - Queries the addresses of all orginal functions to hook.
 * Returns:
 *   void
 */
static void query_all_org_func_addr(void);

/*
 * query_registered_module - Queries the index of a given library name in registered libs.
 *   @module_name: The name of shared library. Both short name ("ld") and full name ("ld-2.17.so") are accepted. 
 * Returns:
 *   The index in registered libs. (-1) if not found. 
 */
static int query_registered_module(const char *module_name);

/*
 * get_position_of_next_line - Determine the offset of the next new line in a string buffer.
 *   @buff: The string buffer
 *   @pos_start: The starting offset to search
 *   @max_buff_size: The max length of buff[]
 * Returns:
 *   The offset of the next new line. (-1) means reaching the end of buffer. 
 */
static int get_position_of_next_line(const char buff[], const int pos_start, const int max_buff_size);

/*
 * Initialize udis86 related stuff before decompiling. 
 */ 
static void init_udis86(void);

/*
 * determine_mem_block_size - Determine we need to change the permission of one or two pages 
 * for a given address.
 *   @addr: The address of the entry of original function. We will change it to a jmp instruction. 
 *   @page_size: The size of one page in current system. 
 * Returns:
 *   The number of bytes we need to change permission with mprotect(). 
 */
static size_t determine_mem_block_size(const void *addr, const unsigned long int page_size);

/*
 * determine_lib_path - Determine the full paths of three libraries, ld.so, libc.so and libpthread.so. 
 */
static void determine_lib_path(void);

/*
 * get_module_maps - Read "/proc/%pid/maps" and extract the names of modules. 
 */
static void get_module_maps(void);

/*
 * allocate_memory_block_for_patches - Allocated memory blocks to hold the patches for hook.  
 */
static void allocate_memory_block_for_patches(void);

/*
 * query_lib_name_in_list - Query the index of the name of a library in lib_name_list[].
 *   @lib_name_str: The ibrary name
 * Returns:
 *   The index in lib_name_list[]. (-1) means not found in the list. 
 */
static int query_lib_name_in_list(const char *lib_name_str);

#endif

