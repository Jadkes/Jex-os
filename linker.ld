/* The bootloader will look at this image and start execution at the symbol
   designated as the entry point. */
ENTRY(_start)

/* Tell where the various sections of the object files will be put in the final
   kernel image. */
SECTIONS
{
	/* Begin putting sections at 1 MiB, a conventional place for kernels to be
	   loaded at by the bootloader. */
	. = 1M;

	/* First put the multiboot header, as it is required to be put very early
	   early in the image or the bootloader won't recognize the file format.
	   Next we'll put the .text section. */
	.text BLOCK(4K) : ALIGN(4K)
	{
		*(.multiboot)
		*(.text)
	}

	/* Initcall tables -- collected function pointers.
	   early_initcalls run first (CPU, memory), then device_initcalls. */
	.early_initcalls BLOCK(4K) : ALIGN(4)
	{
		__early_initcall_start = .;
		*(.early_initcalls)
		__early_initcall_end = .;
	}

	.device_initcalls BLOCK(4K) : ALIGN(4)
	{
		__device_initcall_start = .;
		*(.device_initcalls)
		__device_initcall_end = .;
	}

	/* Read-only data. */
	.rodata BLOCK(4K) : ALIGN(4K)
	{
		*(.rodata)
	}

	/* Read-write data (initialized) */
	.data BLOCK(4K) : ALIGN(4K)
	{
		*(.data)
	}

	/* Kallsyms symbol table (sorted, read-only) */
	.kallsyms BLOCK(4K) : ALIGN(4K)
	{
		__kallsyms_start = .;
		*(.kallsyms)
		__kallsyms_end = .;
	}

	/* Read-write data (uninitialized) and stack */
	.bss BLOCK(4K) : ALIGN(4K)
	{
		*(COMMON)
		*(.bss)
	}

	/* The compiler may produce other sections, by default it will put them in
	   a segment with the same name. Simply add stuff here as needed. */
}
