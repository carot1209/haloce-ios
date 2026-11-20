# GUEST_THREAD_ENTRY.S
#
# guest_thread_entry(thread, stack_top): where the host starts a guest
# thread, on the new thread's own instance of the module. Each instance
# begins with the linker's stack pointer, the main thread's, so the new
# thread's stack (which the host mapped in guest memory) is set first; the
# rest is C (__guest_thread_start, guest_thread_wasm.c).

	.globaltype	__stack_pointer, i32
	.functype	__guest_thread_start (i32) -> ()

	.globl	guest_thread_entry
	.export_name	guest_thread_entry, guest_thread_entry
	.type	guest_thread_entry,@function
guest_thread_entry:
	.functype	guest_thread_entry (i32, i32) -> ()
	local.get	1
	global.set	__stack_pointer
	local.get	0
	call	__guest_thread_start
	end_function

# guest_attach_entry(stack_top) -> thread: the same for a host thread that
# runs guest code (SDL's audio thread), with a stack the host mapped

	.functype	__guest_thread_attach () -> (i32)
	.globl	guest_attach_entry
	.export_name	guest_attach_entry, guest_attach_entry
	.type	guest_attach_entry,@function
guest_attach_entry:
	.functype	guest_attach_entry (i32) -> (i32)
	local.get	0
	global.set	__stack_pointer
	call	__guest_thread_attach
	end_function
