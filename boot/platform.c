extern int kmain();

#ifdef EFI_PLATFORM
#include <efi.h>
#include <efilib.h>
EFI_HANDLE ImageHandleIn;

extern int init_graphics();

EFI_STATUS
	EFIAPI
efi_main (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
	InitializeLib(ImageHandle, SystemTable);
	ST = SystemTable;
	ImageHandleIn = ImageHandle;

	init_graphics();

	return kmain();
}
#else
#include <stdint.h>
#include <stddef.h>
#include "iso9660.h"
#include "util.h"
#include "text.h"
extern char _bss_start[];
extern char _bss_end[];

void * memcpy(void * restrict dest, const void * restrict src, long n) {
	asm volatile("cld; rep movsb"
	            : "=c"((int){0})
	            : "D"(dest), "S"(src), "c"(n)
	            : "flags", "memory");
	return dest;
}

void * memset(void * dest, int c, long n) {
	asm volatile("cld; rep stosb"
	             : "=c"((int){0})
	             : "D"(dest), "a"(c), "c"(n)
	             : "flags", "memory");
	return dest;
}

extern void init_graphics(void);
extern void do_bios_call(uint32_t function, uint32_t arg1);

extern uint32_t vbe_cont_info_mode_off;
extern uint16_t vbe_info_pitch;
extern uint16_t vbe_info_width;
extern uint16_t vbe_info_height;
extern uint8_t vbe_info_bpp;
extern uint16_t vbe_info;

void text_reset(void) {
	/* Hide the cursor */
	outportb(0x3D4, 14);
	outportb(0x3D5, 0xFF);
	outportb(0x3D4, 15);
	outportb(0x3D5, 0xFF);

	/* iirc this disables blink? */
	inportb(0x3DA);
	outportb(0x3C0, 0x30);
	char b = inportb(0x3C1);
	b &= ~8;
	outportb(0x3c0, b);
}

extern int in_graphics_mode;
int bios_text_mode(void) {
	do_bios_call(3, 3);
	vbe_info_width = 0;
	in_graphics_mode = 0;
	text_reset();
}

int bios_video_mode(void) {
	int best_match = 0;
	int match_score = 0;

#define MATCH(w,h,s) if (match_score < s && vbe_info_width == w && vbe_info_height == h) { best_match = *x; match_score = s; }

	uint32_t vbe_addr = ((vbe_cont_info_mode_off & 0xFFFF0000) >> 12) + (vbe_cont_info_mode_off & 0xFFFF);

	for (uint16_t * x = (uint16_t*)vbe_addr; *x != 0xFFFF;  x++) {
		/* Query mode info */
		do_bios_call(2, *x);

		if (!(vbe_info & (1 << 7))) continue;
		if (vbe_info_bpp < 24) continue;

		if (vbe_info_bpp == 32) {
			if (match_score < 9) { best_match = *x; match_score = 9; }
			MATCH(1024,768,10);
			MATCH(1280,720,50);
			MATCH(1920,1080,75);
			MATCH(1440,900,100);
		} else if (vbe_info_bpp == 24) {
			if (!match_score) { best_match = *x; match_score = 1; }
			MATCH(1024,768,3);
			MATCH(1280,720,4);
			MATCH(1920,1080,5);
			MATCH(1440,900,6);
		}

		//print_int_(vbe_info_width); print_("x"); print_int_(vbe_info_height); print_("x"); print_int_(vbe_info_bpp); print_("bpp\n");
	}

	if (best_match) {
		do_bios_call(2, best_match);
		do_bios_call(3, best_match | 0x4000);
	} else {
		vbe_info_width = 0;
	}

	init_graphics();
}

void bios_toggle_mode(void) {
	if (in_graphics_mode) {
		bios_text_mode();
	} else {
		bios_video_mode();
	}
}

int bios_main(void) {
	/* Zero BSS */
	memset(&_bss_start,0,(uintptr_t)&_bss_end-(uintptr_t)&_bss_start);

	text_reset();
	bios_video_mode();


	return kmain();
}

extern volatile uint16_t dap_sectors;
extern volatile uint32_t dap_buffer;
extern volatile uint32_t dap_lba_low;
extern volatile uint32_t dap_lba_high;
extern volatile uint16_t drive_params_bps;
extern uint8_t disk_space[];

int bios_call(char * into, uint32_t sector) {
	dap_sectors = 2048 / drive_params_bps;
	dap_buffer = (uint32_t)disk_space;
	dap_lba_low = sector * dap_sectors;
	dap_lba_high = 0;
	do_bios_call(1,0);
	memcpy(into, disk_space, 2048);
}

iso_9660_volume_descriptor_t * root = NULL;
iso_9660_directory_entry_t * dir_entry = NULL;
static char * dir_entries = NULL;

int navigate(char * name) {
	dir_entry = (iso_9660_directory_entry_t*)&root->root;

	dir_entries = (char*)(DATA_LOAD_BASE + dir_entry->extent_start_LSB * ISO_SECTOR_SIZE);
	bios_call(dir_entries, dir_entry->extent_start_LSB);
	long offset = 0;
	while (1) {
		iso_9660_directory_entry_t * dir = (iso_9660_directory_entry_t *)(dir_entries + offset);
		if (dir->length == 0) {
			if (offset < dir_entry->extent_length_LSB) {
				offset += 1;
				goto try_again;
			}
			break;
		}
		if (!(dir->flags & FLAG_HIDDEN)) {
			char file_name[dir->name_len + 1];
			memcpy(file_name, dir->name, dir->name_len);
			file_name[dir->name_len] = 0;
			char * s = strchr(file_name,';');
			if (s) {
				*s = '\0';
			}
			if (!strcmp(file_name, name)) {
				dir_entry = dir;
				return 1;
			}
		}
		offset += dir->length;
try_again:
		if ((long)(offset) > dir_entry->extent_length_LSB) break;
	}

	return 0;
}


#endif
