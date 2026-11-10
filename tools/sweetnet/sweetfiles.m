/*
 * sweetfiles.dylib: lets fileproviderd accept this port's HFS+ data volume, so the Files app can show "On My iPhone".
 *
 * FileProvider's FPVolumeFPFSSupported(const struct statfs *, BOOL, uint64_t *reasons, id *error) refuses a volume that
 * is read-only (reason 0x8), not local (0x10), quarantined (0x20), not APFS (0x6, f_fstypename must be "apfs") or not
 * encrypted (0x4, APFSVolumeGetVEKState on f_mntfromname). This port's data is HFS+ (no APFS driver in the kernel), so
 * the answer is always "non-apfs": fileproviderd never sets up the local storage domain and the Files app stays empty.
 *
 * FileProviderDaemon calls the function through a call stub whose pointer slot is in the dyld cache (iOS 26.6.2: stub
 * adrp x17 / add x17 / ldr x16, [x17] / br x16). For every image loaded in fileproviderd (now and later) this library finds every stub whose
 * slot holds FPVolumeFPFSSupported (found with dlsym: no addresses) and points the slot at our function, in this process
 * only (vm_protect with VM_PROT_COPY, as the GPU hooks do). Our function calls Apple's and changes its answer to "yes"
 * only when the volume is local, read-write HFS and the only reasons are "non-apfs" / "unencrypted"; every other answer
 * is Apple's. Each call is logged (first 64) to /private/var/tmp/sweetfiles.log and to the unified log, subsystem
 * com.sweet.files.
 *
 * Loaded by an LC_LOAD_WEAK_DYLIB in /System/Library/Frameworks/FileProvider.framework/Support/fileproviderd
 * (work/files-install.py on the PC); inert in any other program. Off without removing anything: create
 * /private/var/tmp/sweetfiles.off and restart fileproviderd. iOS 26, arm64e + pacstrip.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <os/log.h>
#include <ptrauth.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <time.h>
#include <unistd.h>

#define REASON_NON_APFS 0x6ull  /* includes the "unencrypted" bit 0x4 */
#define LOG_FILE "/private/var/tmp/sweetfiles.log"
#define OFF_FILE "/private/var/tmp/sweetfiles.off"

typedef BOOL (*fpfs_fn)(const struct statfs *, BOOL, uint64_t *, id __autoreleasing *);

static os_log_t gLog;
static fpfs_fn gOrig;

static void
note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
note(const char *fmt, ...)
{
	static unsigned lines;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	os_log(gLog, "%{public}s", buf);
	if (lines++ >= 64) {
		return;
	}
	FILE *f = fopen(LOG_FILE, "a");
	if (f) {
		fprintf(f, "%ld pid %d: %s\n", (long)time(NULL), getpid(), buf);
		fclose(f);
	}
}

static BOOL
sweet_fpfs_supported(const struct statfs *st, BOOL feature, uint64_t *reasons, id __autoreleasing *error)
{
	uint64_t mine = 0;
	BOOL yes = gOrig(st, feature, &mine, error);
	if (reasons) {
		*reasons = mine;
	}
	bool ours = !yes && st && strcmp(st->f_fstypename, "hfs") == 0 && !(st->f_flags & MNT_RDONLY) &&
	    (st->f_flags & MNT_LOCAL) && mine != 0 && (mine & ~REASON_NON_APFS) == 0;
	note("check %s (%s on %s, flags %#x, feature %d): Apple %s, reasons %#llx -> %s", st ? st->f_mntonname : "?",
	    st ? st->f_fstypename : "?", st ? st->f_mntfromname : "?", st ? st->f_flags : 0, feature, yes ? "yes" : "no",
	    (unsigned long long)mine, ours ? "YES (ours)" : yes ? "yes" : "no");
	if (!ours) {
		return yes;
	}
	if (reasons) {
		*reasons = 0;
	}
	if (error) {
		*error = nil;
	}
	return YES;
}

/* One stub: adrp x17, page / add x17, x17, #off / ldr x16, [x17] / br x16 (or braa x16, x17). Returns its slot. */
static uintptr_t *
stub_slot(const uint32_t *p, uintptr_t pc, bool *signed_slot)
{
	uint32_t adrp = p[0], add = p[1], ldr = p[2], br = p[3];
	if ((adrp & 0x9f00001f) != 0x90000011 || (add & 0xffc003ff) != 0x91000231 || ldr != 0xf9400230) {
		return NULL;
	}
	if (br != 0xd61f0200 && br != 0xd71f0a11) {
		return NULL;
	}
	int64_t imm = (int64_t)((((adrp >> 5) & 0x7ffff) << 2) | ((adrp >> 29) & 3));
	imm = (imm << 43) >> 31; /* sign-extend 21 bits, times 4096 */
	uintptr_t slot = (pc & ~(uintptr_t)0xfff) + (uintptr_t)imm + ((add >> 10) & 0xfff);
	*signed_slot = br == 0xd71f0a11;
	return (uintptr_t *)slot;
}

static bool
write_slot(uintptr_t *slot, uintptr_t value)
{
	vm_address_t page = (vm_address_t)slot & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		note("vm_protect rw of %p failed: %d", (void *)slot, kr);
		return false;
	}
	*slot = value;
	vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ);
	return true;
}

static unsigned
hook_section(const struct mach_header_64 *mh, const char *sect, unsigned *done)
{
	unsigned long size = 0;
	const uint8_t *stubs = getsectiondata(mh, "__TEXT", sect, &size);
	uintptr_t want = (uintptr_t)ptrauth_strip((void *)gOrig, ptrauth_key_asia);
	uintptr_t ours = (uintptr_t)ptrauth_strip((void *)&sweet_fpfs_supported, ptrauth_key_asia);
	unsigned found = 0;
	for (unsigned long off = 0; stubs && off + 16 <= size; off += 4) {
		bool sig = false;
		uintptr_t *slot = stub_slot((const uint32_t *)(stubs + off), (uintptr_t)(stubs + off), &sig);
		if (!slot) {
			continue;
		}
		uintptr_t now = (uintptr_t)ptrauth_strip((void *)*slot, ptrauth_key_asia);
		if (now != want && now != ours) {
			continue;
		}
		found++;
		if (now == want) {
			uintptr_t v = sig ? (uintptr_t)ptrauth_sign_unauthenticated((void *)ours, ptrauth_key_asia, (uintptr_t)slot) : ours;
			*done += write_slot(slot, v);
		}
		off += 12;
	}
	return found;
}

/*
 * Every image, also those loaded later (plug-in bundles such as FileProviderOverride.bundle): the check is reached from
 * images other than FileProviderDaemon (2026-11-10 test 1: the check logged "non-apfs" without passing our stub).
 */
static void
image_added(const struct mach_header *mh, intptr_t slide)
{
	(void)slide;
	static unsigned seen;
	unsigned done = 0;
	const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
	unsigned found = hook_section(h, "__auth_stubs", &done) + hook_section(h, "__stubs", &done);
	seen++;
	if (found) {
		Dl_info info;
		const char *p = dladdr(mh, &info) && info.dli_fname ? info.dli_fname : "?";
		const char *b = strrchr(p, '/');
		note("%s (image %u): %u stub(s) for FPVolumeFPFSSupported, %u slot(s) pointed at ours now", b ? b + 1 : p, seen,
		    found, done);
	}
}

static void
hook_all(void)
{
	_dyld_register_func_for_add_image(image_added);
	note("watching %u loaded images and any loaded later", _dyld_image_count());
}

__attribute__((constructor)) static void
sweetfiles_init(void)
{
	gLog = os_log_create("com.sweet.files", "files");
	if (strcmp(getprogname(), "fileproviderd") != 0) {
		return;
	}
	if (access(OFF_FILE, F_OK) == 0) {
		note("off (%s exists): Apple's check unchanged", OFF_FILE);
		return;
	}
	gOrig = (fpfs_fn)dlsym(RTLD_DEFAULT, "FPVolumeFPFSSupported");
	if (!gOrig) {
		note("FPVolumeFPFSSupported not found: nothing changed");
		return;
	}
	hook_all();
}
